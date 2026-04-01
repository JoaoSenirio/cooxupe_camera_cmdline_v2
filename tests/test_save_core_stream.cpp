#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "save_core.h"
#include "test_support.h"

namespace {

struct CapturedStreamEvents {
    std::mutex mutex;
    std::vector<FrameStreamEvent> events;
};

std::uint32_t ReadU32Be(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

SensorSnapshot MakeSensor() {
    SensorSnapshot sensor;
    sensor.image_width = 4;
    sensor.image_height = 3;
    sensor.frame_size_bytes = sensor.image_width * sensor.image_height * 2;
    sensor.byte_depth = 2;
    sensor.frame_rate_hz = 10.0;
    sensor.exposure_ms = 1.0;
    sensor.binning_spatial = 1;
    sensor.binning_spectral = 1;
    sensor.wavelengths_nm = {650.0, 550.0, 450.0};
    return sensor;
}

void AppendU16Le(std::vector<std::uint8_t>* bytes, std::uint16_t value) {
    bytes->push_back(static_cast<std::uint8_t>(value & 0xFF));
    bytes->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

std::vector<std::uint8_t> MakeLightChunkBytes() {
    std::vector<std::uint8_t> bytes;
    const std::uint16_t frame0[][4] = {
        {100, 101, 102, 103},
        {200, 201, 202, 203},
        {300, 301, 302, 303},
    };
    const std::uint16_t frame1[][4] = {
        {110, 111, 112, 113},
        {210, 211, 212, 213},
        {310, 311, 312, 313},
    };

    for (int band = 0; band < 3; ++band) {
        for (int x = 0; x < 4; ++x) {
            AppendU16Le(&bytes, frame0[band][x]);
        }
    }
    for (int band = 0; band < 3; ++band) {
        for (int x = 0; x < 4; ++x) {
            AppendU16Le(&bytes, frame1[band][x]);
        }
    }
    return bytes;
}

SaveEvent MakeBeginEvent(std::uint64_t job_id, const std::string& output_dir) {
    SaveEvent begin;
    begin.type = SaveEventType::BeginJob;
    begin.job_id = job_id;
    begin.begin.sample_name = "sample-rgb-stream";
    begin.begin.camera_name = "FX10";
    begin.begin.output_dir = output_dir;
    begin.begin.timestamp_tag = "2026-04-01_14-30-00";
    begin.begin.expected_light_frames = 2;
    begin.begin.expected_dark_frames = 0;
    begin.begin.rgb_wavelength_nm[0] = 650;
    begin.begin.rgb_wavelength_nm[1] = 550;
    begin.begin.rgb_wavelength_nm[2] = 450;
    begin.begin.sensor = MakeSensor();
    begin.begin.acquisition_date_utc = "2026-04-01";
    begin.begin.light_start_time_utc = "14:30:00";
    return begin;
}

SaveEvent MakeLightChunkEvent(std::uint64_t job_id) {
    SaveEvent chunk;
    chunk.type = SaveEventType::LightChunk;
    chunk.job_id = job_id;
    chunk.chunk.bytes = MakeLightChunkBytes();
    chunk.chunk.frame_count = 2;
    chunk.chunk.first_frame_number = 10;
    chunk.chunk.last_frame_number = 11;
    return chunk;
}

SaveEvent MakeEndEvent(std::uint64_t job_id) {
    SaveEvent end;
    end.type = SaveEventType::EndJob;
    end.job_id = job_id;
    end.end.success = true;
    end.end.light_frames = 2;
    end.end.dark_frames = 0;
    end.end.acquisition_date_utc = "2026-04-01";
    end.end.light_start_time_utc = "14:30:00";
    end.end.light_stop_time_utc = "14:30:01";
    return end;
}

bool WaitForStreamEventCount(CapturedStreamEvents* captured,
                             std::size_t expected_count,
                             int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(captured->mutex);
            if (captured->events.size() >= expected_count) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool WaitForFinishedProgress(std::mutex* mutex,
                             bool* has_finished,
                             SaveProgressEvent* finished_event,
                             int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(*mutex);
            if (*has_finished) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    (void)finished_event;
    return false;
}

std::vector<std::uint8_t> ReadBinaryFileBytes(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
}

int TestSaveCoreEmitsRgbBlockAndWritesFullWidthPng() {
    SaveCore save_core(16, 1000);
    CapturedStreamEvents captured;
    save_core.set_frame_stream_sink([&](const FrameStreamEvent& event) {
        std::lock_guard<std::mutex> lock(captured.mutex);
        captured.events.push_back(event);
        return true;
    });

    TEST_ASSERT(save_core.start(), "SaveCore should start");
    const std::uint64_t job_id = 41;
    TEST_ASSERT(save_core.enqueue_event(MakeBeginEvent(job_id, "save-core-stream-output")),
                "BeginJob should enqueue");
    TEST_ASSERT(save_core.enqueue_event(MakeLightChunkEvent(job_id)),
                "LightChunk should enqueue");
    TEST_ASSERT(save_core.enqueue_event(MakeEndEvent(job_id)),
                "EndJob should enqueue");

    TEST_ASSERT(WaitForStreamEventCount(&captured, 3, 2000),
                "stream events should be emitted for begin, block, and end");
    save_core.stop();

    std::vector<FrameStreamEvent> events;
    {
        std::lock_guard<std::mutex> lock(captured.mutex);
        events = captured.events;
    }

    TEST_ASSERT(events.size() == 3, "exactly three frame stream events are expected");
    TEST_ASSERT(events[0].type == FrameStreamEventType::JobBegin, "first stream event must be JobBegin");
    TEST_ASSERT(events[1].type == FrameStreamEventType::LightRgbBlock,
                "second stream event must be the RGB block");
    TEST_ASSERT(events[2].type == FrameStreamEventType::JobEnd, "third stream event must be JobEnd");

    const auto& block = events[1].light_rgb_block;
    TEST_ASSERT(block.line_count == 2, "block must contain the two light lines");
    TEST_ASSERT(block.line_index_start == 0, "first block must start at line 0");
    TEST_ASSERT(block.image_width == 4, "block width must match the full sensor width");
    TEST_ASSERT(block.rgb_pixels.size() == 24, "two lines of four pixels must yield 24 RGB samples");
    TEST_ASSERT(block.rgb_pixels[0] == 100 && block.rgb_pixels[1] == 200 && block.rgb_pixels[2] == 300,
                "first pixel must contain the RGB bands of the first line");
    TEST_ASSERT(block.rgb_pixels[3] == 101 && block.rgb_pixels[4] == 201 && block.rgb_pixels[5] == 301,
                "RGB samples must stay interleaved by pixel");
    TEST_ASSERT(block.rgb_pixels[12] == 110 && block.rgb_pixels[13] == 210 && block.rgb_pixels[14] == 310,
                "second line must append after the first line without overwriting");

    const std::string png_path = events[2].end.final_png_path;
    TEST_ASSERT(!png_path.empty(), "JobEnd must expose the final png path");
    const std::vector<std::uint8_t> png_bytes = ReadBinaryFileBytes(png_path);
    TEST_ASSERT(png_bytes.size() >= 24, "final png file must exist and contain an IHDR chunk");
    TEST_ASSERT(ReadU32Be(png_bytes, 16) == 4, "final png width must keep the full sensor width");
    TEST_ASSERT(ReadU32Be(png_bytes, 20) == 2, "final png height must match the number of light lines");
    return 0;
}

int TestStreamFailureDoesNotFailDiskSave() {
    SaveCore save_core(16, 1000);
    std::mutex progress_mutex;
    SaveProgressEvent finished_event;
    bool has_finished = false;
    save_core.set_progress_sink([&](const SaveProgressEvent& event) {
        if (event.type != SaveProgressType::JobFinished) {
            return;
        }
        std::lock_guard<std::mutex> lock(progress_mutex);
        finished_event = event;
        has_finished = true;
    });

    bool first_emit = true;
    save_core.set_frame_stream_sink([&](const FrameStreamEvent&) {
        if (first_emit) {
            first_emit = false;
            return false;
        }
        return true;
    });

    TEST_ASSERT(save_core.start(), "SaveCore should start");
    const std::uint64_t job_id = 42;
    TEST_ASSERT(save_core.enqueue_event(MakeBeginEvent(job_id, "save-core-stream-failure")),
                "BeginJob should enqueue");
    TEST_ASSERT(save_core.enqueue_event(MakeLightChunkEvent(job_id)),
                "LightChunk should enqueue");
    TEST_ASSERT(save_core.enqueue_event(MakeEndEvent(job_id)),
                "EndJob should enqueue");

    TEST_ASSERT(WaitForFinishedProgress(&progress_mutex, &has_finished, &finished_event, 2000),
                "disk save should still finish even when stream enqueue fails");
    save_core.stop();

    {
        std::lock_guard<std::mutex> lock(progress_mutex);
        TEST_ASSERT(finished_event.success, "stream failure must not turn a successful disk save into failure");
    }
    return 0;
}

}  // namespace

int main() {
    return RunSuite("test_save_core_stream", {
        {"SaveCoreEmitsRgbBlockAndWritesFullWidthPng", TestSaveCoreEmitsRgbBlockAndWritesFullWidthPng},
        {"StreamFailureDoesNotFailDiskSave", TestStreamFailureDoesNotFailDiskSave},
    });
}
