#include <iostream>

#include "capture_core.h"
#include "fake_specsensor_api.h"
#include "test_support.h"

namespace {

int TestInitializeConfiguresTriggerAndBinningBeforeTiming() {
    AppConfig config = MakeTestConfig("specsensor_cli_test_trigger_order");
    RemoveLogFileIfExists(config.log_file_path);

    FakeSpecSensorApi api;
    CaptureCore core(config, &api);

    TEST_ASSERT(core.Initialize(), "CaptureCore initialize should succeed");
    core.Shutdown();

    TEST_ASSERT(api.trigger_mode_index == 0, "trigger mode must be set to Internal");
    TEST_ASSERT(api.exposure_time_auto == false, "manual exposure mode must be enforced");

    const int trigger_idx = api.IndexOfOperation(L"SetEnumIndex:Camera.Trigger.Mode");
    const int spatial_binning_idx = api.IndexOfOperation(L"SetEnumIndex:Camera.Binning.Spatial");
    const int spectral_binning_idx = api.IndexOfOperation(L"SetEnumIndex:Camera.Binning.Spectral");
    const int exposure_auto_idx = api.IndexOfOperation(L"SetBool:Camera.ExposureTime.Auto");
    const int exposure_idx = api.IndexOfOperation(L"SetFloat:Camera.ExposureTime");
    const int frame_rate_idx = api.IndexOfOperation(L"SetFloat:Camera.FrameRate");

    TEST_ASSERT(trigger_idx >= 0, "trigger mode operation must be recorded");
    TEST_ASSERT(spatial_binning_idx >= 0, "spatial binning operation must be recorded");
    TEST_ASSERT(spectral_binning_idx >= 0, "spectral binning operation must be recorded");
    TEST_ASSERT(exposure_auto_idx >= 0, "exposure auto operation must be recorded");
    TEST_ASSERT(exposure_idx >= 0, "exposure operation must be recorded");
    TEST_ASSERT(frame_rate_idx >= 0, "frame rate operation must be recorded");
    TEST_ASSERT(trigger_idx < spatial_binning_idx,
                "trigger mode must be configured before spatial binning");
    TEST_ASSERT(trigger_idx < spectral_binning_idx,
                "trigger mode must be configured before spectral binning");
    TEST_ASSERT(spatial_binning_idx < exposure_auto_idx,
                "spatial binning must be configured before exposure auto");
    TEST_ASSERT(spectral_binning_idx < exposure_auto_idx,
                "spectral binning must be configured before exposure auto");
    TEST_ASSERT(trigger_idx < exposure_auto_idx,
                "trigger mode must be configured before exposure auto");
    TEST_ASSERT(exposure_auto_idx < exposure_idx, "exposure auto must be configured before exposure");
    TEST_ASSERT(exposure_auto_idx < frame_rate_idx,
                "exposure auto must be configured before frame rate");

    const std::string log_contents = ReadTextFile(ResolveLogFilePath(config.log_file_path));
    TEST_ASSERT(StringContains(log_contents, "Before binning: Camera.Image.ReadoutTime=6.137000 ms"),
                "log must contain readout time before binning");
    TEST_ASSERT(StringContains(log_contents, "After binning: Camera.Image.ReadoutTime=6.137000 ms"),
                "log must contain readout time after binning");
    TEST_ASSERT(StringContains(log_contents, "After timing apply: Camera.Image.ReadoutTime=6.137000 ms"),
                "log must contain readout time after timing apply");
    return 0;
}

int TestInitializeFailsWhenInternalTriggerModeIsMissing() {
    AppConfig config = MakeTestConfig("specsensor_cli_test_trigger_missing");
    RemoveLogFileIfExists(config.log_file_path);

    FakeSpecSensorApi api;
    api.trigger_modes = {L"External"};
    CaptureCore core(config, &api);

    TEST_ASSERT(!core.Initialize(), "initialize must fail without Internal trigger mode");
    core.Shutdown();
    return 0;
}

int TestInitializeFailsWhenTriggerReadbackMismatches() {
    AppConfig config = MakeTestConfig("specsensor_cli_test_trigger_readback");
    RemoveLogFileIfExists(config.log_file_path);

    FakeSpecSensorApi api;
    api.override_trigger_mode_readback = true;
    api.trigger_mode_readback_index = 1;
    CaptureCore core(config, &api);

    TEST_ASSERT(!core.Initialize(), "initialize must fail when trigger readback mismatches");
    core.Shutdown();
    return 0;
}

int TestInitializeDefersExposureFrameRateCompatibilityToSdk() {
    AppConfig config = MakeTestConfig("specsensor_cli_test_incompatible_timing");
    config.exposure_ms = 10.0;
    config.frame_rate_hz = 200.0;
    RemoveLogFileIfExists(config.log_file_path);

    FakeSpecSensorApi api;
    CaptureCore core(config, &api);

    TEST_ASSERT(core.Initialize(),
                "initialize should not reject exposure/frame rate locally");
    core.Shutdown();
    return 0;
}

}  // namespace

int main() {
    return RunSuite("test_capture_initialize", {
        {"InitializeConfiguresTriggerAndBinningBeforeTiming",
         TestInitializeConfiguresTriggerAndBinningBeforeTiming},
        {"InitializeFailsWhenInternalTriggerModeIsMissing",
         TestInitializeFailsWhenInternalTriggerModeIsMissing},
        {"InitializeFailsWhenTriggerReadbackMismatches",
         TestInitializeFailsWhenTriggerReadbackMismatches},
        {"InitializeDefersExposureFrameRateCompatibilityToSdk",
         TestInitializeDefersExposureFrameRateCompatibilityToSdk},
    });
}
