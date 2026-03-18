#include "capture_core.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

namespace {

constexpr int kAppStoppedByUser = -30000;
constexpr int kAppSaveQueueError = -30001;
constexpr int kAppInvalidFrameSize = -30002;
constexpr int kAppSnapshotError = -30003;
constexpr auto kMinRestartDelay = std::chrono::milliseconds(250);

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

std::string MakeDayStamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_local{};
#ifdef _WIN32
    localtime_s(&tm_local, &now_time);
#else
    localtime_r(&now_time, &tm_local);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_local, "%Y%m%d");
    return oss.str();
}

std::string MakeFolderTimestampTag() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_local{};
#ifdef _WIN32
    localtime_s(&tm_local, &now_time);
#else
    localtime_r(&now_time, &tm_local);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_local, "%Y-%m-%d_%H-%M-%S");
    return oss.str();
}

std::string MakeUtcDate() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &now_time);
#else
    gmtime_r(&now_time, &tm_utc);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%d");
    return oss.str();
}

std::string MakeUtcTime() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &now_time);
#else
    gmtime_r(&now_time, &tm_utc);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%H:%M:%S");
    return oss.str();
}

std::string NormalizePathSeparators(const std::string& path) {
    std::string normalized = path;
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        if (normalized[i] == '/') {
            normalized[i] = '\\';
        }
    }
    return normalized;
}

void SplitPath(const std::string& full_path, std::string* dir, std::string* file_name) {
    const std::size_t pos = full_path.find_last_of("\\/");
    if (pos == std::string::npos) {
        *dir = "";
        *file_name = full_path;
        return;
    }

    *dir = full_path.substr(0, pos);
    *file_name = full_path.substr(pos + 1);
}

void SplitStemExtension(const std::string& file_name, std::string* stem, std::string* ext) {
    const std::size_t dot_pos = file_name.find_last_of('.');
    if (dot_pos == std::string::npos || dot_pos == 0) {
        *stem = file_name;
        *ext = "";
        return;
    }

    *stem = file_name.substr(0, dot_pos);
    *ext = file_name.substr(dot_pos);
}

bool FileExists(const std::string& path) {
    std::ifstream file(path.c_str(), std::ios::in);
    return file.is_open();
}

#ifdef _WIN32
bool DirectoryExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool CreateDirectoriesRecursive(const std::string& path) {
    if (path.empty()) {
        return true;
    }
    if (DirectoryExists(path)) {
        return true;
    }

    const std::string normalized = NormalizePathSeparators(path);
    std::string current;
    current.reserve(normalized.size());

    for (std::size_t i = 0; i < normalized.size(); ++i) {
        current.push_back(normalized[i]);

        const bool is_sep = (normalized[i] == '\\');
        const bool is_last = (i + 1 == normalized.size());
        if (!is_sep && !is_last) {
            continue;
        }
        if (current.empty()) {
            continue;
        }

        if (current.size() == 2 && current[1] == ':') {
            continue;
        }
        if (current.size() == 3 && current[1] == ':' && current[2] == '\\') {
            continue;
        }

        if (!DirectoryExists(current)) {
            if (!CreateDirectoryA(current.c_str(), nullptr)) {
                const DWORD error = GetLastError();
                if (error != ERROR_ALREADY_EXISTS) {
                    return false;
                }
            }
        }
    }

    if (!DirectoryExists(normalized)) {
        if (!CreateDirectoryA(normalized.c_str(), nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) {
                return false;
            }
        }
    }
    return true;
}
#else
bool CreateDirectoriesRecursive(const std::string&) {
    return true;
}
#endif

int EnumIndexToBinningValue(int index) {
    switch (index) {
        case 0:
            return 1;
        case 1:
            return 2;
        case 2:
            return 4;
        case 3:
            return 8;
        default:
            return -1;
    }
}

std::string NarrowAscii(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c >= 0 && c <= 127) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

bool ParseDoubleFromWide(const std::wstring& text, double* out) {
    if (out == nullptr) {
        return false;
    }
    try {
        *out = std::stod(NarrowAscii(text));
        return true;
    } catch (...) {
        return false;
    }
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

void CaptureCore::set_save_sink(std::function<bool(const SaveEvent&)> save_sink) {
    save_sink_ = save_sink;
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

    SensorSnapshot snapshot;
    if (!FillSensorSnapshot(&snapshot)) {
        local_summary.sdk_error = kAppSnapshotError;
        local_summary.message = "Failed to read camera metadata";
        if (summary != nullptr) {
            *summary = local_summary;
        }
        return false;
    }

    void* frame_buffer = nullptr;
    int error = api_->CreateBuffer(snapshot.frame_size_bytes, &frame_buffer);
    if (error != 0) {
        local_summary.sdk_error = error;
        local_summary.message = Narrow(api_->GetErrorString(error));
        LogApiFailure("SI_CreateBuffer", error);
        if (summary != nullptr) {
            *summary = local_summary;
        }
        return false;
    }

    LogInfo("Frame buffer allocated: " + std::to_string(snapshot.frame_size_bytes) + " bytes");

    const bool has_save_sink = static_cast<bool>(save_sink_);
    const std::uint64_t job_id = has_save_sink ? next_job_id_++ : 0;
    bool begin_sent = false;

    std::string acquisition_date_utc = MakeUtcDate();
    std::string light_start_time_utc;
    std::string light_stop_time_utc;
    std::string dark_start_time_utc;
    std::string dark_stop_time_utc;

    if (has_save_sink) {
        SaveEvent begin;
        begin.type = SaveEventType::BeginJob;
        begin.job_id = job_id;
        begin.begin.sample_name = job.sample_name;
        begin.begin.camera_name = config_.camera_name;
        begin.begin.output_dir = config_.output_dir;
        begin.begin.timestamp_tag = MakeFolderTimestampTag();
        begin.begin.rgb_wavelength_nm[0] = config_.rgb_wavelength_nm[0];
        begin.begin.rgb_wavelength_nm[1] = config_.rgb_wavelength_nm[1];
        begin.begin.rgb_wavelength_nm[2] = config_.rgb_wavelength_nm[2];
        begin.begin.sensor = snapshot;
        begin.begin.acquisition_date_utc = acquisition_date_utc;
        begin.begin.light_start_time_utc = MakeUtcTime();

        if (!EmitSaveEvent(begin, &local_summary)) {
            DisposeFrameBuffer(frame_buffer);
            if (summary != nullptr) {
                *summary = local_summary;
            }
            return false;
        }
        begin_sent = true;
    }

    auto set_error = [&](int code, const std::string& message) {
        if (local_summary.sdk_error == 0) {
            local_summary.sdk_error = code;
            local_summary.message = message;
        }
    };

    std::vector<std::uint8_t> chunk_bytes;
    chunk_bytes.reserve(static_cast<std::size_t>(snapshot.frame_size_bytes) *
                        static_cast<std::size_t>(config_.save_block_frames));
    std::int64_t chunk_frame_count = 0;
    std::int64_t chunk_first_frame = 0;
    std::int64_t chunk_last_frame = 0;

    auto flush_chunk = [&](SaveEventType type) -> bool {
        if (chunk_frame_count <= 0) {
            return true;
        }

        if (!has_save_sink) {
            chunk_bytes.clear();
            chunk_frame_count = 0;
            chunk_first_frame = 0;
            chunk_last_frame = 0;
            return true;
        }

        SaveEvent chunk_event;
        chunk_event.type = type;
        chunk_event.job_id = job_id;
        chunk_event.chunk.frame_count = chunk_frame_count;
        chunk_event.chunk.first_frame_number = chunk_first_frame;
        chunk_event.chunk.last_frame_number = chunk_last_frame;
        chunk_event.chunk.bytes.swap(chunk_bytes);

        if (!EmitSaveEvent(chunk_event, &local_summary)) {
            return false;
        }

        chunk_bytes.clear();
        chunk_frame_count = 0;
        chunk_first_frame = 0;
        chunk_last_frame = 0;
        return true;
    };

    bool acquisition_active = false;

    error = RunCameraCommand(L"Acquisition.Start", "Acquisition.Start");
    if (error != 0) {
        set_error(error, Narrow(api_->GetErrorString(error)));
    } else {
        acquisition_active = true;
    }

    if (error == 0) {
        error = RunCameraCommand(L"Acquisition.RingBuffer.Sync", "Acquisition.RingBuffer.Sync (before LIGHT)");
        if (error != 0) {
            set_error(error, Narrow(api_->GetErrorString(error)));
        }
    }

    if (error == 0) {
        error = RunCameraCommand(L"Camera.OpenShutter", "Camera.OpenShutter");
        if (error != 0) {
            set_error(error, Narrow(api_->GetErrorString(error)));
        }
    }

    std::int64_t prev_light_frame = -1;
    if (error == 0) {
        light_start_time_utc = MakeUtcTime();
        LogInfo("LIGHT phase started: duration_s=" + std::to_string(config_.capture_seconds));
        const auto light_start = std::chrono::steady_clock::now();
        const auto light_deadline = light_start + std::chrono::seconds(config_.capture_seconds);
        while (std::chrono::steady_clock::now() < light_deadline) {
            if (stop_requested_.load()) {
                error = kAppStoppedByUser;
                set_error(error, "Stop requested by user");
                break;
            }

            std::int64_t frame_size = 0;
            std::int64_t frame_number = 0;
            error = api_->Wait(static_cast<std::uint8_t*>(frame_buffer), &frame_size,
                               &frame_number, config_.wait_timeout_ms);
            if (error != 0) {
                set_error(error, Narrow(api_->GetErrorString(error)));
                LogApiFailure("SI_Wait", error);
                break;
            }

            if (frame_size != snapshot.frame_size_bytes) {
                error = kAppInvalidFrameSize;
                set_error(error,
                          "Invalid light frame size. got=" + std::to_string(frame_size) +
                              " expected=" + std::to_string(snapshot.frame_size_bytes));
                break;
            }

            ++local_summary.light_buffers;
            local_summary.last_frame_number = frame_number;

            if (prev_light_frame >= 0 && frame_number > prev_light_frame + 1) {
                ++local_summary.light_drop_incidents;
                local_summary.light_dropped_frames += (frame_number - prev_light_frame - 1);
            }
            prev_light_frame = frame_number;

            if (chunk_frame_count == 0) {
                chunk_first_frame = frame_number;
            }
            chunk_last_frame = frame_number;
            ++chunk_frame_count;

            const std::uint8_t* frame_ptr = static_cast<std::uint8_t*>(frame_buffer);
            chunk_bytes.insert(chunk_bytes.end(), frame_ptr, frame_ptr + frame_size);

            if (chunk_frame_count >= config_.save_block_frames) {
                if (!flush_chunk(SaveEventType::LightChunk)) {
                    error = kAppSaveQueueError;
                    set_error(error, "Save queue timeout while pushing LIGHT chunk");
                    break;
                }
            }

            if ((local_summary.light_buffers % 5000) == 0) {
                LogInfo("LIGHT frames=" + std::to_string(local_summary.light_buffers) +
                        " last_frame_number=" + std::to_string(local_summary.last_frame_number));
            }
        }
        light_stop_time_utc = MakeUtcTime();
    }

    if (chunk_frame_count > 0 && error != kAppSaveQueueError) {
        if (!flush_chunk(SaveEventType::LightChunk)) {
            if (error == 0) {
                error = kAppSaveQueueError;
            }
            set_error(kAppSaveQueueError, "Save queue timeout while flushing LIGHT chunk");
        }
    }

    if (error == 0) {
        error = RunCameraCommand(L"Camera.CloseShutter", "Camera.CloseShutter");
        if (error != 0) {
            set_error(error, Narrow(api_->GetErrorString(error)));
        }
    }

    if (error == 0) {
        LogInfo("Restarting acquisition before DARK to discard queued LIGHT frames");
        error = RunCameraCommand(L"Acquisition.Stop", "Acquisition.Stop (after LIGHT)");
        if (error != 0) {
            set_error(error, Narrow(api_->GetErrorString(error)));
        } else {
            acquisition_active = false;
        }
    }

    if (error == 0) {
        LogInfo("Waiting " + std::to_string(kMinRestartDelay.count()) +
                " ms before restarting acquisition for DARK");
        std::this_thread::sleep_for(kMinRestartDelay);
        error = RunCameraCommand(L"Acquisition.Start", "Acquisition.Start (before DARK)");
        if (error != 0) {
            set_error(error, Narrow(api_->GetErrorString(error)));
        } else {
            acquisition_active = true;
        }
    }

    std::int64_t prev_dark_frame = -1;
    if (error == 0) {
        dark_start_time_utc = MakeUtcTime();
        LogInfo("DARK phase started: target_frames=" + std::to_string(config_.dark_frames));
        for (int i = 0; i < config_.dark_frames; ++i) {
            if (stop_requested_.load()) {
                error = kAppStoppedByUser;
                set_error(error, "Stop requested by user");
                break;
            }

            std::int64_t frame_size = 0;
            std::int64_t frame_number = 0;
            error = api_->Wait(static_cast<std::uint8_t*>(frame_buffer), &frame_size,
                               &frame_number, config_.wait_timeout_ms);
            if (error != 0) {
                set_error(error, Narrow(api_->GetErrorString(error)));
                LogApiFailure("SI_Wait", error);
                break;
            }

            if (frame_size != snapshot.frame_size_bytes) {
                error = kAppInvalidFrameSize;
                set_error(error,
                          "Invalid dark frame size. got=" + std::to_string(frame_size) +
                              " expected=" + std::to_string(snapshot.frame_size_bytes));
                break;
            }

            ++local_summary.dark_buffers;
            local_summary.last_frame_number = frame_number;

            if (prev_dark_frame >= 0 && frame_number > prev_dark_frame + 1) {
                ++local_summary.dark_drop_incidents;
                local_summary.dark_dropped_frames += (frame_number - prev_dark_frame - 1);
            }
            prev_dark_frame = frame_number;

            if (chunk_frame_count == 0) {
                chunk_first_frame = frame_number;
            }
            chunk_last_frame = frame_number;
            ++chunk_frame_count;

            const std::uint8_t* frame_ptr = static_cast<std::uint8_t*>(frame_buffer);
            chunk_bytes.insert(chunk_bytes.end(), frame_ptr, frame_ptr + frame_size);

            if (chunk_frame_count >= config_.save_block_frames) {
                if (!flush_chunk(SaveEventType::DarkChunk)) {
                    error = kAppSaveQueueError;
                    set_error(error, "Save queue timeout while pushing DARK chunk");
                    break;
                }
            }

            if ((local_summary.dark_buffers % 5000) == 0) {
                LogInfo("DARK frames=" + std::to_string(local_summary.dark_buffers) +
                        " last_frame_number=" + std::to_string(local_summary.last_frame_number));
            }
        }
        dark_stop_time_utc = MakeUtcTime();
    }

    if (chunk_frame_count > 0 && error != kAppSaveQueueError) {
        if (!flush_chunk(SaveEventType::DarkChunk)) {
            if (error == 0) {
                error = kAppSaveQueueError;
            }
            set_error(kAppSaveQueueError, "Save queue timeout while flushing DARK chunk");
        }
    }

    if (acquisition_active) {
        const int stop_error = RunCameraCommand(L"Acquisition.Stop", "Acquisition.Stop");
        if (local_summary.sdk_error == 0 && stop_error != 0) {
            set_error(stop_error, Narrow(api_->GetErrorString(stop_error)));
        } else if (stop_error == 0) {
            acquisition_active = false;
        }
    }

    const int dispose_error = DisposeFrameBuffer(frame_buffer);
    if (local_summary.sdk_error == 0 && dispose_error != 0) {
        set_error(dispose_error, Narrow(api_->GetErrorString(dispose_error)));
    }

    local_summary.total_buffers = local_summary.light_buffers + local_summary.dark_buffers;
    local_summary.pass = (local_summary.sdk_error == 0) &&
                         (local_summary.light_buffers > 0) &&
                         (local_summary.dark_buffers == config_.dark_frames);

    if (has_save_sink && begin_sent) {
        SaveEvent end;
        end.type = SaveEventType::EndJob;
        end.job_id = job_id;
        end.end.success = local_summary.pass;
        end.end.sdk_error = local_summary.sdk_error;
        end.end.message = local_summary.message;
        end.end.light_frames = local_summary.light_buffers;
        end.end.dark_frames = local_summary.dark_buffers;
        end.end.light_drop_incidents = local_summary.light_drop_incidents;
        end.end.dark_drop_incidents = local_summary.dark_drop_incidents;
        end.end.light_dropped_frames = local_summary.light_dropped_frames;
        end.end.dark_dropped_frames = local_summary.dark_dropped_frames;
        end.end.acquisition_date_utc = acquisition_date_utc;
        end.end.light_start_time_utc = light_start_time_utc;
        end.end.light_stop_time_utc = light_stop_time_utc;
        end.end.dark_start_time_utc = dark_start_time_utc;
        end.end.dark_stop_time_utc = dark_stop_time_utc;

        if (!EmitSaveEvent(end, &local_summary)) {
            local_summary.pass = false;
        }
    }

    std::ostringstream oss;
    oss << "Capture workflow finished. sample=" << local_summary.sample_name
        << " light=" << local_summary.light_buffers
        << " dark=" << local_summary.dark_buffers
        << " total=" << local_summary.total_buffers
        << " light_drop_incidents=" << local_summary.light_drop_incidents
        << " dark_drop_incidents=" << local_summary.dark_drop_incidents
        << " light_dropped_frames=" << local_summary.light_dropped_frames
        << " dark_dropped_frames=" << local_summary.dark_dropped_frames
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

    const std::string configured_path = NormalizePathSeparators(config_.log_file_path);
    std::string parent_path;
    std::string file_name;
    SplitPath(configured_path, &parent_path, &file_name);

    std::string stem;
    std::string ext;
    SplitStemExtension(file_name, &stem, &ext);
    if (stem.empty()) {
        stem = "specsensor_cli";
    }
    if (ext.empty()) {
        ext = ".log";
    }

    const std::string dated_name = stem + "_" + MakeDayStamp() + ext;
    const std::string final_log_path = parent_path.empty()
                                           ? dated_name
                                           : (parent_path + "\\" + dated_name);

    if (!parent_path.empty()) {
        if (!CreateDirectoriesRecursive(parent_path)) {
            return false;
        }
    }

    const bool file_exists = FileExists(final_log_path);
    if (!file_exists) {
        std::ofstream creator(final_log_path.c_str(), std::ios::out);
        if (!creator.is_open()) {
            return false;
        }
        creator.close();
    }

    log_file_.open(final_log_path.c_str(), std::ios::out | std::ios::app);
    if (log_file_.is_open()) {
        std::cout << "[capture] Logging to file: " << final_log_path << "\n";
    }
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

bool CaptureCore::EmitSaveEvent(const SaveEvent& event, AcquisitionSummary* summary) {
    if (!save_sink_) {
        return true;
    }

    if (save_sink_(event)) {
        return true;
    }

    if (summary != nullptr && summary->sdk_error == 0) {
        summary->sdk_error = kAppSaveQueueError;
        summary->message = "Save queue full or timed out";
    }

    LogError("Failed to enqueue save event. type=" +
             std::to_string(static_cast<int>(event.type)) +
             " job_id=" + std::to_string(event.job_id));
    return false;
}

bool CaptureCore::FillSensorSnapshot(SensorSnapshot* snapshot) {
    if (snapshot == nullptr) {
        return false;
    }

    SensorSnapshot out;

    int error = api_->GetInt(L"Camera.Image.Width", &out.image_width);
    if (error != 0) {
        LogApiFailure("SI_GetInt(Camera.Image.Width)", error);
        return false;
    }

    error = api_->GetInt(L"Camera.Image.Height", &out.image_height);
    if (error != 0) {
        LogApiFailure("SI_GetInt(Camera.Image.Height)", error);
        return false;
    }

    error = api_->GetInt(L"Camera.Image.SizeBytes", &out.frame_size_bytes);
    if (error != 0) {
        LogApiFailure("SI_GetInt(Camera.Image.SizeBytes)", error);
        return false;
    }

    if (out.image_width <= 0 || out.image_height <= 0 || out.frame_size_bytes <= 0) {
        LogError("Invalid camera geometry");
        return false;
    }

    const std::int64_t pixel_count = out.image_width * out.image_height;
    if (pixel_count > 0 && (out.frame_size_bytes % pixel_count) == 0) {
        out.byte_depth = out.frame_size_bytes / pixel_count;
    }
    if (out.byte_depth <= 0) {
        out.byte_depth = 2;
    }

    double frame_rate = 0.0;
    if (api_->GetFloat(L"Camera.FrameRate", &frame_rate) == 0 && frame_rate > 0.0) {
        out.frame_rate_hz = frame_rate;
    } else {
        out.frame_rate_hz = config_.frame_rate_hz;
    }

    double exposure = 0.0;
    if (api_->GetFloat(L"Camera.ExposureTime", &exposure) == 0 && exposure > 0.0) {
        out.exposure_ms = exposure;
    } else {
        out.exposure_ms = config_.exposure_ms;
    }

    out.binning_spatial = config_.binning_spatial;
    out.binning_spectral = config_.binning_spectral;

    int enum_idx = -1;
    if (api_->GetEnumIndex(L"Camera.Binning.Spatial", &enum_idx) == 0) {
        const int value = EnumIndexToBinningValue(enum_idx);
        if (value > 0) {
            out.binning_spatial = value;
        }
    }

    enum_idx = -1;
    if (api_->GetEnumIndex(L"Camera.Binning.Spectral", &enum_idx) == 0) {
        const int value = EnumIndexToBinningValue(enum_idx);
        if (value > 0) {
            out.binning_spectral = value;
        }
    }

    out.calibration_pack = Narrow(config_.calibration_scp_path.c_str());

    std::int64_t int_value = 0;
    if (api_->GetInt(L"Camera.SensorID", &int_value) == 0) {
        out.sensor_id = std::to_string(int_value);
    }

    if (api_->GetInt(L"AcquisitionWindow.Left", &int_value) == 0) {
        out.acquisitionwindow_left = int_value;
    }
    if (api_->GetInt(L"AcquisitionWindow.Top", &int_value) == 0) {
        out.acquisitionwindow_top = int_value;
    }

    double temperature = 0.0;
    if (api_->GetFloat(L"Camera.Temperature", &temperature) == 0) {
        out.has_vnir_temperature = true;
        out.vnir_temperature = temperature;
    }

    int enum_count = 0;
    if (api_->GetEnumCount(L"Camera.WavelengthTable", &enum_count) == 0 && enum_count > 0) {
        out.wavelengths_nm.reserve(static_cast<std::size_t>(enum_count));
        for (int i = 0; i < enum_count; ++i) {
            std::wstring value;
            if (api_->GetEnumStringByIndex(L"Camera.WavelengthTable", i, &value) == 0) {
                double parsed = 0.0;
                if (ParseDoubleFromWide(value, &parsed)) {
                    out.wavelengths_nm.push_back(parsed);
                }
            }
        }
    }

    enum_count = 0;
    if (api_->GetEnumCount(L"Camera.FWHM", &enum_count) == 0 && enum_count > 0) {
        out.fwhm_nm.reserve(static_cast<std::size_t>(enum_count));
        for (int i = 0; i < enum_count; ++i) {
            std::wstring value;
            if (api_->GetEnumStringByIndex(L"Camera.FWHM", i, &value) == 0) {
                double parsed = 0.0;
                if (ParseDoubleFromWide(value, &parsed)) {
                    out.fwhm_nm.push_back(parsed);
                }
            }
        }
    }

    *snapshot = out;
    return true;
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
