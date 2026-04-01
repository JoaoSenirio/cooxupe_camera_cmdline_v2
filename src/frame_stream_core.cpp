#include "frame_stream_core.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

#include "frame_stream_protocol.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

SocketHandle ToSocketHandle(void* socket_ptr) {
    return static_cast<SocketHandle>(reinterpret_cast<uintptr_t>(socket_ptr));
}

void* ToSocketPtr(SocketHandle socket) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(socket));
}
#endif

}  // namespace

FrameStreamCore::FrameStreamCore(std::size_t queue_capacity)
    : events_(queue_capacity) {}

FrameStreamCore::~FrameStreamCore() {
    stop();
}

bool FrameStreamCore::start(const std::string& host,
                            int port,
                            int connect_timeout_ms,
                            int send_timeout_ms) {
    if (host.empty() || port <= 0 || port > 65535 ||
        connect_timeout_ms <= 0 || send_timeout_ms <= 0) {
        return false;
    }

    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return true;
    }

    host_ = host;
    port_ = port;
    connect_timeout_ms_ = connect_timeout_ms;
    send_timeout_ms_ = send_timeout_ms;
    worker_ = std::thread(&FrameStreamCore::worker_loop, this);
    return true;
}

void FrameStreamCore::stop() {
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false)) {
        return;
    }

    events_.close();
    if (worker_.joinable()) {
        worker_.join();
    }
    reset_connection();
}

bool FrameStreamCore::enqueue_event(const FrameStreamEvent& event) {
    if (!started_.load()) {
        return false;
    }
    return events_.push(event);
}

void FrameStreamCore::worker_loop() {
#ifdef _WIN32
    WSADATA wsa_data{};
    const int wsa_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (wsa_result != 0) {
        log_error("WSAStartup failed error=" + std::to_string(wsa_result));
        return;
    }

    FrameStreamEvent event;
    while (events_.pop(&event)) {
        handle_event(event);
    }

    reset_connection();
    WSACleanup();
#else
    log_info("Frame stream is only active on Windows; worker will stay idle");
    FrameStreamEvent event;
    while (events_.pop(&event)) {
    }
#endif
}

void FrameStreamCore::log_info(const std::string& message) {
    std::cout << "[stream] " << message << "\n";
}

void FrameStreamCore::log_error(const std::string& message) {
    std::cerr << "[stream] " << message << "\n";
}

void FrameStreamCore::reset_connection() {
#ifdef _WIN32
    if (connection_.socket != nullptr) {
        const SocketHandle socket = ToSocketHandle(connection_.socket);
        closesocket(socket);
        connection_.socket = nullptr;
    }
#endif
    connection_.active = false;
    connection_.job_disabled = false;
    connection_.job_id = 0;
    connection_.next_sequence = 1;
}

bool FrameStreamCore::handle_event(const FrameStreamEvent& event) {
    if (event.type == FrameStreamEventType::JobBegin) {
        reset_connection();
        connection_.job_id = event.job_id;
        connection_.job_disabled = false;
        if (!ensure_connected_for_job(event.job_id)) {
            connection_.job_disabled = true;
            return true;
        }
        if (!send_event(event)) {
            connection_.job_disabled = true;
            reset_connection();
        }
        return true;
    }

    if (event.job_id != connection_.job_id || connection_.job_id == 0) {
        return true;
    }

    if (connection_.job_disabled) {
        if (event.type == FrameStreamEventType::JobEnd) {
            reset_connection();
        }
        return true;
    }

    if (!connection_.active) {
        connection_.job_disabled = true;
        if (event.type == FrameStreamEventType::JobEnd) {
            reset_connection();
        }
        return true;
    }

    if (!send_event(event)) {
        connection_.job_disabled = true;
        reset_connection();
        return true;
    }

    if (event.type == FrameStreamEventType::JobEnd) {
        reset_connection();
    }
    return true;
}

bool FrameStreamCore::ensure_connected_for_job(std::uint64_t job_id) {
#ifdef _WIN32
    if (connection_.active && connection_.job_id == job_id) {
        return true;
    }

    std::ostringstream port_text;
    port_text << port_;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    if (getaddrinfo(host_.c_str(), port_text.str().c_str(), &hints, &addresses) != 0) {
        log_error("getaddrinfo failed for " + host_ + ":" + port_text.str());
        return false;
    }

    bool connected = false;
    for (addrinfo* current = addresses; current != nullptr; current = current->ai_next) {
        const SocketHandle socket = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (socket == kInvalidSocket) {
            continue;
        }

        u_long non_blocking = 1;
        if (ioctlsocket(socket, FIONBIO, &non_blocking) != 0) {
            closesocket(socket);
            continue;
        }

        const int connect_result = connect(socket, current->ai_addr, static_cast<int>(current->ai_addrlen));
        if (connect_result == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) {
                closesocket(socket);
                continue;
            }

            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(socket, &write_fds);

            timeval timeout{};
            timeout.tv_sec = connect_timeout_ms_ / 1000;
            timeout.tv_usec = (connect_timeout_ms_ % 1000) * 1000;
            const int select_result = select(0, nullptr, &write_fds, nullptr, &timeout);
            if (select_result <= 0) {
                closesocket(socket);
                continue;
            }

            int socket_error = 0;
            int socket_error_len = sizeof(socket_error);
            if (getsockopt(socket, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&socket_error), &socket_error_len) != 0 ||
                socket_error != 0) {
                closesocket(socket);
                continue;
            }
        }

        non_blocking = 0;
        ioctlsocket(socket, FIONBIO, &non_blocking);

        const DWORD send_timeout = static_cast<DWORD>(send_timeout_ms_);
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&send_timeout), sizeof(send_timeout));

        connection_.socket = ToSocketPtr(socket);
        connection_.active = true;
        connection_.job_id = job_id;
        connection_.next_sequence = 1;
        connected = true;
        break;
    }

    freeaddrinfo(addresses);

    if (!connected) {
        log_error("Failed to connect to Matlab server at " + host_ + ":" + port_text.str());
    } else {
        log_info("Connected to Matlab server at " + host_ + ":" + port_text.str() +
                 " job_id=" + std::to_string(job_id));
    }

    return connected;
#else
    (void)job_id;
    return false;
#endif
}

bool FrameStreamCore::send_event(const FrameStreamEvent& event) {
#ifdef _WIN32
    if (!connection_.active || connection_.socket == nullptr) {
        return false;
    }

    std::string error;
    std::vector<std::uint8_t> bytes;
    if (!FrameStreamProtocol::SerializeEvent(event, connection_.next_sequence, &bytes, &error)) {
        log_error("Failed to serialize frame stream event job_id=" + std::to_string(event.job_id) +
                  " error=" + error);
        return false;
    }

    const SocketHandle socket = ToSocketHandle(connection_.socket);
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const int chunk = send(socket,
                               reinterpret_cast<const char*>(bytes.data() + sent),
                               static_cast<int>(bytes.size() - sent),
                               0);
        if (chunk == SOCKET_ERROR || chunk <= 0) {
            log_error("send failed for job_id=" + std::to_string(event.job_id) +
                      " error=" + std::to_string(WSAGetLastError()));
            return false;
        }
        sent += static_cast<std::size_t>(chunk);
    }

    ++connection_.next_sequence;
    return true;
#else
    (void)event;
    return false;
#endif
}
