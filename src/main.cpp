#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <csignal>
#include <iostream>
#include <mutex>
#include <string>

#include "app_config.h"
#include "capture_core.h"
#include "pipe_core.h"
#include "save_core.h"
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

    SaveCore save_core(static_cast<std::size_t>(config.save_queue_capacity),
                       config.save_queue_push_timeout_ms);
    if (!save_core.start()) {
        capture_core.LogError("Failed to start SaveCore");
        capture_core.Shutdown();
        return 1;
    }

    capture_core.set_save_sink([&](const SaveEvent& event) {
        return save_core.enqueue_event(event);
    });

    std::mutex work_mutex;
    std::condition_variable work_cv;
    bool has_pending_job = false;
    AcquisitionJob pending_job;
    bool camera_busy = false;
    bool ctrl_c_logged = false;

    auto request_stop_if_needed = [&]() {
        if (g_ctrl_c_requested == 0 || capture_core.StopRequested()) {
            return;
        }

        if (!ctrl_c_logged) {
            capture_core.LogInfo("CTRL+C received. stop requested.");
            ctrl_c_logged = true;
        }

        capture_core.RequestStop();
        {
            std::lock_guard<std::mutex> lock(work_mutex);
            has_pending_job = false;
        }
        work_cv.notify_all();
    };

    PipeCore pipe_core;
    const bool pipe_started = pipe_core.start(config.pipe_name, [&](const AcquisitionJob& job) {
        std::string message;
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(work_mutex);
            if (g_ctrl_c_requested != 0 || capture_core.StopRequested()) {
                message = "Pipe command rejected: stop requested. sample=" + job.sample_name;
            } else if (camera_busy) {
                message = "Pipe command rejected: camera busy. sample=" + job.sample_name;
            } else if (has_pending_job) {
                message = "Pipe command rejected: pending job exists. sample=" + job.sample_name;
            } else {
                pending_job = job;
                has_pending_job = true;
                accepted = true;
                message = "Pipe command enqueued. sample=" + job.sample_name;
            }
        }

        capture_core.LogInfo(message);
        if (accepted) {
            work_cv.notify_one();
        }
        return accepted;
    });

    if (!pipe_started) {
        capture_core.LogError("Failed to start pipe server on " + config.pipe_name);
        save_core.stop();
        capture_core.Shutdown();
        return 1;
    }

    capture_core.LogInfo("Pipe server started on " + config.pipe_name);
    capture_core.LogInfo("Expected command format: CAPTURE <sample_name>\\n");
    capture_core.LogInfo("Press CTRL+C to shutdown");

    while (true) {
        request_stop_if_needed();
        if (capture_core.StopRequested()) {
            break;
        }

        AcquisitionJob job;
        bool has_job = false;
        {
            std::unique_lock<std::mutex> lock(work_mutex);
            const bool has_work = work_cv.wait_for(
                lock, std::chrono::milliseconds(200),
                [&] { return has_pending_job || capture_core.StopRequested() || g_ctrl_c_requested != 0; });

            if (g_ctrl_c_requested != 0 && !capture_core.StopRequested()) {
                if (!ctrl_c_logged) {
                    capture_core.LogInfo("CTRL+C received. stop requested.");
                    ctrl_c_logged = true;
                }
                capture_core.RequestStop();
                has_pending_job = false;
            }

            if (!has_work || capture_core.StopRequested() || g_ctrl_c_requested != 0) {
                if (capture_core.StopRequested()) {
                    has_pending_job = false;
                }
                continue;
            }

            job = pending_job;
            has_pending_job = false;
            camera_busy = true;
            has_job = true;
        }

        if (!has_job) {
            continue;
        }

        request_stop_if_needed();
        if (capture_core.StopRequested() || g_ctrl_c_requested != 0) {
            capture_core.LogInfo("Discarding queued command due to stop request. sample=" +
                                 job.sample_name);
            std::lock_guard<std::mutex> lock(work_mutex);
            camera_busy = false;
            continue;
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
    save_core.stop();
    capture_core.LogInfo("SaveCore stopped");
    capture_core.Shutdown();
    return 0;
}
