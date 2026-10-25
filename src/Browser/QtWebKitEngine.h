#pragma once
#include "BrowserEngine.h"
#include <fstream>

namespace zero {
// Qt lives in a standalone helper; the supervisor never loads Qt libraries.
class QtWebKitEngine final : public BrowserEngine {
    int control_ = -1;
    bool ready_ = false, loading_ = false;
    double nextSample_ = 0, sampledAt_ = 0, lastHeartbeat_ = 0;
    double nextPressureCheck_ = 0, pressureSince_ = -1;
    bool heartbeatSeen_ = false;
    uint64_t cpuTicks_ = 0;
    std::ofstream diagnostics_;
    bool send(const std::string &);
    void sample();
    void record(const std::string &, const std::string &);

  public:
    using BrowserEngine::BrowserEngine;
    ~QtWebKitEngine() override;
    void start(const std::string &) override;
    bool open(const std::string &) override;
    bool reload() override;
    bool stopLoading() override { return ready_ && send("stop"); }
    bool zoom(bool in) override { return ready_ && send(in ? "zoom-in" : "zoom-out"); }
    bool toggleJavascript() override { return ready_ && send("toggle-js"); }
    bool showDownloads() override { return ready_ && send("downloads"); }
    bool goBack() override { return ready_ && send("back"); }
    bool goForward() override { return ready_ && send("forward"); }
    bool closeDownloads() override { return ready_ && send("close-downloads"); }
    void tick() override;
    void stop() override;
    static bool parseLine(const std::string &, EngineEvent &);
};
} // namespace zero
