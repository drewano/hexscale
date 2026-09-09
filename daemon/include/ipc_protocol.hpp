#pragma once

#include <cstdint>
#include <string_view>

namespace hexscale::ipc {

constexpr std::string_view DEFAULT_SOCKET_PATH = "/run/hexscale/control.sock";
constexpr uint32_t PROTOCOL_MAGIC = 0x48455853; // "HEXS"
constexpr uint32_t PROTOCOL_VERSION = 1;

enum class CommandType : uint16_t {
    GET_STATUS       = 0x0001,
    SET_ENABLED      = 0x0002,
    SET_SHARPNESS    = 0x0003,
    SET_PROFILE      = 0x0004,
    SHUTDOWN_DAEMON  = 0x00FF
};

enum class StatusCode : uint16_t {
    OK                = 0x0000,
    ERROR_INVALID_CMD = 0x0001,
    ERROR_NPU_OFFLINE = 0x0002,
    ERROR_BUSY        = 0x0003,
    ERROR_INTERNAL    = 0x00FF
};

enum class NpuProfile : uint8_t {
    EFFICIENCY  = 0, // Lower frequency, lowest power draw
    BALANCED    = 1, // Default dynamic frequency
    BURST       = 2  // Maximum HTP clock, sub-millisecond priority
};

#pragma pack(push, 1)

struct Header {
    uint32_t magic{PROTOCOL_MAGIC};
    uint32_t version{PROTOCOL_VERSION};
    uint16_t msg_type{0};
    uint16_t payload_size{0};
    uint32_t sequence{0};
};

struct CommandPacket {
    Header header;
    union {
        struct {
            uint8_t enabled; // 1 = enabled, 0 = disabled
        } set_enabled;

        struct {
            float sharpness; // 0.0f - 1.0f
        } set_sharpness;

        struct {
            uint8_t profile; // NpuProfile
        } set_profile;
    } payload;
};

struct StatusPayload {
    uint8_t enabled;
    uint8_t profile;
    uint16_t reserved;
    float sharpness;
    float last_inference_ms;
    float avg_inference_ms;
    uint64_t total_frames_upscaled;
    char model_name[32];
    char target_soc[16]; // e.g. "SM8550"
    char backend_version[16]; // e.g. "HTP-v73"
};

struct ResponsePacket {
    Header header;
    StatusCode status{StatusCode::OK};
    StatusPayload status_data{};
};

#pragma pack(pop)

} // namespace hexscale::ipc
