#pragma once
#include "Config/Config.h"
#include "Display/WaylandBridge.h"
#include "Utils/Process.h"
#include <functional>
namespace zero {
enum class Mode { Lite, QtWebKit, Full };
inline const char *modeName(Mode m) {
    return m == Mode::Lite ? "LITE" : m == Mode::QtWebKit ? "QTWEBKIT" : "FULL";
}
struct EngineEvent {
    enum Kind { LoadStarted, LoadProgress, Redirected, Loaded, Failed, CommandDone, Ready, Crashed, UrlSnapshot, OverlayRequested } kind;
    std::string url, message;
    double seconds = 0;
    int progress = -1;
};
class BrowserEngine {
  protected:
    Process process_, command_;
    const Config &config_;
    WaylandBridge &input_;
    const Paths &paths_;
    std::map<std::string, std::string> environment_;
    std::function<void(const EngineEvent &)> emit_;
    std::string partial_;
    bool stopping_ = false, crashReported_ = false;
    double launched_ = 0, commandStarted_ = 0;
    std::string commandName_;
    void pollCommand();

  public:
    BrowserEngine(const Config &c, WaylandBridge &i, const Paths &p,
                  const std::map<std::string, std::string> &env, std::function<void(const EngineEvent &)> cb)
        : config_(c), input_(i), paths_(p), environment_(env), emit_(std::move(cb)) {}
    virtual ~BrowserEngine() = default;
    virtual void start(const std::string &url) = 0;
    virtual bool open(const std::string &url) = 0;
    virtual bool reload() = 0;
    virtual bool stopLoading() { return false; }
    virtual bool goBack() { return false; }
    virtual bool goForward() { return false; }
    virtual bool zoom(bool) { return false; }
    virtual bool toggleJavascript() { return false; }
    virtual bool showDownloads() { return false; }
    virtual bool closeDownloads() { return false; }
    virtual void snapshot() {}
    virtual void escape() {}
    virtual void cancelEscape() {}
    virtual void tick() = 0;
    virtual void stop();
    bool stopped();
    std::vector<pid_t> pids() {
        return process_.members();
    }
    pid_t pid() const {
        return process_.pid();
    }
};
} // namespace zero
