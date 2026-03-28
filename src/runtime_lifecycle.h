#ifndef RUNTIME_LIFECYCLE_H
#define RUNTIME_LIFECYCLE_H

#include "types.h"

class RuntimeLifecycle {
public:
    RuntimeLifecycle();

    void BootstrapSucceeded();
    void BootstrapFailed();
    void CaptureStarted();
    void WorkflowFinished(bool success);
    void FatalErrorOccurred();
    void ShutdownRequested();

    RuntimeState state() const;
    bool background_workers_may_start() const;
    bool pipe_should_run() const;
    bool can_accept_pipe_commands() const;

private:
    RuntimeState state_ = RuntimeState::BootstrapInteractive;
    bool bootstrap_succeeded_ = false;
};

#endif
