#include "runtime_lifecycle.h"
#include "test_support.h"

namespace {

int TestBootstrapGatesBackgroundStartup() {
    RuntimeLifecycle lifecycle;
    TEST_ASSERT(lifecycle.state() == RuntimeState::BootstrapInteractive,
                "runtime should start in bootstrap state");
    TEST_ASSERT(!lifecycle.background_workers_may_start(),
                "workers must stay stopped before bootstrap succeeds");
    TEST_ASSERT(!lifecycle.pipe_should_run(),
                "pipe must stay offline before bootstrap succeeds");

    lifecycle.BootstrapSucceeded();

    TEST_ASSERT(lifecycle.state() == RuntimeState::ReadyBackground,
                "successful bootstrap should enter ready background state");
    TEST_ASSERT(lifecycle.background_workers_may_start(),
                "workers may start only after bootstrap succeeds");
    TEST_ASSERT(lifecycle.pipe_should_run(),
                "pipe should run after bootstrap succeeds");
    TEST_ASSERT(lifecycle.can_accept_pipe_commands(),
                "ready background state should accept pipe commands");
    return 0;
}

int TestBootstrapFailureStopsRuntime() {
    RuntimeLifecycle lifecycle;
    lifecycle.BootstrapFailed();

    TEST_ASSERT(lifecycle.state() == RuntimeState::Shutdown,
                "bootstrap failure should transition directly to shutdown");
    TEST_ASSERT(!lifecycle.background_workers_may_start(),
                "failed bootstrap must block worker startup");
    TEST_ASSERT(!lifecycle.pipe_should_run(),
                "failed bootstrap must block pipe startup");
    return 0;
}

int TestBusyAndFatalTransitions() {
    RuntimeLifecycle lifecycle;
    lifecycle.BootstrapSucceeded();
    lifecycle.CaptureStarted();

    TEST_ASSERT(lifecycle.state() == RuntimeState::Busy,
                "capture start should move runtime to busy");
    TEST_ASSERT(!lifecycle.can_accept_pipe_commands(),
                "busy state must reject new pipe jobs");

    lifecycle.WorkflowFinished(true);
    TEST_ASSERT(lifecycle.state() == RuntimeState::ReadyBackground,
                "successful workflow should return to ready background");
    TEST_ASSERT(lifecycle.can_accept_pipe_commands(),
                "ready state should resume accepting commands");

    lifecycle.CaptureStarted();
    lifecycle.WorkflowFinished(false);
    TEST_ASSERT(lifecycle.state() == RuntimeState::FatalError,
                "failed workflow should enter fatal error state");
    TEST_ASSERT(!lifecycle.pipe_should_run(),
                "fatal error should stop further pipe use");
    return 0;
}

}  // namespace

int main() {
    return RunSuite("test_runtime_lifecycle",
                    {
                        {"bootstrap_gates_background_startup", &TestBootstrapGatesBackgroundStartup},
                        {"bootstrap_failure_stops_runtime", &TestBootstrapFailureStopsRuntime},
                        {"busy_and_fatal_transitions", &TestBusyAndFatalTransitions},
                    });
}
