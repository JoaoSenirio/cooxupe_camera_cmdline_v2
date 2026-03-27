#include <iostream>

#include "capture_core.h"
#include "fake_specsensor_api.h"
#include "test_support.h"

namespace {

int TestWorkflowKeepsLightDarkSequenceAndSummary() {
    AppConfig config = MakeTestConfig("specsensor_cli_test_workflow");
    RemoveLogFileIfExists(config.log_file_path);

    FakeSpecSensorApi api;
    api.wait_delay_us = 1000;
    api.override_exposure_readback = true;
    api.exposure_readback_value = config.exposure_ms + 0.5;
    api.override_frame_rate_readback = true;
    api.frame_rate_readback_value = config.frame_rate_hz - 5.0;
    api.calculated_frame_rate_value = 198.5;
    CaptureCore core(config, &api);

    TEST_ASSERT(core.Initialize(), "CaptureCore initialize should succeed");

    AcquisitionJob job;
    job.sample_name = "sample-A";
    AcquisitionSummary captured_summary;
    TEST_ASSERT(core.CaptureSample(job, &captured_summary), "capture sample should succeed");
    core.Shutdown();

    TEST_ASSERT(api.loaded == false, "SDK should be unloaded after stop");
    TEST_ASSERT(api.opened == false, "device should be closed after stop");
    TEST_ASSERT(api.initialized, "Initialize command must be called");
    TEST_ASSERT(api.open_index == config.device_index, "device index must match config");
    TEST_ASSERT(api.exposure_time_auto == false, "manual exposure mode must remain disabled");
    TEST_ASSERT(api.exposure_time == config.exposure_ms, "exposure must be configured");
    TEST_ASSERT(api.frame_rate == config.frame_rate_hz, "frame rate must be configured");
    TEST_ASSERT(api.spatial_binning_index == BinningValueToEnumIndex(config.binning_spatial),
                "spatial binning index must be set");
    TEST_ASSERT(api.spectral_binning_index == BinningValueToEnumIndex(config.binning_spectral),
                "spectral binning index must be set");

    TEST_ASSERT(api.HasCommand(L"Acquisition.Start"), "must start acquisition");
    TEST_ASSERT(api.HasCommand(L"Acquisition.RingBuffer.Sync"), "must sync ring buffer");
    TEST_ASSERT(api.HasCommand(L"Camera.OpenShutter"), "must open shutter");
    TEST_ASSERT(api.HasCommand(L"Camera.CloseShutter"), "must close shutter");
    TEST_ASSERT(api.HasCommand(L"Acquisition.Stop"), "must stop acquisition");
    TEST_ASSERT(api.CountCommand(L"Acquisition.Start") == 2,
                "must restart acquisition before DARK");
    TEST_ASSERT(api.CountCommand(L"Acquisition.Stop") == 2,
                "must stop after LIGHT and after DARK");

    TEST_ASSERT(captured_summary.dark_buffers == config.dark_frames,
                "dark buffer count must match configured dark_frames");
    TEST_ASSERT(captured_summary.light_buffers > 0, "must capture at least one light frame");
    TEST_ASSERT(captured_summary.total_buffers >= captured_summary.dark_buffers,
                "total buffers must be >= dark buffers");
    TEST_ASSERT(captured_summary.pass, "summary should pass with min_buffers_required=1");
    return 0;
}

int TestPhaseFpsLoggingFallsBackWhenSdkMetricIsUnavailable() {
    AppConfig config = MakeTestConfig("specsensor_cli_test_fps_fallback");
    RemoveLogFileIfExists(config.log_file_path);

    FakeSpecSensorApi api;
    api.wait_delay_us = 1000;
    api.calculated_frame_rate_supported = false;
    CaptureCore core(config, &api);

    TEST_ASSERT(core.Initialize(), "initialize should succeed without Acquisition.CalculatedFrameRate");

    AcquisitionJob job;
    job.sample_name = "sample-fallback";
    AcquisitionSummary captured_summary;
    TEST_ASSERT(core.CaptureSample(job, &captured_summary), "capture sample should succeed");
    core.Shutdown();

    const std::string log_contents = ReadTextFile(ResolveLogFilePath(config.log_file_path));
    TEST_ASSERT(StringContains(log_contents, "LIGHT fps frames="),
                "fallback log must still contain LIGHT fps line");
    TEST_ASSERT(StringContains(log_contents, "DARK fps frames="),
                "fallback log must still contain DARK fps line");
    TEST_ASSERT(!StringContains(log_contents, "sdk_fps="),
                "fallback log must omit sdk_fps when feature is unavailable");
    return 0;
}

}  // namespace

int main() {
    return RunSuite("test_capture_workflow", {
        {"WorkflowKeepsLightDarkSequenceAndSummary",
         TestWorkflowKeepsLightDarkSequenceAndSummary},
        {"PhaseFpsLoggingFallsBackWhenSdkMetricIsUnavailable",
         TestPhaseFpsLoggingFallsBackWhenSdkMetricIsUnavailable},
    });
}
