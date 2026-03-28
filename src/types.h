#ifndef TYPES_H
#define TYPES_H

#include <cstdint>
#include <string>
#include <vector>

struct AcquisitionJob {
    std::string sample_name;
};

enum class CapturePhase {
    Light,
    Dark
};

struct AcquisitionSummary {
    std::string sample_name;
    std::int64_t light_buffers = 0;
    std::int64_t dark_buffers = 0;
    std::int64_t total_buffers = 0;
    std::int64_t light_drop_incidents = 0;
    std::int64_t dark_drop_incidents = 0;
    std::int64_t light_dropped_frames = 0;
    std::int64_t dark_dropped_frames = 0;
    std::int64_t last_frame_number = 0;
    bool pass = false;
    int sdk_error = 0;
    std::string message;
};

struct SensorSnapshot {
    std::int64_t image_width = 0;
    std::int64_t image_height = 0;
    std::int64_t frame_size_bytes = 0;
    std::int64_t byte_depth = 0;
    double frame_rate_hz = 0.0;
    double exposure_ms = 0.0;
    int binning_spatial = 1;
    int binning_spectral = 1;
    std::string sensor_id;
    std::string calibration_pack;
    std::int64_t acquisitionwindow_left = 0;
    std::int64_t acquisitionwindow_top = 0;
    bool has_vnir_temperature = false;
    double vnir_temperature = 0.0;
    std::vector<double> wavelengths_nm;
    std::vector<double> fwhm_nm;
};

enum class SaveEventType {
    BeginJob,
    LightChunk,
    DarkChunk,
    EndJob
};

struct SaveEventBegin {
    std::string sample_name;
    std::string camera_name;
    std::string output_dir;
    std::string timestamp_tag;
    std::int64_t expected_light_frames = 0;
    std::int64_t expected_dark_frames = 0;
    int rgb_wavelength_nm[3] = {650, 550, 450};
    SensorSnapshot sensor;
    std::string acquisition_date_utc;
    std::string light_start_time_utc;
};

struct SaveEventChunk {
    std::vector<std::uint8_t> bytes;
    std::int64_t frame_count = 0;
    std::int64_t first_frame_number = 0;
    std::int64_t last_frame_number = 0;
};

struct SaveEventEnd {
    bool success = false;
    int sdk_error = 0;
    std::string message;
    std::int64_t light_frames = 0;
    std::int64_t dark_frames = 0;
    std::int64_t light_drop_incidents = 0;
    std::int64_t dark_drop_incidents = 0;
    std::int64_t light_dropped_frames = 0;
    std::int64_t dark_dropped_frames = 0;
    std::string acquisition_date_utc;
    std::string light_start_time_utc;
    std::string light_stop_time_utc;
    std::string dark_start_time_utc;
    std::string dark_stop_time_utc;
};

struct SaveEvent {
    SaveEventType type = SaveEventType::BeginJob;
    std::uint64_t job_id = 0;
    SaveEventBegin begin;
    SaveEventChunk chunk;
    SaveEventEnd end;
};

enum class CaptureProgressType {
    CaptureStarted,
    CaptureProgress,
    CaptureFinished
};

struct CaptureProgressEvent {
    CaptureProgressType type = CaptureProgressType::CaptureStarted;
    std::string sample_name;
    CapturePhase phase = CapturePhase::Light;
    std::int64_t light_frames_captured = 0;
    std::int64_t dark_frames_captured = 0;
    std::int64_t dark_frames_target = 0;
    std::int64_t frame_size_bytes = 0;
    double capture_elapsed_seconds = 0.0;
    double phase_elapsed_seconds = 0.0;
    double capture_target_seconds = 0.0;
    double estimated_frame_rate_hz = 0.0;
    bool success = false;
    int sdk_error = 0;
    std::string message;
};

enum class SaveProgressType {
    JobStarted,
    BytesWritten,
    JobFinished
};

struct SaveProgressEvent {
    SaveProgressType type = SaveProgressType::JobStarted;
    std::uint64_t job_id = 0;
    std::string sample_name;
    std::uint64_t bytes_written = 0;
    std::uint64_t total_bytes = 0;
    double bytes_per_second = 0.0;
    bool success = false;
    std::string message;
};

enum class UiEventType {
    Hide,
    WorkflowUpdate,
    Error,
    Shutdown
};

enum class UiWorkflowStage {
    None,
    StartupConnecting,
    CaptureStarting,
    Capturing,
    CaptureFinished,
    Saving,
    Completed,
    Error
};

struct UiEvent {
    UiEventType type = UiEventType::WorkflowUpdate;
    UiWorkflowStage stage = UiWorkflowStage::None;
    std::string title;
    std::string detail;
    int progress_percent = 0;
    int eta_seconds = -1;
    int auto_hide_delay_ms = 0;
};

enum class UiCommandType {
    ExitRequested
};

struct UiCommand {
    UiCommandType type = UiCommandType::ExitRequested;
};

enum class RuntimeState {
    BootstrapInteractive,
    ReadyBackground,
    Busy,
    FatalError,
    Shutdown
};

#endif
