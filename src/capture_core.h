#ifndef CAPTURE_CORE_H
#define CAPTURE_CORE_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

#include "app_config.h"
#include "specsensor_api.h"
#include "thread_queue.h"
#include "types.h"

int BinningValueToEnumIndex(int binning_value);

class CaptureCore {
public:
    using SummaryCallback = std::function<void(const AcquisitionSummary&)>;

    CaptureCore(const AppConfig& config, ISpecSensorApi* api);
    ~CaptureCore();

    bool start();
    void stop();

    bool enqueue_job(const AcquisitionJob& job);
    void set_summary_callback(SummaryCallback callback);

private:
    void worker_loop();
    int initialize_sensor();
    void cleanup_sensor();

    AcquisitionSummary run_workflow(const AcquisitionJob& job);
    int wait_for_frame(AcquisitionSummary* summary, CapturePhase phase);

    const AppConfig config_;
    ISpecSensorApi* api_;

    ThreadQueue<AcquisitionJob> jobs_;
    SummaryCallback on_summary_;

    std::thread worker_;
    std::atomic<bool> started_{false};

    void* frame_buffer_ = nullptr;
    std::int64_t frame_buffer_size_ = 0;
    bool sensor_ready_ = false;
};

#endif
