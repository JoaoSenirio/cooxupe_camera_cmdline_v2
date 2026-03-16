#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>

#include "app_config.h"
#include "pipe_core.h"
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

void PrintUsage() {
    std::cout << "Usage:\n"
              << "  specsensor_cli.exe              # Connect + configure + pipe + capture test\n"
              << "  specsensor_cli.exe --run        # Same behavior as default\n";
}

bool RunConnectConfigureAndCapture(const AppConfig& config) {
    std::string validation_error;
    if (!ValidateConfig(config, &validation_error)) {
        std::cerr << "[main] Invalid configuration: " << validation_error << "\n";
        return false;
    }

    auto api = CreateSpecSensorApi();

    auto fail = [&](const char* step, int code) {
        std::wcerr << L"[main] " << step
                   << L" failed with code=" << code
                   << L" msg=\"" << api->GetErrorString(code) << L"\"\n";
    };

    int error = api->Load(config.license_path);
    if (error != 0) {
        fail("SI_Load", error);
        return false;
    }
    std::cout << "[main] SI_Load ok\n";

    std::int64_t device_count = 0;
    error = api->GetDeviceCount(&device_count);
    if (error != 0) {
        fail("SI_GetInt(SI_SYSTEM, DeviceCount)", error);
        api->Unload();
        return false;
    }

    std::cout << "[main] DeviceCount=" << device_count << "\n";
    if (config.device_index < 0 || config.device_index >= device_count) {
        std::cerr << "[main] Invalid device_index=" << config.device_index
                  << " (device_count=" << device_count << ")\n";
        api->Unload();
        return false;
    }

    error = api->Open(config.device_index);
    if (error != 0) {
        fail("SI_Open", error);
        api->Unload();
        return false;
    }
    std::cout << "[main] SI_Open ok (device_index=" << config.device_index << ")\n";

    error = api->SetString(L"Camera.CalibrationPack", config.calibration_scp_path);
    if (error != 0) {
        fail("SI_SetString(Camera.CalibrationPack)", error);
        api->Close();
        api->Unload();
        return false;
    }
    std::wcout << L"[main] Calibration .scp loaded: " << config.calibration_scp_path << L"\n";

    error = api->Command(L"Initialize");
    if (error != 0) {
        fail("Initialize", error);
        api->Close();
        api->Unload();
        return false;
    }
    std::cout << "[main] Initialize ok\n";

    error = api->SetFloat(L"Camera.ExposureTime", config.exposure_ms);
    if (error != 0) {
        fail("SI_SetFloat(Camera.ExposureTime)", error);
        api->Close();
        api->Unload();
        return false;
    }
    std::cout << "[main] ExposureTime=" << config.exposure_ms << " ms\n";

    error = api->SetFloat(L"Camera.FrameRate", config.frame_rate_hz);
    if (error != 0) {
        fail("SI_SetFloat(Camera.FrameRate)", error);
        api->Close();
        api->Unload();
        return false;
    }
    std::cout << "[main] FrameRate=" << config.frame_rate_hz << " Hz\n";

    const int spatial_idx = BinningValueToEnumIndex(config.binning_spatial);
    const int spectral_idx = BinningValueToEnumIndex(config.binning_spectral);
    if (spatial_idx < 0 || spectral_idx < 0) {
        std::cerr << "[main] Invalid binning values in config\n";
        api->Close();
        api->Unload();
        return false;
    }

    error = api->SetEnumIndex(L"Camera.Binning.Spatial", spatial_idx);
    if (error != 0) {
        fail("SI_SetEnumIndex(Camera.Binning.Spatial)", error);
        api->Close();
        api->Unload();
        return false;
    }

    error = api->SetEnumIndex(L"Camera.Binning.Spectral", spectral_idx);
    if (error != 0) {
        fail("SI_SetEnumIndex(Camera.Binning.Spectral)", error);
        api->Close();
        api->Unload();
        return false;
    }
    std::cout << "[main] Binning spatial=" << config.binning_spatial
              << " spectral=" << config.binning_spectral << "\n";

    std::int64_t buffer_size = 0;
    error = api->GetInt(L"Camera.Image.SizeBytes", &buffer_size);
    if (error != 0) {
        fail("SI_GetInt(Camera.Image.SizeBytes)", error);
        api->Close();
        api->Unload();
        return false;
    }

    void* frame_buffer = nullptr;
    error = api->CreateBuffer(buffer_size, &frame_buffer);
    if (error != 0) {
        fail("SI_CreateBuffer", error);
        api->Close();
        api->Unload();
        return false;
    }
    std::cout << "[main] Frame buffer allocated: " << buffer_size << " bytes\n";

    std::mutex sample_mutex;
    std::queue<std::string> sample_queue;

    PipeCore pipe_core;
    if (!pipe_core.start(config.pipe_name, [&](const AcquisitionJob& job) {
            std::lock_guard<std::mutex> lock(sample_mutex);
            sample_queue.push(job.sample_name);
        })) {
        std::cerr << "[main] Failed to start PipeCore\n";
        api->DisposeBuffer(frame_buffer);
        api->Close();
        api->Unload();
        return false;
    }
    std::cout << "[main] Pipe listener started on " << config.pipe_name << "\n";

    error = api->Command(L"Acquisition.Start");
    if (error != 0) {
        fail("Acquisition.Start", error);
        pipe_core.stop();
        api->DisposeBuffer(frame_buffer);
        api->Close();
        api->Unload();
        return false;
    }
    std::cout << "[main] Acquisition started\n";

    api->Command(L"Camera.OpenShutter");

    std::int64_t captured = 0;
    std::int64_t last_frame_number = 0;

    while (captured < config.min_buffers_required) {
        std::int64_t frame_size = 0;
        std::int64_t frame_number = 0;

        error = api->Wait(static_cast<std::uint8_t*>(frame_buffer), &frame_size,
                          &frame_number, config.wait_timeout_ms);
        if (error != 0) {
            fail("SI_Wait", error);
            break;
        }

        ++captured;
        last_frame_number = frame_number;

        if ((captured % 100) == 0) {
            std::cout << "[capture] frames=" << captured
                      << " last_frame_number=" << last_frame_number << "\n";
        }

        for (;;) {
            std::string sample_name;
            {
                std::lock_guard<std::mutex> lock(sample_mutex);
                if (sample_queue.empty()) {
                    break;
                }
                sample_name = sample_queue.front();
                sample_queue.pop();
            }
            std::cout << "[pipe] sample_name received: " << sample_name << "\n";
        }
    }

    api->Command(L"Acquisition.Stop");
    pipe_core.stop();

    api->DisposeBuffer(frame_buffer);
    api->Close();
    api->Unload();

    const bool pass = (captured >= config.min_buffers_required) && (error == 0);
    std::cout << "[main] Capture test finished. captured=" << captured
              << " target=" << config.min_buffers_required
              << " pass=" << (pass ? "true" : "false") << "\n";

    return pass;
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

    return RunConnectConfigureAndCapture(config) ? 0 : 1;
}
