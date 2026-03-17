#ifndef CAPTURE_CORE_H
#define CAPTURE_CORE_H

#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>

#include "app_config.h"
#include "specsensor_api.h"
#include "types.h"

int BinningValueToEnumIndex(int binning_value);

class CaptureCore {
public:
    CaptureCore(const AppConfig& config, ISpecSensorApi* api);
    ~CaptureCore();

    bool Initialize();
    bool CaptureSample(const AcquisitionJob& job, AcquisitionSummary* summary);
    void Shutdown();
    void set_save_sink(std::function<bool(const SaveEvent&)> save_sink);

    void RequestStop();
    bool StopRequested() const;

    void LogInfo(const std::string& message);
    void LogError(const std::string& message);

private:
    bool OpenLogFile();

    bool ConnectCamera();
    bool ConfigureCameraParameters();

    int RunCameraCommand(const wchar_t* command, const char* step);
    int DisposeFrameBuffer(void* frame_buffer);
    bool EmitSaveEvent(const SaveEvent& event, AcquisitionSummary* summary);
    bool FillSensorSnapshot(SensorSnapshot* snapshot);

    void LogApiFailure(const char* step, int code);
    void LogMessage(const char* level, const std::string& message);
    static std::string Narrow(const wchar_t* text);

    const AppConfig config_;
    ISpecSensorApi* api_;
    bool initialized_ = false;
    bool shutdown_done_ = false;
    std::atomic<bool> stop_requested_{false};
    std::uint64_t next_job_id_ = 1;
    std::function<bool(const SaveEvent&)> save_sink_;

    mutable std::mutex log_mutex_;
    std::ofstream log_file_;
};

#endif
