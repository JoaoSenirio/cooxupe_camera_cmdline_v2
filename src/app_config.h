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
    int save_queue_capacity;
    int save_block_frames;
    int save_queue_push_timeout_ms;
    std::string camera_name;
    std::string pipe_name;
    std::string log_file_path;
};

inline AppConfig MakeDefaultConfig() {
    AppConfig config{};
    config.license_path = L"C:/Users/Public/Documents/Specim/SpecSensor.lic";
    config.device_index = 5;
    config.exposure_ms = 6.5;
    config.frame_rate_hz = 150.0;
    config.binning_spatial = 2;
    config.binning_spectral = 1;
    config.calibration_scp_path = L"E:/Calibrations/4210325_20211207_calpack.scp";
    config.output_dir = "C:/SpecSensor/output";
    config.rgb_wavelength_nm[0] = 1039;
    config.rgb_wavelength_nm[1] = 1370;
    config.rgb_wavelength_nm[2] = 1625;
    config.capture_seconds = 40;
    config.dark_frames = 50;
    config.wait_timeout_ms = 1000;
    config.min_buffers_required = 5000;
    config.save_queue_capacity = 200;
    config.save_block_frames = 64;
    config.save_queue_push_timeout_ms = 2000;
    config.camera_name = "FX17";
    config.pipe_name = "\\\\.\\pipe\\specsensor_sample_pipe";
    config.log_file_path = "C:/SpecimOutput/specsensor_cli.log";
    return config;
}

bool ValidateConfig(const AppConfig& config, std::string* error);

#endif
