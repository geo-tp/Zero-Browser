#pragma once
#include "BrowserEngine.h"
#include "NetSurfEscape.h"

namespace zero {
class NetSurfEngine final : public BrowserEngine {
    NetSurfEscape escape_{input_};
    double readyAt_ = 0, actionAt_ = 0;
    int action_ = 0;
    std::string address_;
    bool ready_ = false;

  public:
    using BrowserEngine::BrowserEngine;
    void start(const std::string &) override;
    bool open(const std::string &) override;
    bool reload() override;
    void tick() override;
    void snapshot() override;
    void escape() override;
    void cancelEscape() override { escape_.cancel(); }
    void stop() override;
};
} // namespace zero
