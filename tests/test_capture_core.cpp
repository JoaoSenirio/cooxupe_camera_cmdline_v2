#include <iostream>
#include <string>

#include "app_config.h"
#include "capture_core.h"
#include "fake_specsensor_api.h"

#define TEST_ASSERT(cond, msg)                                      \
    do {                                                            \
        if (!(cond)) {                                              \
            std::cerr << "[FAIL] " << msg << "\n";               \
            return 1;                                               \
        }                                                           \
    } while (0)

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

int TestWorkflowWithFakeApi() {
    AppConfig config = MakeDefaultConfig();
    config.device_index = 0;
    config.capture_seconds = 1;
    config.dark_frames = 5;
    config.min_buffers_required = 1;
    config.log_file_path = "/tmp/specsensor_cli_test.log";

    FakeSpecSensorApi api;
    api.wait_delay_us = 1000;
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
    TEST_ASSERT(api.exposure_time == config.exposure_ms, "exposure must be configured");
    TEST_ASSERT(api.frame_rate == config.frame_rate_hz, "frame rate must be configured");
    TEST_ASSERT(api.spatial_binning_index == 0, "spatial binning index must be set");
    TEST_ASSERT(api.spectral_binning_index == 0, "spectral binning index must be set");

    TEST_ASSERT(api.HasCommand(L"Acquisition.Start"), "must start acquisition");
    TEST_ASSERT(api.HasCommand(L"Acquisition.RingBuffer.Sync"), "must sync ring buffer");
    TEST_ASSERT(api.HasCommand(L"Camera.OpenShutter"), "must open shutter");
    TEST_ASSERT(api.HasCommand(L"Camera.CloseShutter"), "must close shutter");
    TEST_ASSERT(api.HasCommand(L"Acquisition.Stop"), "must stop acquisition");

    TEST_ASSERT(captured_summary.dark_buffers == config.dark_frames,
                "dark buffer count must match configured dark_frames");
    TEST_ASSERT(captured_summary.light_buffers > 0, "must capture at least one light frame");
    TEST_ASSERT(captured_summary.total_buffers >= captured_summary.dark_buffers,
                "total buffers must be >= dark buffers");
    TEST_ASSERT(captured_summary.pass, "summary should pass with min_buffers_required=1");

    return 0;
}

int main() {
    if (TestValidateConfig() != 0) {
        return 1;
    }
    if (TestBinningMap() != 0) {
        return 1;
    }
    if (TestWorkflowWithFakeApi() != 0) {
        return 1;
    }

    std::cout << "[PASS] All tests passed\n";
    return 0;
}
