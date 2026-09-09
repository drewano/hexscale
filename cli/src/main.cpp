#include "ipc_protocol.hpp"
#include "qnn_backend.hpp"

#include <iostream>
#include <vector>
#include <chrono>
#include <numeric>
#include <algorithm>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>

namespace {

bool send_ipc_command(const hexscale::ipc::CommandPacket& cmd, hexscale::ipc::ResponsePacket& out_resp,
                      const std::string& socket_path = std::string(hexscale::ipc::DEFAULT_SOCKET_PATH)) {
    int sock_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        std::cerr << "Error: Failed to create socket." << std::endl;
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(sock_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Error: Could not connect to hexscaled at " << socket_path 
                  << " (" << std::strerror(errno) << ")." << std::endl;
        std::cerr << "Ensure that hexscaled.service is running." << std::endl;
        ::close(sock_fd);
        return false;
    }

    if (::send(sock_fd, &cmd, sizeof(cmd), 0) != sizeof(cmd)) {
        std::cerr << "Error: Failed to send command." << std::endl;
        ::close(sock_fd);
        return false;
    }

    if (::recv(sock_fd, &out_resp, sizeof(out_resp), 0) != sizeof(out_resp)) {
        std::cerr << "Error: Failed to receive response from daemon." << std::endl;
        ::close(sock_fd);
        return false;
    }

    ::close(sock_fd);
    return true;
}

void print_status(const hexscale::ipc::ResponsePacket& resp) {
    std::cout << "\n--- Hexscale NPU Upscaler Status ---" << std::endl;
    std::cout << "Target Hardware     : " << resp.status_data.target_soc << std::endl;
    std::cout << "Backend Architecture: " << resp.status_data.backend_version << std::endl;
    std::cout << "Active Model        : " << resp.status_data.model_name << std::endl;
    std::cout << "Upscaling State     : " << (resp.status_data.enabled ? "ENABLED" : "DISABLED") << std::endl;
    std::cout << "Sharpness           : " << resp.status_data.sharpness * 100.0f << " %" << std::endl;
    std::cout << "Last Inference Time : " << resp.status_data.last_inference_ms << " ms" << std::endl;
    std::cout << "Avg Inference Time  : " << resp.status_data.avg_inference_ms << " ms" << std::endl;
    std::cout << "Total Frames Served : " << resp.status_data.total_frames_upscaled << std::endl;
    std::cout << "------------------------------------\n" << std::endl;
}

void run_benchmark(uint32_t iterations) {
    std::cout << "=================================================" << std::endl;
    std::cout << "   Hexscale XLSR-INT8 Latency Benchmark          " << std::endl;
    std::cout << "   Resolution: 1280x720 -> 1920x1080 (RGB24)    " << std::endl;
    std::cout << "   Iterations: " << iterations << std::endl;
    std::cout << "=================================================" << std::endl;

    hexscale::qnn::QnnHtpBackend backend;
    backend.initialize();

    const size_t input_size = 1280 * 720 * 3;
    const size_t output_size = 1920 * 1080 * 3;

    std::vector<uint8_t> dummy_input(input_size, 128);
    std::vector<uint8_t> dummy_output(output_size, 0);

    // Warmup
    for (int i = 0; i < 10; ++i) {
        backend.execute_host_memory(dummy_input.data(), input_size, dummy_output.data(), output_size);
    }

    std::vector<double> latencies_us;
    latencies_us.reserve(iterations);

    for (uint32_t i = 0; i < iterations; ++i) {
        auto metrics = backend.execute_host_memory(dummy_input.data(), input_size, dummy_output.data(), output_size);
        latencies_us.push_back(static_cast<double>(metrics.duration.count()));
    }

    std::sort(latencies_us.begin(), latencies_us.end());
    double min_us = latencies_us.front();
    double max_us = latencies_us.back();
    double median_us = latencies_us[latencies_us.size() / 2];
    double p99_us = latencies_us[static_cast<size_t>(latencies_us.size() * 0.99)];
    double sum = std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0);
    double avg_us = sum / latencies_us.size();

    std::cout << "\nBenchmark Results:" << std::endl;
    std::cout << "  Min Latency   : " << min_us / 1000.0 << " ms (" << min_us << " us)" << std::endl;
    std::cout << "  Avg Latency   : " << avg_us / 1000.0 << " ms (" << avg_us << " us)" << std::endl;
    std::cout << "  Median Latency: " << median_us / 1000.0 << " ms (" << median_us << " us)" << std::endl;
    std::cout << "  P99 Latency   : " << p99_us / 1000.0 << " ms (" << p99_us << " us)" << std::endl;
    std::cout << "  Max Latency   : " << max_us / 1000.0 << " ms (" << max_us << " us)" << std::endl;
    std::cout << "  Theoretical FPS: " << 1000000.0 / avg_us << " FPS" << std::endl;
    std::cout << "=================================================\n" << std::endl;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Hexscale CLI Tool v0.1.0\n"
                  << "Usage:\n"
                  << "  hexscale-cli --status               Query status from hexscaled\n"
                  << "  hexscale-cli --enable               Enable upscaling in real-time\n"
                  << "  hexscale-cli --disable              Disable upscaling\n"
                  << "  hexscale-cli --sharpness <0.0-1.0>  Adjust sharpness factor\n"
                  << "  hexscale-cli --bench [iterations]   Run on-device latency benchmark\n";
        return 0;
    }

    std::string action = argv[1];

    if (action == "--status") {
        hexscale::ipc::CommandPacket cmd{};
        cmd.header.magic = hexscale::ipc::PROTOCOL_MAGIC;
        cmd.header.version = hexscale::ipc::PROTOCOL_VERSION;
        cmd.header.msg_type = static_cast<uint16_t>(hexscale::ipc::CommandType::GET_STATUS);

        hexscale::ipc::ResponsePacket resp{};
        if (send_ipc_command(cmd, resp)) {
            print_status(resp);
        }
    } else if (action == "--enable" || action == "--disable") {
        hexscale::ipc::CommandPacket cmd{};
        cmd.header.magic = hexscale::ipc::PROTOCOL_MAGIC;
        cmd.header.version = hexscale::ipc::PROTOCOL_VERSION;
        cmd.header.msg_type = static_cast<uint16_t>(hexscale::ipc::CommandType::SET_ENABLED);
        cmd.payload.set_enabled.enabled = (action == "--enable") ? 1 : 0;

        hexscale::ipc::ResponsePacket resp{};
        if (send_ipc_command(cmd, resp)) {
            std::cout << "Successfully " << (action == "--enable" ? "enabled" : "disabled") 
                      << " Hexscale upscaling." << std::endl;
        }
    } else if (action == "--sharpness" && argc > 2) {
        float val = std::stof(argv[2]);
        hexscale::ipc::CommandPacket cmd{};
        cmd.header.magic = hexscale::ipc::PROTOCOL_MAGIC;
        cmd.header.version = hexscale::ipc::PROTOCOL_VERSION;
        cmd.header.msg_type = static_cast<uint16_t>(hexscale::ipc::CommandType::SET_SHARPNESS);
        cmd.payload.set_sharpness.sharpness = val;

        hexscale::ipc::ResponsePacket resp{};
        if (send_ipc_command(cmd, resp)) {
            std::cout << "Successfully set sharpness to " << val * 100.0f << "%." << std::endl;
        }
    } else if (action == "--bench") {
        uint32_t iters = (argc > 2) ? std::stoul(argv[2]) : 200;
        run_benchmark(iters);
    } else {
        std::cerr << "Unknown action: " << action << std::endl;
        return 1;
    }

    return 0;
}
