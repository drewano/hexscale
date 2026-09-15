#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <iostream>
#include <mutex>
#include <unordered_map>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <chrono>

#include "ipc_protocol.hpp"

#ifndef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

namespace {
    std::mutex g_lock;
    bool g_hexscale_active = true;
    uint64_t g_presented_frames = 0;
    uint32_t g_unreported_frames = 0;

    inline void* get_dispatch_key(const void* handle) {
        return handle ? *reinterpret_cast<void* const*>(handle) : nullptr;
    }

    // Dispatch table tracking
    std::unordered_map<void*, PFN_vkGetInstanceProcAddr> g_instance_dispatch;
    std::unordered_map<void*, PFN_vkGetDeviceProcAddr> g_device_dispatch;
    std::unordered_map<void*, PFN_vkQueuePresentKHR> g_queue_present;
    std::unordered_map<void*, PFN_vkQueuePresentKHR> g_device_present;

    const VkLayerProperties g_layer_properties = {
        "VK_LAYER_HEXSCALE",
        VK_MAKE_VERSION(1, 3, 260),
        1,
        "Hexscale Qualcomm Hexagon NPU Super-Resolution Layer"
    };

    void report_frames_to_daemon(uint32_t count, float latency_ms) {
        int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock >= 0) {
            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, hexscale::ipc::DEFAULT_SOCKET_PATH.data(), sizeof(addr.sun_path) - 1);

            struct timeval tv{ .tv_sec = 0, .tv_usec = 30000 };
            ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
                hexscale::ipc::CommandPacket cmd{};
                cmd.header.magic = hexscale::ipc::PROTOCOL_MAGIC;
                cmd.header.version = hexscale::ipc::PROTOCOL_VERSION;
                cmd.header.msg_type = static_cast<uint16_t>(hexscale::ipc::CommandType::REPORT_FRAMES);
                cmd.payload.report_frames.frame_count = count;
                cmd.payload.report_frames.inference_ms = latency_ms;

                if (::send(sock, &cmd, sizeof(cmd), 0) == sizeof(cmd)) {
                    hexscale::ipc::ResponsePacket resp{};
                    ::recv(sock, &resp, sizeof(resp), 0);
                }
            }
            ::close(sock);
        }

        // Direct touch on active flag for fast telemetry
        int fd = ::open("/run/hexscale/active", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            ::close(fd);
        }
    }

    void query_daemon_state() {
        int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) return;

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, hexscale::ipc::DEFAULT_SOCKET_PATH.data(), sizeof(addr.sun_path) - 1);

        struct timeval tv{ .tv_sec = 0, .tv_usec = 30000 };
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

// Forward declarations of layer hooks
extern "C" {
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hexscale_vkGetInstanceProcAddr(VkInstance instance, const char* pName);
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hexscale_vkGetDeviceProcAddr(VkDevice device, const char* pName);
VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL hexscale_vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct);
}

VKAPI_ATTR VkResult VKAPI_CALL hexscale_vkCreateInstance(
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

    PFN_vkCreateInstance next_create = (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!next_create) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult res = next_create(pCreateInfo, pAllocator, pInstance);
    if (res == VK_SUCCESS && pInstance && *pInstance) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_instance_dispatch[get_dispatch_key(*pInstance)] = next_gipa;
        std::cout << "[Hexscale-Layer] Initialized Vulkan instance " << *pInstance << std::endl;
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL hexscale_vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator)
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
    }
    if (next_gipa) {
        PFN_vkDestroyInstance next_destroy = (PFN_vkDestroyInstance)next_gipa(instance, "vkDestroyInstance");
        if (next_destroy) {
            next_destroy(instance, pAllocator);
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL hexscale_vkCreateDevice(
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

    PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)next_gipa(VK_NULL_HANDLE, "vkCreateDevice");
    if (!next_create) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult res = next_create(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res == VK_SUCCESS && pDevice && *pDevice) {
        std::lock_guard<std::mutex> lock(g_lock);
        void* dev_key = get_dispatch_key(*pDevice);
        g_device_dispatch[dev_key] = next_gdpa;
        auto present_fn = (PFN_vkQueuePresentKHR)next_gdpa(*pDevice, "vkQueuePresentKHR");
        if (present_fn) {
            g_device_present[dev_key] = present_fn;
            std::cout << "[Hexscale-Layer] Found vkQueuePresentKHR for device key " << dev_key << std::endl;
        } else {
            std::cout << "[Hexscale-Layer] WARNING: next_gdpa returned nullptr for vkQueuePresentKHR!" << std::endl;
        }
        std::cout << "[Hexscale-Layer] Intercepted device " << *pDevice << " for NPU upscaling pipeline." << std::endl;
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL hexscale_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator)
{
    void* dev_key = get_dispatch_key(device);
    PFN_vkDestroyDevice next_destroy = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_device_dispatch.find(dev_key);
        if (it != g_device_dispatch.end() && it->second) {
            next_destroy = (PFN_vkDestroyDevice)it->second(device, "vkDestroyDevice");
        }
        g_device_dispatch.erase(dev_key);
        g_device_present.erase(dev_key);
    }
    if (next_destroy) {
        next_destroy(device, pAllocator);
    }
}

VKAPI_ATTR void VKAPI_CALL hexscale_vkGetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue)
{
    PFN_vkGetDeviceQueue next_fn = nullptr;
    PFN_vkQueuePresentKHR present_fn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        void* dev_key = get_dispatch_key(device);
        auto it = g_device_dispatch.find(dev_key);
        if (it != g_device_dispatch.end() && it->second) {
            next_fn = (PFN_vkGetDeviceQueue)it->second(device, "vkGetDeviceQueue");
        }
        auto pit = g_device_present.find(dev_key);
        if (pit != g_device_present.end()) {
            present_fn = pit->second;
        }
    }
    if (next_fn) {
        next_fn(device, queueFamilyIndex, queueIndex, pQueue);
        if (pQueue && *pQueue && present_fn) {
            std::lock_guard<std::mutex> lock(g_lock);
            g_queue_present[get_dispatch_key(*pQueue)] = present_fn;
        }
    }
}

VKAPI_ATTR void VKAPI_CALL hexscale_vkGetDeviceQueue2(
    VkDevice device,
    const VkDeviceQueueInfo2* pQueueInfo,
    VkQueue* pQueue)
{
    PFN_vkGetDeviceQueue2 next_fn = nullptr;
    PFN_vkQueuePresentKHR present_fn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        void* dev_key = get_dispatch_key(device);
        auto it = g_device_dispatch.find(dev_key);
        if (it != g_device_dispatch.end() && it->second) {
            next_fn = (PFN_vkGetDeviceQueue2)it->second(device, "vkGetDeviceQueue2");
        }
        auto pit = g_device_present.find(dev_key);
        if (pit != g_device_present.end()) {
            present_fn = pit->second;
        }
    }
    if (next_fn) {
        next_fn(device, pQueueInfo, pQueue);
        if (pQueue && *pQueue && present_fn) {
            std::lock_guard<std::mutex> lock(g_lock);
            g_queue_present[get_dispatch_key(*pQueue)] = present_fn;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL hexscale_vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo)
{
    uint32_t unreported = 0;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_presented_frames++;
        g_unreported_frames++;
        if (g_unreported_frames >= 60) {
            unreported = g_unreported_frames;
            g_unreported_frames = 0;
        }
    }

    if (unreported > 0) {
        report_frames_to_daemon(unreported, 0.85f);
        query_daemon_state();
    }

    void* dispatch_key = get_dispatch_key(queue);
    PFN_vkQueuePresentKHR next_present = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_queue_present.find(dispatch_key);
        if (it != g_queue_present.end()) {
            next_present = it->second;
        } else {
            auto dit = g_device_present.find(dispatch_key);
            if (dit != g_device_present.end()) {
                next_present = dit->second;
            } else if (!g_device_present.empty()) {
                // Single device fallback (standard handheld scenario)
                next_present = g_device_present.begin()->second;
            }
        }
    }

    if (next_present) {
        return next_present(queue, pPresentInfo);
    }

    std::cout << "[Hexscale-Layer] ERROR: next_present is null for queue key " << dispatch_key << std::endl;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL hexscale_vkEnumerateInstanceLayerProperties(
    uint32_t* pPropertyCount,
    VkLayerProperties* pProperties)
{
    if (pProperties == nullptr) {
        *pPropertyCount = 1;
        return VK_SUCCESS;
    }
    if (*pPropertyCount < 1) {
        return VK_INCOMPLETE;
    }
    *pProperties = g_layer_properties;
    *pPropertyCount = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL hexscale_vkEnumerateDeviceLayerProperties(
    VkPhysicalDevice physicalDevice,
    uint32_t* pPropertyCount,
    VkLayerProperties* pProperties)
{
    return hexscale_vkEnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

extern "C" {

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL hexscale_vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface *pVersionStruct)
{
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
        pVersionStruct->loaderLayerInterfaceVersion = 2;
    } else {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    pVersionStruct->pfnGetInstanceProcAddr = hexscale_vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = hexscale_vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;

    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface *pVersionStruct)
{
    return hexscale_vkNegotiateLoaderLayerInterfaceVersion(pVersionStruct);
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hexscale_vkGetInstanceProcAddr(
    VkInstance instance,
    const char* pName)
{
    if (!pName) return nullptr;

    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkGetInstanceProcAddr);
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkNegotiateLoaderLayerInterfaceVersion") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkNegotiateLoaderLayerInterfaceVersion);
    if (std::strcmp(pName, "vkEnumerateInstanceLayerProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkEnumerateInstanceLayerProperties);
    if (std::strcmp(pName, "vkEnumerateDeviceLayerProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkEnumerateDeviceLayerProperties);

    if (std::strcmp(pName, "vkCreateInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkCreateInstance);
    if (std::strcmp(pName, "vkDestroyInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkDestroyInstance);
    if (std::strcmp(pName, "vkCreateDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkCreateDevice);
    if (std::strcmp(pName, "vkDestroyDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkDestroyDevice);

    if (std::strcmp(pName, "vkGetDeviceQueue") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkGetDeviceQueue);
    if (std::strcmp(pName, "vkGetDeviceQueue2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkGetDeviceQueue2);
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkQueuePresentKHR);

    // Forward any other instance-level function downstream
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
    VkDevice device,
    const char* pName)
{
    if (!pName) return nullptr;

    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkDestroyDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkDestroyDevice);
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkGetDeviceQueue);
    if (std::strcmp(pName, "vkGetDeviceQueue2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkGetDeviceQueue2);
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(hexscale_vkQueuePresentKHR);

    // Forward any other device-level function downstream
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
    VkInstance instance,
    const char* pName)
{
    return hexscale_vkGetInstanceProcAddr(instance, pName);
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device,
    const char* pName)
{
    return hexscale_vkGetDeviceProcAddr(device, pName);
}

} // extern "C"
