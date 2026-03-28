#ifndef UI_ENGINE_H
#define UI_ENGINE_H

#include <functional>
#include <memory>

#include "types.h"

class UiEngine {
public:
    using CommandSink = std::function<void(const UiCommand&)>;

    UiEngine();
    ~UiEngine();

    bool start(CommandSink command_sink);
    void stop();
    bool publish(const UiEvent& event);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
