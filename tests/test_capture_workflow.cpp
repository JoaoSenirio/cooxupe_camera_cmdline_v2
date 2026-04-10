#include <cmath>
#include <iostream>
#include <vector>

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

int TestWorkflowNormalizesSpectralMetadataToEffectiveBandCount() {
    AppConfig config = MakeTestConfig("specsensor_cli_test_binned_wavelengths");
    RemoveLogFileIfExists(config.log_file_path);

    FakeSpecSensorApi api;
    api.wait_delay_us = 1000;
    api.image_width = 4;
    api.image_height = 2;
    api.frame_size_bytes = 16;
    api.wavelength_table_values = {L"500.0", L"510.0", L"520.0", L"530.0"};
    api.fwhm_values = {L"10.0", L"12.0", L"14.0", L"16.0"};

    CaptureCore core(config, &api);
    TEST_ASSERT(core.Initialize(), "CaptureCore initialize should succeed");

    bool saw_begin = false;
    WorkItem begin_item;
    core.set_work_sink([&](WorkItem item) {
        if (item.type == WorkItemType::BeginJob) {
            begin_item = item;
            saw_begin = true;
        }
        return true;
    });

    AcquisitionJob job;
    job.sample_name = "sample-binned";
    AcquisitionSummary captured_summary;
    TEST_ASSERT(core.CaptureSample(job, &captured_summary), "capture sample should succeed");
    core.Shutdown();

    TEST_ASSERT(saw_begin, "work sink must receive BeginJob");
    TEST_ASSERT(begin_item.begin.sensor.image_height == 2, "effective image_height must be preserved");
    TEST_ASSERT(begin_item.begin.sensor.wavelengths_nm.size() == 2,
                "wavelength table must match effective band count");
    TEST_ASSERT(begin_item.begin.sensor.fwhm_nm.size() == 2,
                "FWHM table must match effective band count");
    TEST_ASSERT(std::fabs(begin_item.begin.sensor.wavelengths_nm[0] - 505.0) < 1e-9,
                "first rebinned wavelength must be averaged");
    TEST_ASSERT(std::fabs(begin_item.begin.sensor.wavelengths_nm[1] - 525.0) < 1e-9,
                "second rebinned wavelength must be averaged");
    TEST_ASSERT(std::fabs(begin_item.begin.sensor.fwhm_nm[0] - 11.0) < 1e-9,
                "first rebinned fwhm must be averaged");
    TEST_ASSERT(std::fabs(begin_item.begin.sensor.fwhm_nm[1] - 15.0) < 1e-9,
                "second rebinned fwhm must be averaged");
    TEST_ASSERT(captured_summary.pass, "summary should still pass");
    return 0;
}

int TestWorkflowUsesConfiguredChunkFrameTarget() {
    AppConfig config = MakeTestConfig("specsensor_cli_test_chunk_target");
    RemoveLogFileIfExists(config.log_file_path);
    config.save_block_frames = 7;
    config.dark_frames = 0;

    FakeSpecSensorApi api;
    api.wait_delay_us = 1000;

    CaptureCore core(config, &api);
    TEST_ASSERT(core.Initialize(), "CaptureCore initialize should succeed");

    std::vector<std::int64_t> light_chunk_sizes;
    core.set_work_sink([&](WorkItem item) {
        if (item.type == WorkItemType::LightChunk) {
            light_chunk_sizes.push_back(item.chunk.frame_count);
        }
        return true;
    });

    AcquisitionJob job;
    job.sample_name = "sample-chunk-target";
    AcquisitionSummary captured_summary;
    TEST_ASSERT(core.CaptureSample(job, &captured_summary), "capture sample should succeed");
    core.Shutdown();

    TEST_ASSERT(!light_chunk_sizes.empty(), "workflow must emit at least one light chunk");
    bool saw_full_chunk = false;
    for (std::size_t i = 0; i < light_chunk_sizes.size(); ++i) {
        TEST_ASSERT(light_chunk_sizes[i] > 0, "chunk frame count must be positive");
        TEST_ASSERT(light_chunk_sizes[i] <= config.save_block_frames,
                    "chunk frame count must respect save_block_frames");
        if (light_chunk_sizes[i] == config.save_block_frames) {
            saw_full_chunk = true;
        }
    }

    TEST_ASSERT(saw_full_chunk, "workflow should emit at least one full-sized configured chunk");
    TEST_ASSERT(captured_summary.pass, "summary should still pass");
    return 0;
}

}  // namespace

int main() {
    return RunSuite("test_capture_workflow", {
        {"WorkflowKeepsLightDarkSequenceAndSummary",
         TestWorkflowKeepsLightDarkSequenceAndSummary},
        {"PhaseFpsLoggingFallsBackWhenSdkMetricIsUnavailable",
         TestPhaseFpsLoggingFallsBackWhenSdkMetricIsUnavailable},
        {"WorkflowNormalizesSpectralMetadataToEffectiveBandCount",
         TestWorkflowNormalizesSpectralMetadataToEffectiveBandCount},
        {"WorkflowUsesConfiguredChunkFrameTarget",
         TestWorkflowUsesConfiguredChunkFrameTarget},
    });
}
