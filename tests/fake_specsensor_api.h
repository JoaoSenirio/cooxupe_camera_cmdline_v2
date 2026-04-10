#ifndef FAKE_SPECSENSOR_API_H
#define FAKE_SPECSENSOR_API_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
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
        operations.push_back(L"Command:" + feature);
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

    int SetBool(const std::wstring& feature, bool value) override {
        operations.push_back(L"SetBool:" + feature);
        if (feature == L"Camera.ExposureTime.Auto") {
            if (set_exposure_time_auto_error != 0) {
                return set_exposure_time_auto_error;
            }
            exposure_time_auto = value;
        }
        return 0;
    }

    int SetFloat(const std::wstring& feature, double value) override {
        operations.push_back(L"SetFloat:" + feature);
        if (feature == L"Camera.ExposureTime") {
            if (set_exposure_time_error != 0) {
                return set_exposure_time_error;
            }
            exposure_time = value;
        } else if (feature == L"Camera.FrameRate") {
            if (set_frame_rate_error != 0) {
                return set_frame_rate_error;
            }
            frame_rate = value;
        }
        return 0;
    }

    int SetString(const std::wstring& feature, const std::wstring& value) override {
        operations.push_back(L"SetString:" + feature);
        if (feature == L"Camera.CalibrationPack") {
            calibration_pack = value;
        }
        return 0;
    }

    int SetEnumIndex(const std::wstring& feature, int value) override {
        operations.push_back(L"SetEnumIndex:" + feature);
        if (feature == L"Camera.Trigger.Mode") {
            if (value < 0 || value >= static_cast<int>(trigger_modes.size())) {
                return -2;
            }
            trigger_mode_index = value;
            return 0;
        }
        if (feature == L"Camera.Binning.Spatial") {
            spatial_binning_index = value;
        } else if (feature == L"Camera.Binning.Spectral") {
            spectral_binning_index = value;
        }
        return 0;
    }

    int GetInt(const std::wstring& feature, std::int64_t* value) override {
        if (feature == L"Camera.Image.Width") {
            if (value != nullptr) {
                *value = image_width;
            }
            return 0;
        }
        if (feature == L"Camera.Image.Height") {
            if (value != nullptr) {
                *value = image_height;
            }
            return 0;
        }
        if (feature == L"Camera.Image.SizeBytes") {
            if (value != nullptr) {
                *value = frame_size_bytes;
            }
            return 0;
        }
        if (feature == L"Camera.SensorID") {
            if (value != nullptr) {
                *value = sensor_id;
            }
            return 0;
        }
        if (feature == L"AcquisitionWindow.Left" || feature == L"AcquisitionWindow.Top") {
            if (value != nullptr) {
                *value = 0;
            }
            return 0;
        }
        return -2;
    }

    int GetBool(const std::wstring& feature, bool* value) override {
        if (feature == L"Camera.ExposureTime.Auto") {
            if (value != nullptr) {
                *value = exposure_time_auto;
            }
            return 0;
        }
        return -2;
    }

    int GetFloat(const std::wstring& feature, double* value) override {
        if (feature == L"Camera.ExposureTime") {
            if (value != nullptr) {
                *value = override_exposure_readback ? exposure_readback_value : exposure_time;
            }
            return 0;
        }
        if (feature == L"Camera.FrameRate") {
            if (value != nullptr) {
                *value = override_frame_rate_readback ? frame_rate_readback_value : frame_rate;
            }
            return 0;
        }
        if (feature == L"Camera.Image.ReadoutTime") {
            if (value != nullptr) {
                *value = image_readout_time_ms;
            }
            return 0;
        }
        if (feature == L"Camera.Temperature") {
            if (value != nullptr) {
                *value = temperature_c;
            }
            return 0;
        }
        if (feature == L"Acquisition.CalculatedFrameRate") {
            if (!calculated_frame_rate_supported) {
                return -2;
            }
            if (value != nullptr) {
                *value = calculated_frame_rate_value > 0.0 ? calculated_frame_rate_value : frame_rate;
            }
            return 0;
        }
        return -2;
    }

    int GetEnumIndex(const std::wstring& feature, int* value) override {
        if (feature == L"Camera.Trigger.Mode") {
            if (value != nullptr) {
                *value = override_trigger_mode_readback ? trigger_mode_readback_index
                                                       : trigger_mode_index;
            }
            return 0;
        }
        if (feature == L"Camera.Binning.Spatial") {
            if (value != nullptr) {
                *value = spatial_binning_index;
            }
            return 0;
        }
        if (feature == L"Camera.Binning.Spectral") {
            if (value != nullptr) {
                *value = spectral_binning_index;
            }
            return 0;
        }
        return -2;
    }

    int GetEnumCount(const std::wstring& feature, int* count) override {
        if (feature == L"Camera.Trigger.Mode") {
            if (count != nullptr) {
                *count = static_cast<int>(trigger_modes.size());
            }
            return 0;
        }
        if (feature == L"Camera.WavelengthTable") {
            if (count != nullptr) {
                *count = static_cast<int>(wavelength_table_values.size());
            }
            return 0;
        }
        if (feature == L"Camera.FWHM") {
            if (count != nullptr) {
                *count = static_cast<int>(fwhm_values.size());
            }
            return 0;
        }
        if (count != nullptr) {
            *count = 0;
        }
        return 0;
    }

    int GetEnumStringByIndex(const std::wstring& feature, int index, std::wstring* value) override {
        if (feature == L"Camera.Trigger.Mode") {
            if (index < 0 || index >= static_cast<int>(trigger_modes.size())) {
                return -2;
            }
            if (value != nullptr) {
                *value = trigger_modes[static_cast<std::size_t>(index)];
            }
            return 0;
        }
        if (feature == L"Camera.WavelengthTable") {
            if (index < 0 || index >= static_cast<int>(wavelength_table_values.size())) {
                return -2;
            }
            if (value != nullptr) {
                *value = wavelength_table_values[static_cast<std::size_t>(index)];
            }
            return 0;
        }
        if (feature == L"Camera.FWHM") {
            if (index < 0 || index >= static_cast<int>(fwhm_values.size())) {
                return -2;
            }
            if (value != nullptr) {
                *value = fwhm_values[static_cast<std::size_t>(index)];
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

        if (wait_delay_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(wait_delay_us));
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

    int CountCommand(const std::wstring& name) const {
        return static_cast<int>(std::count(commands.begin(), commands.end(), name));
    }

    int IndexOfOperation(const std::wstring& name) const {
        const auto it = std::find(operations.begin(), operations.end(), name);
        if (it == operations.end()) {
            return -1;
        }
        return static_cast<int>(std::distance(operations.begin(), it));
    }

    bool loaded = false;
    bool opened = false;
    bool initialized = false;
    bool acquiring = false;
    std::int64_t device_count = 2;
    int open_index = -1;
    double exposure_time = 0.0;
    double frame_rate = 0.0;
    bool exposure_time_auto = false;
    int set_exposure_time_auto_error = 0;
    int set_exposure_time_error = 0;
    int set_frame_rate_error = 0;
    std::wstring calibration_pack;
    int spatial_binning_index = -1;
    int spectral_binning_index = -1;
    std::int64_t image_width = 64;
    std::int64_t image_height = 32;
    std::int64_t frame_size_bytes = 4096;
    std::int64_t sensor_id = 12345;
    double image_readout_time_ms = 6.137;
    double temperature_c = 27.5;
    int wait_delay_us = 0;
    bool override_exposure_readback = false;
    double exposure_readback_value = 0.0;
    bool override_frame_rate_readback = false;
    double frame_rate_readback_value = 0.0;
    bool calculated_frame_rate_supported = true;
    double calculated_frame_rate_value = 0.0;
    bool override_trigger_mode_readback = false;
    int trigger_mode_readback_index = 0;
    std::wstring load_license_path;
    std::vector<std::wstring> commands;
    std::vector<std::wstring> operations;
    std::vector<std::wstring> trigger_modes{L"Internal", L"External"};
    std::vector<std::wstring> wavelength_table_values;
    std::vector<std::wstring> fwhm_values;
    int trigger_mode_index = 0;

private:
    std::atomic<std::int64_t> current_frame{0};
    std::vector<std::uint8_t> allocated_buffer;
};

#endif
