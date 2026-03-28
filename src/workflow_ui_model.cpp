#include "workflow_ui_model.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int kCaptureStartProgress = 5;
constexpr int kCaptureLightEndProgress = 75;
constexpr int kCaptureDoneProgress = 85;
constexpr int kWorkflowDoneProgress = 100;
constexpr int kSuccessAutoHideDelayMs = 5000;
constexpr double kMinimumSaveBytesPerSecond = 8.0 * 1024.0 * 1024.0;

int ClampProgress(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 100) {
        return 100;
    }
    return value;
}

int CeilToIntSeconds(double value) {
    if (value <= 0.0) {
        return 0;
    }
    return static_cast<int>(std::ceil(value));
}

}  // namespace

WorkflowUiModel::WorkflowUiModel() = default;

void WorkflowUiModel::Reset() {
    sample_name_.clear();
    last_progress_percent_ = 0;
    capture_target_seconds_ = 0;
    dark_frames_target_ = 0;
    frame_size_bytes_ = 0;
    expected_light_frames_ = 0;
    expected_dark_frames_ = 0;
    expected_save_bytes_ = 0;
    estimated_frame_rate_hz_ = 0.0;
    default_save_bytes_per_second_ = 0.0;
    capture_finished_ = false;
}

UiEvent WorkflowUiModel::OnCaptureStarted(const CaptureProgressEvent& event) {
    Reset();

    sample_name_ = event.sample_name;
    capture_target_seconds_ = static_cast<int>(std::max(0.0, event.capture_target_seconds));
    dark_frames_target_ = std::max<std::int64_t>(0, event.dark_frames_target);
    frame_size_bytes_ = std::max<std::int64_t>(0, event.frame_size_bytes);
    estimated_frame_rate_hz_ = event.estimated_frame_rate_hz > 0.0 ? event.estimated_frame_rate_hz : 1.0;

    expected_light_frames_ = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(std::llround(event.capture_target_seconds * estimated_frame_rate_hz_)));
    expected_dark_frames_ = dark_frames_target_;
    expected_save_bytes_ = static_cast<std::uint64_t>(
        (expected_light_frames_ + expected_dark_frames_) * std::max<std::int64_t>(0, frame_size_bytes_));
    default_save_bytes_per_second_ =
        std::max(kMinimumSaveBytesPerSecond, estimated_frame_rate_hz_ * static_cast<double>(frame_size_bytes_));

    return MakeWorkflowEvent(UiWorkflowStage::CaptureStarting,
                             "Iniciando captura",
                             BuildSampleDetail("Preparando workflow"),
                             ClampMonotonicProgress(kCaptureStartProgress),
                             EstimateCaptureEtaSeconds(event),
                             0);
}

UiEvent WorkflowUiModel::OnCaptureProgress(const CaptureProgressEvent& event) {
    int progress_percent = kCaptureStartProgress;
    if (event.phase == CapturePhase::Light) {
        const double target = std::max(1.0, event.capture_target_seconds);
        const double ratio = std::max(0.0, std::min(1.0, event.phase_elapsed_seconds / target));
        progress_percent = kCaptureStartProgress +
                           static_cast<int>(std::lround((kCaptureLightEndProgress - kCaptureStartProgress) * ratio));
    } else {
        const double target_frames = std::max<std::int64_t>(1, dark_frames_target_);
        const double ratio = std::max(0.0, std::min(1.0, static_cast<double>(event.dark_frames_captured) /
                                                             static_cast<double>(target_frames)));
        progress_percent = kCaptureLightEndProgress +
                           static_cast<int>(std::lround((kCaptureDoneProgress - kCaptureLightEndProgress) * ratio));
    }

    return MakeWorkflowEvent(UiWorkflowStage::Capturing,
                             "Capturando",
                             BuildSampleDetail("Captura em andamento"),
                             ClampMonotonicProgress(progress_percent),
                             EstimateCaptureEtaSeconds(event),
                             0);
}

UiEvent WorkflowUiModel::OnCaptureFinished(const CaptureProgressEvent& event) {
    capture_finished_ = true;
    return MakeWorkflowEvent(UiWorkflowStage::CaptureFinished,
                             "Captura finalizada",
                             BuildSampleDetail("Aguardando persistencia em disco"),
                             ClampMonotonicProgress(kCaptureDoneProgress),
                             EstimateSaveEtaSeconds(0, expected_save_bytes_, default_save_bytes_per_second_),
                             0);
}

UiEvent WorkflowUiModel::OnSaveProgress(const SaveProgressEvent& event) {
    if (event.type == SaveProgressType::JobFinished && !event.success) {
        return MakeFatalError(event.message.empty() ? "Falha no salvamento em disco" : event.message);
    }

    if (event.type == SaveProgressType::JobFinished) {
        last_progress_percent_ = kWorkflowDoneProgress;
        return MakeWorkflowEvent(UiWorkflowStage::Completed,
                                 "Concluído",
                                 BuildSampleDetail("Workflow finalizado com sucesso"),
                                 kWorkflowDoneProgress,
                                 0,
                                 kSuccessAutoHideDelayMs);
    }

    std::uint64_t total_bytes = event.total_bytes;
    if (total_bytes == 0) {
        total_bytes = expected_save_bytes_;
    }
    if (total_bytes < event.bytes_written) {
        total_bytes = event.bytes_written;
    }

    int progress_percent = kCaptureDoneProgress;
    if (total_bytes > 0) {
        const double ratio = static_cast<double>(event.bytes_written) /
                             static_cast<double>(std::max<std::uint64_t>(1, total_bytes));
        progress_percent = kCaptureDoneProgress +
                           static_cast<int>(std::lround((kWorkflowDoneProgress - kCaptureDoneProgress) *
                                                        std::max(0.0, std::min(1.0, ratio))));
    }

    return MakeWorkflowEvent(UiWorkflowStage::Saving,
                             "Salvando",
                             BuildSampleDetail("Persistindo arquivos em disco"),
                             ClampMonotonicProgress(progress_percent),
                             EstimateSaveEtaSeconds(event.bytes_written, total_bytes, event.bytes_per_second),
                             0);
}

UiEvent WorkflowUiModel::MakeFatalError(const std::string& message) {
    UiEvent event;
    event.type = UiEventType::Error;
    event.stage = UiWorkflowStage::Error;
    event.title = "Erro";
    event.detail = message.empty() ? "Falha durante o workflow da câmera" : message;
    event.progress_percent = last_progress_percent_;
    event.eta_seconds = -1;
    event.auto_hide_delay_ms = 0;
    return event;
}

UiEvent WorkflowUiModel::MakeHideEvent() const {
    UiEvent event;
    event.type = UiEventType::Hide;
    event.stage = UiWorkflowStage::None;
    return event;
}

UiEvent WorkflowUiModel::MakeWorkflowEvent(UiWorkflowStage stage,
                                           const std::string& title,
                                           const std::string& detail,
                                           int progress_percent,
                                           int eta_seconds,
                                           int auto_hide_delay_ms) const {
    UiEvent event;
    event.type = UiEventType::WorkflowUpdate;
    event.stage = stage;
    event.title = title;
    event.detail = detail;
    event.progress_percent = progress_percent;
    event.eta_seconds = eta_seconds;
    event.auto_hide_delay_ms = auto_hide_delay_ms;
    return event;
}

int WorkflowUiModel::ClampMonotonicProgress(int progress_percent) {
    last_progress_percent_ = std::max(last_progress_percent_, ClampProgress(progress_percent));
    return last_progress_percent_;
}

int WorkflowUiModel::EstimateCaptureEtaSeconds(const CaptureProgressEvent& event) const {
    const double remaining_light_seconds =
        event.phase == CapturePhase::Light
            ? std::max(0.0, event.capture_target_seconds - event.phase_elapsed_seconds)
            : 0.0;

    double remaining_dark_seconds = 0.0;
    if (estimated_frame_rate_hz_ > 0.0) {
        const std::int64_t remaining_dark_frames =
            event.phase == CapturePhase::Dark
                ? std::max<std::int64_t>(0, dark_frames_target_ - event.dark_frames_captured)
                : dark_frames_target_;
        remaining_dark_seconds =
            static_cast<double>(remaining_dark_frames) / std::max(estimated_frame_rate_hz_, 1.0);
    }

    const double remaining_save_seconds =
        static_cast<double>(expected_save_bytes_) /
        std::max(default_save_bytes_per_second_, kMinimumSaveBytesPerSecond);

    return CeilToIntSeconds(remaining_light_seconds + remaining_dark_seconds + remaining_save_seconds);
}

int WorkflowUiModel::EstimateSaveEtaSeconds(std::uint64_t bytes_written,
                                            std::uint64_t total_bytes,
                                            double bytes_per_second) const {
    if (total_bytes <= bytes_written) {
        return 0;
    }

    const std::uint64_t remaining_bytes = total_bytes - bytes_written;
    const double effective_bps = bytes_per_second > 0.0
                                     ? bytes_per_second
                                     : std::max(default_save_bytes_per_second_, kMinimumSaveBytesPerSecond);
    return CeilToIntSeconds(static_cast<double>(remaining_bytes) / effective_bps);
}

std::string WorkflowUiModel::BuildSampleDetail(const std::string& prefix) const {
    if (sample_name_.empty()) {
        return prefix;
    }
    return prefix + ": " + sample_name_;
}
