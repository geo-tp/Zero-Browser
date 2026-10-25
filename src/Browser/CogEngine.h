#pragma once
#include "BrowserEngine.h"

namespace zero {
class CogEngine final : public BrowserEngine {
    bool ready_ = false;
    bool control(const std::string &, const std::string &arg = "");

  public:
    using BrowserEngine::BrowserEngine;
    void start(const std::string &) override;
    bool open(const std::string &) override;
    bool reload() override;
    bool zoom(bool in) override;
    void tick() override;
    void stop() override;
    static bool parseLine(const std::string &, EngineEvent &);
};
} // namespace zero
