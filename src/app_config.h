#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <string>

struct AppConfig {
    std::wstring license_path;
    int device_index;
    double exposure_ms;
    double frame_rate_hz;
    int binning_spatial;
    int binning_spectral;
    std::wstring calibration_scp_path;
    std::string output_dir;
    int rgb_wavelength_nm[3];
    int capture_seconds;
    int dark_frames;
    int wait_timeout_ms;
    int min_buffers_required;
    std::string pipe_name;
    std::string log_file_path;
};

inline AppConfig MakeDefaultConfig() {
    AppConfig config{};
    config.license_path = L"C:/Users/Public/Documents/Specim/SpecSensor.lic";
    config.device_index = 10;
    config.exposure_ms = 4.0;
    config.frame_rate_hz = 120.0;
    config.binning_spatial = 1;
    config.binning_spectral = 1;
    config.calibration_scp_path = L"E:/Calibrations/3210495_20220310_calpack.scp";
    config.output_dir = "C:/SpecSensor/output";
    config.rgb_wavelength_nm[0] = 650;
    config.rgb_wavelength_nm[1] = 550;
    config.rgb_wavelength_nm[2] = 450;
    config.capture_seconds = 2;
    config.dark_frames = 100;
    config.wait_timeout_ms = 1000;
    config.min_buffers_required = 5000;
    config.pipe_name = "\\\\.\\pipe\\specsensor_sample_pipe";
    config.log_file_path = "C:SpecimOutput/specsensor_cli.log";
    return config;
}

bool ValidateConfig(const AppConfig& config, std::string* error);

#endif
