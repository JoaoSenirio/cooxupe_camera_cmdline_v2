#include <iostream>

#include "capture_core.h"
#include "fake_specsensor_api.h"
#include "test_support.h"

namespace {

int TestInitializeLogsRawSdkErrorCodeOnFrameRateFailure() {
    AppConfig config = MakeTestConfig("specsensor_cli_test_raw_sdk_error");
    RemoveLogFileIfExists(config.log_file_path);

    FakeSpecSensorApi api;
    api.set_frame_rate_error = -4321;
    CaptureCore core(config, &api);

    TEST_ASSERT(!core.Initialize(), "initialize must fail when frame rate set fails");
    core.Shutdown();

    const std::string log_contents = ReadTextFile(ResolveLogFilePath(config.log_file_path));
    TEST_ASSERT(StringContains(log_contents, "SI_SetFloat(Camera.FrameRate) failed with raw_code=-4321"),
                "log must contain raw SDK error code");
    TEST_ASSERT(StringContains(log_contents, "hex=0xFFFFEF1F"),
                "log must contain hexadecimal SDK error code");
    return 0;
}

int TestWorkflowLogsReadbackWarningsAndPhaseFps() {
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

    const std::string log_contents = ReadTextFile(ResolveLogFilePath(config.log_file_path));
    TEST_ASSERT(StringContains(log_contents, "ExposureTime readback differs from requested value"),
                "log must contain exposure readback warning");
    TEST_ASSERT(StringContains(log_contents, "FrameRate readback differs from requested value"),
                "log must contain frame rate readback warning");
    TEST_ASSERT(StringContains(log_contents, "LIGHT fps frames="),
                "log must contain LIGHT fps line");
    TEST_ASSERT(StringContains(log_contents, "DARK fps frames="),
                "log must contain DARK fps line");
    TEST_ASSERT(StringContains(log_contents, "sdk_fps=198.50"),
                "log must contain SDK FPS when available");
    TEST_ASSERT(StringContains(log_contents, "Capture workflow finished. sample=sample-A"),
                "log must contain workflow summary");
    return 0;
}

}  // namespace

int main() {
    return RunSuite("test_capture_logging", {
        {"InitializeLogsRawSdkErrorCodeOnFrameRateFailure",
         TestInitializeLogsRawSdkErrorCodeOnFrameRateFailure},
        {"WorkflowLogsReadbackWarningsAndPhaseFps",
         TestWorkflowLogsReadbackWarningsAndPhaseFps},
    });
}
