// VK_LAYER_HEXSCALE — Vulkan implicit layer applying a contrast-adaptive
// sharpening (CAS) pass to swapchain images right before presentation.
//
// Design rules, learned the hard way on real hardware:
//   1. Fail-open: any error, missing capability or unknown object makes the
//      layer fall back to plain passthrough. A sharpening layer must never
//      be able to black-screen or freeze a game.
//   2. The present path never blocks: telemetry runs on a background thread,
//      Vulkan work is submitted fire-and-forget with deferred destruction.
//   3. Processing happens in-place on the swapchain image (CAS into an
//      RGBA8 target -> verbatim copy back), so compositor-side upscaling and
//      present timing are untouched.
//   4. The pass is a fragment-shader fullscreen triangle sampling the frame
//      through a native-format view — works for RGBA and BGRA swapchains
//      alike (gamescope presents BGRA on KMS planes). The BGRA variant
//      pre-swizzles its output so the verbatim copy-back lands correctly.
//
// Enabled with ENABLE_HEXSCALE=1 (manifest), killed with DISABLE_HEXSCALE=1,
// sharpness with HEXSCALE_SHARPNESS=0..1 (or live from the daemon / QAM),
// debug logging with HEXSCALE_DEBUG=1.

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ipc_protocol.hpp"

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

#ifndef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

// SPIR-V for cas.vert / cas.frag (see scripts/build-shader.sh to regenerate).
#include "cas_spv.h"

namespace {

constexpr uint32_t kMaxSwapchainImages = 8;    // DXVK/gamescope use 2-4
constexpr uint32_t kMaxWaitSemaphores = 8;     // more -> passthrough
constexpr uint64_t kSemaphoreRetireDelay = 30; // presents before destroying

// ---------------------------------------------------------------------------
// Configuration (env, read once at load)
// ---------------------------------------------------------------------------

struct Config {
    bool enabled = true;      // DISABLE_HEXSCALE=1 kills every interception
    bool debug = false;       // HEXSCALE_DEBUG=1 -> stderr diagnostics
    float sharpness = 0.5f;   // HEXSCALE_SHARPNESS 0..1
};

Config load_config() {
    Config c;
    if (const char* v = getenv("DISABLE_HEXSCALE")) {
        if (v[0] == '1' && v[1] == '\0') c.enabled = false;
    }
    if (const char* v = getenv("HEXSCALE_DEBUG")) {
        if (v[0] == '1' && v[1] == '\0') c.debug = true;
    }
    if (const char* v = getenv("HEXSCALE_SHARPNESS")) {
        float f = strtof(v, nullptr);
        if (f >= 0.0f && f <= 1.0f) c.sharpness = f;
    }
    return c;
}

const Config g_config = load_config();

#define HEX_LOG(...)                                  \
    do {                                              \
        if (g_config.debug) {                         \
            fprintf(stderr, "[hexscale] " __VA_ARGS__); \
            fputc('\n', stderr);                      \
        }                                             \
    } while (0)

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

std::mutex g_lock;  // guards all maps below

std::unordered_map<void*, PFN_vkGetInstanceProcAddr> g_instance_dispatch;
std::unordered_map<void*, PFN_vkGetDeviceProcAddr> g_device_dispatch;

// Any live instance + its chained gipa, so per-device setup can query
// physical-device properties (instance-level entry points).
PFN_vkGetInstanceProcAddr g_any_gipa = nullptr;
VkInstance g_any_instance = VK_NULL_HANDLE;

std::atomic<bool> g_daemon_active{true};      // daemon says processing is on
std::atomic<float> g_daemon_sharpness{-1.0f}; // -1 = no daemon opinion yet

inline void* get_dispatch_key(const void* handle) {
    return handle ? *reinterpret_cast<void* const*>(handle) : nullptr;
}

// ---------------------------------------------------------------------------
// Async telemetry: frame accounting + daemon polling, never on present path
// ---------------------------------------------------------------------------

class Telemetry {
public:
    void start() {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_thread.joinable()) return;
        m_running = true;
        m_thread = std::thread(&Telemetry::run, this);
    }

    ~Telemetry() {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_running = false;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    void add_frames(uint32_t count, float gpu_ms) {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_pending_frames += count;
            if (gpu_ms >= 0.0f) {
                m_last_ms = gpu_ms;
                m_has_timing = true;
            }
        }
        m_cv.notify_one();
    }

private:
    void run() {
        std::unique_lock<std::mutex> lk(m_mtx);
        while (m_running) {
            m_cv.wait_for(lk, std::chrono::seconds(1), [this] { return !m_running; });
            if (!m_running) break;
            uint32_t frames = m_pending_frames;
            m_pending_frames = 0;
            float ms = m_has_timing ? m_last_ms : 0.0f;
            lk.unlock();

            if (frames > 0) {
                send_report(frames, ms);
                touch_active_file();
            }
            poll_daemon_state();

            lk.lock();
        }
    }

    // Daemon socket candidates, resolved once per attempt: explicit
    // override, the system service location, then the user service one.
    static bool fill_socket_path(struct sockaddr_un& addr) {
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        const char* candidates[3] = {};
        candidates[0] = getenv("HEXSCALE_SOCKET");
        candidates[1] = hexscale::ipc::DEFAULT_SOCKET_PATH.data();
        static std::string xdg_path;  // outlives the calls
        if (const char* xdg = getenv("XDG_RUNTIME_DIR")) {
            xdg_path = std::string(xdg) + "/hexscale/control.sock";
            candidates[2] = xdg_path.c_str();
        }
        for (const char* c : candidates) {
            if (c && c[0] && std::strlen(c) < sizeof(addr.sun_path)) {
                std::strncpy(addr.sun_path, c, sizeof(addr.sun_path) - 1);
                return true;
            }
        }
        return false;
    }

    static bool rpc(const hexscale::ipc::CommandPacket& cmd,
                    hexscale::ipc::ResponsePacket& resp) {
        int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) return false;
        ::fcntl(sock, F_SETFD, FD_CLOEXEC);

        struct sockaddr_un addr{};
        if (!fill_socket_path(addr)) {
            ::close(sock);
            return false;
        }

        struct timeval tv{.tv_sec = 0, .tv_usec = 50000};
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        bool ok = false;
        if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            if (::send(sock, &cmd, sizeof(cmd), 0) == sizeof(cmd)) {
                ok = ::recv(sock, &resp, sizeof(resp), 0) == sizeof(resp);
            }
        }
        ::close(sock);
        return ok;
    }

    void send_report(uint32_t frames, float gpu_ms) {
        hexscale::ipc::CommandPacket cmd{};
        cmd.header.magic = hexscale::ipc::PROTOCOL_MAGIC;
        cmd.header.version = hexscale::ipc::PROTOCOL_VERSION;
        cmd.header.msg_type = static_cast<uint16_t>(hexscale::ipc::CommandType::REPORT_FRAMES);
        cmd.payload.report_frames.frame_count = frames;
        cmd.payload.report_frames.inference_ms = gpu_ms;
        hexscale::ipc::ResponsePacket resp{};
        rpc(cmd, resp);
    }

    void poll_daemon_state() {
        hexscale::ipc::CommandPacket cmd{};
        cmd.header.magic = hexscale::ipc::PROTOCOL_MAGIC;
        cmd.header.version = hexscale::ipc::PROTOCOL_VERSION;
        cmd.header.msg_type = static_cast<uint16_t>(hexscale::ipc::CommandType::GET_STATUS);
        hexscale::ipc::ResponsePacket resp{};
        if (rpc(cmd, resp)) {
            g_daemon_active.store(resp.status_data.enabled != 0);
            float s = resp.status_data.sharpness;
            if (s >= 0.0f && s <= 1.0f) g_daemon_sharpness.store(s);
        }
        // Daemon unreachable: keep last known sharpness and stay enabled —
        // the layer must never hard-stop a game because hexscaled died.
    }

    static void touch_active_file() {
        int fd = ::open("/run/hexscale/active", O_WRONLY | O_CLOEXEC);
        if (fd >= 0) ::close(fd);
    }

    std::thread m_thread;
    std::mutex m_mtx;
    std::condition_variable m_cv;
    bool m_running = false;
    uint32_t m_pending_frames = 0;
    float m_last_ms = 0.0f;
    bool m_has_timing = false;
};

Telemetry g_telemetry;

// ---------------------------------------------------------------------------
// Per-device CAS state
// ---------------------------------------------------------------------------

struct DeviceState {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    float timestamp_period_ns = 1.0f;

    // CAS resources shared by every swapchain of this device
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;       // RGBA swapchains
    VkPipeline pipeline_bgra = VK_NULL_HANDLE;  // BGRA swapchains
    bool pipeline_failed = false;               // one failed attempt -> inert
    VkQueryPool timing_pool = VK_NULL_HANDLE;
    uint32_t timing_seq = 0;

    // Queue family info per VkQueue handle (filled by vkGetDeviceQueue hooks)
    std::unordered_map<VkQueue, uint32_t> queue_families;
    uint32_t any_gfx_compute_family = UINT32_MAX;
    uint32_t any_gfx_compute_timestamp_bits = 0;

    // Command pool for the CAS submits (lazily created)
    VkCommandPool cmd_pool = VK_NULL_HANDLE;

    // Per-present bookkeeping. Fences/command buffers are retired as soon as
    // their fence signals; present semaphores additionally wait a bounded
    // number of further presents (a present that consumed them is long done).
    struct Pending {
        VkFence fence;
        VkSemaphore semaphore;
        VkCommandBuffer cmd;
        uint32_t timing_pair; // first query of this frame's pair, or UINT32_MAX
        uint64_t present_seq; // for the semaphore retirement window
    };
    std::vector<Pending> pending;
    uint64_t present_counter = 0;
};

struct SwapchainState {
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t image_count = 0;
    VkImage images[kMaxSwapchainImages] = {};

    // CAS resources: RGBA8 target (+framebuffer), sampled views/sets per image
    VkImage intermediate = VK_NULL_HANDLE;
    VkDeviceMemory intermediate_mem = VK_NULL_HANDLE;
    VkImageView out_view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkImageView in_views[kMaxSwapchainImages] = {};
    VkDescriptorSet sets[kMaxSwapchainImages] = {};

    bool usable = false; // false -> passthrough for this swapchain forever
};

std::unordered_map<void*, DeviceState*> g_devices;             // key: device dispatch key
std::unordered_map<VkSwapchainKHR, SwapchainState*> g_swapchains;

// Resolve a device-level function through the next layer in the chain.
template <typename Fn>
Fn devfn(DeviceState* ds, const char* name) {
    return reinterpret_cast<Fn>(ds->gdpa(ds->device, name));
}

// ---------------------------------------------------------------------------
// CAS resource creation / destruction (error-checked, fail-open)
// ---------------------------------------------------------------------------

bool create_cas_pipeline(DeviceState* ds) {
    VkDevice dev = ds->device;
    auto create_dsl = devfn<PFN_vkCreateDescriptorSetLayout>(ds, "vkCreateDescriptorSetLayout");
    auto create_pl = devfn<PFN_vkCreatePipelineLayout>(ds, "vkCreatePipelineLayout");
    auto create_module = devfn<PFN_vkCreateShaderModule>(ds, "vkCreateShaderModule");
    auto create_pipelines = devfn<PFN_vkCreateGraphicsPipelines>(ds, "vkCreateGraphicsPipelines");
    auto destroy_module = devfn<PFN_vkDestroyShaderModule>(ds, "vkDestroyShaderModule");
    auto create_rp = devfn<PFN_vkCreateRenderPass>(ds, "vkCreateRenderPass");
    auto create_sampler = devfn<PFN_vkCreateSampler>(ds, "vkCreateSampler");
    auto create_query_pool = devfn<PFN_vkCreateQueryPool>(ds, "vkCreateQueryPool");

    // Trivial sampler (texelFetch ignores filtering, the descriptor just
    // needs a valid one).
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    if (create_sampler(dev, &sampler_info, nullptr, &ds->sampler) != VK_SUCCESS)
        return false;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo dsl_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl_info.bindingCount = 1;
    dsl_info.pBindings = &binding;
    if (create_dsl(dev, &dsl_info, nullptr, &ds->dsl) != VK_SUCCESS)
        return false;

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = sizeof(float);

    VkPipelineLayoutCreateInfo pl_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl_info.setLayoutCount = 1;
    pl_info.pSetLayouts = &ds->dsl;
    pl_info.pushConstantRangeCount = 1;
    pl_info.pPushConstantRanges = &pc;
    if (create_pl(dev, &pl_info, nullptr, &ds->pl) != VK_SUCCESS)
        return false;

    // Render pass: single RGBA8 attachment, contents discarded on load,
    // handed over to the copy in TRANSFER_SRC layout at the end.
    VkAttachmentDescription attach{};
    attach.format = VK_FORMAT_R8G8B8A8_UNORM;
    attach.samples = VK_SAMPLE_COUNT_1_BIT;
    attach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attach.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkRenderPassCreateInfo rp_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &attach;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    if (create_rp(dev, &rp_info, nullptr, &ds->render_pass) != VK_SUCCESS)
        return false;

    // Shader modules
    auto make_module = [&](const uint32_t* code, size_t bytes, VkShaderModule& out) {
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize = bytes;
        sm.pCode = code;
        return create_module(dev, &sm, nullptr, &out) == VK_SUCCESS;
    };
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE, fs_bgra = VK_NULL_HANDLE;
    if (!make_module(k_cas_vert_spv, sizeof(k_cas_vert_spv), vs)) return false;
    if (!make_module(k_cas_frag_spv, sizeof(k_cas_frag_spv), fs)) return false;
    if (!make_module(k_cas_frag_bgra_spv, sizeof(k_cas_frag_bgra_spv), fs_bgra)) return false;

    // Graphics pipeline: fullscreen triangle, dynamic viewport/scissor.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_asm{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_asm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_attach{};
    blend_attach.blendEnable = VK_FALSE;
    blend_attach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attach;

    VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo gp_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp_info.stageCount = 2;
    gp_info.pStages = stages;
    gp_info.pVertexInputState = &vertex_input;
    gp_info.pInputAssemblyState = &input_asm;
    gp_info.pViewportState = &viewport_state;
    gp_info.pRasterizationState = &raster;
    gp_info.pMultisampleState = &multisample;
    gp_info.pColorBlendState = &blend;
    gp_info.pDynamicState = &dynamic;
    gp_info.layout = ds->pl;
    gp_info.renderPass = ds->render_pass;
    gp_info.subpass = 0;

    VkResult r1 = create_pipelines(dev, VK_NULL_HANDLE, 1, &gp_info, nullptr, &ds->pipeline);
    stages[1].module = fs_bgra;
    VkResult r2 =
        create_pipelines(dev, VK_NULL_HANDLE, 1, &gp_info, nullptr, &ds->pipeline_bgra);

    destroy_module(dev, vs, nullptr);
    destroy_module(dev, fs, nullptr);
    destroy_module(dev, fs_bgra, nullptr);
    if (r1 != VK_SUCCESS || r2 != VK_SUCCESS) return false;

    if (ds->any_gfx_compute_timestamp_bits >= 1) {
        VkQueryPoolCreateInfo qp_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qp_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qp_info.queryCount = 64; // 32 in-flight timestamp pairs, round-robin
        if (create_query_pool(dev, &qp_info, nullptr, &ds->timing_pool) != VK_SUCCESS)
            ds->timing_pool = VK_NULL_HANDLE; // timing is optional
    }

    HEX_LOG("CAS pipelines created (timestamp pool: %s)",
            ds->timing_pool ? "yes" : "no");
    return true;
}

bool allocate_descriptor_sets(DeviceState* ds, SwapchainState* sc) {
    VkDevice dev = ds->device;
    auto create_pool = devfn<PFN_vkCreateDescriptorPool>(ds, "vkCreateDescriptorPool");
    auto alloc_sets = devfn<PFN_vkAllocateDescriptorSets>(ds, "vkAllocateDescriptorSets");
    auto update_sets = devfn<PFN_vkUpdateDescriptorSets>(ds, "vkUpdateDescriptorSets");

    if (ds->descriptor_pool == VK_NULL_HANDLE) {
        VkDescriptorPoolSize pool_sizes[1]{};
        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_sizes[0].descriptorCount = 4 * kMaxSwapchainImages; // a few swapchains
        VkDescriptorPoolCreateInfo dp_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp_info.maxSets = 4 * kMaxSwapchainImages;
        dp_info.poolSizeCount = 1;
        dp_info.pPoolSizes = pool_sizes;
        if (create_pool(dev, &dp_info, nullptr, &ds->descriptor_pool) != VK_SUCCESS)
            return false;
    }

    std::vector<VkDescriptorSetLayout> layouts(sc->image_count, ds->dsl);
    VkDescriptorSetAllocateInfo alloc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc_info.descriptorPool = ds->descriptor_pool;
    alloc_info.descriptorSetCount = sc->image_count;
    alloc_info.pSetLayouts = layouts.data();
    if (alloc_sets(dev, &alloc_info, sc->sets) != VK_SUCCESS)
        return false;

    for (uint32_t i = 0; i < sc->image_count; i++) {
        VkDescriptorImageInfo info{};
        info.sampler = ds->sampler;
        info.imageView = sc->in_views[i];
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = sc->sets[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &info;
        update_sets(dev, 1, &write, 0, nullptr);
    }
    return true;
}

void create_swapchain_resources(DeviceState* ds, SwapchainState* sc) {
    VkDevice dev = ds->device;
    auto vkCreateImage = devfn<PFN_vkCreateImage>(ds, "vkCreateImage");
    auto vkGetImageMemoryRequirements =
        devfn<PFN_vkGetImageMemoryRequirements>(ds, "vkGetImageMemoryRequirements");
    auto vkAllocateMemory = devfn<PFN_vkAllocateMemory>(ds, "vkAllocateMemory");
    auto vkBindImageMemory = devfn<PFN_vkBindImageMemory>(ds, "vkBindImageMemory");
    auto vkCreateImageView = devfn<PFN_vkCreateImageView>(ds, "vkCreateImageView");
    auto vkCreateFramebuffer = devfn<PFN_vkCreateFramebuffer>(ds, "vkCreateFramebuffer");

    // RGBA8 target: color attachment for the CAS pass + copy source. On
    // BGRA swapchains the shader pre-swizzles so the verbatim copy-back is
    // correct; on SRGB swapchains raw bytes roundtrip unchanged.
    VkImageCreateInfo img_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    img_info.extent = {sc->extent.width, sc->extent.height, 1};
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &img_info, nullptr, &sc->intermediate) != VK_SUCCESS) return;

    VkMemoryRequirements mem_req{};
    vkGetImageMemoryRequirements(dev, sc->intermediate, &mem_req);

    // Any memory type allowed by the requirements works; on Adreno the
    // device-local types are the ones reported for optimal-tiling images.
    uint32_t type_index = UINT32_MAX;
    for (uint32_t i = 0; i < 32; i++) {
        if (mem_req.memoryTypeBits & (1u << i)) {
            type_index = i;
            break;
        }
    }
    if (type_index == UINT32_MAX) return;

    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = type_index;
    if (vkAllocateMemory(dev, &alloc_info, nullptr, &sc->intermediate_mem) != VK_SUCCESS)
        return;
    if (vkBindImageMemory(dev, sc->intermediate, sc->intermediate_mem, 0) != VK_SUCCESS)
        return;

    auto make_view = [&](VkImage image, VkFormat format, VkImageView& out) {
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format; // native format: sampling handles BGRA fine
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return vkCreateImageView(dev, &view_info, nullptr, &out) == VK_SUCCESS;
    };

    if (!make_view(sc->intermediate, VK_FORMAT_R8G8B8A8_UNORM, sc->out_view)) return;
    // Input views keep the swapchain's own format (SRGB included: raw bytes
    // are read as-is and written back verbatim).
    for (uint32_t i = 0; i < sc->image_count; i++) {
        if (!make_view(sc->images[i], sc->format, sc->in_views[i])) return;
    }

    VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb_info.renderPass = ds->render_pass;
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = &sc->out_view;
    fb_info.width = sc->extent.width;
    fb_info.height = sc->extent.height;
    fb_info.layers = 1;
    if (vkCreateFramebuffer(dev, &fb_info, nullptr, &sc->framebuffer) != VK_SUCCESS)
        return;

    sc->usable = allocate_descriptor_sets(ds, sc);
    HEX_LOG("swapchain %ux%u fmt=%u: %s", sc->extent.width, sc->extent.height,
            static_cast<uint32_t>(sc->format), sc->usable ? "ready" : "FAILED");
}

void destroy_swapchain_resources(DeviceState* ds, SwapchainState* sc) {
    VkDevice dev = ds->device;
    if (sc->sets[0] != VK_NULL_HANDLE && ds->descriptor_pool != VK_NULL_HANDLE) {
        devfn<PFN_vkFreeDescriptorSets>(ds, "vkFreeDescriptorSets")(
            dev, ds->descriptor_pool, sc->image_count, sc->sets);
    }
    auto destroy_view = devfn<PFN_vkDestroyImageView>(ds, "vkDestroyImageView");
    for (uint32_t i = 0; i < kMaxSwapchainImages; i++) {
        if (sc->in_views[i]) destroy_view(dev, sc->in_views[i], nullptr);
    }
    if (sc->out_view) destroy_view(dev, sc->out_view, nullptr);
    if (sc->framebuffer) {
        devfn<PFN_vkDestroyFramebuffer>(ds, "vkDestroyFramebuffer")(
            dev, sc->framebuffer, nullptr);
    }
    if (sc->intermediate) {
        devfn<PFN_vkDestroyImage>(ds, "vkDestroyImage")(dev, sc->intermediate, nullptr);
    }
    if (sc->intermediate_mem) {
        devfn<PFN_vkFreeMemory>(ds, "vkFreeMemory")(dev, sc->intermediate_mem, nullptr);
    }
    delete sc;
}

// Retire finished per-present objects. Fences and command buffers go away as
// soon as the fence signals; the present semaphore additionally waits for a
// bounded window of later presents (matching what shipping overlay layers do).
void reap_pending(DeviceState* ds) {
    VkDevice dev = ds->device;
    auto get_fence = devfn<PFN_vkGetFenceStatus>(ds, "vkGetFenceStatus");
    auto destroy_fence = devfn<PFN_vkDestroyFence>(ds, "vkDestroyFence");
    auto destroy_sem = devfn<PFN_vkDestroySemaphore>(ds, "vkDestroySemaphore");
    auto free_cmd = devfn<PFN_vkFreeCommandBuffers>(ds, "vkFreeCommandBuffers");
    auto get_results = devfn<PFN_vkGetQueryPoolResults>(ds, "vkGetQueryPoolResults");

    std::vector<DeviceState::Pending> still_pending;
    still_pending.reserve(ds->pending.size());
    const uint64_t now = ds->present_counter;

    for (auto& p : ds->pending) {
        if (p.fence != VK_NULL_HANDLE && get_fence(dev, p.fence) != VK_SUCCESS) {
            still_pending.push_back(p);
            continue;
        }

        // Harvest GPU timing (two 64-bit timestamps) while the fence owns it.
        if (p.timing_pair != UINT32_MAX && ds->timing_pool != VK_NULL_HANDLE) {
            uint64_t stamps[2] = {0, 0};
            VkResult r = get_results(dev, ds->timing_pool, p.timing_pair, 2, sizeof(stamps),
                                     stamps, sizeof(stamps[0]), 0);
            if (r == VK_SUCCESS && stamps[1] >= stamps[0] && ds->timestamp_period_ns > 0) {
                float ms = static_cast<float>(
                    static_cast<double>(stamps[1] - stamps[0]) *
                    ds->timestamp_period_ns / 1e6);
                g_telemetry.add_frames(0, ms);
            }
        }

        if (p.fence != VK_NULL_HANDLE) destroy_fence(dev, p.fence, nullptr);
        if (p.cmd != VK_NULL_HANDLE) free_cmd(dev, ds->cmd_pool, 1, &p.cmd);

        if (now - p.present_seq < kSemaphoreRetireDelay) {
            // Fence done but the present that consumed the semaphore is not
            // provably finished: keep it a bit longer.
            p.fence = VK_NULL_HANDLE;
            p.cmd = VK_NULL_HANDLE;
            p.timing_pair = UINT32_MAX;
            still_pending.push_back(p);
        } else {
            destroy_sem(dev, p.semaphore, nullptr);
        }
    }
    ds->pending.swap(still_pending);
}

void destroy_device_state(DeviceState* ds) {
    if (!ds) return;
    if (ds->gdpa && ds->device) {
        VkDevice dev = ds->device;
        auto destroy_fence = devfn<PFN_vkDestroyFence>(ds, "vkDestroyFence");
        auto destroy_sem = devfn<PFN_vkDestroySemaphore>(ds, "vkDestroySemaphore");
        auto destroy_pool = devfn<PFN_vkDestroyCommandPool>(ds, "vkDestroyCommandPool");
        auto destroy_desc_pool =
            devfn<PFN_vkDestroyDescriptorPool>(ds, "vkDestroyDescriptorPool");

        // The app is destroying the device; everything must be done by now.
        devfn<PFN_vkDeviceWaitIdle>(ds, "vkDeviceWaitIdle")(dev);
        for (auto& p : ds->pending) {
            if (p.fence) destroy_fence(dev, p.fence, nullptr);
            if (p.semaphore) destroy_sem(dev, p.semaphore, nullptr);
        }
        if (ds->cmd_pool) destroy_pool(dev, ds->cmd_pool, nullptr);
        if (ds->descriptor_pool) destroy_desc_pool(dev, ds->descriptor_pool, nullptr);
        if (ds->pipeline) {
            devfn<PFN_vkDestroyPipeline>(ds, "vkDestroyPipeline")(dev, ds->pipeline, nullptr);
        }
        if (ds->pipeline_bgra) {
            devfn<PFN_vkDestroyPipeline>(ds, "vkDestroyPipeline")(dev, ds->pipeline_bgra, nullptr);
        }
        if (ds->sampler) {
            devfn<PFN_vkDestroySampler>(ds, "vkDestroySampler")(dev, ds->sampler, nullptr);
        }
        if (ds->render_pass) {
            devfn<PFN_vkDestroyRenderPass>(ds, "vkDestroyRenderPass")(dev, ds->render_pass, nullptr);
        }
        if (ds->pl) {
            devfn<PFN_vkDestroyPipelineLayout>(ds, "vkDestroyPipelineLayout")(
                dev, ds->pl, nullptr);
        }
        if (ds->dsl) {
            devfn<PFN_vkDestroyDescriptorSetLayout>(ds, "vkDestroyDescriptorSetLayout")(
                dev, ds->dsl, nullptr);
        }
        if (ds->timing_pool) {
            devfn<PFN_vkDestroyQueryPool>(ds, "vkDestroyQueryPool")(
                dev, ds->timing_pool, nullptr);
        }
    }
    delete ds;
}

// ---------------------------------------------------------------------------
// CAS submit at present time
// ---------------------------------------------------------------------------

// Returns the fresh semaphore gating the rewritten present, or VK_NULL_HANDLE
// on failure — the caller must then present the ORIGINAL pPresentInfo.
VkSemaphore try_cas_present(DeviceState* ds, SwapchainState* sc, VkQueue queue,
                            const VkPresentInfoKHR* pPresentInfo) {
    if (!sc->usable || ds->pipeline == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    if (pPresentInfo->waitSemaphoreCount > kMaxWaitSemaphores) return VK_NULL_HANDLE;

    VkDevice dev = ds->device;
    auto destroy_fence = devfn<PFN_vkDestroyFence>(ds, "vkDestroyFence");
    auto destroy_sem = devfn<PFN_vkDestroySemaphore>(ds, "vkDestroySemaphore");
    auto free_cmd = devfn<PFN_vkFreeCommandBuffers>(ds, "vkFreeCommandBuffers");

    const VkPipeline pipeline = (sc->format == VK_FORMAT_B8G8R8A8_UNORM ||
                                 sc->format == VK_FORMAT_B8G8R8A8_SRGB)
                                    ? ds->pipeline_bgra
                                    : ds->pipeline;
    if (!pipeline) return VK_NULL_HANDLE;

    // Queue family for the submits (tracked via vkGetDeviceQueue hooks).
    uint32_t family = UINT32_MAX;
    {
        auto it = ds->queue_families.find(queue);
        if (it != ds->queue_families.end()) family = it->second;
    }
    if (family == UINT32_MAX) family = ds->any_gfx_compute_family;
    if (family == UINT32_MAX) return VK_NULL_HANDLE;

    if (ds->cmd_pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = family;
        if (devfn<PFN_vkCreateCommandPool>(ds, "vkCreateCommandPool")(
                dev, &pool_info, nullptr, &ds->cmd_pool) != VK_SUCCESS)
            return VK_NULL_HANDLE;
    }

    reap_pending(ds);

    // Fresh per-present objects: fence (completion), semaphore (present gate),
    // command buffer (the CAS pass). Retired later by reap_pending().
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo sem_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkCommandBufferAllocateInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_info.commandPool = ds->cmd_pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;

    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    auto fail = [&]() {
        if (fence) destroy_fence(dev, fence, nullptr);
        if (semaphore) destroy_sem(dev, semaphore, nullptr);
        if (cmd) free_cmd(dev, ds->cmd_pool, 1, &cmd);
        return VK_NULL_HANDLE;
    };

    if (devfn<PFN_vkCreateFence>(ds, "vkCreateFence")(
            dev, &fence_info, nullptr, &fence) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    if (devfn<PFN_vkCreateSemaphore>(ds, "vkCreateSemaphore")(
            dev, &sem_info, nullptr, &semaphore) != VK_SUCCESS)
        return fail();
    if (devfn<PFN_vkAllocateCommandBuffers>(ds, "vkAllocateCommandBuffers")(
            dev, &cmd_info, &cmd) != VK_SUCCESS)
        return fail();

    const uint32_t image_index = pPresentInfo->pImageIndices[0];
    VkImage swap_image = sc->images[image_index];

    float sharpness = g_daemon_sharpness.load();
    if (sharpness < 0.0f) sharpness = g_config.sharpness;
    if (sharpness < 0.01f) {
        // Effectively off — don't pay for a pass nobody will see.
        return fail();
    }

    const bool want_timing = ds->timing_pool != VK_NULL_HANDLE;
    uint32_t timing_pair = UINT32_MAX;
    if (want_timing) timing_pair = (ds->timing_seq++ * 2) % 64;

    // --- Record the CAS pass ---
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (devfn<PFN_vkBeginCommandBuffer>(ds, "vkBeginCommandBuffer")(cmd, &begin_info) !=
        VK_SUCCESS)
        return fail();

    if (want_timing) {
        devfn<PFN_vkCmdResetQueryPool>(ds, "vkCmdResetQueryPool")(
            cmd, ds->timing_pool, timing_pair, 2);
        devfn<PFN_vkCmdWriteTimestamp>(ds, "vkCmdWriteTimestamp")(
            cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, ds->timing_pool, timing_pair);
    }

    // Swapchain image: PRESENT_SRC -> SHADER_READ (sampled by the pass)
    VkImageMemoryBarrier to_read{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_read.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_read.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_read.image = swap_image;
    to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    devfn<PFN_vkCmdPipelineBarrier>(ds, "vkCmdPipelineBarrier")(
        cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &to_read);

    // Fullscreen CAS draw into the RGBA8 intermediate; the render pass ends
    // with the target in TRANSFER_SRC layout.
    VkRenderPassBeginInfo rp_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp_begin.renderPass = ds->render_pass;
    rp_begin.framebuffer = sc->framebuffer;
    rp_begin.renderArea.offset = {0, 0};
    rp_begin.renderArea.extent = {sc->extent.width, sc->extent.height};

    devfn<PFN_vkCmdBeginRenderPass>(ds, "vkCmdBeginRenderPass")(
        cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    devfn<PFN_vkCmdBindPipeline>(ds, "vkCmdBindPipeline")(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    VkViewport viewport{};
    viewport.width = static_cast<float>(sc->extent.width);
    viewport.height = static_cast<float>(sc->extent.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {sc->extent.width, sc->extent.height};
    devfn<PFN_vkCmdSetViewport>(ds, "vkCmdSetViewport")(cmd, 0, 1, &viewport);
    devfn<PFN_vkCmdSetScissor>(ds, "vkCmdSetScissor")(cmd, 0, 1, &scissor);

    devfn<PFN_vkCmdBindDescriptorSets>(ds, "vkCmdBindDescriptorSets")(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ds->pl, 0, 1, &sc->sets[image_index],
        0, nullptr);
    devfn<PFN_vkCmdPushConstants>(ds, "vkCmdPushConstants")(
        cmd, ds->pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float), &sharpness);
    devfn<PFN_vkCmdDraw>(ds, "vkCmdDraw")(cmd, 3, 1, 0, 0);
    devfn<PFN_vkCmdEndRenderPass>(ds, "vkCmdEndRenderPass")(cmd);

    // Swap: SHADER_READ -> TRANSFER_DST, verbatim copy from the intermediate
    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_dst.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = swap_image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    devfn<PFN_vkCmdPipelineBarrier>(ds, "vkCmdPipelineBarrier")(
        cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &to_dst);

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {sc->extent.width, sc->extent.height, 1};
    devfn<PFN_vkCmdCopyImage>(ds, "vkCmdCopyImage")(
        cmd, sc->intermediate, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        swap_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Swap: TRANSFER_DST -> PRESENT_SRC, ready for the real present
    VkImageMemoryBarrier to_present{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = swap_image;
    to_present.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    devfn<PFN_vkCmdPipelineBarrier>(ds, "vkCmdPipelineBarrier")(
        cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &to_present);

    if (want_timing) {
        devfn<PFN_vkCmdWriteTimestamp>(ds, "vkCmdWriteTimestamp")(
            cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, ds->timing_pool, timing_pair + 1);
    }

    if (devfn<PFN_vkEndCommandBuffer>(ds, "vkEndCommandBuffer")(cmd) != VK_SUCCESS)
        return fail();

    // --- Submit: waits for the app's semaphores, signals our own ---
    VkPipelineStageFlags stages[kMaxWaitSemaphores];
    for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++) {
        stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
    submit.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
    submit.pWaitDstStageMask = stages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &semaphore;

    if (devfn<PFN_vkQueueSubmit>(ds, "vkQueueSubmit")(queue, 1, &submit, fence) != VK_SUCCESS)
        return fail();

    ds->present_counter++;
    ds->pending.push_back(
        {fence, semaphore, cmd, want_timing ? timing_pair : UINT32_MAX, ds->present_counter});
    g_telemetry.add_frames(1, -1.0f);
    return semaphore;
}

} // namespace

// Forward declarations (used by the negotiation entry point below)
extern "C" {
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hexscale_vkGetInstanceProcAddr(
    VkInstance instance, const char* pName);
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hexscale_vkGetDeviceProcAddr(
    VkDevice device, const char* pName);
}

// ---------------------------------------------------------------------------
// Layer entry points
// ---------------------------------------------------------------------------

extern "C" {

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL hexscale_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance)
{
    auto* chain_info = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    while (chain_info && (chain_info->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                          chain_info->function != VK_LAYER_LINK_INFO)) {
        chain_info = (VkLayerInstanceCreateInfo*)chain_info->pNext;
    }
    if (!chain_info || !chain_info->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr next_gipa = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    PFN_vkCreateInstance next_create =
        (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = next_create(pCreateInfo, pAllocator, pInstance);
    if (res == VK_SUCCESS && pInstance && *pInstance) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_instance_dispatch[get_dispatch_key(*pInstance)] = next_gipa;
        g_any_gipa = next_gipa;
        g_any_instance = *pInstance;
    }
    return res;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL hexscale_vkDestroyInstance(
    VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
    void* key = get_dispatch_key(instance);
    PFN_vkGetInstanceProcAddr next_gipa = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_instance_dispatch.find(key);
        if (it != g_instance_dispatch.end()) {
            next_gipa = it->second;
            g_instance_dispatch.erase(it);
        }
        if (g_any_instance == instance) {
            g_any_instance = VK_NULL_HANDLE;
            g_any_gipa = nullptr;
        }
    }
    if (next_gipa) {
        PFN_vkDestroyInstance next_destroy =
            (PFN_vkDestroyInstance)next_gipa(instance, "vkDestroyInstance");
        if (next_destroy) next_destroy(instance, pAllocator);
    }
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL hexscale_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice)
{
    auto* chain_info = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (chain_info && (chain_info->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                          chain_info->function != VK_LAYER_LINK_INFO)) {
        chain_info = (VkLayerDeviceCreateInfo*)chain_info->pNext;
    }
    if (!chain_info || !chain_info->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr next_gipa = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    PFN_vkCreateDevice next_create =
        (PFN_vkCreateDevice)next_gipa(VK_NULL_HANDLE, "vkCreateDevice");
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = next_create(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS || !pDevice || !*pDevice) return res;

    auto* ds = new (std::nothrow) DeviceState();
    if (!ds) return res;

    ds->device = *pDevice;
    ds->physical_device = physicalDevice;
    ds->gdpa = next_gdpa;

    // Physical-device properties via any live instance (instance-level call).
    {
        PFN_vkGetInstanceProcAddr gipa = nullptr;
        VkInstance inst = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            gipa = g_any_gipa;
            inst = g_any_instance;
        }
        if (gipa && inst) {
            PFN_vkGetPhysicalDeviceProperties get_props =
                (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
            PFN_vkGetPhysicalDeviceQueueFamilyProperties get_families =
                (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
                    gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
            if (get_props) {
                VkPhysicalDeviceProperties props{};
                get_props(physicalDevice, &props);
                ds->timestamp_period_ns =
                    props.limits.timestampPeriod > 0 ? props.limits.timestampPeriod : 1.0f;
            }
            if (get_families) {
                uint32_t count = 0;
                get_families(physicalDevice, &count, nullptr);
                std::vector<VkQueueFamilyProperties> families(count);
                get_families(physicalDevice, &count, families.data());
                for (uint32_t i = 0; i < count; i++) {
                    if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                        (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                        ds->any_gfx_compute_family = i;
                        ds->any_gfx_compute_timestamp_bits = families[i].timestampValidBits;
                        break;
                    }
                }
            }
        }
    }

    {
        // Device tracked; the CAS pipeline is built lazily at the first
        // swapchain creation — creating objects from inside this hook ran
        // before upper layers finished their per-device setup.
        std::lock_guard<std::mutex> lock(g_lock);
        g_device_dispatch[get_dispatch_key(*pDevice)] = next_gdpa;
        g_devices[get_dispatch_key(*pDevice)] = ds;
    }
    return res;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL hexscale_vkDestroyDevice(
    VkDevice device, const VkAllocationCallbacks* pAllocator)
{
    void* key = get_dispatch_key(device);
    PFN_vkGetDeviceProcAddr next_gdpa = nullptr;
    DeviceState* ds = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_device_dispatch.find(key);
        if (it != g_device_dispatch.end()) {
            next_gdpa = it->second;
            g_device_dispatch.erase(it);
        }
        auto dit = g_devices.find(key);
        if (dit != g_devices.end()) {
            ds = dit->second;
            g_devices.erase(dit);
        }
    }

    // Free our resources (waits for queue idle) before tearing down dispatch.
    if (ds) destroy_device_state(ds);

    if (next_gdpa) {
        PFN_vkDestroyDevice next_destroy =
            (PFN_vkDestroyDevice)next_gdpa(device, "vkDestroyDevice");
        if (next_destroy) next_destroy(device, pAllocator);
    }
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL hexscale_vkGetDeviceQueue(
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue)
{
    PFN_vkGetDeviceQueue next_fn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_device_dispatch.find(get_dispatch_key(device));
        if (it != g_device_dispatch.end() && it->second) {
            next_fn = (PFN_vkGetDeviceQueue)it->second(device, "vkGetDeviceQueue");
        }
    }
    if (!next_fn) return;
    next_fn(device, queueFamilyIndex, queueIndex, pQueue);

    if (pQueue && *pQueue) {
        std::lock_guard<std::mutex> lock(g_lock);
        auto dit = g_devices.find(get_dispatch_key(device));
        if (dit != g_devices.end()) {
            dit->second->queue_families[*pQueue] = queueFamilyIndex;
        }
    }
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL hexscale_vkGetDeviceQueue2(
    VkDevice device, const VkDeviceQueueInfo2* pQueueInfo, VkQueue* pQueue)
{
    PFN_vkGetDeviceQueue2 next_fn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_device_dispatch.find(get_dispatch_key(device));
        if (it != g_device_dispatch.end() && it->second) {
            next_fn = (PFN_vkGetDeviceQueue2)it->second(device, "vkGetDeviceQueue2");
        }
    }
    if (!next_fn) return;
    next_fn(device, pQueueInfo, pQueue);

    if (pQueue && *pQueue) {
        std::lock_guard<std::mutex> lock(g_lock);
        auto dit = g_devices.find(get_dispatch_key(device));
        if (dit != g_devices.end()) {
            dit->second->queue_families[*pQueue] = pQueueInfo->queueFamilyIndex;
        }
    }
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL hexscale_vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain)
{
    PFN_vkGetDeviceProcAddr next_gdpa = nullptr;
    DeviceState* ds = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_device_dispatch.find(get_dispatch_key(device));
        if (it != g_device_dispatch.end()) next_gdpa = it->second;
        auto dit = g_devices.find(get_dispatch_key(device));
        if (dit != g_devices.end()) ds = dit->second;
    }

    // Any 4x8 UNORM/SRGB format goes through the CAS pass (the fragment
    // path samples via native-format views); anything else stays untouched.
    const bool format_ok = pCreateInfo->imageFormat == VK_FORMAT_R8G8B8A8_UNORM ||
                           pCreateInfo->imageFormat == VK_FORMAT_R8G8B8A8_SRGB ||
                           pCreateInfo->imageFormat == VK_FORMAT_B8G8R8A8_UNORM ||
                           pCreateInfo->imageFormat == VK_FORMAT_B8G8R8A8_SRGB;

    PFN_vkCreateSwapchainKHR next_create =
        next_gdpa ? (PFN_vkCreateSwapchainKHR)next_gdpa(device, "vkCreateSwapchainKHR") : nullptr;
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;

    if (!g_config.enabled || !ds || !format_ok) {
        if (g_config.debug && !format_ok) {
            HEX_LOG("swapchain format %u unsupported -> passthrough",
                    static_cast<uint32_t>(pCreateInfo->imageFormat));
        }
        return next_create(device, pCreateInfo, pAllocator, pSwapchain);
    }

    if (ds->pipeline == VK_NULL_HANDLE && !ds->pipeline_failed) {
        if (!create_cas_pipeline(ds)) {
            ds->pipeline_failed = true; // inert for this device, fail-open
            HEX_LOG("pipeline creation failed -> layer inert on this device");
        }
    }
    if (ds->pipeline == VK_NULL_HANDLE) {
        return next_create(device, pCreateInfo, pAllocator, pSwapchain);
    }

    // Request the extra usage the pass needs (sampling + copy back).
    // Apps lose nothing; if the driver refuses, retry the original info.
    VkSwapchainCreateInfoKHR patched = *pCreateInfo;
    patched.imageUsage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkResult res = next_create(device, &patched, pAllocator, pSwapchain);
    if (res != VK_SUCCESS || !pSwapchain || !*pSwapchain) {
        return next_create(device, pCreateInfo, pAllocator, pSwapchain);
    }

    auto* sc = new (std::nothrow) SwapchainState();
    if (!sc) return res;
    sc->device = device;
    sc->format = pCreateInfo->imageFormat;
    sc->extent = pCreateInfo->imageExtent;

    PFN_vkGetSwapchainImagesKHR get_images =
        (PFN_vkGetSwapchainImagesKHR)next_gdpa(device, "vkGetSwapchainImagesKHR");

    uint32_t count = 0;
    if (get_images && get_images(device, *pSwapchain, &count, nullptr) == VK_SUCCESS &&
        count > 0 && count <= kMaxSwapchainImages &&
        get_images(device, *pSwapchain, &count, sc->images) == VK_SUCCESS) {
        sc->image_count = count;
        create_swapchain_resources(ds, sc);
        if (sc->usable) g_telemetry.start();
    }

    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_swapchains[*pSwapchain] = sc; // usable=false -> passthrough
    }
    return res;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL hexscale_vkDestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
{
    PFN_vkDestroySwapchainKHR next_destroy = nullptr;
    DeviceState* ds = nullptr;
    SwapchainState* sc = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_device_dispatch.find(get_dispatch_key(device));
        if (it != g_device_dispatch.end() && it->second) {
            next_destroy =
                (PFN_vkDestroySwapchainKHR)it->second(device, "vkDestroySwapchainKHR");
        }
        auto dit = g_devices.find(get_dispatch_key(device));
        if (dit != g_devices.end()) ds = dit->second;
        auto sit = g_swapchains.find(swapchain);
        if (sit != g_swapchains.end()) {
            sc = sit->second;
            g_swapchains.erase(sit);
        }
    }

    // Retire finished work, then free our resources for this swapchain.
    if (sc && ds && ds->gdpa) {
        reap_pending(ds);
        destroy_swapchain_resources(ds, sc);
    } else if (sc) {
        delete sc;
    }

    if (next_destroy) next_destroy(device, swapchain, pAllocator);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL hexscale_vkQueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    DeviceState* ds = nullptr;
    SwapchainState* sc = nullptr;

    if (g_config.enabled && g_daemon_active.load() &&
        pPresentInfo->swapchainCount == 1 && pPresentInfo->pSwapchains &&
        pPresentInfo->pImageIndices) {
        std::lock_guard<std::mutex> lock(g_lock);
        auto sit = g_swapchains.find(pPresentInfo->pSwapchains[0]);
        if (sit != g_swapchains.end()) {
            sc = sit->second;
            auto dit = g_devices.find(get_dispatch_key(queue));
            if (dit != g_devices.end()) {
                ds = dit->second;
            } else if (!g_devices.empty()) {
                // Present queue created before our layer tracked it:
                // single-device handhelds just use the one device.
                ds = g_devices.begin()->second;
            }
        }
    }

    PFN_vkQueuePresentKHR next_present = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_device_dispatch.find(get_dispatch_key(queue));
        if (it != g_device_dispatch.end() && it->second) {
            next_present = (PFN_vkQueuePresentKHR)it->second(
                reinterpret_cast<VkDevice>(queue), "vkQueuePresentKHR");
        } else if (!g_device_dispatch.empty()) {
            next_present = (PFN_vkQueuePresentKHR)
                g_device_dispatch.begin()->second(
                    reinterpret_cast<VkDevice>(queue), "vkQueuePresentKHR");
        }
    }
    if (!next_present) return VK_SUCCESS; // cannot forward: fail-open

    if (ds && sc && sc->usable && pPresentInfo->pImageIndices[0] < sc->image_count) {
        VkSemaphore our_sem = try_cas_present(ds, sc, queue, pPresentInfo);
        if (our_sem != VK_NULL_HANDLE) {
            // Present rewritten: our semaphore now gates the real present.
            VkPresentInfoKHR rewritten = *pPresentInfo;
            rewritten.pWaitSemaphores = &our_sem;
            rewritten.waitSemaphoreCount = 1;
            return next_present(queue, &rewritten);
        }
    }

    return next_present(queue, pPresentInfo);
}

// ---------------------------------------------------------------------------
// Enumeration / negotiation / dispatch
// ---------------------------------------------------------------------------

const VkLayerProperties g_layer_properties = {
    "VK_LAYER_HEXSCALE",
    VK_MAKE_VERSION(1, 3, 260),
    1,
    "Hexscale contrast-adaptive sharpening (GPU CAS) for Qualcomm handhelds",
};

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL hexscale_vkEnumerateInstanceLayerProperties(
    uint32_t* pPropertyCount, VkLayerProperties* pProperties)
{
    if (!pPropertyCount) return VK_INCOMPLETE;
    if (pProperties == nullptr) {
        *pPropertyCount = 1;
        return VK_SUCCESS;
    }
    if (*pPropertyCount < 1) return VK_INCOMPLETE;
    *pProperties = g_layer_properties;
    *pPropertyCount = 1;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL hexscale_vkEnumerateDeviceLayerProperties(
    VkPhysicalDevice, uint32_t* pPropertyCount, VkLayerProperties* pProperties)
{
    return hexscale_vkEnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL hexscale_vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface* pVersionStruct)
{
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pVersionStruct->loaderLayerInterfaceVersion < 2) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = hexscale_vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = hexscale_vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hexscale_vkGetInstanceProcAddr(
    VkInstance instance, const char* pName)
{
    if (!pName) return nullptr;

    if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkGetInstanceProcAddr);
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkGetDeviceProcAddr);
    if (strcmp(pName, "vkNegotiateLoaderLayerInterfaceVersion") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkNegotiateLoaderLayerInterfaceVersion);
    if (strcmp(pName, "vkEnumerateInstanceLayerProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkEnumerateInstanceLayerProperties);
    if (strcmp(pName, "vkEnumerateDeviceLayerProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkEnumerateDeviceLayerProperties);
    if (strcmp(pName, "vkCreateInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkCreateInstance);
    if (strcmp(pName, "vkDestroyInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkDestroyInstance);
    // vkCreateDevice is queryable with a NULL instance per spec; layers in
    // the chain above us (e.g. gamescope WSI/vkroots) resolve their downchain
    // exactly that way. Answering nullptr here made them call 0x0.
    if (strcmp(pName, "vkCreateDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkCreateDevice);

    if (instance != VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_instance_dispatch.find(get_dispatch_key(instance));
        if (it != g_instance_dispatch.end() && it->second) {
            return it->second(instance, pName);
        }
    }
    return nullptr;
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hexscale_vkGetDeviceProcAddr(
    VkDevice device, const char* pName)
{
    if (!pName) return nullptr;

    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkGetDeviceProcAddr);
    if (strcmp(pName, "vkCreateDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkCreateDevice);
    if (strcmp(pName, "vkDestroyDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkDestroyDevice);
    if (strcmp(pName, "vkGetDeviceQueue") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkGetDeviceQueue);
    if (strcmp(pName, "vkGetDeviceQueue2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkGetDeviceQueue2);
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkCreateSwapchainKHR);
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkDestroySwapchainKHR);
    if (strcmp(pName, "vkQueuePresentKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkQueuePresentKHR);

    if (device != VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_device_dispatch.find(get_dispatch_key(device));
        if (it != g_device_dispatch.end() && it->second) {
            return it->second(device, pName);
        }
    }
    return nullptr;
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance, const char* pName)
{
    return hexscale_vkGetInstanceProcAddr(instance, pName);
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device, const char* pName)
{
    return hexscale_vkGetDeviceProcAddr(device, pName);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface* pVersionStruct)
{
    return hexscale_vkNegotiateLoaderLayerInterfaceVersion(pVersionStruct);
}

} // extern "C"
