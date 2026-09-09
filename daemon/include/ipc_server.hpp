#pragma once

#include "ipc_protocol.hpp"
#include <string>
#include <functional>
#include <atomic>
#include <thread>

namespace hexscale::ipc {

class IpcServer {
public:
    using CommandHandler = std::function<ResponsePacket(const CommandPacket&)>;

    IpcServer();
    ~IpcServer();

    bool start(const std::string& socket_path = std::string(DEFAULT_SOCKET_PATH));
    void stop();

    void set_command_handler(CommandHandler handler) {
        m_handler = std::move(handler);
    }

    [[nodiscard]] bool is_running() const { return m_running; }

private:
    void worker_loop();

    int m_server_fd{-1};
    std::string m_socket_path;
    std::atomic<bool> m_running{false};
    std::thread m_worker_thread;
    CommandHandler m_handler;
};

} // namespace hexscale::ipc
