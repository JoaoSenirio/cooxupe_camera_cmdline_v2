#include "capture_core.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

constexpr int kAppStoppedByUser = -30000;

std::string MakeTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_local{};
#ifdef _WIN32
    localtime_s(&tm_local, &now_time);
#else
    localtime_r(&now_time, &tm_local);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_local, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

}  // namespace

int BinningValueToEnumIndex(int binning_value) {
    switch (binning_value) {
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

CaptureCore::CaptureCore(const AppConfig& config, ISpecSensorApi* api)
    : config_(config), api_(api) {}

CaptureCore::~CaptureCore() {
    Shutdown();
}

bool CaptureCore::Initialize() {
    if (api_ == nullptr) {
        std::cerr << "[capture] API pointer is null\n";
        return false;
    }
    shutdown_done_ = false;

    if (!OpenLogFile()) {
        std::cerr << "[capture] Failed to open log file: " << config_.log_file_path << "\n";
    }

    std::string validation_error;
    if (!ValidateConfig(config_, &validation_error)) {
        LogError("Invalid configuration: " + validation_error);
        return false;
    }

    if (!ConnectCamera()) {
        return false;
    }

    if (!ConfigureCameraParameters()) {
        Shutdown();
        return false;
    }

    initialized_ = true;
    LogInfo("CaptureCore initialized and ready");
    return true;
}

bool CaptureCore::CaptureSample(const AcquisitionJob& job, AcquisitionSummary* summary) {
    AcquisitionSummary local_summary;
    local_summary.sample_name = job.sample_name;

    if (!initialized_) {
        local_summary.sdk_error = -1;
        local_summary.message = "CaptureCore is not initialized";
        if (summary != nullptr) {
            *summary = local_summary;
        }
        return false;
    }

    std::int64_t buffer_size = 0;
    int error = api_->GetInt(L"Camera.Image.SizeBytes", &buffer_size);
    if (error != 0) {
        local_summary.sdk_error = error;
        local_summary.message = Narrow(api_->GetErrorString(error));
        LogApiFailure("SI_GetInt(Camera.Image.SizeBytes)", error);
        if (summary != nullptr) {
            *summary = local_summary;
        }
        return false;
    }

    void* frame_buffer = nullptr;
    error = api_->CreateBuffer(buffer_size, &frame_buffer);
    if (error != 0) {
        local_summary.sdk_error = error;
        local_summary.message = Narrow(api_->GetErrorString(error));
        LogApiFailure("SI_CreateBuffer", error);
        if (summary != nullptr) {
            *summary = local_summary;
        }
        return false;
    }

    LogInfo("Frame buffer allocated: " + std::to_string(buffer_size) + " bytes");

    error = RunCameraCommand(L"Acquisition.Start", "Acquisition.Start");
    if (error != 0) {
        local_summary.sdk_error = error;
        local_summary.message = Narrow(api_->GetErrorString(error));
        DisposeFrameBuffer(frame_buffer);
        if (summary != nullptr) {
            *summary = local_summary;
        }
        return false;
    }

    error = RunCameraCommand(L"Acquisition.RingBuffer.Sync", "Acquisition.RingBuffer.Sync (before LIGHT)");
    if (error != 0) {
        local_summary.sdk_error = error;
        local_summary.message = Narrow(api_->GetErrorString(error));
    }

    if (error == 0) {
        error = RunCameraCommand(L"Camera.OpenShutter", "Camera.OpenShutter");
        if (error != 0) {
            local_summary.sdk_error = error;
            local_summary.message = Narrow(api_->GetErrorString(error));
        }
    }

    if (error == 0) {
        LogInfo("LIGHT phase started: duration_s=" + std::to_string(config_.capture_seconds));
        const auto light_start = std::chrono::steady_clock::now();
        const auto light_deadline = light_start + std::chrono::seconds(config_.capture_seconds);
        while (std::chrono::steady_clock::now() < light_deadline) {
            if (stop_requested_.load()) {
                error = kAppStoppedByUser;
                local_summary.sdk_error = error;
                local_summary.message = "Stop requested by user";
                break;
            }

            std::int64_t frame_size = 0;
            std::int64_t frame_number = 0;
            error = api_->Wait(static_cast<std::uint8_t*>(frame_buffer), &frame_size,
                               &frame_number, config_.wait_timeout_ms);
            if (error != 0) {
                local_summary.sdk_error = error;
                local_summary.message = Narrow(api_->GetErrorString(error));
                LogApiFailure("SI_Wait", error);
                break;
            }

            ++local_summary.light_buffers;
            local_summary.last_frame_number = frame_number;
            if ((local_summary.light_buffers % 5000) == 0) {
                LogInfo("LIGHT frames=" + std::to_string(local_summary.light_buffers) +
                        " last_frame_number=" + std::to_string(local_summary.last_frame_number));
            }
        }
    }

    if (error == 0) {
        error = RunCameraCommand(L"Camera.CloseShutter", "Camera.CloseShutter");
        if (error != 0) {
            local_summary.sdk_error = error;
            local_summary.message = Narrow(api_->GetErrorString(error));
        }
    }

    if (error == 0) {
        error = RunCameraCommand(L"Acquisition.RingBuffer.Sync", "Acquisition.RingBuffer.Sync (before DARK)");
        if (error != 0) {
            local_summary.sdk_error = error;
            local_summary.message = Narrow(api_->GetErrorString(error));
        }
    }

    if (error == 0) {
        LogInfo("DARK phase started: target_frames=" + std::to_string(config_.dark_frames));
        for (int i = 0; i < config_.dark_frames; ++i) {
            if (stop_requested_.load()) {
                error = kAppStoppedByUser;
                local_summary.sdk_error = error;
                local_summary.message = "Stop requested by user";
                break;
            }

            std::int64_t frame_size = 0;
            std::int64_t frame_number = 0;
            error = api_->Wait(static_cast<std::uint8_t*>(frame_buffer), &frame_size,
                               &frame_number, config_.wait_timeout_ms);
            if (error != 0) {
                local_summary.sdk_error = error;
                local_summary.message = Narrow(api_->GetErrorString(error));
                LogApiFailure("SI_Wait", error);
                break;
            }

            ++local_summary.dark_buffers;
            local_summary.last_frame_number = frame_number;
            if ((local_summary.dark_buffers % 5000) == 0) {
                LogInfo("DARK frames=" + std::to_string(local_summary.dark_buffers) +
                        " last_frame_number=" + std::to_string(local_summary.last_frame_number));
            }
        }
    }

    const int stop_error = RunCameraCommand(L"Acquisition.Stop", "Acquisition.Stop");
    if (local_summary.sdk_error == 0 && stop_error != 0) {
        local_summary.sdk_error = stop_error;
        local_summary.message = Narrow(api_->GetErrorString(stop_error));
    }

    const int dispose_error = DisposeFrameBuffer(frame_buffer);
    if (local_summary.sdk_error == 0 && dispose_error != 0) {
        local_summary.sdk_error = dispose_error;
        local_summary.message = Narrow(api_->GetErrorString(dispose_error));
    }

    local_summary.total_buffers = local_summary.light_buffers + local_summary.dark_buffers;
    local_summary.pass = (local_summary.sdk_error == 0) &&
                         (local_summary.light_buffers > 0) &&
                         (local_summary.dark_buffers == config_.dark_frames);

    std::ostringstream oss;
    oss << "Capture workflow finished. sample=" << local_summary.sample_name
        << " light=" << local_summary.light_buffers
        << " dark=" << local_summary.dark_buffers
        << " total=" << local_summary.total_buffers
        << " last_frame=" << local_summary.last_frame_number
        << " pass=" << (local_summary.pass ? "true" : "false");
    if (local_summary.sdk_error != 0) {
        oss << " error=" << local_summary.sdk_error;
    }
    if (!local_summary.message.empty()) {
        oss << " msg=\"" << local_summary.message << "\"";
    }
    LogInfo(oss.str());

    if (summary != nullptr) {
        *summary = local_summary;
    }
    return local_summary.pass;
}

void CaptureCore::Shutdown() {
    if (api_ == nullptr) {
        return;
    }
    if (shutdown_done_) {
        return;
    }

    if (initialized_) {
        const int close_error = api_->Close();
        if (close_error != 0) {
            LogApiFailure("SI_Close", close_error);
        } else {
            LogInfo("SI_Close ok");
        }
    }

    const int unload_error = api_->Unload();
    if (unload_error != 0) {
        LogApiFailure("SI_Unload", unload_error);
    } else {
        LogInfo("SI_Unload ok");
    }

    initialized_ = false;
    shutdown_done_ = true;
}

void CaptureCore::RequestStop() {
    stop_requested_.store(true);
    LogInfo("Stop requested");
}

bool CaptureCore::StopRequested() const {
    return stop_requested_.load();
}

void CaptureCore::LogInfo(const std::string& message) {
    LogMessage("INFO", message);
}

void CaptureCore::LogError(const std::string& message) {
    LogMessage("ERROR", message);
}

bool CaptureCore::OpenLogFile() {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (log_file_.is_open()) {
        return true;
    }
    log_file_.open(config_.log_file_path.c_str(), std::ios::out | std::ios::app);
    return log_file_.is_open();
}

bool CaptureCore::ConnectCamera() {
    int error = api_->Load(config_.license_path);
    if (error != 0) {
        LogApiFailure("SI_Load", error);
        return false;
    }
    LogInfo("SI_Load ok");

    std::int64_t device_count = 0;
    error = api_->GetDeviceCount(&device_count);
    if (error != 0) {
        LogApiFailure("SI_GetInt(SI_SYSTEM, DeviceCount)", error);
        api_->Unload();
        return false;
    }

    LogInfo("DeviceCount=" + std::to_string(device_count));
    if (config_.device_index < 0 || config_.device_index >= device_count) {
        LogError("Invalid device_index=" + std::to_string(config_.device_index) +
                 " (device_count=" + std::to_string(device_count) + ")");
        api_->Unload();
        return false;
    }

    error = api_->Open(config_.device_index);
    if (error != 0) {
        LogApiFailure("SI_Open", error);
        api_->Unload();
        return false;
    }
    LogInfo("SI_Open ok (device_index=" + std::to_string(config_.device_index) + ")");

    error = api_->SetString(L"Camera.CalibrationPack", config_.calibration_scp_path);
    if (error != 0) {
        LogApiFailure("SI_SetString(Camera.CalibrationPack)", error);
        api_->Close();
        api_->Unload();
        return false;
    }
    LogInfo("Calibration .scp loaded");

    error = api_->Command(L"Initialize");
    if (error != 0) {
        LogApiFailure("Initialize", error);
        api_->Close();
        api_->Unload();
        return false;
    }
    LogInfo("Initialize ok");

    return true;
}

bool CaptureCore::ConfigureCameraParameters() {
    int error = api_->SetFloat(L"Camera.ExposureTime", config_.exposure_ms);
    if (error != 0) {
        LogApiFailure("SI_SetFloat(Camera.ExposureTime)", error);
        return false;
    }
    LogInfo("ExposureTime=" + std::to_string(config_.exposure_ms) + " ms");

    error = api_->SetFloat(L"Camera.FrameRate", config_.frame_rate_hz);
    if (error != 0) {
        LogApiFailure("SI_SetFloat(Camera.FrameRate)", error);
        return false;
    }
    LogInfo("FrameRate=" + std::to_string(config_.frame_rate_hz) + " Hz");

    const int spatial_idx = BinningValueToEnumIndex(config_.binning_spatial);
    const int spectral_idx = BinningValueToEnumIndex(config_.binning_spectral);
    if (spatial_idx < 0 || spectral_idx < 0) {
        LogError("Invalid binning values in config");
        return false;
    }

    error = api_->SetEnumIndex(L"Camera.Binning.Spatial", spatial_idx);
    if (error != 0) {
        LogApiFailure("SI_SetEnumIndex(Camera.Binning.Spatial)", error);
        return false;
    }

    error = api_->SetEnumIndex(L"Camera.Binning.Spectral", spectral_idx);
    if (error != 0) {
        LogApiFailure("SI_SetEnumIndex(Camera.Binning.Spectral)", error);
        return false;
    }

    LogInfo("Binning spatial=" + std::to_string(config_.binning_spatial) +
            " spectral=" + std::to_string(config_.binning_spectral));
    return true;
}

int CaptureCore::RunCameraCommand(const wchar_t* command, const char* step) {
    const int error = api_->Command(command);
    if (error != 0) {
        LogApiFailure(step, error);
    } else {
        LogInfo(std::string(step) + " ok");
    }
    return error;
}

int CaptureCore::DisposeFrameBuffer(void* frame_buffer) {
    if (frame_buffer == nullptr) {
        LogInfo("No frame buffer to dispose");
        return 0;
    }

    LogInfo("Disposing frame buffer");
    const int error = api_->DisposeBuffer(frame_buffer);
    if (error != 0) {
        LogApiFailure("SI_DisposeBuffer", error);
    } else {
        LogInfo("Frame buffer disposed");
    }
    return error;
}

void CaptureCore::LogApiFailure(const char* step, int code) {
    std::ostringstream oss;
    oss << step << " failed with code=" << code
        << " msg=\"" << Narrow(api_->GetErrorString(code)) << "\"";
    LogError(oss.str());
}

void CaptureCore::LogMessage(const char* level, const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    const std::string line = "[" + MakeTimestamp() + "] [" + level + "] " + message;
    if (std::string(level) == "ERROR") {
        std::cerr << line << "\n";
    } else {
        std::cout << line << "\n";
    }

    if (log_file_.is_open()) {
        log_file_ << line << "\n";
        log_file_.flush();
    }
}

std::string CaptureCore::Narrow(const wchar_t* text) {
    if (text == nullptr) {
        return "";
    }

    std::string out;
    while (*text != 0) {
        const wchar_t c = *text++;
        if (c >= 0 && c <= 127) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('?');
        }
    }
    return out;
}
