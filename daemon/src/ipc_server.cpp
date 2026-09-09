#include "ipc_server.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <filesystem>
#include <cstring>
#include <cerrno>

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#include <fcntl.h>

namespace hexscale::ipc {

IpcServer::IpcServer() = default;

IpcServer::~IpcServer() {
    stop();
}

bool IpcServer::start(const std::string& socket_path) {
    stop();
    m_socket_path = socket_path;

    // Ensure parent directory exists
    std::filesystem::path path(m_socket_path);
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    // Remove existing stale socket if present
    ::unlink(m_socket_path.c_str());

    m_server_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_server_fd < 0) {
        std::cerr << "[IPC] Failed to create socket (" << std::strerror(errno) << ")" << std::endl;
        return false;
    }
    ::fcntl(m_server_fd, F_SETFD, FD_CLOEXEC);

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, m_socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(m_server_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[IPC] Failed to bind socket to " << m_socket_path 
                  << " (" << std::strerror(errno) << ")" << std::endl;
        ::close(m_server_fd);
        m_server_fd = -1;
        return false;
    }

    // Set socket permissions so Steam / user processes can communicate
    ::chmod(m_socket_path.c_str(), 0666);

    if (::listen(m_server_fd, 8) < 0) {
        std::cerr << "[IPC] Failed to listen on socket (" << std::strerror(errno) << ")" << std::endl;
        ::close(m_server_fd);
        m_server_fd = -1;
        return false;
    }

    m_running = true;
    m_worker_thread = std::thread(&IpcServer::worker_loop, this);

    std::cout << "[IPC] Server listening on " << m_socket_path << std::endl;
    return true;
}

void IpcServer::stop() {
    if (m_running.exchange(false)) {
        if (m_server_fd >= 0) {
            ::shutdown(m_server_fd, SHUT_RDWR);
            ::close(m_server_fd);
            m_server_fd = -1;
        }

        if (m_worker_thread.joinable()) {
            m_worker_thread.join();
        }

        ::unlink(m_socket_path.c_str());
        std::cout << "[IPC] Server stopped." << std::endl;
    }
}

void IpcServer::worker_loop() {
    while (m_running) {
        struct sockaddr_un client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = ::accept(m_server_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (!m_running) break;
            continue;
        }

        CommandPacket cmd{};
        ssize_t bytes_read = ::recv(client_fd, &cmd, sizeof(cmd), 0);

        if (bytes_read == sizeof(cmd) && cmd.header.magic == PROTOCOL_MAGIC) {
            ResponsePacket resp{};
            if (m_handler) {
                resp = m_handler(cmd);
            } else {
                resp.header.magic = PROTOCOL_MAGIC;
                resp.header.version = PROTOCOL_VERSION;
                resp.status = StatusCode::OK;
            }

            ::send(client_fd, &resp, sizeof(resp), 0);
        } else {
            std::cerr << "[IPC] Invalid command packet received from client." << std::endl;
        }

        ::close(client_fd);
    }
}

} // namespace hexscale::ipc
