#ifndef SAVE_CORE_H
#define SAVE_CORE_H

#include <atomic>
#include <thread>

#include "thread_queue.h"
#include "types.h"

class SaveCore {
public:
    SaveCore();
    ~SaveCore();

    bool start();
    void stop();

    bool enqueue_summary(const AcquisitionSummary& summary);

private:
    void worker_loop();

    ThreadQueue<AcquisitionSummary> summaries_;
    std::thread worker_;
    std::atomic<bool> started_{false};
};

#endif
