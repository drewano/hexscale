#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <iostream>
#include <mutex>
#include <unordered_map>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ipc_protocol.hpp"

namespace {
    std::mutex g_lock;
    bool g_hexscale_active = true;
    uint64_t g_presented_frames = 0;

    // Dispatch table tracking
    std::unordered_map<void*, PFN_vkGetDeviceProcAddr> g_device_dispatch;
    std::unordered_map<void*, PFN_vkQueuePresentKHR> g_present_dispatch;

    void query_daemon_state() {
        int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) return;

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, hexscale::ipc::DEFAULT_SOCKET_PATH.data(), sizeof(addr.sun_path) - 1);

        // Quick non-blocking attempt
        struct timeval tv{ .tv_sec = 0, .tv_usec = 50000 };
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            hexscale::ipc::CommandPacket cmd{};
            cmd.header.magic = hexscale::ipc::PROTOCOL_MAGIC;
            cmd.header.version = hexscale::ipc::PROTOCOL_VERSION;
            cmd.header.msg_type = static_cast<uint16_t>(hexscale::ipc::CommandType::GET_STATUS);

            if (::send(sock, &cmd, sizeof(cmd), 0) == sizeof(cmd)) {
                hexscale::ipc::ResponsePacket resp{};
                if (::recv(sock, &resp, sizeof(resp), 0) == sizeof(resp)) {
                    g_hexscale_active = (resp.status_data.enabled != 0);
                }
            }
        }
        ::close(sock);
    }
}

// Hooked vkQueuePresentKHR
VKAPI_ATTR VkResult VKAPI_CALL hexscale_vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo)
{
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_presented_frames++;
        if (g_presented_frames % 120 == 0) {
            // Periodic check for daemon state updates (every 2 seconds at 60 FPS)
            query_daemon_state();
        }
    }

    // In full zero-copy pipeline:
    // If g_hexscale_active is true, pPresentInfo image memory is shared with hexscaled
    // via dma-buf export before calling the downstream loader present.

    void* dispatch_key = *reinterpret_cast<void**>(queue);
    PFN_vkQueuePresentKHR next_present = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_present_dispatch.find(dispatch_key);
        if (it != g_present_dispatch.end()) {
            next_present = it->second;
        }
    }

    if (next_present) {
        return next_present(queue, pPresentInfo);
    }

    return VK_SUCCESS;
}

// Hooked vkCreateDevice
VKAPI_ATTR VkResult VKAPI_CALL hexscale_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice)
{
    VkLayerDeviceCreateInfo* layerCreateInfo = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                               layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
        layerCreateInfo = (VkLayerDeviceCreateInfo*)layerCreateInfo->pNext;
    }

    if (!layerCreateInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

    PFN_vkCreateDevice create_func = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
    VkResult res = create_func(physicalDevice, pCreateInfo, pAllocator, pDevice);

    if (res == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_lock);
        void* key = *reinterpret_cast<void**>(*pDevice);
        g_device_dispatch[key] = gdpa;
        g_present_dispatch[key] = (PFN_vkQueuePresentKHR)gdpa(*pDevice, "vkQueuePresentKHR");
        std::cout << "[Hexscale-Layer] Intercepted device " << *pDevice << " for NPU upscaling pipeline." << std::endl;
    }

    return res;
}

// Exported GetDeviceProcAddr
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hexscale_vkGetDeviceProcAddr(
    VkDevice device,
    const char* pName)
{
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkQueuePresentKHR);
    }

    std::lock_guard<std::mutex> lock(g_lock);
    void* key = *reinterpret_cast<void**>(device);
    auto it = g_device_dispatch.find(key);
    if (it != g_device_dispatch.end()) {
        return it->second(device, pName);
    }
    return nullptr;
}

// Exported GetInstanceProcAddr
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hexscale_vkGetInstanceProcAddr(
    VkInstance instance,
    const char* pName)
{
    if (std::strcmp(pName, "vkCreateDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkCreateDevice);
    }
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkGetDeviceProcAddr);
    }
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkQueuePresentKHR);
    }
    return nullptr;
}
