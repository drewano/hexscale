#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <span>
#include <chrono>

namespace hexscale::qnn {

struct ModelTensorInfo {
    std::string name;
    uint32_t width{0};
    uint32_t height{0};
    uint32_t channels{0};
    size_t size_bytes{0};
    uint32_t data_type{0}; // 0 = INT8, 1 = UINT8, 2 = FP16
    float scale{1.0f};
    int32_t zero_point{0};
};

struct InferenceMetrics {
    std::chrono::microseconds duration{0};
    bool success{false};
};

class QnnHtpBackend {
public:
    QnnHtpBackend();
    ~QnnHtpBackend();

    // Disable copy
    QnnHtpBackend(const QnnHtpBackend&) = delete;
    QnnHtpBackend& operator=(const QnnHtpBackend&) = delete;

    // Initialize backend using Qualcomm AI Engine Direct (QNN HTP v73 for SM8550)
    bool initialize(const std::string& lib_path = "libQnnHtp.so");
    void shutdown();

    // Load precompiled context binary (.bin) generated for HTP v73
    bool load_context_binary(const std::string& model_bin_path);

    // Get input/output metadata
    [[nodiscard]] const ModelTensorInfo& get_input_info() const { return m_input_info; }
    [[nodiscard]] const ModelTensorInfo& get_output_info() const { return m_output_info; }

    // Execute synchronous inference on dma-buf inputs and outputs
    InferenceMetrics execute_dmabuf(int input_dmabuf_fd, int output_dmabuf_fd);

    // Execute inference from host memory buffers (used by hexscale-cli)
    InferenceMetrics execute_host_memory(const uint8_t* input_data, size_t input_size,
                                         uint8_t* output_data, size_t output_size);

    [[nodiscard]] bool is_ready() const { return m_is_ready; }
    [[nodiscard]] const std::string& get_model_name() const { return m_model_name; }

private:
    bool m_is_ready{false};
    std::string m_model_name{"XLSR-x1.5-INT8"};
    void* m_qnn_lib_handle{nullptr};
    void* m_qnn_context_handle{nullptr};
    void* m_qnn_graph_handle{nullptr};

    ModelTensorInfo m_input_info{
        .name = "input_rgb",
        .width = 1280,
        .height = 720,
        .channels = 3,
        .size_bytes = 1280 * 720 * 3,
        .data_type = 0 // INT8
    };

    ModelTensorInfo m_output_info{
        .name = "output_rgb",
        .width = 1920,
        .height = 1080,
        .channels = 3,
        .size_bytes = 1920 * 1080 * 3,
        .data_type = 0 // INT8
    };
};

} // namespace hexscale::qnn
