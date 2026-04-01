#ifndef FRAME_STREAM_CORE_H
#define FRAME_STREAM_CORE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include "thread_queue.h"
#include "types.h"

class FrameStreamCore {
public:
    explicit FrameStreamCore(std::size_t queue_capacity = 8);
    ~FrameStreamCore();

    bool start(const std::string& host, int port, int connect_timeout_ms, int send_timeout_ms);
    void stop();

    bool enqueue_event(const FrameStreamEvent& event);

private:
    struct ConnectionState {
        bool active = false;
        bool job_disabled = false;
        std::uint64_t job_id = 0;
        std::uint64_t next_sequence = 1;
#ifdef _WIN32
        void* socket = nullptr;
#endif
    };

    void worker_loop();
    void log_info(const std::string& message);
    void log_error(const std::string& message);
    void reset_connection();
    bool handle_event(const FrameStreamEvent& event);
    bool ensure_connected_for_job(std::uint64_t job_id);
    bool send_event(const FrameStreamEvent& event);

    ThreadQueue<FrameStreamEvent> events_;
    std::thread worker_;
    std::atomic<bool> started_{false};
    std::string host_;
    int port_ = 0;
    int connect_timeout_ms_ = 0;
    int send_timeout_ms_ = 0;
    ConnectionState connection_;
};

#endif
