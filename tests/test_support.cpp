#include "test_support.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;

std::string GetEnvVar(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

fs::path GetRepoRootPath() {
    const std::string configured = GetEnvVar("TEST_REPO_ROOT");
    if (!configured.empty()) {
        return fs::path(configured);
    }
    return fs::current_path();
}

fs::path GetArtifactsLogDirPath() {
    const std::string configured = GetEnvVar("TEST_ARTIFACTS_DIR");
    if (!configured.empty()) {
        return fs::path(configured);
    }
    return GetRepoRootPath() / "tests" / "artifacts" / "logs";
}

}  // namespace

int RunSuite(const char* suite_name, const std::vector<TestCase>& tests) {
    if (!EnsureTestLogWorkingDirectory()) {
        std::cerr << "[FAIL] " << suite_name << ": failed to prepare test log directory\n";
        return 1;
    }

    for (const TestCase& test : tests) {
        std::cout << "[RUN] " << suite_name << "." << test.name << "\n";
        if (test.fn() != 0) {
            return 1;
        }
    }

    std::cout << "[PASS] " << suite_name << "\n";
    return 0;
}

std::string MakeDayStamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_local{};
#ifdef _WIN32
    localtime_s(&tm_local, &now_time);
#else
    localtime_r(&now_time, &tm_local);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_local, "%Y%m%d");
    return oss.str();
}

std::string ResolveLogFilePath(const std::string& configured_path) {
    const fs::path configured(configured_path);
    const fs::path parent = configured.parent_path();

    std::string stem = configured.stem().string();
    std::string ext = configured.extension().string();
    if (stem.empty()) {
        stem = "specsensor_cli";
    }
    if (ext.empty()) {
        ext = ".log";
    }

    const fs::path resolved_name = stem + "_" + MakeDayStamp() + ext;
    const fs::path resolved = parent.empty() ? resolved_name : (parent / resolved_name);
    return resolved.lexically_normal().string();
}

std::string ReadTextFile(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::in);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

bool StringContains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

AppConfig MakeTestConfig(const std::string& log_tag) {
    AppConfig config = MakeDefaultConfig();
    config.device_index = 0;
    config.capture_seconds = 1;
    config.dark_frames = 5;
    config.min_buffers_required = 1;
    config.output_dir = "test-output";
    config.log_file_path = log_tag + ".log";
    return config;
}

void RemoveLogFileIfExists(const std::string& configured_path) {
    std::error_code error;
    std::filesystem::remove(ResolveLogFilePath(configured_path), error);
}

std::string GetArtifactsLogDir() {
    return GetArtifactsLogDirPath().string();
}

bool EnsureTestLogWorkingDirectory() {
    std::error_code error;
    const fs::path log_dir = GetArtifactsLogDirPath();
    fs::create_directories(log_dir, error);
    if (error) {
        return false;
    }

    fs::current_path(log_dir, error);
    return !error;
}
