#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "app_config.h"
#include "capture_core.h"
#include "pipe_core.h"
#include "save_core.h"
#include "specsensor_api.h"

namespace {

void PrintUsage() {
    std::cout << "Usage:\n"
              << "  specsensor_cli.exe                # Daemon mode via named pipe\n"
              << "  specsensor_cli.exe --sample NAME  # Single acquisition without pipe\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    AppConfig config = MakeDefaultConfig();

    bool single_sample_mode = false;
    std::string single_sample_name;

    if (argc > 1) {
        if (std::string(argv[1]) == "--sample") {
            if (argc < 3) {
                PrintUsage();
                return 1;
            }
            single_sample_mode = true;
            single_sample_name = argv[2];
        } else if (std::string(argv[1]) == "--help") {
            PrintUsage();
            return 0;
        } else {
            PrintUsage();
            return 1;
        }
    }

    auto api = CreateSpecSensorApi();
    SaveCore save_core;
    CaptureCore capture_core(config, api.get());

    std::mutex summary_mutex;
    std::condition_variable summary_cv;
    bool single_sample_done = false;

    capture_core.set_summary_callback([&](const AcquisitionSummary& summary) {
        save_core.enqueue_summary(summary);
        if (single_sample_mode && summary.sample_name == single_sample_name) {
            std::lock_guard<std::mutex> lock(summary_mutex);
            single_sample_done = true;
            summary_cv.notify_all();
        }
    });

    if (!save_core.start()) {
        std::cerr << "[main] Failed to start SaveCore\n";
        return 1;
    }

    if (!capture_core.start()) {
        std::cerr << "[main] Failed to start CaptureCore\n";
        save_core.stop();
        return 1;
    }

    PipeCore pipe_core;
    if (single_sample_mode) {
        AcquisitionJob job;
        job.sample_name = single_sample_name;
        if (!capture_core.enqueue_job(job)) {
            std::cerr << "[main] Failed to enqueue sample\n";
            capture_core.stop();
            save_core.stop();
            return 1;
        }

        std::unique_lock<std::mutex> lock(summary_mutex);
        summary_cv.wait(lock, [&]() { return single_sample_done; });
    } else {
        if (!pipe_core.start(config.pipe_name, [&](const AcquisitionJob& job) {
                capture_core.enqueue_job(job);
            })) {
            std::cerr << "[main] Failed to start PipeCore\n";
            capture_core.stop();
            save_core.stop();
            return 1;
        }

        std::cout << "[main] Running daemon. Pipe: " << config.pipe_name << "\n";
        std::cout << "[main] Type 'q' and press ENTER to exit.\n";

        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "q" || line == "quit" || line == "exit") {
                break;
            }
        }

        pipe_core.stop();
    }

    capture_core.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    save_core.stop();
    return 0;
}
