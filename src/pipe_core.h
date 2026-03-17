#ifndef PIPE_CORE_H
#define PIPE_CORE_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "types.h"

class PipeCore {
public:
    using JobCallback = std::function<bool(const AcquisitionJob&)>;

    PipeCore();
    ~PipeCore();

    bool start(const std::string& pipe_name, JobCallback callback);
    void stop();

private:
    void worker_loop();
    bool process_text(const std::string& text_chunk, std::uint64_t connection_id);

    std::string pipe_name_;
    JobCallback callback_;

    std::thread worker_;
    std::atomic<bool> started_{false};
    std::atomic<bool> wake_for_shutdown_{false};
    std::string pending_line_;
};

#endif
