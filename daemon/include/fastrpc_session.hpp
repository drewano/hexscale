#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <span>

namespace hexscale::fastrpc {

// Standard FastRPC device paths
constexpr const char* FASTRPC_CDSP_DEV = "/dev/fastrpc-cdsp";
constexpr const char* FASTRPC_CDSP_SECURE_DEV = "/dev/fastrpc-cdsp-secure";

// FastRPC IOCTL Definitions (matching Linux kernel drivers/misc/fastrpc.c)
#define FASTRPC_IOCTL_INVOKE         _IOWR('R', 1, struct fastrpc_ioctl_invoke)
#define FASTRPC_IOCTL_MMAP           _IOWR('R', 2, struct fastrpc_ioctl_mmap)
#define FASTRPC_IOCTL_MUNMAP         _IOWR('R', 3, struct fastrpc_ioctl_munmap)
#define FASTRPC_IOCTL_INIT           _IOWR('R', 4, struct fastrpc_ioctl_init)
#define FASTRPC_IOCTL_GET_DSP_INFO   _IOWR('R', 13, struct fastrpc_ioctl_dsp_capabilities)

enum FastRpcDomain {
    CDSP_DOMAIN = 3 // Compute DSP domain (NPU)
};

struct FastRpcRemoteArg {
    void* ptr;
    size_t length;
};

struct FastRpcMemoryMapping {
    int fd{-1};
    uint32_t flags{0};
    uintptr_t vaddrout{0};
    size_t size{0};
};

class FastRpcSession {
public:
    FastRpcSession();
    ~FastRpcSession();

    // Disable copy semantics
    FastRpcSession(const FastRpcSession&) = delete;
    FastRpcSession& operator=(const FastRpcSession&) = delete;

    // Move semantics
    FastRpcSession(FastRpcSession&& other) noexcept;
    FastRpcSession& operator=(FastRpcSession&& other) noexcept;

    // Initialize session with CDSP
    bool initialize(const std::string& dev_node = FASTRPC_CDSP_DEV);
    void close();

    [[nodiscard]] bool is_connected() const { return m_fd >= 0; }
    [[nodiscard]] int get_fd() const { return m_fd; }

    // Map a dma-buf descriptor into the CDSP SMMU address space
    bool map_dmabuf(int dmabuf_fd, size_t size, uintptr_t& out_dsp_addr);
    bool unmap_dmabuf(uintptr_t dsp_addr, size_t size);

    // Invoke remote method on Hexagon CDSP
    bool invoke(uint32_t handle, uint32_t sc, std::span<FastRpcRemoteArg> args);

    // Set performance voting / power level
    bool set_performance_profile(uint32_t profile);

private:
    int m_fd{-1};
    std::string m_dev_path;
    std::vector<FastRpcMemoryMapping> m_active_mappings;
};

} // namespace hexscale::fastrpc
