#include "app_config.h"

#include <sstream>

namespace {

bool IsValidBinning(int value) {
    return value == 1 || value == 2 || value == 4 || value == 8;
}

}  // namespace

bool ValidateConfig(const AppConfig& config, std::string* error) {
    std::ostringstream oss;

    if (config.license_path.empty()) {
        oss << "license_path must not be empty";
    } else if (config.device_index < 0) {
        oss << "device_index must be >= 0";
    } else if (config.exposure_ms <= 0.0) {
        oss << "exposure_ms must be > 0";
    } else if (config.frame_rate_hz <= 0.0) {
        oss << "frame_rate_hz must be > 0";
    } else if (!IsValidBinning(config.binning_spatial)) {
        oss << "binning_spatial must be one of: 1, 2, 4, 8";
    } else if (!IsValidBinning(config.binning_spectral)) {
        oss << "binning_spectral must be one of: 1, 2, 4, 8";
    } else if (config.calibration_scp_path.empty()) {
        oss << "calibration_scp_path must not be empty";
    } else if (config.output_dir.empty()) {
        oss << "output_dir must not be empty";
    } else if (config.rgb_wavelength_nm[0] <= 0 ||
               config.rgb_wavelength_nm[1] <= 0 ||
               config.rgb_wavelength_nm[2] <= 0) {
        oss << "rgb_wavelength_nm values must be > 0";
    } else if (config.capture_seconds <= 0) {
        oss << "capture_seconds must be > 0";
    } else if (config.dark_frames < 0) {
        oss << "dark_frames must be >= 0";
    } else if (config.wait_timeout_ms <= 0) {
        oss << "wait_timeout_ms must be > 0";
    } else if (config.min_buffers_required <= 0) {
        oss << "min_buffers_required must be > 0";
    } else if (config.save_queue_capacity <= 0) {
        oss << "save_queue_capacity must be > 0";
    } else if (config.save_block_frames <= 0) {
        oss << "save_block_frames must be > 0";
    } else if (config.save_queue_push_timeout_ms <= 0) {
        oss << "save_queue_push_timeout_ms must be > 0";
    } else if (config.camera_name.empty()) {
        oss << "camera_name must not be empty";
    } else if (config.pipe_name.empty()) {
        oss << "pipe_name must not be empty";
    } else if (config.log_file_path.empty()) {
        oss << "log_file_path must not be empty";
    }

    if (error != nullptr) {
        *error = oss.str();
    }

    return oss.str().empty();
}
