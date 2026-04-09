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

FrameStreamCore::FrameStreamCore(std::size_t queue_capacity) {
    (void)queue_capacity;
}

FrameStreamCore::~FrameStreamCore() {
    stop();
}

bool FrameStreamCore::start(SharedWorkQueue* work_queue,
                            const std::string& host,
                            int port,
                            int connect_timeout_ms,
                            int send_timeout_ms) {
    if (work_queue == nullptr || host.empty() || port <= 0 || port > 65535 ||
        connect_timeout_ms <= 0 || send_timeout_ms <= 0) {
        return false;
    }

    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return true;
    }

    work_queue_ = work_queue;
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

    if (worker_.joinable()) {
        worker_.join();
    }
    reset_connection();
    work_queue_ = nullptr;
}

void FrameStreamCore::worker_loop() {
#ifdef _WIN32
    WSADATA wsa_data{};
    const int wsa_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (wsa_result != 0) {
        log_error("WSAStartup failed error=" + std::to_string(wsa_result));
        return;
    }

    if (work_queue_ != nullptr) {
        SharedWorkQueue::Lease lease;
        while (started_.load() && work_queue_->pop(SharedWorkConsumer::Stream, &lease)) {
            const bool ok = lease.item != nullptr && handle_item(*lease.item);
            if (!ok) {
                const std::uint64_t job_id = lease.item ? lease.item->job_id : 0;
                log_error("Stream consumer failed for job_id=" + std::to_string(job_id));
                work_queue_->ack(lease, false, "stream consumer failed");
                break;
            }
            work_queue_->ack(lease, true);
        }
    }

    reset_connection();
    WSACleanup();
#else
    log_info("Frame stream is only active on Windows; worker will stay idle");
    if (work_queue_ != nullptr) {
        SharedWorkQueue::Lease lease;
        while (started_.load() && work_queue_->pop(SharedWorkConsumer::Stream, &lease)) {
            work_queue_->ack(lease, false, "stream consumer unavailable on this platform");
            break;
        }
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
}

bool FrameStreamCore::handle_item(const WorkItem& item) {
    if (item.type == WorkItemType::BeginJob) {
        reset_connection();
        if (!ensure_connected()) {
            return false;
        }
    } else if (!connection_.active) {
        return false;
    }

    if (!send_item(item)) {
        reset_connection();
        return false;
    }

    if (item.type == WorkItemType::EndJob) {
        reset_connection();
    }
    return true;
}

bool FrameStreamCore::ensure_connected() {
#ifdef _WIN32
    if (connection_.active) {
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

        const DWORD timeout = static_cast<DWORD>(send_timeout_ms_);
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        connection_.socket = ToSocketPtr(socket);
        connection_.active = true;
        connected = true;
        break;
    }

    freeaddrinfo(addresses);

    if (!connected) {
        log_error("Failed to connect to Matlab server at " + host_ + ":" + port_text.str());
    } else {
        log_info("Connected to Matlab server at " + host_ + ":" + port_text.str());
    }

    return connected;
#else
    return false;
#endif
}

bool FrameStreamCore::send_item(const WorkItem& item) {
#ifdef _WIN32
    if (!connection_.active || connection_.socket == nullptr) {
        return false;
    }

    std::string error;
    FrameStreamProtocol::SerializedMessage message;
    if (!FrameStreamProtocol::SerializeWorkItem(item, &message, &error)) {
        log_error("Failed to serialize stream item job_id=" + std::to_string(item.job_id) +
                  " error=" + error);
        return false;
    }

    if (!send_bytes(message.header.data(), message.header.size())) {
        return false;
    }
    if (!message.inline_payload.empty() &&
        !send_bytes(message.inline_payload.data(), message.inline_payload.size())) {
        return false;
    }
    if (message.external_payload != nullptr && !message.external_payload->empty() &&
        !send_bytes(message.external_payload->data(), message.external_payload->size())) {
        return false;
    }

    if (!receive_ack()) {
        log_error("Matlab did not acknowledge stream item job_id=" + std::to_string(item.job_id));
        return false;
    }
    return true;
#else
    (void)item;
    return false;
#endif
}

bool FrameStreamCore::send_bytes(const std::uint8_t* bytes, std::size_t size) {
#ifdef _WIN32
    if (!connection_.active || connection_.socket == nullptr || bytes == nullptr) {
        return false;
    }

    const SocketHandle socket = ToSocketHandle(connection_.socket);
    std::size_t sent = 0;
    while (sent < size) {
        const int chunk = send(socket,
                               reinterpret_cast<const char*>(bytes + sent),
                               static_cast<int>(size - sent),
                               0);
        if (chunk == SOCKET_ERROR || chunk <= 0) {
            log_error("send failed error=" + std::to_string(WSAGetLastError()));
            return false;
        }
        sent += static_cast<std::size_t>(chunk);
    }
    return true;
#else
    (void)bytes;
    (void)size;
    return false;
#endif
}

bool FrameStreamCore::receive_ack() {
#ifdef _WIN32
    if (!connection_.active || connection_.socket == nullptr) {
        return false;
    }

    std::vector<std::uint8_t> ack_bytes(FrameStreamProtocol::kAckBytes, 0);
    const SocketHandle socket = ToSocketHandle(connection_.socket);
    std::size_t received = 0;
    while (received < ack_bytes.size()) {
        const int chunk = recv(socket,
                               reinterpret_cast<char*>(ack_bytes.data() + received),
                               static_cast<int>(ack_bytes.size() - received),
                               0);
        if (chunk == SOCKET_ERROR || chunk <= 0) {
            log_error("recv ack failed error=" + std::to_string(WSAGetLastError()));
            return false;
        }
        received += static_cast<std::size_t>(chunk);
    }

    FrameStreamProtocol::Ack ack;
    std::string error;
    if (!FrameStreamProtocol::ParseAck(ack_bytes, &ack, &error)) {
        log_error("Invalid ack from Matlab: " + error);
        return false;
    }
    return ack.status == 0;
#else
    return false;
#endif
}
