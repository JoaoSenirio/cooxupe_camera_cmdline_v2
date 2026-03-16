#include "pipe_core.h"

#include <chrono>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

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
    HANDLE wake = CreateFileA(pipe_name_.c_str(), GENERIC_READ, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (wake != INVALID_HANDLE_VALUE) {
        CloseHandle(wake);
    }
#endif

    if (worker_.joinable()) {
        worker_.join();
    }

    pending_line_.clear();
}

void PipeCore::process_text(const std::string& text_chunk) {
    pending_line_.append(text_chunk);

    std::size_t pos = pending_line_.find('\n');
    while (pos != std::string::npos) {
        std::string line = pending_line_.substr(0, pos);
        pending_line_.erase(0, pos + 1);

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (!line.empty()) {
            AcquisitionJob job;
            job.sample_name = line;
            callback_(job);
        }

        pos = pending_line_.find('\n');
    }
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

        std::cout << "[pipe] client connected\n";

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
                process_text(std::string(buffer, buffer + bytes_read));
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
