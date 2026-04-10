#include "capture_core.h"

#include <algorithm>
#include <cmath>
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
constexpr double kFloatReadbackTolerance = 1e-6;
constexpr std::size_t kDefaultChunkTargetFrames = 64;
constexpr std::size_t kChunkMaxBytes = 512U * 1024U * 1024U;

std::vector<double> RebinSpectralSeries(const std::vector<double>& input,
                                        std::size_t target_count) {
    if (input.empty() || target_count == 0) {
        return {};
    }
    if (input.size() == target_count) {
        return input;
    }

    std::vector<double> rebinned;
    rebinned.reserve(target_count);
    for (std::size_t i = 0; i < target_count; ++i) {
        std::size_t begin = (i * input.size()) / target_count;
        std::size_t end = ((i + 1) * input.size()) / target_count;
        if (begin >= input.size()) {
            begin = input.size() - 1;
        }
        if (end <= begin) {
            end = std::min(input.size(), begin + 1);
        }

        double sum = 0.0;
        for (std::size_t j = begin; j < end; ++j) {
            sum += input[j];
        }
        rebinned.push_back(sum / static_cast<double>(end - begin));
    }
    return rebinned;
}

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

bool WideEqualsAscii(const std::wstring& lhs, const char* rhs_ascii) {
    if (rhs_ascii == nullptr) {
        return false;
    }

    std::size_t i = 0;
    for (; i < lhs.size() && rhs_ascii[i] != 0; ++i) {
        if (lhs[i] != static_cast<wchar_t>(rhs_ascii[i])) {
            return false;
        }
    }
    return i == lhs.size() && rhs_ascii[i] == 0;
}

bool NearlyEqual(double lhs, double rhs) {
    return std::fabs(lhs - rhs) <= kFloatReadbackTolerance;
}

std::size_t DetermineChunkFrameTarget(std::int64_t frame_size_bytes,
                                      int configured_target_frames) {
    const std::size_t default_target_frames =
        configured_target_frames > 0
            ? static_cast<std::size_t>(configured_target_frames)
            : kDefaultChunkTargetFrames;
    if (frame_size_bytes <= 0) {
        return default_target_frames;
    }

    const std::size_t frame_bytes = static_cast<std::size_t>(frame_size_bytes);
    if (frame_bytes == 0) {
        return default_target_frames;
    }

    const std::size_t target_bytes = default_target_frames * frame_bytes;
    if (target_bytes <= kChunkMaxBytes) {
        return default_target_frames;
    }

    const std::size_t reduced = kChunkMaxBytes / frame_bytes;
    return std::max<std::size_t>(1, std::min(default_target_frames, reduced));
}

void LogReadoutTimeIfAvailable(ISpecSensorApi* api,
                               const std::function<void(const std::string&)>& log_info) {
    if (api == nullptr) {
        return;
    }

    double readout_time_ms = 0.0;
    const int error = api->GetFloat(L"Camera.Image.ReadoutTime", &readout_time_ms);
    if (error != 0) {
        return;
    }

    std::ostringstream oss;
    oss << "Camera.Image.ReadoutTime=" << std::fixed << std::setprecision(6)
        << readout_time_ms << " ms";
    log_info(oss.str());
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

void CaptureCore::set_work_sink(std::function<bool(WorkItem)> work_sink) {
    work_sink_ = work_sink;
}

void CaptureCore::set_progress_sink(std::function<void(const CaptureProgressEvent&)> progress_sink) {
    progress_sink_ = progress_sink;
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

    const bool has_work_sink = static_cast<bool>(work_sink_);
    const std::uint64_t job_id = has_work_sink ? next_job_id_++ : 0;
    bool begin_sent = false;
    const double effective_frame_rate_hz =
        snapshot.frame_rate_hz > 0.0 ? snapshot.frame_rate_hz : config_.frame_rate_hz;
    const std::int64_t expected_light_frames = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(std::llround(static_cast<double>(config_.capture_seconds) *
                                                  std::max(0.0, effective_frame_rate_hz))));
    const std::int64_t expected_dark_frames = std::max(0, config_.dark_frames);

    std::string acquisition_date_utc = MakeUtcDate();
    std::string light_start_time_utc;
    std::string light_stop_time_utc;
    std::string dark_start_time_utc;
    std::string dark_stop_time_utc;
    const auto capture_started_at = std::chrono::steady_clock::now();
    auto light_phase_started_at = capture_started_at;
    auto dark_phase_started_at = capture_started_at;
    auto last_light_progress_at = capture_started_at - std::chrono::seconds(1);
    auto last_dark_progress_at = capture_started_at - std::chrono::seconds(1);

    auto emit_progress = [&](CaptureProgressType type,
                             CapturePhase phase,
                             const std::chrono::steady_clock::time_point& phase_started_at) {
        if (!progress_sink_) {
            return;
        }

        CaptureProgressEvent event;
        event.type = type;
        event.sample_name = job.sample_name;
        event.phase = phase;
        event.light_frames_captured = local_summary.light_buffers;
        event.dark_frames_captured = local_summary.dark_buffers;
        event.dark_frames_target = config_.dark_frames;
        event.frame_size_bytes = snapshot.frame_size_bytes;
        event.capture_elapsed_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - capture_started_at).count();
        event.phase_elapsed_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - phase_started_at).count();
        event.capture_target_seconds = static_cast<double>(config_.capture_seconds);
        event.estimated_frame_rate_hz = effective_frame_rate_hz;
        event.success = local_summary.pass;
        event.sdk_error = local_summary.sdk_error;
        event.message = local_summary.message;
        progress_sink_(event);
    };

    if (has_work_sink) {
        WorkItem begin;
        begin.type = WorkItemType::BeginJob;
        begin.job_id = job_id;
        begin.begin.sample_name = job.sample_name;
        begin.begin.camera_name = config_.camera_name;
        begin.begin.output_dir = config_.output_dir;
        begin.begin.timestamp_tag = MakeFolderTimestampTag();
        begin.begin.expected_light_frames = expected_light_frames;
        begin.begin.expected_dark_frames = expected_dark_frames;
        begin.begin.rgb_wavelength_nm[0] = config_.rgb_wavelength_nm[0];
        begin.begin.rgb_wavelength_nm[1] = config_.rgb_wavelength_nm[1];
        begin.begin.rgb_wavelength_nm[2] = config_.rgb_wavelength_nm[2];
        begin.begin.sensor = snapshot;
        begin.begin.acquisition_date_utc = acquisition_date_utc;
        begin.begin.light_start_time_utc = MakeUtcTime();

        if (!EmitWorkItem(std::move(begin), &local_summary)) {
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

    const std::size_t chunk_target_frames =
        DetermineChunkFrameTarget(snapshot.frame_size_bytes, config_.save_block_frames);
    const std::size_t chunk_target_bytes =
        static_cast<std::size_t>(snapshot.frame_size_bytes) * chunk_target_frames;
    {
        std::ostringstream oss;
        oss << "Work chunk target frames=" << chunk_target_frames
            << " bytes=" << chunk_target_bytes
            << " configured_frames=" << config_.save_block_frames;
        LogInfo(oss.str());
    }
    std::vector<std::uint8_t> chunk_bytes;
    chunk_bytes.reserve(chunk_target_bytes);
    std::int64_t chunk_frame_count = 0;
    std::int64_t chunk_first_frame = 0;
    std::int64_t chunk_last_frame = 0;

    auto make_phase_fps_logger = [&](const char* phase_name) {
        const auto phase_started_at = std::chrono::steady_clock::now();
        auto last_report_at = phase_started_at;
        std::int64_t last_report_frames = 0;

        return [&, phase_name, phase_started_at, last_report_at, last_report_frames]
               (std::int64_t total_frames, bool force) mutable {
            if (total_frames <= 0) {
                return;
            }

            const auto now = std::chrono::steady_clock::now();
            const auto elapsed_since_report = now - last_report_at;
            if (!force && elapsed_since_report < std::chrono::seconds(1)) {
                return;
            }

            const auto interval_frames = total_frames - last_report_frames;
            const double interval_seconds =
                std::chrono::duration<double>(elapsed_since_report).count();
            if (interval_frames <= 0 || interval_seconds <= 0.0) {
                return;
            }

            const double local_fps =
                static_cast<double>(interval_frames) / interval_seconds;
            const double average_seconds =
                std::chrono::duration<double>(now - phase_started_at).count();
            const double average_fps =
                average_seconds > 0.0
                    ? (static_cast<double>(total_frames) / average_seconds)
                    : 0.0;

            std::ostringstream oss;
            oss << phase_name
                << " fps frames=" << total_frames
                << " local_fps=" << std::fixed << std::setprecision(2) << local_fps
                << " avg_fps=" << std::fixed << std::setprecision(2) << average_fps;

            double sdk_fps = 0.0;
            if (api_->GetFloat(L"Acquisition.CalculatedFrameRate", &sdk_fps) == 0 &&
                sdk_fps > 0.0) {
                oss << " sdk_fps=" << std::fixed << std::setprecision(2) << sdk_fps;
            }

            LogInfo(oss.str());
            last_report_at = now;
            last_report_frames = total_frames;
        };
    };

    auto flush_chunk = [&](WorkItemType type) -> bool {
        if (chunk_frame_count <= 0) {
            return true;
        }

        if (!has_work_sink) {
            chunk_bytes.clear();
            chunk_bytes.reserve(chunk_target_bytes);
            chunk_frame_count = 0;
            chunk_first_frame = 0;
            chunk_last_frame = 0;
            return true;
        }

        WorkItem chunk_event;
        chunk_event.type = type;
        chunk_event.job_id = job_id;
        chunk_event.chunk.frame_count = chunk_frame_count;
        chunk_event.chunk.first_frame_number = chunk_first_frame;
        chunk_event.chunk.last_frame_number = chunk_last_frame;
        chunk_event.chunk.bytes =
            std::make_shared<const std::vector<std::uint8_t>>(std::move(chunk_bytes));

        if (!EmitWorkItem(std::move(chunk_event), &local_summary)) {
            return false;
        }

        chunk_bytes = std::vector<std::uint8_t>();
        chunk_bytes.reserve(chunk_target_bytes);
        chunk_frame_count = 0;
        chunk_first_frame = 0;
        chunk_last_frame = 0;
        return true;
    };

    bool acquisition_active = false;

    emit_progress(CaptureProgressType::CaptureStarted, CapturePhase::Light, capture_started_at);
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
        auto log_light_fps = make_phase_fps_logger("LIGHT");
        light_start_time_utc = MakeUtcTime();
        LogInfo("LIGHT phase started: duration_s=" + std::to_string(config_.capture_seconds));
        const auto light_start = std::chrono::steady_clock::now();
        light_phase_started_at = light_start;
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

            if (static_cast<std::size_t>(chunk_frame_count) >= chunk_target_frames) {
                if (!flush_chunk(WorkItemType::LightChunk)) {
                    error = kAppSaveQueueError;
                    set_error(error, "Save queue timeout while pushing LIGHT chunk");
                    break;
                }
            }

            if ((local_summary.light_buffers % 5000) == 0) {
                LogInfo("LIGHT frames=" + std::to_string(local_summary.light_buffers) +
                        " last_frame_number=" + std::to_string(local_summary.last_frame_number));
            }

            log_light_fps(local_summary.light_buffers, false);
            const auto now = std::chrono::steady_clock::now();
            if ((now - last_light_progress_at) >= std::chrono::milliseconds(250)) {
                emit_progress(CaptureProgressType::CaptureProgress, CapturePhase::Light, light_phase_started_at);
                last_light_progress_at = now;
            }
        }
        log_light_fps(local_summary.light_buffers, true);
        emit_progress(CaptureProgressType::CaptureProgress, CapturePhase::Light, light_phase_started_at);
        light_stop_time_utc = MakeUtcTime();
    }

    if (chunk_frame_count > 0 && error != kAppSaveQueueError) {
        if (!flush_chunk(WorkItemType::LightChunk)) {
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
        auto log_dark_fps = make_phase_fps_logger("DARK");
        dark_start_time_utc = MakeUtcTime();
        LogInfo("DARK phase started: target_frames=" + std::to_string(config_.dark_frames));
        dark_phase_started_at = std::chrono::steady_clock::now();
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

            if (static_cast<std::size_t>(chunk_frame_count) >= chunk_target_frames) {
                if (!flush_chunk(WorkItemType::DarkChunk)) {
                    error = kAppSaveQueueError;
                    set_error(error, "Save queue timeout while pushing DARK chunk");
                    break;
                }
            }

            if ((local_summary.dark_buffers % 5000) == 0) {
                LogInfo("DARK frames=" + std::to_string(local_summary.dark_buffers) +
                        " last_frame_number=" + std::to_string(local_summary.last_frame_number));
            }

            log_dark_fps(local_summary.dark_buffers, false);
            const auto now = std::chrono::steady_clock::now();
            if ((now - last_dark_progress_at) >= std::chrono::milliseconds(250)) {
                emit_progress(CaptureProgressType::CaptureProgress, CapturePhase::Dark, dark_phase_started_at);
                last_dark_progress_at = now;
            }
        }
        log_dark_fps(local_summary.dark_buffers, true);
        emit_progress(CaptureProgressType::CaptureProgress, CapturePhase::Dark, dark_phase_started_at);
        dark_stop_time_utc = MakeUtcTime();
    }

    if (chunk_frame_count > 0 && error != kAppSaveQueueError) {
        if (!flush_chunk(WorkItemType::DarkChunk)) {
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

    emit_progress(CaptureProgressType::CaptureFinished,
                  error == 0 ? CapturePhase::Dark : CapturePhase::Light,
                  error == 0 ? dark_phase_started_at : light_phase_started_at);

    if (has_work_sink && begin_sent) {
        WorkItem end;
        end.type = WorkItemType::EndJob;
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

        if (!EmitWorkItem(std::move(end), &local_summary)) {
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
    int trigger_mode_count = 0;
    int error = api_->GetEnumCount(L"Camera.Trigger.Mode", &trigger_mode_count);
    if (error != 0) {
        LogApiFailure("SI_GetEnumCount(Camera.Trigger.Mode)", error);
        return false;
    }

    int internal_trigger_mode_index = -1;
    for (int i = 0; i < trigger_mode_count; ++i) {
        std::wstring trigger_mode;
        error = api_->GetEnumStringByIndex(L"Camera.Trigger.Mode", i, &trigger_mode);
        if (error != 0) {
            LogApiFailure("SI_GetEnumStringByIndex(Camera.Trigger.Mode)", error);
            return false;
        }
        if (WideEqualsAscii(trigger_mode, "Internal")) {
            internal_trigger_mode_index = i;
            break;
        }
    }

    if (internal_trigger_mode_index < 0) {
        LogError("Camera.Trigger.Mode does not expose enum value Internal");
        return false;
    }

    error = api_->SetEnumIndex(L"Camera.Trigger.Mode", internal_trigger_mode_index);
    if (error != 0) {
        LogApiFailure("SI_SetEnumIndex(Camera.Trigger.Mode)", error);
        return false;
    }

    int applied_trigger_mode_index = -1;
    error = api_->GetEnumIndex(L"Camera.Trigger.Mode", &applied_trigger_mode_index);
    if (error != 0) {
        LogApiFailure("SI_GetEnumIndex(Camera.Trigger.Mode)", error);
        return false;
    }
    if (applied_trigger_mode_index != internal_trigger_mode_index) {
        LogError("Camera.Trigger.Mode readback mismatch. requested=Internal applied_index=" +
                 std::to_string(applied_trigger_mode_index));
        return false;
    }
    LogInfo("Trigger.Mode requested=Internal applied=Internal");

    const int spatial_idx = BinningValueToEnumIndex(config_.binning_spatial);
    const int spectral_idx = BinningValueToEnumIndex(config_.binning_spectral);
    if (spatial_idx < 0 || spectral_idx < 0) {
        LogError("Invalid binning values in config");
        return false;
    }

    LogReadoutTimeIfAvailable(api_, [this](const std::string& message) {
        LogInfo("Before binning: " + message);
    });

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
    LogReadoutTimeIfAvailable(api_, [this](const std::string& message) {
        LogInfo("After binning: " + message);
    });

    error = api_->SetBool(L"Camera.ExposureTime.Auto", false);
    if (error != 0) {
        LogApiFailure("SI_SetBool(Camera.ExposureTime.Auto)", error);
        return false;
    }
    LogInfo("ExposureTime.Auto requested=false applied=false");

    error = api_->SetFloat(L"Camera.ExposureTime", config_.exposure_ms);
    if (error != 0) {
        LogApiFailure("SI_SetFloat(Camera.ExposureTime)", error);
        return false;
    }

    error = api_->SetFloat(L"Camera.FrameRate", config_.frame_rate_hz);
    if (error != 0) {
        LogApiFailure("SI_SetFloat(Camera.FrameRate)", error);
        return false;
    }

    LogReadoutTimeIfAvailable(api_, [this](const std::string& message) {
        LogInfo("After timing apply: " + message);
    });

    double applied_exposure_ms = 0.0;
    error = api_->GetFloat(L"Camera.ExposureTime", &applied_exposure_ms);
    if (error != 0) {
        LogApiFailure("SI_GetFloat(Camera.ExposureTime)", error);
        return false;
    }

    std::ostringstream exposure_log;
    exposure_log << "ExposureTime requested=" << std::fixed << std::setprecision(6)
                 << config_.exposure_ms << " ms"
                 << " applied=" << applied_exposure_ms << " ms"
                 << " delta=" << (applied_exposure_ms - config_.exposure_ms) << " ms";
    LogInfo(exposure_log.str());
    if (!NearlyEqual(applied_exposure_ms, config_.exposure_ms)) {
        LogInfo("ExposureTime readback differs from requested value; continuing with applied value");
    }

    double applied_frame_rate_hz = 0.0;
    error = api_->GetFloat(L"Camera.FrameRate", &applied_frame_rate_hz);
    if (error != 0) {
        LogApiFailure("SI_GetFloat(Camera.FrameRate)", error);
        return false;
    }

    std::ostringstream frame_rate_log;
    frame_rate_log << "FrameRate requested=" << std::fixed << std::setprecision(6)
                   << config_.frame_rate_hz << " Hz"
                   << " applied=" << applied_frame_rate_hz << " Hz"
                   << " delta=" << (applied_frame_rate_hz - config_.frame_rate_hz) << " Hz";
    LogInfo(frame_rate_log.str());
    if (!NearlyEqual(applied_frame_rate_hz, config_.frame_rate_hz)) {
        LogInfo("FrameRate readback differs from requested value; continuing with applied value");
    }
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

bool CaptureCore::EmitWorkItem(WorkItem item, AcquisitionSummary* summary) {
    if (!work_sink_) {
        return true;
    }

    const WorkItemType item_type = item.type;
    const std::uint64_t item_job_id = item.job_id;
    if (work_sink_(std::move(item))) {
        return true;
    }

    if (summary != nullptr && summary->sdk_error == 0) {
        summary->sdk_error = kAppSaveQueueError;
        summary->message = "Save queue full or timed out";
    }

    LogError("Failed to enqueue save event. type=" +
             std::to_string(static_cast<int>(item_type)) +
             " job_id=" + std::to_string(item_job_id));
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
    if (!out.wavelengths_nm.empty() &&
        out.wavelengths_nm.size() != static_cast<std::size_t>(out.image_height)) {
        const std::size_t original_count = out.wavelengths_nm.size();
        out.wavelengths_nm = RebinSpectralSeries(out.wavelengths_nm,
                                                 static_cast<std::size_t>(out.image_height));
        LogInfo("Normalized wavelength table from " + std::to_string(original_count) +
                " to " + std::to_string(out.wavelengths_nm.size()) +
                " entries to match effective band count");
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
    if (!out.fwhm_nm.empty() &&
        out.fwhm_nm.size() != static_cast<std::size_t>(out.image_height)) {
        const std::size_t original_count = out.fwhm_nm.size();
        out.fwhm_nm = RebinSpectralSeries(out.fwhm_nm,
                                          static_cast<std::size_t>(out.image_height));
        LogInfo("Normalized FWHM table from " + std::to_string(original_count) +
                " to " + std::to_string(out.fwhm_nm.size()) +
                " entries to match effective band count");
    }

    *snapshot = out;
    return true;
}

void CaptureCore::LogApiFailure(const char* step, int code) {
    std::ostringstream oss;
    oss << step << " failed with raw_code=" << code
        << " hex=0x" << std::uppercase << std::hex
        << static_cast<unsigned int>(static_cast<std::uint32_t>(code))
        << std::dec
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
