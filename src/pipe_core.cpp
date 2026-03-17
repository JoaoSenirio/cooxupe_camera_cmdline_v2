#include "pipe_core.h"

#include <chrono>
#include <cctype>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(begin, end - begin);
}

bool ParseCommand(const std::string& line, AcquisitionJob* job) {
    constexpr const char* kPrefix = "CAPTURE ";
    constexpr std::size_t kPrefixLen = 8;
    if (line.size() <= kPrefixLen || line.compare(0, kPrefixLen, kPrefix) != 0) {
        return false;
    }

    const std::string sample_name = Trim(line.substr(kPrefixLen));
    if (sample_name.empty()) {
        return false;
    }

    job->sample_name = sample_name;
    return true;
}

}  // namespace

PipeCore::PipeCore() = default;

PipeCore::~PipeCore() {
    stop();
}

bool PipeCore::start(const std::string& pipe_name, JobCallback callback) {
    if (pipe_name.empty() || !callback) {
        return false;
    }

    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        return true;
    }

    pipe_name_ = pipe_name;
    callback_ = callback;
    worker_ = std::thread(&PipeCore::worker_loop, this);
    return true;
}

void PipeCore::stop() {
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false)) {
        return;
    }

#ifdef _WIN32
    HANDLE wake = CreateFileA(pipe_name_.c_str(), GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (wake == INVALID_HANDLE_VALUE) {
        wake = CreateFileA(pipe_name_.c_str(), GENERIC_WRITE | GENERIC_READ, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (wake != INVALID_HANDLE_VALUE) {
        CloseHandle(wake);
    }
#endif

    if (worker_.joinable()) {
        worker_.join();
    }

    pending_line_.clear();
}

bool PipeCore::process_text(const std::string& text_chunk) {
    std::cout << "[pipe] chunk received (" << text_chunk.size() << " bytes): \""
              << text_chunk << "\"\n";
    pending_line_.append(text_chunk);

    const std::size_t pos = pending_line_.find('\n');
    if (pos == std::string::npos) {
        return true;
    }

    std::string line = pending_line_.substr(0, pos);
    pending_line_.erase(0, pos + 1);

    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::cout << "[pipe] line received: \"" << line << "\"\n";

    AcquisitionJob job;
    if (!ParseCommand(line, &job)) {
        std::cerr << "[pipe] invalid command. expected: CAPTURE <sample_name>\n";
        return false;
    }

    if (!callback_(job)) {
        std::cout << "[pipe] command rejected (camera busy). sample=" << job.sample_name << "\n";
        return false;
    }

    std::cout << "[pipe] command accepted. sample=" << job.sample_name << "\n";
    return false;
}

void PipeCore::worker_loop() {
#ifdef _WIN32
    while (started_.load()) {
        HANDLE pipe = CreateNamedPipeA(
            pipe_name_.c_str(), PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);

        if (pipe == INVALID_HANDLE_VALUE) {
            std::cerr << "[pipe] CreateNamedPipe failed\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                                   ? TRUE
                                   : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(pipe);
            continue;
        }

        std::cout << "[pipe] connection received on " << pipe_name_ << "\n";
        pending_line_.clear();

        char buffer[1024];
        while (started_.load()) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                const DWORD error = GetLastError();
                if (error == ERROR_BROKEN_PIPE) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            if (available == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            const DWORD bytes_to_read =
                available < sizeof(buffer) ? available : static_cast<DWORD>(sizeof(buffer));

            DWORD bytes_read = 0;
            if (!ReadFile(pipe, buffer, bytes_to_read, &bytes_read, nullptr)) {
                const DWORD error = GetLastError();
                if (error == ERROR_BROKEN_PIPE) {
                    break;
                }
                std::cerr << "[pipe] ReadFile failed\n";
                break;
            }

            if (bytes_read > 0) {
                if (!process_text(std::string(buffer, buffer + bytes_read))) {
                    break;
                }
            }
        }

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
#else
    std::cerr << "[pipe] Named pipe is only implemented on Windows\n";
#endif
}
