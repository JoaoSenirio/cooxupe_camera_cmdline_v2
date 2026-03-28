#ifndef SAVE_CORE_H
#define SAVE_CORE_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "thread_queue.h"
#include "types.h"

class SaveCore {
public:
    SaveCore(std::size_t queue_capacity = 200, int enqueue_timeout_ms = 2000);
    ~SaveCore();

    bool start();
    void stop();

    bool enqueue_event(const SaveEvent& event);
    void set_progress_sink(std::function<void(const SaveProgressEvent&)> progress_sink);

private:
    struct ActiveJob {
        bool open = false;
        std::uint64_t job_id = 0;
        std::string sample_name;
        std::string camera_name;
        std::string acquisition_name;
        std::string acquisition_dir;
        std::string capture_dir;
        std::string light_raw_path;
        std::string dark_raw_path;
        std::string light_hdr_path;
        std::string dark_hdr_path;
        std::string light_log_path;
        std::string dark_log_path;
        std::string png_path;
        SensorSnapshot sensor;
        std::ofstream light_raw_file;
        std::ofstream dark_raw_file;
        int red_band_index = 0;
        int green_band_index = 0;
        int blue_band_index = 0;
        std::vector<std::uint16_t> thumb_red;
        std::vector<std::uint16_t> thumb_green;
        std::vector<std::uint16_t> thumb_blue;
        std::int64_t thumb_lines = 0;
        std::string acquisition_date_utc;
        std::string light_start_time_utc;
        std::uint64_t expected_total_bytes = 0;
        std::uint64_t bytes_written = 0;
        std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::time_point{};
    };

    bool handle_begin(const SaveEvent& event);
    bool handle_chunk(const SaveEvent& event);
    bool handle_end(const SaveEvent& event);
    void close_open_files();
    void reset_active_job();

    bool write_hdr(const std::string& path, std::int64_t lines, const SaveEventEnd& end, bool dark);
    bool write_drop_log(const std::string& path, std::int64_t drop_incidents,
                        std::int64_t dropped_frames, std::int64_t frames_recorded);
    bool write_rgb_png();

    void log_info(const std::string& message);
    void log_error(const std::string& message);
    void emit_progress(const SaveProgressEvent& event);

    void worker_loop();

    ThreadQueue<SaveEvent> events_;
    std::chrono::milliseconds enqueue_timeout_;
    std::thread worker_;
    std::atomic<bool> started_{false};
    ActiveJob active_;
    std::function<void(const SaveProgressEvent&)> progress_sink_;
};

#endif
