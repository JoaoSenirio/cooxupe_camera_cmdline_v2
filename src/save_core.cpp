#include "save_core.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#else
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace {

std::string MakeLocalTimestampTag() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_local{};
#ifdef _WIN32
    localtime_s(&tm_local, &now_time);
#else
    localtime_r(&now_time, &tm_local);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_local, "%Y-%m-%d_%H-%M-%S");
    return oss.str();
}

std::string NormalizePathSeparators(const std::string& path) {
    std::string normalized = path;
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        if (normalized[i] == '/') {
            normalized[i] = '\\';
        }
    }
    return normalized;
}

bool IsAllowedTokenChar(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '-' || c == '_';
}

std::string SanitizeToken(const std::string& token, const char* fallback) {
    std::string out;
    out.reserve(token.size());
    for (std::size_t i = 0; i < token.size(); ++i) {
        const char c = token[i];
        out.push_back(IsAllowedTokenChar(c) ? c : '_');
    }

    while (!out.empty() && out.front() == '_') {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }

    if (out.empty()) {
        out = fallback;
    }
    return out;
}

std::string JoinPath(const std::string& base, const std::string& leaf) {
    if (base.empty()) {
        return leaf;
    }
    if (leaf.empty()) {
        return base;
    }

    const char tail = base.back();
    if (tail == '\\' || tail == '/') {
        return base + leaf;
    }
    return base + "\\" + leaf;
}

#ifdef _WIN32
bool PathExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
}

bool DirectoryExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
#else
bool PathExists(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

bool DirectoryExists(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode) != 0;
}
#endif

bool CreateDirectoriesRecursive(const std::string& path) {
    if (path.empty() || DirectoryExists(path)) {
        return true;
    }

    const std::string normalized = NormalizePathSeparators(path);
    std::string current;
    current.reserve(normalized.size());

    for (std::size_t i = 0; i < normalized.size(); ++i) {
        current.push_back(normalized[i]);

        const bool is_sep = (normalized[i] == '\\');
        const bool is_last = (i + 1 == normalized.size());
        if (!is_sep && !is_last) {
            continue;
        }
        if (current.empty()) {
            continue;
        }

        if (current.size() == 2 && current[1] == ':') {
            continue;
        }
        if (current.size() == 3 && current[1] == ':' && current[2] == '\\') {
            continue;
        }

#ifdef _WIN32
        if (!DirectoryExists(current)) {
            if (!CreateDirectoryA(current.c_str(), nullptr)) {
                const DWORD error = GetLastError();
                if (error != ERROR_ALREADY_EXISTS) {
                    return false;
                }
            }
        }
#else
        std::string posix = current;
        std::replace(posix.begin(), posix.end(), '\\', '/');
        if (!DirectoryExists(posix)) {
            if (mkdir(posix.c_str(), 0777) != 0 && errno != EEXIST) {
                return false;
            }
        }
#endif
    }

    return DirectoryExists(normalized);
}

int MapByteDepthToEnviDataType(std::int64_t byte_depth) {
    switch (byte_depth) {
        case 1:
            return 1;
        case 2:
            return 12;
        case 4:
            return 4;
        default:
            return 1;
    }
}

int ClampInt(int value, int min_v, int max_v) {
    if (value < min_v) {
        return min_v;
    }
    if (value > max_v) {
        return max_v;
    }
    return value;
}

int ClosestWavelengthIndex(const std::vector<double>& wavelengths, int target_nm) {
    if (wavelengths.empty()) {
        return 0;
    }

    int best = 0;
    double best_delta = std::abs(wavelengths[0] - static_cast<double>(target_nm));
    for (std::size_t i = 1; i < wavelengths.size(); ++i) {
        const double delta = std::abs(wavelengths[i] - static_cast<double>(target_nm));
        if (delta < best_delta) {
            best_delta = delta;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void WriteMultilineNumberBlock(std::ostream& out,
                               const std::string& key,
                               const std::vector<double>& values,
                               int precision,
                               bool comma_separated_lines) {
    out << key << " = {\r\n";
    out << std::fixed << std::setprecision(precision);
    for (std::size_t i = 0; i < values.size(); ++i) {
        out << values[i];
        if (comma_separated_lines && i + 1 < values.size()) {
            out << ",";
        }
        out << "\r\n";
    }
    out << "}\r\n";
}

std::uint16_t ReadU16BilPixel(const std::uint8_t* frame,
                              std::int64_t byte_depth,
                              std::int64_t samples,
                              int band,
                              int sample) {
    if (frame == nullptr || byte_depth <= 0 || samples <= 0 || band < 0 || sample < 0) {
        return 0;
    }

    const std::size_t offset =
        (static_cast<std::size_t>(band) * static_cast<std::size_t>(samples) +
         static_cast<std::size_t>(sample)) *
        static_cast<std::size_t>(byte_depth);

    if (byte_depth == 1) {
        return frame[offset];
    }
    if (byte_depth == 2) {
        return static_cast<std::uint16_t>(frame[offset] |
                                          (static_cast<std::uint16_t>(frame[offset + 1]) << 8));
    }
    if (byte_depth == 4) {
        const std::uint32_t value = static_cast<std::uint32_t>(frame[offset]) |
                                    (static_cast<std::uint32_t>(frame[offset + 1]) << 8) |
                                    (static_cast<std::uint32_t>(frame[offset + 2]) << 16) |
                                    (static_cast<std::uint32_t>(frame[offset + 3]) << 24);
        return static_cast<std::uint16_t>((value >> 16) & 0xFFFF);
    }

    return 0;
}

std::uint8_t NormalizeToByte(std::uint16_t value, std::uint16_t min_v, std::uint16_t max_v) {
    if (max_v <= min_v) {
        return 0;
    }
    const double t = static_cast<double>(value - min_v) / static_cast<double>(max_v - min_v);
    const int scaled = static_cast<int>(t * 255.0 + 0.5);
    return static_cast<std::uint8_t>(ClampInt(scaled, 0, 255));
}

void WriteBigEndian32(std::vector<std::uint8_t>* out, std::uint32_t value) {
    out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out->push_back(static_cast<std::uint8_t>(value & 0xFF));
}

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    static std::uint32_t table[256] = {};
    static bool initialized = false;

    if (!initialized) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1U) ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
            }
            table[i] = c;
        }
        initialized = true;
    }

    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

std::uint32_t Adler32(const std::uint8_t* data, std::size_t size) {
    constexpr std::uint32_t kMod = 65521U;
    std::uint32_t a = 1U;
    std::uint32_t b = 0U;

    for (std::size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % kMod;
        b = (b + a) % kMod;
    }

    return (b << 16U) | a;
}

void AppendPngChunk(std::vector<std::uint8_t>* out,
                    const char type[4],
                    const std::vector<std::uint8_t>& payload) {
    WriteBigEndian32(out, static_cast<std::uint32_t>(payload.size()));
    const std::size_t type_pos = out->size();
    out->push_back(static_cast<std::uint8_t>(type[0]));
    out->push_back(static_cast<std::uint8_t>(type[1]));
    out->push_back(static_cast<std::uint8_t>(type[2]));
    out->push_back(static_cast<std::uint8_t>(type[3]));
    out->insert(out->end(), payload.begin(), payload.end());
    const std::uint32_t crc = Crc32(out->data() + type_pos, 4 + payload.size());
    WriteBigEndian32(out, crc);
}

std::vector<std::uint8_t> ZlibStore(const std::vector<std::uint8_t>& input) {
    std::vector<std::uint8_t> out;
    out.reserve(input.size() + 16 + (input.size() / 65535) * 5);

    out.push_back(0x78);
    out.push_back(0x01);

    std::size_t offset = 0;
    while (offset < input.size()) {
        const std::size_t block_size = std::min<std::size_t>(65535, input.size() - offset);
        const bool last = (offset + block_size == input.size());

        out.push_back(last ? 0x01 : 0x00);

        const std::uint16_t len = static_cast<std::uint16_t>(block_size);
        const std::uint16_t nlen = static_cast<std::uint16_t>(~len);
        out.push_back(static_cast<std::uint8_t>(len & 0xFF));
        out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>(nlen & 0xFF));
        out.push_back(static_cast<std::uint8_t>((nlen >> 8) & 0xFF));

        out.insert(out.end(), input.begin() + static_cast<long>(offset),
                   input.begin() + static_cast<long>(offset + block_size));

        offset += block_size;
    }

    const std::uint32_t adler = Adler32(input.data(), input.size());
    WriteBigEndian32(&out, adler);
    return out;
}

}  // namespace

SaveCore::SaveCore(std::size_t queue_capacity, int enqueue_timeout_ms)
    : events_(queue_capacity),
      enqueue_timeout_(std::chrono::milliseconds(
          enqueue_timeout_ms > 0 ? enqueue_timeout_ms : 2000)) {}

SaveCore::~SaveCore() {
    stop();
}

bool SaveCore::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return true;
    }

    worker_ = std::thread(&SaveCore::worker_loop, this);
    return true;
}

void SaveCore::stop() {
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false)) {
        return;
    }

    events_.close();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool SaveCore::enqueue_event(const SaveEvent& event) {
    if (!started_.load()) {
        return false;
    }
    return events_.push_for(event, enqueue_timeout_);
}

bool SaveCore::handle_begin(const SaveEvent& event) {
    if (active_.open) {
        log_error("Received BeginJob while previous job is still open. Closing previous files.");
        close_open_files();
        reset_active_job();
    }

    const SaveEventBegin& begin = event.begin;
    const std::string root_dir = NormalizePathSeparators(begin.output_dir);
    if (root_dir.empty()) {
        log_error("BeginJob rejected: output_dir is empty");
        return false;
    }

    if (!CreateDirectoriesRecursive(root_dir)) {
        log_error("Failed to create output root directory: " + root_dir);
        return false;
    }

    const std::string camera_token = SanitizeToken(begin.camera_name, "CAMERA");
    const std::string sample_token = SanitizeToken(begin.sample_name, "SAMPLE");
    const std::string timestamp_token = SanitizeToken(
        begin.timestamp_tag.empty() ? MakeLocalTimestampTag() : begin.timestamp_tag,
        "timestamp");

    const std::string base_name = camera_token + "_" + timestamp_token + "_" + sample_token;

    std::string acquisition_name = base_name;
    std::string acquisition_dir = JoinPath(root_dir, acquisition_name);
    for (int suffix = 1; PathExists(acquisition_dir); ++suffix) {
        std::ostringstream oss;
        oss << base_name << "_" << std::setw(2) << std::setfill('0') << suffix;
        acquisition_name = oss.str();
        acquisition_dir = JoinPath(root_dir, acquisition_name);
    }

    if (!CreateDirectoriesRecursive(acquisition_dir)) {
        log_error("Failed to create acquisition directory: " + acquisition_dir);
        return false;
    }

    const std::string capture_dir = JoinPath(acquisition_dir, "capture");
    if (!CreateDirectoriesRecursive(capture_dir)) {
        log_error("Failed to create capture directory: " + capture_dir);
        return false;
    }

    active_.job_id = event.job_id;
    active_.open = true;
    active_.sample_name = begin.sample_name;
    active_.camera_name = camera_token;
    active_.acquisition_name = acquisition_name;
    active_.acquisition_dir = acquisition_dir;
    active_.capture_dir = capture_dir;
    active_.sensor = begin.sensor;
    active_.acquisition_date_utc = begin.acquisition_date_utc;
    active_.light_start_time_utc = begin.light_start_time_utc;

    active_.light_raw_path = JoinPath(capture_dir, acquisition_name + ".raw");
    active_.dark_raw_path = JoinPath(capture_dir, "DARKREF_" + acquisition_name + ".raw");
    active_.light_hdr_path = JoinPath(capture_dir, acquisition_name + ".hdr");
    active_.dark_hdr_path = JoinPath(capture_dir, "DARKREF_" + acquisition_name + ".hdr");
    active_.light_log_path = JoinPath(capture_dir, acquisition_name + ".log.txt");
    active_.dark_log_path = JoinPath(capture_dir, "DARKREF_" + acquisition_name + ".log.txt");
    active_.png_path = JoinPath(acquisition_dir, acquisition_name + ".png");

    active_.light_raw_file.open(active_.light_raw_path.c_str(), std::ios::binary | std::ios::trunc);
    if (!active_.light_raw_file.is_open()) {
        log_error("Failed to open light raw file: " + active_.light_raw_path);
        reset_active_job();
        return false;
    }

    active_.dark_raw_file.open(active_.dark_raw_path.c_str(), std::ios::binary | std::ios::trunc);
    if (!active_.dark_raw_file.is_open()) {
        log_error("Failed to open dark raw file: " + active_.dark_raw_path);
        reset_active_job();
        return false;
    }

    const int max_band = std::max(0, static_cast<int>(active_.sensor.image_height - 1));
    if (!active_.sensor.wavelengths_nm.empty()) {
        active_.red_band_index = ClosestWavelengthIndex(active_.sensor.wavelengths_nm,
                                                        begin.rgb_wavelength_nm[0]);
        active_.green_band_index = ClosestWavelengthIndex(active_.sensor.wavelengths_nm,
                                                          begin.rgb_wavelength_nm[1]);
        active_.blue_band_index = ClosestWavelengthIndex(active_.sensor.wavelengths_nm,
                                                         begin.rgb_wavelength_nm[2]);
    } else {
        active_.red_band_index = (max_band * 3) / 4;
        active_.green_band_index = (max_band * 2) / 4;
        active_.blue_band_index = (max_band * 1) / 4;
    }

    active_.red_band_index = ClampInt(active_.red_band_index, 0, max_band);
    active_.green_band_index = ClampInt(active_.green_band_index, 0, max_band);
    active_.blue_band_index = ClampInt(active_.blue_band_index, 0, max_band);

    log_info("BeginJob accepted. sample=" + begin.sample_name +
             " dir=" + acquisition_dir +
             " queue_job_id=" + std::to_string(event.job_id));
    return true;
}

bool SaveCore::handle_chunk(const SaveEvent& event) {
    if (!active_.open) {
        log_error("Chunk received without active job");
        return false;
    }
    if (event.job_id != active_.job_id) {
        log_error("Chunk job_id mismatch. expected=" + std::to_string(active_.job_id) +
                  " got=" + std::to_string(event.job_id));
        return false;
    }

    const SaveEventChunk& chunk = event.chunk;
    if (chunk.frame_count <= 0) {
        return true;
    }

    if (active_.sensor.frame_size_bytes <= 0 || active_.sensor.image_width <= 0 ||
        active_.sensor.image_height <= 0 || active_.sensor.byte_depth <= 0) {
        log_error("Invalid sensor geometry in active job");
        return false;
    }

    const std::size_t expected_bytes =
        static_cast<std::size_t>(chunk.frame_count) *
        static_cast<std::size_t>(active_.sensor.frame_size_bytes);
    if (chunk.bytes.size() != expected_bytes) {
        log_error("Chunk size mismatch. expected=" + std::to_string(expected_bytes) +
                  " got=" + std::to_string(chunk.bytes.size()));
        return false;
    }

    std::ofstream* out = nullptr;
    const bool dark_chunk = event.type == SaveEventType::DarkChunk;
    if (dark_chunk) {
        out = &active_.dark_raw_file;
    } else {
        out = &active_.light_raw_file;
    }

    out->write(reinterpret_cast<const char*>(chunk.bytes.data()),
               static_cast<std::streamsize>(chunk.bytes.size()));
    if (!out->good()) {
        log_error(std::string("Failed writing ") + (dark_chunk ? "dark" : "light") + " raw data");
        return false;
    }

    if (!dark_chunk) {
        const int width = static_cast<int>(active_.sensor.image_width);
        const int frame_size = static_cast<int>(active_.sensor.frame_size_bytes);
        const std::int64_t byte_depth = active_.sensor.byte_depth;
        const std::int64_t samples = active_.sensor.image_width;

        for (std::int64_t f = 0; f < chunk.frame_count; ++f) {
            const std::uint8_t* frame = chunk.bytes.data() + static_cast<std::size_t>(f) * frame_size;
            for (int x = 0; x < width; ++x) {
                active_.thumb_red.push_back(ReadU16BilPixel(frame, byte_depth, samples,
                                                            active_.red_band_index, x));
                active_.thumb_green.push_back(ReadU16BilPixel(frame, byte_depth, samples,
                                                              active_.green_band_index, x));
                active_.thumb_blue.push_back(ReadU16BilPixel(frame, byte_depth, samples,
                                                             active_.blue_band_index, x));
            }
            ++active_.thumb_lines;
        }
    }

    return true;
}

bool SaveCore::handle_end(const SaveEvent& event) {
    if (!active_.open) {
        log_error("EndJob received without active job");
        return false;
    }
    if (event.job_id != active_.job_id) {
        log_error("EndJob job_id mismatch. expected=" + std::to_string(active_.job_id) +
                  " got=" + std::to_string(event.job_id));
        return false;
    }

    const SaveEventEnd& end = event.end;
    close_open_files();

    bool ok = true;

    if (!write_hdr(active_.light_hdr_path, end.light_frames, end, false)) {
        ok = false;
    }
    if (!write_hdr(active_.dark_hdr_path, end.dark_frames, end, true)) {
        ok = false;
    }
    if (!write_drop_log(active_.light_log_path,
                        end.light_drop_incidents,
                        end.light_dropped_frames,
                        end.light_frames)) {
        ok = false;
    }
    if (!write_drop_log(active_.dark_log_path,
                        end.dark_drop_incidents,
                        end.dark_dropped_frames,
                        end.dark_frames)) {
        ok = false;
    }
    if (end.light_frames > 0) {
        if (!write_rgb_png()) {
            ok = false;
        }
    }

    std::ostringstream oss;
    oss << "EndJob sample=" << active_.sample_name
        << " success=" << (end.success ? "true" : "false")
        << " sdk_error=" << end.sdk_error
        << " light_frames=" << end.light_frames
        << " dark_frames=" << end.dark_frames
        << " output=" << active_.acquisition_dir;
    if (!end.message.empty()) {
        oss << " msg=\"" << end.message << "\"";
    }
    log_info(oss.str());

    reset_active_job();
    return ok;
}

void SaveCore::close_open_files() {
    if (active_.light_raw_file.is_open()) {
        active_.light_raw_file.close();
    }
    if (active_.dark_raw_file.is_open()) {
        active_.dark_raw_file.close();
    }
}

void SaveCore::reset_active_job() {
    close_open_files();
    active_ = ActiveJob{};
}

bool SaveCore::write_hdr(const std::string& path,
                         std::int64_t lines,
                         const SaveEventEnd& end,
                         bool dark) {
    std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        log_error("Failed to open HDR file: " + path);
        return false;
    }

    const SensorSnapshot& sensor = active_.sensor;
    const int data_type = MapByteDepthToEnviDataType(sensor.byte_depth);

    const std::string acquisition_date =
        end.acquisition_date_utc.empty() ? active_.acquisition_date_utc : end.acquisition_date_utc;

    const std::string start_time = dark ? end.dark_start_time_utc : end.light_start_time_utc;
    const std::string stop_time = dark ? end.dark_stop_time_utc : end.light_stop_time_utc;

    out << "ENVI\r\n";
    out << "description = {\r\n";
    out << "File Imported into ENVI}\r\n";
    out << "file type = ENVI\r\n\r\n";
    out << "sensor type = " << active_.camera_name << " , specsensor_cli\r\n";
    out << "acquisition date = DATE(yyyy-mm-dd): " << acquisition_date << "\r\n";
    out << "Start Time = UTC TIME: " << start_time << "\r\n";
    out << "Stop Time = UTC TIME: " << stop_time << "\r\n\r\n";
    out << "samples = " << sensor.image_width << "\r\n";
    out << "bands = " << sensor.image_height << "\r\n";
    out << "lines = " << lines << "\r\n\r\n";
    out << "errors = {none}\r\n\r\n";
    out << "interleave = bil\r\n";
    out << "data type = " << data_type << "\r\n";
    out << "header offset = 0\r\n";
    out << "byte order = 0\r\n";
    out << "x start = 0\r\n";
    out << "y start = 0\r\n";
    out << "default bands = {"
        << (active_.red_band_index + 1) << ", "
        << (active_.green_band_index + 1) << ", "
        << (active_.blue_band_index + 1) << "}\r\n\r\n";
    out << "himg = {1, " << sensor.image_width << "}\r\n";
    out << "vimg = {1, " << sensor.image_height << "}\r\n";
    out << "hroi = {1, " << sensor.image_width << "}\r\n";
    out << "vroi = {1, " << sensor.image_height << "}\r\n\r\n";

    out << std::fixed << std::setprecision(2);
    out << "fps = " << sensor.frame_rate_hz << "\r\n";
    out << "fps_qpf = " << sensor.frame_rate_hz << "\r\n";
    out << std::setprecision(6);
    out << "tint = " << sensor.exposure_ms << "\r\n";
    out << "binning = {" << sensor.binning_spatial << ", " << sensor.binning_spectral << "}\r\n";
    out << "trigger mode = Internal\r\n";
    out << "fodis = {0, 0}\r\n";
    if (!sensor.sensor_id.empty()) {
        out << "sensorid = " << sensor.sensor_id << "\r\n";
    }
    out << "acquisitionwindow left = " << sensor.acquisitionwindow_left << "\r\n";
    out << "acquisitionwindow top = " << sensor.acquisitionwindow_top << "\r\n";
    if (!sensor.calibration_pack.empty()) {
        out << "calibration pack = " << sensor.calibration_pack << "\r\n";
    }
    out << "\r\n";

    if (sensor.has_vnir_temperature) {
        out << "VNIR temperature = " << std::fixed << std::setprecision(2)
            << sensor.vnir_temperature << "\r\n\r\n";
        out << "temperature = {\r\n";
        out << std::fixed << std::setprecision(2) << sensor.vnir_temperature << "\r\n";
        out << "}\r\n\r\n";
    }

    if (!sensor.wavelengths_nm.empty()) {
        WriteMultilineNumberBlock(out, "Wavelength", sensor.wavelengths_nm, 2, true);
        out << "\r\n";
    }

    if (!sensor.fwhm_nm.empty()) {
        WriteMultilineNumberBlock(out, "fwhm", sensor.fwhm_nm, 2, true);
    }

    if (!out.good()) {
        log_error("Failed writing HDR file: " + path);
        return false;
    }

    return true;
}

bool SaveCore::write_drop_log(const std::string& path,
                              std::int64_t drop_incidents,
                              std::int64_t dropped_frames,
                              std::int64_t frames_recorded) {
    std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        log_error("Failed to open dropped frame log: " + path);
        return false;
    }

    out << "Lumo Recorder - Dropped Frame Log\r\n";
    out << "  " << drop_incidents << " dropped frame incidents, "
        << dropped_frames << " dropped frames\r\n\r\n";
    out << "  " << frames_recorded << " frames recorded\r\n";

    if (!out.good()) {
        log_error("Failed writing dropped frame log: " + path);
        return false;
    }

    return true;
}

bool SaveCore::write_rgb_png() {
    const int width = static_cast<int>(active_.sensor.image_width);
    const int lines = static_cast<int>(active_.thumb_lines);
    if (width <= 0 || lines <= 0) {
        log_error("Invalid thumbnail dimensions: width=" + std::to_string(width) +
                  " lines=" + std::to_string(lines));
        return false;
    }

    const std::size_t expected =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(lines);
    if (active_.thumb_red.size() != expected ||
        active_.thumb_green.size() != expected ||
        active_.thumb_blue.size() != expected) {
        log_error("Thumbnail channel size mismatch");
        return false;
    }

    const int max_width = 1920;
    const int max_height = 1080;
    const int step_x = std::max(1, (width + max_width - 1) / std::max(1, max_width));
    const int step_y = std::max(1, (lines + max_height - 1) / std::max(1, max_height));
    const int out_w = (width + step_x - 1) / step_x;
    const int out_h = (lines + step_y - 1) / step_y;

    std::uint16_t rmin = std::numeric_limits<std::uint16_t>::max();
    std::uint16_t gmin = std::numeric_limits<std::uint16_t>::max();
    std::uint16_t bmin = std::numeric_limits<std::uint16_t>::max();
    std::uint16_t rmax = 0;
    std::uint16_t gmax = 0;
    std::uint16_t bmax = 0;

    for (std::size_t i = 0; i < expected; ++i) {
        rmin = std::min(rmin, active_.thumb_red[i]);
        gmin = std::min(gmin, active_.thumb_green[i]);
        bmin = std::min(bmin, active_.thumb_blue[i]);
        rmax = std::max(rmax, active_.thumb_red[i]);
        gmax = std::max(gmax, active_.thumb_green[i]);
        bmax = std::max(bmax, active_.thumb_blue[i]);
    }

    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(out_h) * (1 + static_cast<std::size_t>(out_w) * 3));

    for (int y = 0; y < out_h; ++y) {
        raw.push_back(0);
        const int src_y = std::min(lines - 1, y * step_y);
        for (int x = 0; x < out_w; ++x) {
            const int src_x = std::min(width - 1, x * step_x);
            const std::size_t idx =
                static_cast<std::size_t>(src_y) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(src_x);
            raw.push_back(NormalizeToByte(active_.thumb_red[idx], rmin, rmax));
            raw.push_back(NormalizeToByte(active_.thumb_green[idx], gmin, gmax));
            raw.push_back(NormalizeToByte(active_.thumb_blue[idx], bmin, bmax));
        }
    }

    std::vector<std::uint8_t> png;
    const std::array<std::uint8_t, 8> signature = {137, 80, 78, 71, 13, 10, 26, 10};
    png.insert(png.end(), signature.begin(), signature.end());

    std::vector<std::uint8_t> ihdr;
    WriteBigEndian32(&ihdr, static_cast<std::uint32_t>(out_w));
    WriteBigEndian32(&ihdr, static_cast<std::uint32_t>(out_h));
    ihdr.push_back(8);
    ihdr.push_back(2);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    AppendPngChunk(&png, "IHDR", ihdr);

    const std::vector<std::uint8_t> compressed = ZlibStore(raw);
    AppendPngChunk(&png, "IDAT", compressed);
    AppendPngChunk(&png, "IEND", std::vector<std::uint8_t>());

    std::ofstream out(active_.png_path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        log_error("Failed to open PNG file: " + active_.png_path);
        return false;
    }

    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out.good()) {
        log_error("Failed writing PNG file: " + active_.png_path);
        return false;
    }

    return true;
}

void SaveCore::log_info(const std::string& message) {
    std::cout << "[save] " << message << "\n";
}

void SaveCore::log_error(const std::string& message) {
    std::cerr << "[save] " << message << "\n";
}

void SaveCore::worker_loop() {
    SaveEvent event;
    while (events_.pop(&event)) {
        bool ok = false;
        if (event.type == SaveEventType::BeginJob) {
            ok = handle_begin(event);
        } else if (event.type == SaveEventType::LightChunk ||
                   event.type == SaveEventType::DarkChunk) {
            ok = handle_chunk(event);
        } else if (event.type == SaveEventType::EndJob) {
            ok = handle_end(event);
        }

        if (!ok) {
            log_error("Event processing failed. type=" + std::to_string(static_cast<int>(event.type)) +
                      " job_id=" + std::to_string(event.job_id));
        }
    }

    if (active_.open) {
        log_error("Worker stopped with an open job. Forcing file close.");
        close_open_files();
        reset_active_job();
    }
}
