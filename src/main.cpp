#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include "app_config.h"
#include "capture_core.h"
#include "frame_stream_core.h"
#include "pipe_core.h"
#include "runtime_lifecycle.h"
#include "save_core.h"
#include "specsensor_api.h"
#include "ui_engine.h"
#include "workflow_ui_model.h"

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

#ifdef _WIN32
void DisableConsoleQuickEdit() {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == INVALID_HANDLE_VALUE || input == nullptr) {
        return;
    }

    DWORD mode = 0;
    if (!GetConsoleMode(input, &mode)) {
        return;
    }

    DWORD updated_mode = mode;
    updated_mode |= ENABLE_EXTENDED_FLAGS;
#ifdef ENABLE_QUICK_EDIT_MODE
    updated_mode &= ~static_cast<DWORD>(ENABLE_QUICK_EDIT_MODE);
#endif
    if (updated_mode != mode) {
        SetConsoleMode(input, updated_mode);
    }
}

std::string NarrowAscii(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t c : text) {
        out.push_back(c >= 0 && c <= 127 ? static_cast<char>(c) : '?');
    }
    return out;
}

std::vector<std::string> BuildArgsFromCommandLine() {
    std::vector<std::string> args;
    int argc = 0;
    LPWSTR* argv_wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv_wide == nullptr) {
        return args;
    }

    for (int i = 0; i < argc; ++i) {
        args.push_back(NarrowAscii(argv_wide[i]));
    }
    LocalFree(argv_wide);
    return args;
}
#endif

std::vector<std::string> BuildArgsFromMain(int argc, char* argv[]) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return args;
}

int RunApplication(const std::vector<std::string>& args) {
#ifdef _WIN32
    DisableConsoleQuickEdit();
#endif

    AppConfig config = MakeDefaultConfig();
    if (args.size() > 1) {
        const std::string& arg = args[1];
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

    RuntimeLifecycle runtime;
    WorkflowUiModel ui_model;
    std::mutex ui_model_mutex;

    auto api = CreateSpecSensorApi();
    CaptureCore capture_core(config, api.get());

    if (!capture_core.Initialize()) {
        runtime.BootstrapFailed();
        return 1;
    }
    runtime.BootstrapSucceeded();

    std::mutex work_mutex;
    std::condition_variable work_cv;
    bool has_pending_job = false;
    bool workflow_busy = false;
    AcquisitionJob pending_job;
    bool ctrl_c_logged = false;

    std::atomic<bool> shutdown_requested{false};
    std::atomic<bool> fatal_state{false};
    std::atomic<bool> ui_exit_requested{false};
    std::atomic<bool> save_progress_dirty{false};

    std::mutex save_progress_mutex;
    SaveProgressEvent latest_save_progress;
    bool has_latest_save_progress = false;

    UiEngine ui_engine;
    if (!ui_engine.start([&](const UiCommand& command) {
            if (command.type == UiCommandType::ExitRequested) {
                ui_exit_requested.store(true);
                capture_core.RequestStop();
                work_cv.notify_all();
            }
        })) {
        capture_core.LogError("Failed to start Win32 UI engine");
        capture_core.Shutdown();
        return 1;
    }

    SaveCore save_core(static_cast<std::size_t>(config.save_queue_capacity),
                       config.save_queue_push_timeout_ms);
    FrameStreamCore frame_stream_core(static_cast<std::size_t>(config.matlab_stream_queue_capacity));
    save_core.set_progress_sink([&](const SaveProgressEvent& event) {
        {
            std::lock_guard<std::mutex> lock(save_progress_mutex);
            latest_save_progress = event;
            has_latest_save_progress = true;
        }
        save_progress_dirty.store(true);
        work_cv.notify_all();
    });

    if (!runtime.background_workers_may_start() || !save_core.start()) {
        capture_core.LogError("Failed to start SaveCore");
        ui_engine.stop();
        capture_core.Shutdown();
        return 1;
    }

    bool frame_stream_running = false;
    if (config.matlab_stream_enabled) {
        frame_stream_running = frame_stream_core.start(config.matlab_stream_host,
                                                       config.matlab_stream_port,
                                                       config.matlab_stream_connect_timeout_ms,
                                                       config.matlab_stream_send_timeout_ms);
        if (frame_stream_running) {
            capture_core.LogInfo("Frame stream worker started for Matlab at " +
                                 config.matlab_stream_host + ":" +
                                 std::to_string(config.matlab_stream_port));
        } else {
            capture_core.LogError("Failed to start Matlab frame stream worker; continuing without stream");
        }
    }

    if (frame_stream_running) {
        save_core.set_frame_stream_sink([&](const FrameStreamEvent& event) {
            return frame_stream_core.enqueue_event(event);
        });
    }

    capture_core.set_save_sink([&](const SaveEvent& event) {
        return save_core.enqueue_event(event);
    });

    capture_core.set_progress_sink([&](const CaptureProgressEvent& event) {
        if (shutdown_requested.load() || fatal_state.load()) {
            return;
        }
        if (event.type == CaptureProgressType::CaptureFinished && !event.success) {
            return;
        }

        UiEvent ui_event;
        {
            std::lock_guard<std::mutex> lock(ui_model_mutex);
            if (event.type == CaptureProgressType::CaptureStarted) {
                ui_event = ui_model.OnCaptureStarted(event);
            } else if (event.type == CaptureProgressType::CaptureProgress) {
                ui_event = ui_model.OnCaptureProgress(event);
            } else {
                ui_event = ui_model.OnCaptureFinished(event);
            }
        }
        ui_engine.publish(ui_event);
    });

    PipeCore pipe_core;

    auto request_shutdown = [&](const std::string& log_message) {
        const bool already_requested = shutdown_requested.exchange(true);
        if (!already_requested && !log_message.empty()) {
            capture_core.LogInfo(log_message);
        }
        runtime.ShutdownRequested();
        capture_core.RequestStop();
        {
            std::lock_guard<std::mutex> lock(work_mutex);
            has_pending_job = false;
        }
        work_cv.notify_all();
    };

    auto enter_fatal_state = [&](const std::string& message) {
        const bool already_fatal = fatal_state.exchange(true);
        if (already_fatal) {
            return;
        }

        runtime.FatalErrorOccurred();
        capture_core.LogError("Fatal workflow error: " + message);
        capture_core.RequestStop();
        pipe_core.stop();
        {
            std::lock_guard<std::mutex> lock(work_mutex);
            has_pending_job = false;
        }

        UiEvent error_event;
        {
            std::lock_guard<std::mutex> lock(ui_model_mutex);
            error_event = ui_model.MakeFatalError(message);
        }
        ui_engine.publish(error_event);
        work_cv.notify_all();
    };

    auto consume_stop_requests = [&]() {
        if (g_ctrl_c_requested != 0 && !ctrl_c_logged) {
            capture_core.LogInfo("CTRL+C received. shutdown requested.");
            ctrl_c_logged = true;
        }
        if (g_ctrl_c_requested != 0) {
            request_shutdown("");
            return;
        }
        if (ui_exit_requested.load()) {
            request_shutdown("UI exit requested. shutdown requested.");
        }
    };

    auto process_latest_save_progress = [&]() {
        if (!save_progress_dirty.load()) {
            return;
        }

        SaveProgressEvent event;
        bool has_event = false;
        {
            std::lock_guard<std::mutex> lock(save_progress_mutex);
            if (!has_latest_save_progress) {
                save_progress_dirty.store(false);
                return;
            }
            event = latest_save_progress;
            has_latest_save_progress = false;
            save_progress_dirty.store(false);
            has_event = true;
        }

        if (!has_event || fatal_state.load()) {
            return;
        }

        UiEvent ui_event;
        {
            std::lock_guard<std::mutex> lock(ui_model_mutex);
            ui_event = ui_model.OnSaveProgress(event);
        }

        if (event.type == SaveProgressType::JobFinished) {
            {
                std::lock_guard<std::mutex> lock(work_mutex);
                workflow_busy = false;
            }

            if (event.success) {
                runtime.WorkflowFinished(true);
            } else {
                enter_fatal_state(event.message.empty() ? "Falha no salvamento em disco" : event.message);
                return;
            }
        }

        ui_engine.publish(ui_event);
    };

    const bool pipe_started = runtime.pipe_should_run() &&
                              pipe_core.start(config.pipe_name, [&](const AcquisitionJob& job) {
                                  std::string message;
                                  bool accepted = false;
                                  {
                                      std::lock_guard<std::mutex> lock(work_mutex);
                                      if (shutdown_requested.load() || fatal_state.load() ||
                                          g_ctrl_c_requested != 0 || capture_core.StopRequested()) {
                                          message = "Pipe command rejected: stop requested. sample=" + job.sample_name;
                                      } else if (workflow_busy) {
                                          message = "Pipe command rejected: workflow busy. sample=" + job.sample_name;
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
        ui_engine.stop();
        capture_core.Shutdown();
        return 1;
    }

    capture_core.LogInfo("SDK bootstrap completed. background mode ready.");
    capture_core.LogInfo("Pipe server started on " + config.pipe_name);
    capture_core.LogInfo("Expected command format: CAPTURE <sample_name>\\n");
    capture_core.LogInfo("Press CTRL+C or use tray icon to shutdown");

    while (!shutdown_requested.load()) {
        consume_stop_requests();
        process_latest_save_progress();
        if (shutdown_requested.load()) {
            break;
        }

        if (fatal_state.load()) {
            std::unique_lock<std::mutex> lock(work_mutex);
            work_cv.wait_for(lock, std::chrono::milliseconds(200), [&] {
                return shutdown_requested.load() ||
                       save_progress_dirty.load() ||
                       ui_exit_requested.load() ||
                       g_ctrl_c_requested != 0;
            });
            continue;
        }

        AcquisitionJob job;
        bool should_run_job = false;
        {
            std::unique_lock<std::mutex> lock(work_mutex);
            work_cv.wait_for(lock, std::chrono::milliseconds(200), [&] {
                return has_pending_job ||
                       save_progress_dirty.load() ||
                       ui_exit_requested.load() ||
                       shutdown_requested.load() ||
                       g_ctrl_c_requested != 0;
            });

            if (shutdown_requested.load() || save_progress_dirty.load() ||
                ui_exit_requested.load() || g_ctrl_c_requested != 0) {
                continue;
            }

            if (!has_pending_job || workflow_busy) {
                continue;
            }

            job = pending_job;
            has_pending_job = false;
            workflow_busy = true;
            should_run_job = true;
        }

        if (!should_run_job) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(save_progress_mutex);
            has_latest_save_progress = false;
        }
        save_progress_dirty.store(false);

        runtime.CaptureStarted();
        capture_core.LogInfo("Starting capture request sample=" + job.sample_name);

        AcquisitionSummary summary;
        const bool capture_ok = capture_core.CaptureSample(job, &summary);
        capture_core.LogInfo("Finished capture request sample=" + job.sample_name +
                             " pass=" + std::string(summary.pass ? "true" : "false"));

        if (!capture_ok) {
            enter_fatal_state(summary.message.empty() ? "Falha durante a captura da amostra " + job.sample_name
                                                      : summary.message);
        }

        process_latest_save_progress();
    }

    pipe_core.stop();
    capture_core.LogInfo("Pipe server stopped");
    save_core.stop();
    capture_core.LogInfo("SaveCore stopped");
    frame_stream_core.stop();
    if (frame_stream_running) {
        capture_core.LogInfo("FrameStreamCore stopped");
    }
    {
        std::lock_guard<std::mutex> lock(ui_model_mutex);
        ui_engine.publish(ui_model.MakeHideEvent());
    }
    ui_engine.stop();
    capture_core.Shutdown();

    return fatal_state.load() ? 1 : 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    return RunApplication(BuildArgsFromMain(argc, argv));
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return RunApplication(BuildArgsFromCommandLine());
}
#endif
