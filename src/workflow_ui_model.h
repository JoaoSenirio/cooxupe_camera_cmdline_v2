#ifndef WORKFLOW_UI_MODEL_H
#define WORKFLOW_UI_MODEL_H

#include <cstdint>
#include <string>

#include "types.h"

class WorkflowUiModel {
public:
    WorkflowUiModel();

    void Reset();
    UiEvent OnCaptureStarted(const CaptureProgressEvent& event);
    UiEvent OnCaptureProgress(const CaptureProgressEvent& event);
    UiEvent OnCaptureFinished(const CaptureProgressEvent& event);
    UiEvent OnSaveProgress(const SaveProgressEvent& event);
    UiEvent MakeFatalError(const std::string& message);
    UiEvent MakeHideEvent() const;

private:
    UiEvent MakeWorkflowEvent(UiWorkflowStage stage,
                              const std::string& title,
                              const std::string& detail,
                              int progress_percent,
                              int eta_seconds,
                              int auto_hide_delay_ms) const;
    int ClampMonotonicProgress(int progress_percent);
    int EstimateCaptureEtaSeconds(const CaptureProgressEvent& event) const;
    int EstimateSaveEtaSeconds(std::uint64_t bytes_written,
                               std::uint64_t total_bytes,
                               double bytes_per_second) const;
    std::string BuildSampleDetail(const std::string& prefix) const;

    std::string sample_name_;
    int last_progress_percent_ = 0;
    int capture_target_seconds_ = 0;
    std::int64_t dark_frames_target_ = 0;
    std::int64_t frame_size_bytes_ = 0;
    std::int64_t expected_light_frames_ = 0;
    std::int64_t expected_dark_frames_ = 0;
    std::uint64_t expected_save_bytes_ = 0;
    double estimated_frame_rate_hz_ = 0.0;
    double default_save_bytes_per_second_ = 0.0;
    bool capture_finished_ = false;
};

#endif
