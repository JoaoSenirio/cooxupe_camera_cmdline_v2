#ifndef FRAME_STREAM_CORE_H
#define FRAME_STREAM_CORE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include "shared_work_queue.h"
#include "types.h"

class FrameStreamCore {
public:
    explicit FrameStreamCore(std::size_t queue_capacity = 8);
    ~FrameStreamCore();

    bool start(SharedWorkQueue* work_queue,
               const std::string& host,
               int port,
               int connect_timeout_ms,
               int send_timeout_ms);
    void stop();

private:
    struct ConnectionState {
        bool active = false;
#ifdef _WIN32
        void* socket = nullptr;
#endif
    };

    void worker_loop();
    void log_info(const std::string& message);
    void log_error(const std::string& message);
    void reset_connection();
    bool handle_item(const WorkItem& item);
    bool ensure_connected();
    bool send_item(const WorkItem& item);
    bool send_bytes(const std::uint8_t* bytes, std::size_t size);
    bool receive_ack();

    std::thread worker_;
    std::atomic<bool> started_{false};
    SharedWorkQueue* work_queue_ = nullptr;
    std::string host_;
    int port_ = 0;
    int connect_timeout_ms_ = 0;
    int send_timeout_ms_ = 0;
    ConnectionState connection_;
};

#endif
