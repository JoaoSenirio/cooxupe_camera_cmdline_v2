#include "capture_core.h"

#include <chrono>
#include <iostream>
#include <string>

namespace {

constexpr int kAppConfigError = -20000;
constexpr int kAppDeviceIndexError = -20001;

std::string Narrow(const wchar_t* text) {
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
    stop();
}

bool CaptureCore::start() {
    if (api_ == nullptr) {
        std::cerr << "[capture] API pointer is null\n";
        return false;
    }

    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return true;
    }

    worker_ = std::thread(&CaptureCore::worker_loop, this);
    return true;
}

void CaptureCore::stop() {
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false)) {
        return;
    }

    jobs_.close();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool CaptureCore::enqueue_job(const AcquisitionJob& job) {
    if (!started_.load()) {
        return false;
    }
    return jobs_.push(job);
}

void CaptureCore::set_summary_callback(SummaryCallback callback) {
    on_summary_ = callback;
}

void CaptureCore::worker_loop() {
    const int init_error = initialize_sensor();
    if (init_error != 0) {
        AcquisitionSummary summary;
        summary.sample_name = "<startup>";
        summary.sdk_error = init_error;
        summary.pass = false;
        summary.message = Narrow(api_->GetErrorString(init_error));
        if (summary.message.empty()) {
            summary.message = "CaptureCore initialization failed";
        }
        if (on_summary_) {
            on_summary_(summary);
        }
        cleanup_sensor();
        return;
    }

    AcquisitionJob job;
    while (jobs_.pop(&job)) {
        AcquisitionSummary summary = run_workflow(job);
        if (on_summary_) {
            on_summary_(summary);
        }
    }

    cleanup_sensor();
}

int CaptureCore::initialize_sensor() {
    std::string validation_error;
    if (!ValidateConfig(config_, &validation_error)) {
        std::cerr << "[capture] Invalid configuration: " << validation_error << "\n";
        return kAppConfigError;
    }

    int error = api_->Load(config_.license_path);
    if (error != 0) {
        return error;
    }

    std::int64_t device_count = 0;
    error = api_->GetDeviceCount(&device_count);
    if (error != 0) {
        return error;
    }

    if (config_.device_index >= device_count) {
        std::cerr << "[capture] device_index=" << config_.device_index
                  << " is out of range (device_count=" << device_count << ")\n";
        return kAppDeviceIndexError;
    }

    error = api_->Open(config_.device_index);
    if (error != 0) {
        return error;
    }

    error = api_->Command(L"Initialize");
    if (error != 0) {
        return error;
    }

    error = api_->SetFloat(L"Camera.ExposureTime", config_.exposure_ms);
    if (error != 0) {
        return error;
    }

    error = api_->SetFloat(L"Camera.FrameRate", config_.frame_rate_hz);
    if (error != 0) {
        return error;
    }

    const int spatial_index = BinningValueToEnumIndex(config_.binning_spatial);
    const int spectral_index = BinningValueToEnumIndex(config_.binning_spectral);

    if (spatial_index < 0 || spectral_index < 0) {
        return kAppConfigError;
    }

    error = api_->SetEnumIndex(L"Camera.Binning.Spatial", spatial_index);
    if (error != 0) {
        return error;
    }

    error = api_->SetEnumIndex(L"Camera.Binning.Spectral", spectral_index);
    if (error != 0) {
        return error;
    }

    error = api_->GetInt(L"Camera.Image.SizeBytes", &frame_buffer_size_);
    if (error != 0) {
        return error;
    }

    error = api_->CreateBuffer(frame_buffer_size_, &frame_buffer_);
    if (error != 0) {
        return error;
    }

    sensor_ready_ = true;
    std::cout << "[capture] Sensor initialized. Buffer size: " << frame_buffer_size_ << " bytes\n";
    return 0;
}

void CaptureCore::cleanup_sensor() {
    if (frame_buffer_ != nullptr) {
        api_->DisposeBuffer(frame_buffer_);
        frame_buffer_ = nullptr;
    }

    if (sensor_ready_) {
        api_->Close();
    }

    api_->Unload();
    sensor_ready_ = false;
}

AcquisitionSummary CaptureCore::run_workflow(const AcquisitionJob& job) {
    AcquisitionSummary summary;
    summary.sample_name = job.sample_name;

    int error = api_->Command(L"Acquisition.Start");
    if (error != 0) {
        summary.sdk_error = error;
        summary.message = Narrow(api_->GetErrorString(error));
        return summary;
    }

    bool acquisition_started = true;

    error = api_->Command(L"Camera.OpenShutter");
    if (error != 0) {
        summary.sdk_error = error;
        summary.message = Narrow(api_->GetErrorString(error));
        goto stop_acquisition;
    }

    {
        const auto started = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - started <
               std::chrono::seconds(config_.capture_seconds)) {
            error = wait_for_frame(&summary, CapturePhase::Light);
            if (error != 0) {
                summary.sdk_error = error;
                summary.message = Narrow(api_->GetErrorString(error));
                goto stop_acquisition;
            }
        }
    }

    error = api_->Command(L"Camera.CloseShutter");
    if (error != 0) {
        summary.sdk_error = error;
        summary.message = Narrow(api_->GetErrorString(error));
        goto stop_acquisition;
    }

    error = api_->Command(L"Acquisition.RingBuffer.Sync");
    if (error != 0) {
        summary.sdk_error = error;
        summary.message = Narrow(api_->GetErrorString(error));
        goto stop_acquisition;
    }

    for (int i = 0; i < config_.dark_frames; ++i) {
        error = wait_for_frame(&summary, CapturePhase::Dark);
        if (error != 0) {
            summary.sdk_error = error;
            summary.message = Narrow(api_->GetErrorString(error));
            goto stop_acquisition;
        }
    }

stop_acquisition:
    if (acquisition_started) {
        const int stop_error = api_->Command(L"Acquisition.Stop");
        if (summary.sdk_error == 0 && stop_error != 0) {
            summary.sdk_error = stop_error;
            summary.message = Narrow(api_->GetErrorString(stop_error));
        }
    }

    if (summary.sdk_error == 0) {
        summary.pass = summary.total_buffers >= config_.min_buffers_required;
        if (!summary.pass) {
            summary.message = "Total buffers below min_buffers_required";
        }
    }

    return summary;
}

int CaptureCore::wait_for_frame(AcquisitionSummary* summary, CapturePhase phase) {
    std::int64_t frame_size = 0;
    std::int64_t frame_number = 0;

    int error = api_->Wait(static_cast<std::uint8_t*>(frame_buffer_), &frame_size,
                           &frame_number, config_.wait_timeout_ms);
    if (error != 0) {
        return error;
    }

    summary->last_frame_number = frame_number;
    ++summary->total_buffers;

    if (phase == CapturePhase::Light) {
        ++summary->light_buffers;
    } else {
        ++summary->dark_buffers;
    }

    return 0;
}
