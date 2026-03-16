#ifndef FAKE_SPECSENSOR_API_H
#define FAKE_SPECSENSOR_API_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "specsensor_api.h"

class FakeSpecSensorApi final : public ISpecSensorApi {
public:
    int Load(const std::wstring& license_path) override {
        loaded = true;
        load_license_path = license_path;
        return 0;
    }

    int Unload() override {
        loaded = false;
        return 0;
    }

    int GetDeviceCount(std::int64_t* count) override {
        if (count != nullptr) {
            *count = device_count;
        }
        return 0;
    }

    int Open(int device_index) override {
        open_index = device_index;
        opened = true;
        return 0;
    }

    int Close() override {
        opened = false;
        return 0;
    }

    int Command(const std::wstring& feature) override {
        commands.push_back(feature);
        if (feature == L"Acquisition.Start") {
            acquiring = true;
            return 0;
        }
        if (feature == L"Acquisition.Stop") {
            acquiring = false;
            return 0;
        }
        if (feature == L"Acquisition.RingBuffer.Sync") {
            return 0;
        }
        if (feature == L"Initialize") {
            initialized = true;
            return 0;
        }
        if (feature == L"Camera.OpenShutter" || feature == L"Camera.CloseShutter") {
            return 0;
        }
        return 0;
    }

    int SetFloat(const std::wstring& feature, double value) override {
        if (feature == L"Camera.ExposureTime") {
            exposure_time = value;
        } else if (feature == L"Camera.FrameRate") {
            frame_rate = value;
        }
        return 0;
    }

    int SetString(const std::wstring& feature, const std::wstring& value) override {
        if (feature == L"Camera.CalibrationPack") {
            calibration_pack = value;
        }
        return 0;
    }

    int SetEnumIndex(const std::wstring& feature, int value) override {
        if (feature == L"Camera.Binning.Spatial") {
            spatial_binning_index = value;
        } else if (feature == L"Camera.Binning.Spectral") {
            spectral_binning_index = value;
        }
        return 0;
    }

    int GetInt(const std::wstring& feature, std::int64_t* value) override {
        if (feature == L"Camera.Image.SizeBytes") {
            if (value != nullptr) {
                *value = frame_size_bytes;
            }
            return 0;
        }
        return -2;
    }

    int CreateBuffer(std::int64_t size_bytes, void** buffer) override {
        allocated_buffer.resize(static_cast<std::size_t>(size_bytes));
        if (buffer != nullptr) {
            *buffer = allocated_buffer.data();
        }
        return 0;
    }

    int DisposeBuffer(void*) override {
        allocated_buffer.clear();
        allocated_buffer.shrink_to_fit();
        return 0;
    }

    int Wait(std::uint8_t*, std::int64_t* frame_size,
             std::int64_t* frame_number, std::int64_t) override {
        if (!acquiring) {
            return -3;
        }

        const std::int64_t next = ++current_frame;
        if (frame_size != nullptr) {
            *frame_size = frame_size_bytes;
        }
        if (frame_number != nullptr) {
            *frame_number = next;
        }
        return 0;
    }

    const wchar_t* GetErrorString(int) const override {
        return L"fake-error";
    }

    bool HasCommand(const std::wstring& name) const {
        return std::find(commands.begin(), commands.end(), name) != commands.end();
    }

    bool loaded = false;
    bool opened = false;
    bool initialized = false;
    bool acquiring = false;
    std::int64_t device_count = 2;
    int open_index = -1;
    double exposure_time = 0.0;
    double frame_rate = 0.0;
    std::wstring calibration_pack;
    int spatial_binning_index = -1;
    int spectral_binning_index = -1;
    std::int64_t frame_size_bytes = 4096;
    std::wstring load_license_path;
    std::vector<std::wstring> commands;

private:
    std::atomic<std::int64_t> current_frame{0};
    std::vector<std::uint8_t> allocated_buffer;
};

#endif
