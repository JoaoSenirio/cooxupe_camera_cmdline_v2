#include <iostream>
#include <vector>

#include "app_config.h"
#include "capture_core.h"
#include "test_support.h"

namespace {

int TestValidateConfig() {
    AppConfig config = MakeDefaultConfig();
    std::string error;
    TEST_ASSERT(ValidateConfig(config, &error), "default config should be valid");

    config.license_path.clear();
    TEST_ASSERT(!ValidateConfig(config, &error), "empty license_path must be invalid");
    return 0;
}

int TestMatlabStreamConfigValidation() {
    AppConfig config = MakeDefaultConfig();
    std::string error;

    config.matlab_stream_queue_capacity = 0;
    TEST_ASSERT(!ValidateConfig(config, &error), "stream queue capacity must be validated");

    config = MakeDefaultConfig();
    config.matlab_stream_port = 70000;
    TEST_ASSERT(!ValidateConfig(config, &error), "stream port must stay within TCP range");

    config = MakeDefaultConfig();
    config.matlab_stream_host.clear();
    TEST_ASSERT(!ValidateConfig(config, &error), "stream host must not be empty");
    return 0;
}

int TestBinningMap() {
    TEST_ASSERT(BinningValueToEnumIndex(1) == 0, "1x binning maps to 0");
    TEST_ASSERT(BinningValueToEnumIndex(2) == 1, "2x binning maps to 1");
    TEST_ASSERT(BinningValueToEnumIndex(4) == 2, "4x binning maps to 2");
    TEST_ASSERT(BinningValueToEnumIndex(8) == 3, "8x binning maps to 3");
    TEST_ASSERT(BinningValueToEnumIndex(16) == -1, "invalid binning maps to -1");
    return 0;
}

}  // namespace

int main() {
    return RunSuite("test_app_config", {
        {"ValidateConfig", TestValidateConfig},
        {"MatlabStreamConfigValidation", TestMatlabStreamConfigValidation},
        {"BinningMap", TestBinningMap},
    });
}
