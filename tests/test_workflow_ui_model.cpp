#include "test_support.h"
#include "workflow_ui_model.h"

namespace {

int TestWorkflowProgressAndAutohide() {
    WorkflowUiModel model;

    CaptureProgressEvent start;
    start.type = CaptureProgressType::CaptureStarted;
    start.sample_name = "sample-01";
    start.capture_target_seconds = 10.0;
    start.dark_frames_target = 5;
    start.frame_size_bytes = 1024;
    start.estimated_frame_rate_hz = 100.0;

    UiEvent ui = model.OnCaptureStarted(start);
    TEST_ASSERT(ui.type == UiEventType::WorkflowUpdate, "capture start should produce workflow update");
    TEST_ASSERT(ui.title == "Iniciando captura", "capture start title mismatch");
    TEST_ASSERT(ui.progress_percent == 5, "capture start progress should begin at 5%");
    TEST_ASSERT(ui.eta_seconds >= 0, "capture start eta must be non-negative");

    CaptureProgressEvent light = start;
    light.type = CaptureProgressType::CaptureProgress;
    light.phase = CapturePhase::Light;
    light.phase_elapsed_seconds = 5.0;
    light.capture_elapsed_seconds = 5.0;
    light.light_frames_captured = 500;
    ui = model.OnCaptureProgress(light);
    TEST_ASSERT(ui.title == "Capturando", "light progress should show capturing title");
    TEST_ASSERT(ui.progress_percent > 5, "light progress should advance progress bar");

    CaptureProgressEvent dark = start;
    dark.type = CaptureProgressType::CaptureProgress;
    dark.phase = CapturePhase::Dark;
    dark.dark_frames_captured = 3;
    dark.phase_elapsed_seconds = 0.03;
    dark.capture_elapsed_seconds = 10.03;
    ui = model.OnCaptureProgress(dark);
    TEST_ASSERT(ui.progress_percent >= 75, "dark progress should be mapped to final capture band");

    CaptureProgressEvent finished = dark;
    finished.type = CaptureProgressType::CaptureFinished;
    finished.success = true;
    ui = model.OnCaptureFinished(finished);
    TEST_ASSERT(ui.title == "Captura finalizada", "capture finish title mismatch");
    TEST_ASSERT(ui.progress_percent == 85, "capture finish should reserve 85% for save stage");

    SaveProgressEvent save_started;
    save_started.type = SaveProgressType::JobStarted;
    save_started.sample_name = "sample-01";
    save_started.total_bytes = 10000;
    ui = model.OnSaveProgress(save_started);
    TEST_ASSERT(ui.title == "Salvando", "save start should show saving title");
    TEST_ASSERT(ui.progress_percent >= 85, "save start must not regress progress");

    SaveProgressEvent save_progress = save_started;
    save_progress.type = SaveProgressType::BytesWritten;
    save_progress.bytes_written = 5000;
    save_progress.bytes_per_second = 1000.0;
    ui = model.OnSaveProgress(save_progress);
    TEST_ASSERT(ui.progress_percent >= 85, "save progress must stay in save band");
    TEST_ASSERT(ui.eta_seconds >= 0, "save eta must be non-negative");

    SaveProgressEvent save_finished = save_progress;
    save_finished.type = SaveProgressType::JobFinished;
    save_finished.bytes_written = 10000;
    save_finished.total_bytes = 10000;
    save_finished.success = true;
    ui = model.OnSaveProgress(save_finished);
    TEST_ASSERT(ui.title == "Concluído", "save finish should show completion title");
    TEST_ASSERT(ui.progress_percent == 100, "successful workflow should end at 100%");
    TEST_ASSERT(ui.auto_hide_delay_ms == 5000,
                "successful workflow should auto-hide exactly 5 seconds after save completion");
    return 0;
}

int TestFatalErrorEvent() {
    WorkflowUiModel model;
    UiEvent ui = model.MakeFatalError("falha simulada");
    TEST_ASSERT(ui.type == UiEventType::Error, "fatal error should emit error ui event");
    TEST_ASSERT(ui.title == "Erro", "fatal error title mismatch");
    TEST_ASSERT(ui.auto_hide_delay_ms == 0, "fatal error popup must not auto-hide");
    TEST_ASSERT(ui.detail == "falha simulada", "fatal error detail mismatch");
    return 0;
}

}  // namespace

int main() {
    return RunSuite("test_workflow_ui_model",
                    {
                        {"workflow_progress_and_autohide", &TestWorkflowProgressAndAutohide},
                        {"fatal_error_event", &TestFatalErrorEvent},
                    });
}
