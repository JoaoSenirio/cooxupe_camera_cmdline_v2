#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "app_config.h"
#include "capture_core.h"
#include "fake_specsensor_api.h"

#define TEST_ASSERT(cond, msg)                                      \
    do {                                                            \
        if (!(cond)) {                                              \
            std::cerr << "[FAIL] " << msg << "\n";                  \
            return 1;                                               \
        }                                                           \
    } while (0)

namespace {

std::string MakeDayStamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_local{};
    localtime_r(&now_time, &tm_local);

    std::ostringstream oss;
    oss << std::put_time(&tm_local, "%Y%m%d");
    return oss.str();
}

std::string ResolveLogFilePath(const std::string& configured_path) {
    std::string normalized = configured_path;
    for (char& ch : normalized) {
        if (ch == '/') {
            ch = '\\';
        }
    }

    const std::size_t slash_pos = normalized.find_last_of("\\/");
    const std::string dir = slash_pos == std::string::npos ? "" : normalized.substr(0, slash_pos);
    const std::string file_name =
        slash_pos == std::string::npos ? normalized : normalized.substr(slash_pos + 1);

    const std::size_t dot_pos = file_name.find_last_of('.');
    std::string stem = file_name;
    std::string ext;
    if (dot_pos != std::string::npos && dot_pos != 0) {
        stem = file_name.substr(0, dot_pos);
        ext = file_name.substr(dot_pos);
    }
    if (stem.empty()) {
        stem = "specsensor_cli";
    }
    if (ext.empty()) {
        ext = ".log";
    }

    const std::string dated_name = stem + "_" + MakeDayStamp() + ext;
    if (dir.empty()) {
        return dated_name;
    }
    return dir + "\\" + dated_name;
}

std::string ReadTextFile(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::in);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

bool StringContains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

AppConfig MakeTestConfig(const std::string& log_tag) {
    AppConfig config = MakeDefaultConfig();
    config.device_index = 0;
    config.capture_seconds = 1;
    config.dark_frames = 5;
    config.min_buffers_required = 1;
    config.log_file_path = "/tmp/" + log_tag + ".log";
    return config;
}

void RemoveLogFileIfExists(const std::string& configured_path) {
    const std::string resolved_path = ResolveLogFilePath(configured_path);
    std::remove(resolved_path.c_str());
}

}  // namespace

int TestValidateConfig() {
    AppConfig config = MakeDefaultConfig();
    std::string error;
    TEST_ASSERT(ValidateConfig(config, &error), "default config should be valid");

    config.license_path.clear();
    TEST_ASSERT(!ValidateConfig(config, &error), "empty license_path must be invalid");
    return 0;
}

int TestBinningMap() {
    TEST_ASSERT(BinningValueToEnumIndex(1) == 0, "1x binning maps to 0");
    TEST_ASSERT(BinningValueToEnumIndex(2) == 1, "2x binning maps to 1");
    TEST_ASSERT(BinningValueToEnumIndex(4) == 2, "4x binning maps to 2");
    TEST_ASSERT(BinningValueToEnumIndex(8) == 3, "8x binning maps to 3");
    TEST_ASSERT(BinningValueToEnumIndex(16) == -1, "invalid binning maps to -1");
    return 0;
}

int TestInitializeConfiguresInternalTriggerBeforeFloats() {
    AppConfig config = MakeTestConfig("specsensor_cli_test_trigger_order");
    RemoveLogFileIfExists(config.log_file_path);

    FakeSpecSensorApi api;
    CaptureCore core(config, &api);

    TEST_ASSERT(core.Initialize(), "CaptureCore initialize should succeed");
    core.Shutdown();

    TEST_ASSERT(api.trigger_mode_index == 0, "trigger mode must be set to Internal");
    TEST_ASSERT(api.exposure_time_auto == false, "manual exposure mode must be enforced");

    const int trigger_idx = api.IndexOfOperation(L"SetEnumIndex:Camera.Trigger.Mode");
    const int exposure_auto_idx = api.IndexOfOperation(L"SetBool:Camera.ExposureTime.Auto");
    const int exposure_idx = api.IndexOfOperation(L"SetFloat:Camera.ExposureTime");
    const int frame_rate_idx = api.IndexOfOperation(L"SetFloat:Camera.FrameRate");

    TEST_ASSERT(trigger_idx >= 0, "trigger mode operation must be recorded");
    TEST_ASSERT(exposure_auto_idx >= 0, "exposure auto operation must be recorded");
    TEST_ASSERT(exposure_idx >= 0, "exposure operation must be recorded");
    TEST_ASSERT(frame_rate_idx >= 0, "frame rate operation must be recorded");
    TEST_ASSERT(trigger_idx < exposure_auto_idx,
                "trigger mode must be configured before exposure auto");
    TEST_ASSERT(exposure_auto_idx < exposure_idx, "exposure auto must be configured before exposure");
    TEST_ASSERT(exposure_auto_idx < frame_rate_idx,
                "exposure auto must be configured before frame rate");
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

int TestWorkflowWithReadbackWarningsAndPhaseFpsLogs() {
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

int main() {
    if (TestValidateConfig() != 0) {
        return 1;
    }
    if (TestBinningMap() != 0) {
        return 1;
    }
    if (TestInitializeConfiguresInternalTriggerBeforeFloats() != 0) {
        return 1;
    }
    if (TestInitializeFailsWhenInternalTriggerModeIsMissing() != 0) {
        return 1;
    }
    if (TestInitializeFailsWhenTriggerReadbackMismatches() != 0) {
        return 1;
    }
    if (TestInitializeDefersExposureFrameRateCompatibilityToSdk() != 0) {
        return 1;
    }
    if (TestInitializeLogsRawSdkErrorCodeOnFrameRateFailure() != 0) {
        return 1;
    }
    if (TestWorkflowWithReadbackWarningsAndPhaseFpsLogs() != 0) {
        return 1;
    }
    if (TestPhaseFpsLoggingFallsBackWhenSdkMetricIsUnavailable() != 0) {
        return 1;
    }

    std::cout << "[PASS] All tests passed\n";
    return 0;
}
