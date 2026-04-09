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

std::shared_ptr<const std::vector<std::uint8_t>> MakeLightChunkBytes() {
    auto bytes = std::make_shared<std::vector<std::uint8_t>>();
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
            AppendU16Le(bytes.get(), frame0[band][x]);
        }
    }
    for (int band = 0; band < 3; ++band) {
        for (int x = 0; x < 4; ++x) {
            AppendU16Le(bytes.get(), frame1[band][x]);
        }
    }
    return bytes;
}

WorkItem MakeBeginItem(std::uint64_t job_id, const std::string& output_dir) {
    WorkItem begin;
    begin.type = WorkItemType::BeginJob;
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

WorkItem MakeLightChunkItem(std::uint64_t job_id) {
    WorkItem chunk;
    chunk.type = WorkItemType::LightChunk;
    chunk.job_id = job_id;
    chunk.chunk.bytes = MakeLightChunkBytes();
    chunk.chunk.frame_count = 2;
    chunk.chunk.first_frame_number = 10;
    chunk.chunk.last_frame_number = 11;
    return chunk;
}

WorkItem MakeEndItem(std::uint64_t job_id) {
    WorkItem end;
    end.type = WorkItemType::EndJob;
    end.job_id = job_id;
    end.end.success = true;
    end.end.light_frames = 2;
    end.end.dark_frames = 0;
    end.end.acquisition_date_utc = "2026-04-01";
    end.end.light_start_time_utc = "14:30:00";
    end.end.light_stop_time_utc = "14:30:01";
    return end;
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

int TestSaveCorePersistsSharedLightChunkAndWritesPng() {
    SharedWorkQueue work_queue(4, SharedWorkQueue::kConsumerSaveMask);
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

    TEST_ASSERT(save_core.start(&work_queue), "SaveCore should start");
    const std::uint64_t job_id = 41;
    const std::string output_dir =
        "save-core-stream-output-" +
        std::to_string(static_cast<long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TEST_ASSERT(work_queue.publish(MakeBeginItem(job_id, output_dir), std::chrono::milliseconds(1000)),
                "BeginJob should publish");
    TEST_ASSERT(work_queue.publish(MakeLightChunkItem(job_id), std::chrono::milliseconds(1000)),
                "LightChunk should publish");
    TEST_ASSERT(work_queue.publish(MakeEndItem(job_id), std::chrono::milliseconds(1000)),
                "EndJob should publish");
    work_queue.close();

    TEST_ASSERT(WaitForFinishedProgress(&progress_mutex, &has_finished, &finished_event, 2000),
                "save core should finish writing the job");
    save_core.stop();

    {
        std::lock_guard<std::mutex> lock(progress_mutex);
        TEST_ASSERT(finished_event.success, "finished save progress must report success");
    }

    const std::string png_path =
        output_dir + "/FX10_2026-04-01_14-30-00_sample-rgb-stream/"
        "FX10_2026-04-01_14-30-00_sample-rgb-stream.png";
    const std::vector<std::uint8_t> png_bytes = ReadBinaryFileBytes(png_path);
    TEST_ASSERT(png_bytes.size() >= 24, "final png file must exist and contain an IHDR chunk");
    TEST_ASSERT(ReadU32Be(png_bytes, 16) == 4, "final png width must keep the full sensor width");
    TEST_ASSERT(ReadU32Be(png_bytes, 20) == 2, "final png height must match the number of light lines");
    return 0;
}

}  // namespace

int main() {
    return RunSuite("test_save_core_stream", {
        {"SaveCorePersistsSharedLightChunkAndWritesPng", TestSaveCorePersistsSharedLightChunkAndWritesPng},
    });
}
