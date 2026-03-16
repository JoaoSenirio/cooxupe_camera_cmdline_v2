#include "save_core.h"

#include <iostream>

SaveCore::SaveCore() = default;

SaveCore::~SaveCore() {
    stop();
}

bool SaveCore::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return true;
    }

    worker_ = std::thread(&SaveCore::worker_loop, this);
    return true;
}

void SaveCore::stop() {
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false)) {
        return;
    }

    summaries_.close();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool SaveCore::enqueue_summary(const AcquisitionSummary& summary) {
    if (!started_.load()) {
        return false;
    }
    return summaries_.push(summary);
}

void SaveCore::worker_loop() {
    AcquisitionSummary summary;
    while (summaries_.pop(&summary)) {
        std::cout << "[save] sample=" << summary.sample_name
                  << " light=" << summary.light_buffers
                  << " dark=" << summary.dark_buffers
                  << " total=" << summary.total_buffers
                  << " last_frame=" << summary.last_frame_number
                  << " pass=" << (summary.pass ? "true" : "false")
                  << " error=" << summary.sdk_error;

        if (!summary.message.empty()) {
            std::cout << " msg=\"" << summary.message << "\"";
        }

        std::cout << "\n";
    }
}
