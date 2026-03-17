#include "pipe_core.h"

#include <chrono>
#include <cctype>
#include <iostream>
#include <string>

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

std::string RemoveNullBytes(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\0') {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::string StripUtf8Bom(const std::string& text) {
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        return text.substr(3);
    }
    return text;
}

void PipeInfo(const std::string& message) {
    std::cout << "[pipe] " << message << std::endl;
}

void PipeError(const std::string& message) {
    std::cerr << "[pipe] " << message << std::endl;
}

std::string ConnTag(std::uint64_t connection_id) {
    return "[conn=" + std::to_string(connection_id) + "] ";
}

bool ParseCommand(const std::string& line, AcquisitionJob* job) {
    constexpr const char* kPrefix = "CAPTURE ";
    constexpr std::size_t kPrefixLen = 8;

    const std::string sanitized = StripUtf8Bom(RemoveNullBytes(line));
    if (sanitized.size() <= kPrefixLen || sanitized.compare(0, kPrefixLen, kPrefix) != 0) {
        return false;
    }

    const std::string sample_name = Trim(sanitized.substr(kPrefixLen));
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
    wake_for_shutdown_.store(false);
    worker_ = std::thread(&PipeCore::worker_loop, this);
    return true;
}

void PipeCore::stop() {
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false)) {
        return;
    }

#ifdef _WIN32
    wake_for_shutdown_.store(true);
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

    wake_for_shutdown_.store(false);
    pending_line_.clear();
}

bool PipeCore::process_text(const std::string& text_chunk, std::uint64_t connection_id) {
    PipeInfo(ConnTag(connection_id) + "chunk received (" +
             std::to_string(text_chunk.size()) + " bytes): \"" + text_chunk + "\"");
    pending_line_.append(text_chunk);

    while (true) {
        const std::size_t pos = pending_line_.find('\n');
        if (pos == std::string::npos) {
            return true;
        }

        std::string line = pending_line_.substr(0, pos);
        pending_line_.erase(0, pos + 1);

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            PipeInfo(ConnTag(connection_id) + "empty line ignored");
            continue;
        }

        PipeInfo(ConnTag(connection_id) + "line received: \"" + line + "\"");

        if (!started_.load()) {
            PipeInfo(ConnTag(connection_id) + "stop requested, discarding pending command");
            return false;
        }

        AcquisitionJob job;
        if (!ParseCommand(line, &job)) {
            PipeError(ConnTag(connection_id) +
                      "invalid command. expected: CAPTURE <sample_name>");
            return false;
        }

        if (!callback_(job)) {
            PipeInfo(ConnTag(connection_id) +
                     "command rejected (camera busy). sample=" + job.sample_name);
            return false;
        }

        PipeInfo(ConnTag(connection_id) + "command accepted. sample=" + job.sample_name);
        return false;
    }
}

void PipeCore::worker_loop() {
#ifdef _WIN32
    std::uint64_t connection_seq = 0;
    while (started_.load()) {
        PipeInfo("waiting for client on " + pipe_name_);
        HANDLE pipe = CreateNamedPipeA(
            pipe_name_.c_str(), PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);

        if (pipe == INVALID_HANDLE_VALUE) {
            PipeError("CreateNamedPipe failed error=" + std::to_string(GetLastError()));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                                   ? TRUE
                                   : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            PipeError("ConnectNamedPipe failed error=" + std::to_string(GetLastError()));
            CloseHandle(pipe);
            continue;
        }

        const std::uint64_t connection_id = ++connection_seq;
        PipeInfo(ConnTag(connection_id) + "connection received on " + pipe_name_);
        pending_line_.clear();

        const bool shutdown_wake = wake_for_shutdown_.exchange(false);
        if (!started_.load() || shutdown_wake) {
            PipeInfo(ConnTag(connection_id) + "shutdown wake connection received");
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }

        char buffer[1024];
        while (started_.load()) {
            DWORD bytes_read = 0;
            if (!ReadFile(pipe, buffer, static_cast<DWORD>(sizeof(buffer)), &bytes_read, nullptr)) {
                const DWORD error = GetLastError();
                if (error == ERROR_BROKEN_PIPE) {
                    if (!pending_line_.empty()) {
                        process_text("\n", connection_id);
                    }
                    PipeInfo(ConnTag(connection_id) + "client disconnected");
                    break;
                }
                PipeError(ConnTag(connection_id) +
                          "ReadFile failed error=" + std::to_string(error));
                break;
            }

            if (bytes_read > 0) {
                if (!process_text(std::string(buffer, buffer + bytes_read), connection_id)) {
                    break;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
