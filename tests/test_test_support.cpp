#include <filesystem>
#include <iostream>

#include "test_support.h"

namespace {

namespace fs = std::filesystem;

int TestResolveLogFilePathAddsDateSuffix() {
    const std::string resolved = ResolveLogFilePath("suite.log");
    TEST_ASSERT(StringContains(resolved, "suite_" + MakeDayStamp() + ".log"),
                "resolved path must include the dated suffix");
    return 0;
}

int TestMakeTestConfigUsesRelativeLogFileName() {
    const AppConfig config = MakeTestConfig("sample-log");
    TEST_ASSERT(config.log_file_path == "sample-log.log",
                "test config should keep a relative log file name");
    TEST_ASSERT(config.output_dir == "test-output",
                "test config should use the test output placeholder");
    return 0;
}

int TestEnsureTestLogWorkingDirectoryUsesArtifactsDir() {
    TEST_ASSERT(EnsureTestLogWorkingDirectory(), "working directory setup should succeed");
    const fs::path current = fs::current_path();
    const fs::path expected = fs::path(GetArtifactsLogDir());
    TEST_ASSERT(current == expected, "working directory must match the configured artifacts dir");
    return 0;
}

}  // namespace

int main() {
    return RunSuite("test_test_support", {
        {"ResolveLogFilePathAddsDateSuffix", TestResolveLogFilePathAddsDateSuffix},
        {"MakeTestConfigUsesRelativeLogFileName", TestMakeTestConfigUsesRelativeLogFileName},
        {"EnsureTestLogWorkingDirectoryUsesArtifactsDir",
         TestEnsureTestLogWorkingDirectoryUsesArtifactsDir},
    });
}
