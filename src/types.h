#ifndef TYPES_H
#define TYPES_H

#include <cstdint>
#include <string>

struct AcquisitionJob {
    std::string sample_name;
};

enum class CapturePhase {
    Light,
    Dark
};

struct AcquisitionSummary {
    std::string sample_name;
    std::int64_t light_buffers = 0;
    std::int64_t dark_buffers = 0;
    std::int64_t total_buffers = 0;
    std::int64_t last_frame_number = 0;
    bool pass = false;
    int sdk_error = 0;
    std::string message;
};

#endif
