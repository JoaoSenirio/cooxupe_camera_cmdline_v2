#include <iostream>
#include <string>

#include "app_config.h"
#include "specsensor_api.h"

namespace {

void PrintUsage() {
    std::cout << "Usage:\n"
              << "  specsensor_cli.exe                # Connect-only check (default)\n"
              << "  specsensor_cli.exe --connect-only # Connect-only check\n";
}

bool RunConnectOnly(const AppConfig& config) {
    std::string validation_error;
    if (!ValidateConfig(config, &validation_error)) {
        std::cerr << "[connect] Invalid configuration: " << validation_error << "\n";
        return false;
    }

    auto api = CreateSpecSensorApi();
    auto fail = [&](const char* step, int code) -> bool {
        std::wcerr << L"[connect] Failed at " << step
                   << L" with code=" << code
                   << L" msg=\"" << api->GetErrorString(code) << L"\"\n";
        return false;
    };

    int error = api->Load(config.license_path);
    if (error != 0) {
        return fail("SI_Load", error);
    }

    std::int64_t device_count = 0;
    error = api->GetDeviceCount(&device_count);
    if (error != 0) {
        api->Unload();
        return fail("SI_GetInt(SI_SYSTEM, DeviceCount)", error);
    }

    std::cout << "[connect] Device count: " << device_count << "\n";
    if (config.device_index < 0 || config.device_index >= device_count) {
        std::cerr << "[connect] Invalid device_index=" << config.device_index
                  << " (device_count=" << device_count << ")\n";
        api->Unload();
        return false;
    }

    error = api->Open(config.device_index);
    if (error != 0) {
        api->Unload();
        return fail("SI_Open", error);
    }

    error = api->Command(L"Initialize");
    if (error != 0) {
        api->Close();
        api->Unload();
        return fail("Initialize", error);
    }

    std::cout << "[connect] Camera connected and initialized successfully.\n";

    api->Close();
    api->Unload();
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    AppConfig config = MakeDefaultConfig();

    if (argc > 1) {
        const std::string arg = argv[1];
        if (arg == "--help") {
            PrintUsage();
            return 0;
        }
        if (arg != "--connect-only") {
            PrintUsage();
            return 1;
        }
    }

    return RunConnectOnly(config) ? 0 : 1;
}
