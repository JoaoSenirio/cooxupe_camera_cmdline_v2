#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

#include "app_config.h"
#include "specsensor_api.h"

namespace {

int BinningValueToEnumIndex(int value) {
    switch (value) {
        case 1:
            return 0;
        case 2:
            return 1;
        case 4:
            return 2;
        case 8:
            return 3;
        default:
            return -1;
    }
}

void LogApiFailure(ISpecSensorApi& api, const char* step, int code) {
    std::wcerr << L"[main] " << step
               << L" failed with code=" << code
               << L" msg=\"" << api.GetErrorString(code) << L"\"\n";
}

bool RunCameraCommand(ISpecSensorApi& api, const wchar_t* command, const char* step) {
    const int error = api.Command(command);
    if (error != 0) {
        LogApiFailure(api, step, error);
        return false;
    }
    std::cout << "[main] " << step << " ok\n";
    return true;
}

bool DisposeFrameBuffer(ISpecSensorApi& api, void* frame_buffer) {
    if (frame_buffer == nullptr) {
        std::cout << "[main] No frame buffer to dispose\n";
        return true;
    }

    std::cout << "[main] Disposing frame buffer\n";
    const int error = api.DisposeBuffer(frame_buffer);
    if (error != 0) {
        LogApiFailure(api, "SI_DisposeBuffer", error);
        return false;
    }
    std::cout << "[main] Frame buffer disposed\n";
    return true;
}

void PrintUsage() {
    std::cout << "Usage:\n"
              << "  specsensor_cli.exe              # Connect + configure + 10x light/dark capture test\n"
              << "  specsensor_cli.exe --run        # Same behavior as default\n";
}

bool ConnectCamera(const AppConfig& config, ISpecSensorApi& api) {
    int error = api.Load(config.license_path);
    if (error != 0) {
        LogApiFailure(api, "SI_Load", error);
        return false;
    }
    std::cout << "[main] SI_Load ok\n";

    std::int64_t device_count = 0;
    error = api.GetDeviceCount(&device_count);
    if (error != 0) {
        LogApiFailure(api, "SI_GetInt(SI_SYSTEM, DeviceCount)", error);
        api.Unload();
        return false;
    }

    std::cout << "[main] DeviceCount=" << device_count << "\n";
    if (config.device_index < 0 || config.device_index >= device_count) {
        std::cerr << "[main] Invalid device_index=" << config.device_index
                  << " (device_count=" << device_count << ")\n";
        api.Unload();
        return false;
    }

    error = api.Open(config.device_index);
    if (error != 0) {
        LogApiFailure(api, "SI_Open", error);
        api.Unload();
        return false;
    }
    std::cout << "[main] SI_Open ok (device_index=" << config.device_index << ")\n";

    error = api.SetString(L"Camera.CalibrationPack", config.calibration_scp_path);
    if (error != 0) {
        LogApiFailure(api, "SI_SetString(Camera.CalibrationPack)", error);
        api.Close();
        api.Unload();
        return false;
    }
    std::wcout << L"[main] Calibration .scp loaded: " << config.calibration_scp_path << L"\n";

    error = api.Command(L"Initialize");
    if (error != 0) {
        LogApiFailure(api, "Initialize", error);
        api.Close();
        api.Unload();
        return false;
    }
    std::cout << "[main] Initialize ok\n";

    return true;
}

bool ConfigureCameraParameters(const AppConfig& config, ISpecSensorApi& api) {
    int error = api.SetFloat(L"Camera.ExposureTime", config.exposure_ms);
    if (error != 0) {
        LogApiFailure(api, "SI_SetFloat(Camera.ExposureTime)", error);
        return false;
    }
    std::cout << "[main] ExposureTime=" << config.exposure_ms << " ms\n";

    error = api.SetFloat(L"Camera.FrameRate", config.frame_rate_hz);
    if (error != 0) {
        LogApiFailure(api, "SI_SetFloat(Camera.FrameRate)", error);
        return false;
    }
    std::cout << "[main] FrameRate=" << config.frame_rate_hz << " Hz\n";

    const int spatial_idx = BinningValueToEnumIndex(config.binning_spatial);
    const int spectral_idx = BinningValueToEnumIndex(config.binning_spectral);
    if (spatial_idx < 0 || spectral_idx < 0) {
        std::cerr << "[main] Invalid binning values in config\n";
        return false;
    }

    error = api.SetEnumIndex(L"Camera.Binning.Spatial", spatial_idx);
    if (error != 0) {
        LogApiFailure(api, "SI_SetEnumIndex(Camera.Binning.Spatial)", error);
        return false;
    }

    error = api.SetEnumIndex(L"Camera.Binning.Spectral", spectral_idx);
    if (error != 0) {
        LogApiFailure(api, "SI_SetEnumIndex(Camera.Binning.Spectral)", error);
        return false;
    }
    std::cout << "[main] Binning spatial=" << config.binning_spatial
              << " spectral=" << config.binning_spectral << "\n";

    return true;
}

bool CaptureFrames(const AppConfig& config, ISpecSensorApi& api) {
    std::int64_t buffer_size = 0;
    int error = api.GetInt(L"Camera.Image.SizeBytes", &buffer_size);
    if (error != 0) {
        LogApiFailure(api, "SI_GetInt(Camera.Image.SizeBytes)", error);
        return false;
    }

    void* frame_buffer = nullptr;
    error = api.CreateBuffer(buffer_size, &frame_buffer);
    if (error != 0) {
        LogApiFailure(api, "SI_CreateBuffer", error);
        return false;
    }
    std::cout << "[main] Frame buffer allocated: " << buffer_size << " bytes\n";

    if (!RunCameraCommand(api, L"Acquisition.Start", "Acquisition.Start")) {
        DisposeFrameBuffer(api, frame_buffer);
        return false;
    }

    if (!RunCameraCommand(api, L"Acquisition.RingBuffer.Sync", "Acquisition.RingBuffer.Sync (before LIGHT)")) {
        RunCameraCommand(api, L"Acquisition.Stop", "Acquisition.Stop");
        DisposeFrameBuffer(api, frame_buffer);
        return false;
    }

    if (!RunCameraCommand(api, L"Camera.OpenShutter", "Camera.OpenShutter")) {
        RunCameraCommand(api, L"Acquisition.Stop", "Acquisition.Stop");
        DisposeFrameBuffer(api, frame_buffer);
        return false;
    }

    std::int64_t light_buffers = 0;
    std::int64_t dark_buffers = 0;
    std::int64_t last_frame_number = 0;

    const auto light_start = std::chrono::steady_clock::now();
    const auto light_deadline = light_start + std::chrono::seconds(config.capture_seconds);
    std::cout << "[main] LIGHT phase started: duration_s=" << config.capture_seconds << "\n";

    while (std::chrono::steady_clock::now() < light_deadline) {
        std::int64_t frame_size = 0;
        std::int64_t frame_number = 0;

        error = api.Wait(static_cast<std::uint8_t*>(frame_buffer), &frame_size,
                         &frame_number, config.wait_timeout_ms);
        if (error != 0) {
            LogApiFailure(api, "SI_Wait", error);
            break;
        }

        ++light_buffers;
        last_frame_number = frame_number;

        if ((light_buffers % 100) == 0) {
            std::cout << "[light] frames=" << light_buffers
                      << " last_frame_number=" << last_frame_number << "\n";
        }
    }

    if (error == 0 && !RunCameraCommand(api, L"Camera.CloseShutter", "Camera.CloseShutter")) {
        error = -1;
    }

    if (error == 0) {
        if (!RunCameraCommand(api, L"Acquisition.RingBuffer.Sync", "Acquisition.RingBuffer.Sync (before DARK)")) {
            error = -1;
        }
    }

    if (error == 0) {
        std::cout << "[main] DARK phase started: target_frames=" << config.dark_frames << "\n";
        for (int i = 0; i < config.dark_frames; ++i) {
            std::int64_t frame_size = 0;
            std::int64_t frame_number = 0;

            error = api.Wait(static_cast<std::uint8_t*>(frame_buffer), &frame_size,
                             &frame_number, config.wait_timeout_ms);
            if (error != 0) {
                LogApiFailure(api, "SI_Wait", error);
                break;
            }

            ++dark_buffers;
            last_frame_number = frame_number;

            if ((dark_buffers % 100) == 0) {
                std::cout << "[dark] frames=" << dark_buffers
                          << " last_frame_number=" << last_frame_number << "\n";
            }
        }
    }

    if (!RunCameraCommand(api, L"Acquisition.Stop", "Acquisition.Stop") && error == 0) {
        error = -1;
    }

    if (!DisposeFrameBuffer(api, frame_buffer) && error == 0) {
        error = -1;
    }

    const std::int64_t total_buffers = light_buffers + dark_buffers;
    const bool workflow_ok = (error == 0) && (light_buffers > 0) &&
                             (dark_buffers == config.dark_frames);
    std::cout << "[main] Auto workflow finished. light=" << light_buffers
              << " dark=" << dark_buffers
              << " total=" << total_buffers
              << " last_frame=" << last_frame_number
              << " min_required=" << config.min_buffers_required
              << " workflow_ok=" << (workflow_ok ? "true" : "false") << "\n";

    const bool pass = workflow_ok;
    std::cout << "[main] Auto test result: pass=" << (pass ? "true" : "false") << "\n";

    return pass;
}

bool RunCaptureWorkflowTestLoop(const AppConfig& config, ISpecSensorApi& api, int iterations) {
    const int test_iterations = (iterations < 10) ? 10 : iterations;
    int passed_iterations = 0;

    std::cout << "[main] Running automatic capture workflow loop: iterations="
              << test_iterations
              << " light=" << config.capture_seconds
              << "s dark_frames=" << config.dark_frames << "\n";

    for (int i = 0; i < test_iterations; ++i) {
        std::cout << "[main] Iteration " << (i + 1) << "/" << test_iterations << "\n";
        if (!CaptureFrames(config, api)) {
            std::cout << "[main] Iteration " << (i + 1) << " failed\n";
            return false;
        }
        ++passed_iterations;
    }

    std::cout << "[main] Capture workflow loop finished. passed="
              << passed_iterations << "/" << test_iterations << "\n";
    return passed_iterations == test_iterations;
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

    std::string validation_error;
    if (!ValidateConfig(config, &validation_error)) {
        std::cerr << "[main] Invalid configuration: " << validation_error << "\n";
        return 1;
    }

    auto api = CreateSpecSensorApi();
    if (!ConnectCamera(config, *api)) {
        return 1;
    }

    bool pass = false;
    if (!ConfigureCameraParameters(config, *api)) {
        pass = false;
    } else {
        pass = RunCaptureWorkflowTestLoop(config, *api, 10);
    }

    api->Close();
    api->Unload();
    return pass ? 0 : 1;
}
