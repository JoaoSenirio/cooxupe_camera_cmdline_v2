#include "runtime_lifecycle.h"

RuntimeLifecycle::RuntimeLifecycle() = default;

void RuntimeLifecycle::BootstrapSucceeded() {
    bootstrap_succeeded_ = true;
    state_ = RuntimeState::ReadyBackground;
}

void RuntimeLifecycle::BootstrapFailed() {
    bootstrap_succeeded_ = false;
    state_ = RuntimeState::Shutdown;
}

void RuntimeLifecycle::CaptureStarted() {
    if (state_ == RuntimeState::ReadyBackground) {
        state_ = RuntimeState::Busy;
    }
}

void RuntimeLifecycle::WorkflowFinished(bool success) {
    if (!bootstrap_succeeded_) {
        state_ = RuntimeState::Shutdown;
        return;
    }

    state_ = success ? RuntimeState::ReadyBackground : RuntimeState::FatalError;
}

void RuntimeLifecycle::FatalErrorOccurred() {
    if (state_ != RuntimeState::Shutdown) {
        state_ = RuntimeState::FatalError;
    }
}

void RuntimeLifecycle::ShutdownRequested() {
    state_ = RuntimeState::Shutdown;
}

RuntimeState RuntimeLifecycle::state() const {
    return state_;
}

bool RuntimeLifecycle::background_workers_may_start() const {
    return bootstrap_succeeded_ && state_ != RuntimeState::Shutdown;
}

bool RuntimeLifecycle::pipe_should_run() const {
    return bootstrap_succeeded_ &&
           state_ != RuntimeState::FatalError &&
           state_ != RuntimeState::Shutdown;
}

bool RuntimeLifecycle::can_accept_pipe_commands() const {
    return state_ == RuntimeState::ReadyBackground;
}
