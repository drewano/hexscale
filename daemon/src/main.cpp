#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "fastrpc_session.hpp"
#include "qnn_backend.hpp"
#include "ipc_server.hpp"
#include "ipc_protocol.hpp"

namespace {
    std::atomic<bool> g_running{true};

    void signal_handler(int sig) {
        std::cout << "\n[hexscaled] Received signal " << sig << ", shutting down..." << std::endl;
        g_running = false;
    }
}

int main(int argc, char* argv[]) {
    std::cout << "=================================================" << std::endl;
    std::cout << "   Hexscale Daemon (hexscaled) v0.2.0            " << std::endl;
    std::cout << "   Sharpening control + NPU telemetry service    " << std::endl;
    std::cout << "=================================================" << std::endl;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string socket_path = std::string(hexscale::ipc::DEFAULT_SOCKET_PATH);
    std::string model_path = "/usr/share/hexscale/models/xlsr_htp_v73.bin";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model_path = argv[++i];
        }
    }

    // 1. Initialize FastRPC Session
    hexscale::fastrpc::FastRpcSession fastrpc;
    fastrpc.initialize();

    // 2. Initialize QNN HTP Backend
    hexscale::qnn::QnnHtpBackend qnn;
    if (!qnn.initialize()) {
        std::cerr << "[hexscaled] Warning: Failed to initialize QNN backend. Falling back." << std::endl;
    }
    qnn.load_context_binary(model_path);

    // 3. State variables
    std::atomic<bool> upscaling_enabled{true};
    std::atomic<float> current_sharpness{0.75f};
    std::atomic<uint8_t> current_profile{static_cast<uint8_t>(hexscale::ipc::NpuProfile::BALANCED)};
    std::atomic<uint64_t> total_frames{0};
    std::atomic<float> last_latency_ms{0.0f};
    std::atomic<float> avg_latency_ms{0.0f};
    uint64_t timed_samples = 0;

    // 4. Start IPC Server
    hexscale::ipc::IpcServer ipc;
    ipc.set_command_handler([&](const hexscale::ipc::CommandPacket& cmd, int passed_fd) -> hexscale::ipc::ResponsePacket {
        hexscale::ipc::ResponsePacket resp{};
        resp.header.magic = hexscale::ipc::PROTOCOL_MAGIC;
        resp.header.version = hexscale::ipc::PROTOCOL_VERSION;
        resp.header.sequence = cmd.header.sequence;

        switch (static_cast<hexscale::ipc::CommandType>(cmd.header.msg_type)) {
            case hexscale::ipc::CommandType::GET_STATUS: {
                resp.status = hexscale::ipc::StatusCode::OK;
                resp.status_data.enabled = upscaling_enabled ? 1 : 0;
                resp.status_data.profile = current_profile.load();
                resp.status_data.sharpness = current_sharpness.load();
                resp.status_data.last_inference_ms = last_latency_ms.load();
                resp.status_data.avg_inference_ms = avg_latency_ms.load();
                resp.status_data.total_frames_upscaled = total_frames.load();
                std::strncpy(resp.status_data.model_name, qnn.get_model_name().c_str(),
                             sizeof(resp.status_data.model_name) - 1);
                std::strncpy(resp.status_data.target_soc, "SM8550",
                             sizeof(resp.status_data.target_soc) - 1);
                std::strncpy(resp.status_data.backend_version,
                             qnn.is_htp_available() ? "HTP-v73" : "GPU-CAS",
                             sizeof(resp.status_data.backend_version) - 1);
                break;
            }
            case hexscale::ipc::CommandType::SET_ENABLED: {
                upscaling_enabled = (cmd.payload.set_enabled.enabled != 0);
                std::cout << "[hexscaled] Upscaling state set to: " 
                          << (upscaling_enabled ? "ENABLED" : "DISABLED") << std::endl;
                resp.status = hexscale::ipc::StatusCode::OK;
                break;
            }
            case hexscale::ipc::CommandType::SET_SHARPNESS: {
                current_sharpness = std::clamp(cmd.payload.set_sharpness.sharpness, 0.0f, 1.0f);
                std::cout << "[hexscaled] Sharpness updated to: " << current_sharpness.load() << std::endl;
                resp.status = hexscale::ipc::StatusCode::OK;
                break;
            }
            case hexscale::ipc::CommandType::SET_PROFILE: {
                current_profile = cmd.payload.set_profile.profile;
                fastrpc.set_performance_profile(current_profile.load());
                std::cout << "[hexscaled] NPU Profile updated to: " << static_cast<int>(current_profile.load()) << std::endl;
                resp.status = hexscale::ipc::StatusCode::OK;
                break;
            }
            case hexscale::ipc::CommandType::REPORT_FRAMES: {
                uint32_t count = cmd.payload.report_frames.frame_count;
                total_frames.fetch_add(count);
                float ms = cmd.payload.report_frames.inference_ms;
                if (ms > 0.0f) {
                    last_latency_ms.store(ms);
                    // Incremental running average, no history buffer needed.
                    timed_samples++;
                    float prev = avg_latency_ms.load();
                    avg_latency_ms.store(prev + (ms - prev) / static_cast<float>(timed_samples));
                }
                int fd = ::open("/run/hexscale/active", O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (fd >= 0) {
                    ::close(fd);
                }
                resp.status = hexscale::ipc::StatusCode::OK;
                break;
            }
            case hexscale::ipc::CommandType::REGISTER_DMABUF: {
                uint32_t width = cmd.payload.register_dmabuf.width;
                uint32_t height = cmd.payload.register_dmabuf.height;
                uint32_t stride = cmd.payload.register_dmabuf.stride;
                uint64_t size = cmd.payload.register_dmabuf.size;
                std::cout << "[hexscaled] Received REGISTER_DMABUF: " << width << "x" << height 
                          << ", stride=" << stride << ", size=" << size << " bytes" << std::endl;

                if (passed_fd >= 0) {
                    std::cout << "[hexscaled] Received DMA-BUF file descriptor fd=" << passed_fd 
                              << " via SCM_RIGHTS" << std::endl;
                    // Bound CDSP mappings: evict the oldest when full.
                    while (fastrpc.mapping_count() >= 8) {
                        fastrpc.unmap_oldest();
                    }
                    uintptr_t dsp_addr = 0;
                    bool mapped = fastrpc.map_dmabuf(passed_fd, size, dsp_addr);
                    if (mapped) {
                        std::cout << "[hexscaled] >> FastRPC SMMU Mapping SUCCESS! CDSP Virtual Address: 0x"
                                  << std::hex << dsp_addr << std::dec << " (Size: " << size << " bytes)" << std::endl;
                        resp.status = hexscale::ipc::StatusCode::OK;
                    } else {
                        // Map failed: the fd is still ours to release.
                        ::close(passed_fd);
                        std::cerr << "[hexscaled] CDSP map failed for this buffer; "
                                     "GPU path unaffected." << std::endl;
                        resp.status = hexscale::ipc::StatusCode::OK;
                    }
                } else {
                    std::cerr << "[hexscaled] REGISTER_DMABUF received without file descriptor." << std::endl;
                    resp.status = hexscale::ipc::StatusCode::ERROR_INVALID_CMD;
                }
                break;
            }
            case hexscale::ipc::CommandType::SHUTDOWN_DAEMON: {
                std::cout << "[hexscaled] IPC requested daemon shutdown." << std::endl;
                g_running = false;
                resp.status = hexscale::ipc::StatusCode::OK;
                break;
            }
            default:
                resp.status = hexscale::ipc::StatusCode::ERROR_INVALID_CMD;
                break;
        }
        return resp;
    });

    if (!ipc.start(socket_path)) {
        std::cerr << "[hexscaled] Fatal: Could not start IPC server on " << socket_path << std::endl;
        return 1;
    }

    std::cout << "[hexscaled] Daemon initialized successfully. Ready to process frames." << std::endl;

    auto last_active_check = std::chrono::steady_clock::now();
    uint64_t prev_frames = 0;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_active_check).count() >= 3) {
            last_active_check = now;
            uint64_t curr_frames = total_frames.load();
            if (curr_frames == prev_frames) {
                ::unlink("/run/hexscale/active");
            }
            prev_frames = curr_frames;
        }
    }

    ::unlink("/run/hexscale/active");
    ipc.stop();
    fastrpc.close();
    std::cout << "[hexscaled] Clean shutdown complete." << std::endl;
    return 0;
}
