#include "specsensor_api.h"

#include <cstdint>
#include <memory>

#ifdef _WIN32
#include <windows.h>

#if defined(__has_include)
#if __has_include("SI_errors.h")
#include "SI_errors.h"
#elif __has_include("SI_erros.h")
#include "SI_erros.h"
#elif __has_include("C:/Program Files (x86)/Specim/SDKs/SpecSensor/2020_519/include/SI_errors.h")
#include "C:/Program Files (x86)/Specim/SDKs/SpecSensor/2020_519/include/SI_errors.h"
#elif __has_include("C:/Program Files (x86)/Specim/SDKs/SpecSensor/2020_519/bin/SI_erros.h")
#include "C:/Program Files (x86)/Specim/SDKs/SpecSensor/2020_519/bin/SI_erros.h"
#elif __has_include("C:/Program Files (x86)/Specim/SDKs/SpecSensor/include/SI_errors.h")
#include "C:/Program Files (x86)/Specim/SDKs/SpecSensor/include/SI_errors.h"
#elif __has_include("C:/Program Files/Specim/SDKs/SpecSensor/include/SI_errors.h")
#include "C:/Program Files/Specim/SDKs/SpecSensor/include/SI_errors.h"
#elif __has_include("C:/Program Files (x86)/Specim/SDKs/SpecSensor/include/SI_erros.h")
#include "C:/Program Files (x86)/Specim/SDKs/SpecSensor/include/SI_erros.h"
#elif __has_include("C:/Program Files/Specim/SDKs/SpecSensor/include/SI_erros.h")
#include "C:/Program Files/Specim/SDKs/SpecSensor/include/SI_erros.h"
#else
#error "SpecSensor header not found: expected SI_errors.h (or SI_erros.h). Check SpecSensorSdkDir/include in the .vcxproj."
#endif
#else
#include "SI_errors.h"
#endif

#if defined(__has_include)
#if __has_include("SI_sensor.h")
#include "SI_sensor.h"
#elif __has_include("C:/Program Files (x86)/Specim/SDKs/SpecSensor/2020_519/include/SI_sensor.h")
#include "C:/Program Files (x86)/Specim/SDKs/SpecSensor/2020_519/include/SI_sensor.h"
#elif __has_include("C:/Program Files (x86)/Specim/SDKs/SpecSensor/include/SI_sensor.h")
#include "C:/Program Files (x86)/Specim/SDKs/SpecSensor/include/SI_sensor.h"
#elif __has_include("C:/Program Files/Specim/SDKs/SpecSensor/include/SI_sensor.h")
#include "C:/Program Files/Specim/SDKs/SpecSensor/include/SI_sensor.h"
#else
#error "SpecSensor header not found: expected SI_sensor.h. Check SpecSensorSdkDir/include in the .vcxproj."
#endif
#else
#include "SI_sensor.h"
#endif

#if !defined(SI_SYSTEM) && defined(SI_System)
#define SI_SYSTEM SI_System
#endif
#if !defined(SI_System) && defined(SI_SYSTEM)
#define SI_System SI_SYSTEM
#endif
#if !defined(SI_SYSTEM) && !defined(SI_System)
#define SI_SYSTEM 0
#define SI_System 0
#endif

namespace {

bool DirectoryExists(const wchar_t* path) {
    if (path == nullptr || *path == 0) {
        return false;
    }

    const DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void ConfigureSpecSensorDllSearchPath() {
    const wchar_t* candidates[] = {
        L"C:\\Program Files (x86)\\Specim\\SDKs\\SpecSensor\\2020_519\\bin\\x64",
        L"C:\\Program Files\\Specim\\SDKs\\SpecSensor\\2020_519\\bin\\x64",
        L"C:\\Program Files (x86)\\Specim\\SDKs\\SpecSensor\\bin\\x64",
        L"C:\\Program Files\\Specim\\SDKs\\SpecSensor\\bin\\x64",
    };

    for (const wchar_t* path : candidates) {
        if (DirectoryExists(path)) {
            SetDllDirectoryW(path);
            return;
        }
    }
}

}  // namespace

class SpecSensorApiSdk final : public ISpecSensorApi {
public:
    int Load(const std::wstring& license_path) override {
        ConfigureSpecSensorDllSearchPath();
        const int error = SI_Load(license_path.c_str());
        if (error == 0) {
            loaded_ = true;
        }
        return error;
    }

    int Unload() override {
        if (!loaded_) {
            return 0;
        }

        const int error = SI_Unload();
        if (error == 0) {
            loaded_ = false;
        }
        return error;
    }

    int GetDeviceCount(std::int64_t* count) override {
        SI_64 value = 0;
        const int error = SI_GetInt(SI_SYSTEM, L"DeviceCount", &value);
        if (error == 0 && count != nullptr) {
            *count = static_cast<std::int64_t>(value);
        }
        return error;
    }

    int Open(int device_index) override {
        return SI_Open(device_index, &handle_);
    }

    int Close() override {
        if (handle_ == 0) {
            return 0;
        }

        const int error = SI_Close(handle_);
        if (error == 0) {
            handle_ = 0;
        }
        return error;
    }

    int Command(const std::wstring& feature) override {
        return SI_Command(handle_, feature.c_str());
    }

    int SetFloat(const std::wstring& feature, double value) override {
        return SI_SetFloat(handle_, feature.c_str(), value);
    }

    int SetEnumIndex(const std::wstring& feature, int value) override {
        return SI_SetEnumIndex(handle_, feature.c_str(), value);
    }

    int GetInt(const std::wstring& feature, std::int64_t* value) override {
        SI_64 sdk_value = 0;
        const int error = SI_GetInt(handle_, feature.c_str(), &sdk_value);
        if (error == 0 && value != nullptr) {
            *value = static_cast<std::int64_t>(sdk_value);
        }
        return error;
    }

    int CreateBuffer(std::int64_t size_bytes, void** buffer) override {
        return SI_CreateBuffer(handle_, static_cast<SI_64>(size_bytes), buffer);
    }

    int DisposeBuffer(void* buffer) override {
        return SI_DisposeBuffer(handle_, buffer);
    }

    int Wait(std::uint8_t* buffer, std::int64_t* frame_size,
             std::int64_t* frame_number, std::int64_t timeout_ms) override {
        SI_64 sdk_frame_size = 0;
        SI_64 sdk_frame_number = 0;
        const int error =
            SI_Wait(handle_, buffer, &sdk_frame_size, &sdk_frame_number,
                    static_cast<SI_64>(timeout_ms));

        if (error == 0) {
            if (frame_size != nullptr) {
                *frame_size = static_cast<std::int64_t>(sdk_frame_size);
            }
            if (frame_number != nullptr) {
                *frame_number = static_cast<std::int64_t>(sdk_frame_number);
            }
        }

        return error;
    }

    const wchar_t* GetErrorString(int code) const override {
        return SI_GetErrorString(code);
    }

private:
    SI_H handle_ = 0;
    bool loaded_ = false;
};

#else

class SpecSensorApiSdk final : public ISpecSensorApi {
public:
    int Load(const std::wstring&) override { return -1; }
    int Unload() override { return 0; }
    int GetDeviceCount(std::int64_t*) override { return -1; }
    int Open(int) override { return -1; }
    int Close() override { return 0; }
    int Command(const std::wstring&) override { return -1; }
    int SetFloat(const std::wstring&, double) override { return -1; }
    int SetEnumIndex(const std::wstring&, int) override { return -1; }
    int GetInt(const std::wstring&, std::int64_t*) override { return -1; }
    int CreateBuffer(std::int64_t, void**) override { return -1; }
    int DisposeBuffer(void*) override { return 0; }
    int Wait(std::uint8_t*, std::int64_t*, std::int64_t*, std::int64_t) override {
        return -1;
    }
    const wchar_t* GetErrorString(int) const override {
        return L"SpecSensor SDK is only available in Windows builds.";
    }
};

#endif

std::unique_ptr<ISpecSensorApi> CreateSpecSensorApi() {
    return std::make_unique<SpecSensorApiSdk>();
}
