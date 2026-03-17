#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <string>

#include "app_config.h"
#include "capture_core.h"
#include "pipe_core.h"
#include "specsensor_api.h"

namespace {

volatile std::sig_atomic_t g_ctrl_c_requested = 0;

void SignalHandler(int signal_value) {
    if (signal_value == SIGINT) {
        g_ctrl_c_requested = 1;
    }
}

void PrintUsage() {
    std::cout << "Usage:\n"
              << "  specsensor_cli.exe              # Connect + configure + pipe server + on-demand capture\n"
              << "  specsensor_cli.exe --run        # Same behavior as default\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    AppConfig config = MakeDefaultConfig();

    if (argc > 1) {
        const std::string arg = argv[1];
        if (arg == "--help") {
            PrintUsage();
            return 0;
        }
        if (arg != "--run") {
            PrintUsage();
            return 1;
        }
    }

    std::signal(SIGINT, SignalHandler);

    auto api = CreateSpecSensorApi();
    CaptureCore capture_core(config, api.get());
    if (!capture_core.Initialize()) {
        return 1;
    }

    std::mutex work_mutex;
    std::condition_variable work_cv;
    bool has_pending_job = false;
    AcquisitionJob pending_job;
    bool camera_busy = false;

    PipeCore pipe_core;
    const bool pipe_started = pipe_core.start(config.pipe_name, [&](const AcquisitionJob& job) {
        std::lock_guard<std::mutex> lock(work_mutex);
        if (camera_busy || has_pending_job || capture_core.StopRequested()) {
            return false;
        }
        pending_job = job;
        has_pending_job = true;
        work_cv.notify_one();
        return true;
    });

    if (!pipe_started) {
        capture_core.LogError("Failed to start pipe server on " + config.pipe_name);
        capture_core.Shutdown();
        return 1;
    }

    capture_core.LogInfo("Pipe server started on " + config.pipe_name);
    capture_core.LogInfo("Expected command format: CAPTURE <sample_name>\\n");
    capture_core.LogInfo("Press CTRL+C to shutdown");

    while (!capture_core.StopRequested()) {
        if (g_ctrl_c_requested != 0) {
            capture_core.RequestStop();
            break;
        }

        AcquisitionJob job;
        {
            std::unique_lock<std::mutex> lock(work_mutex);
            const bool has_work = work_cv.wait_for(
                lock, std::chrono::milliseconds(200),
                [&] { return has_pending_job || capture_core.StopRequested(); });
            if (!has_work || capture_core.StopRequested()) {
                continue;
            }

            job = pending_job;
            has_pending_job = false;
            camera_busy = true;
        }

        capture_core.LogInfo("Starting capture request sample=" + job.sample_name);
        AcquisitionSummary summary;
        capture_core.CaptureSample(job, &summary);
        capture_core.LogInfo("Finished capture request sample=" + job.sample_name +
                             " pass=" + std::string(summary.pass ? "true" : "false"));

        {
            std::lock_guard<std::mutex> lock(work_mutex);
            camera_busy = false;
        }
    }

    pipe_core.stop();
    capture_core.LogInfo("Pipe server stopped");
    capture_core.Shutdown();
    return 0;
}
