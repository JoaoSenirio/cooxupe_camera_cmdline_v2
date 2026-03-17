#include <windows.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr const char* kDefaultPipeName = "\\\\.\\pipe\\specsensor_sample_pipe";

HANDLE ConnectPipeWithRetry(const std::string& pipe_name) {
    while (true) {
        HANDLE pipe = CreateFileA(pipe_name.c_str(), GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (pipe != INVALID_HANDLE_VALUE) {
            std::cout << "[client] Connected to pipe: " << pipe_name << "\n";
            return pipe;
        }

        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PIPE_BUSY) {
            std::cout << "[client] Waiting for server...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        std::cerr << "[client] CreateFile failed, error=" << error << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

bool SendSample(HANDLE pipe, const std::string& sample_name) {
    const std::string payload = "CAPTURE " + sample_name + "\n";
    DWORD bytes_written = 0;
    if (!WriteFile(pipe, payload.data(), static_cast<DWORD>(payload.size()),
                   &bytes_written, nullptr)) {
        return false;
    }

    FlushFileBuffers(pipe);
    return bytes_written == payload.size();
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string pipe_name = kDefaultPipeName;
    if (argc >= 2) {
        pipe_name = argv[1];
    }

    std::cout << "Windows Pipe Client\n";
    std::cout << "Pipe: " << pipe_name << "\n";
    std::cout << "Command format: CAPTURE <sample_name>\n";
    std::cout << "Type sample names and press ENTER. Type 'q' to exit.\n\n";

    HANDLE pipe = ConnectPipeWithRetry(pipe_name);

    std::string sample_name;
    while (true) {
        std::cout << "sample_name> ";
        if (!std::getline(std::cin, sample_name)) {
            break;
        }

        if (sample_name == "q" || sample_name == "quit" || sample_name == "exit") {
            break;
        }

        if (sample_name.empty()) {
            continue;
        }

        if (!SendSample(pipe, sample_name)) {
            std::cerr << "[client] Write failed. Reconnecting...\n";
            CloseHandle(pipe);
            pipe = ConnectPipeWithRetry(pipe_name);
            if (!SendSample(pipe, sample_name)) {
                std::cerr << "[client] Failed to send after reconnect.\n";
                continue;
            }
        }

        std::cout << "[client] Sent: CAPTURE " << sample_name << "\n";
    }

    CloseHandle(pipe);
    std::cout << "[client] Exit\n";
    return 0;
}
