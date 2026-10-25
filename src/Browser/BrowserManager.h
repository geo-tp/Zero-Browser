#pragma once
#include "BrowserEngine.h"
#include "History/History.h"
#include "Memory/MemoryManager.h"
#include <memory>
namespace zero {
class BrowserManager {
  public:
    enum class State { Idle, Starting, Active, Stopping, Settling, Error };

  private:
    Config &config_;
    Paths paths_;
    WaylandBridge &input_;
    std::map<std::string, std::string> environment_;
    std::unique_ptr<BrowserEngine> engine_;
    History history_;
    MemoryManager memory_;
    State state_ = State::Idle;
    Mode mode_ = Mode::Lite, target_ = Mode::Lite;
    std::string message_, error_;
    double stateSince_ = 0, requested_ = 0, loadStarted_ = 0, nextSample_ = 0;
    bool loading_ = false, pendingNavigation_ = false, restart_ = false, quitting_ = false, blocked_ = false,
         crashed_ = false;
    int commandFailures_ = 0;
    MemorySample sample_;
    std::vector<EngineEvent> events_;
    bool overlayRequested_ = false;
    bool selectionRequested_ = false;
    std::function<void(Mode)> prepareDisplay_;
    std::string loadPhase_;
    std::string hoveredLink_;
    std::string zoomNotice_;
    double zoomNoticeUntil_ = 0;
    bool downloadsVisible_ = false;
    bool javascriptEnabled_ = true;
    int loadProgress_ = -1;
    double loadFinishedAt_ = 0, lastLoadSeconds_ = 0;
    void launch();
    void handle(const EngineEvent &);
    void persist();
    void transition(Mode, bool restart);

  public:
    BrowserManager(Config &, const Paths &, WaylandBridge &, const std::map<std::string, std::string> &,
                   std::function<void(Mode)> prepareDisplay = {});
    ~BrowserManager();
    void start(Mode, const std::string &url = "");
    void tick();
    void stop();
    bool stopped() { return !engine_ || engine_->stopped(); }
    bool clearHistory();
    void selectEngine() { error_.clear(); stop(); selectionRequested_ = true; }
    bool navigate(const std::string &);
    bool back();
    bool forward();
    bool reload();
    bool stopLoading();
    bool zoom(bool in) { return engine_ && active() && engine_->zoom(in); }
    bool toggleJavascript();
    bool javascriptEnabled() const { return javascriptEnabled_; }
    bool downloadsVisible() const { return downloadsVisible_; }
    bool showDownloads() { return engine_ && active() && engine_->showDownloads(); }
    bool closeDownloads() { return engine_ && active() && engine_->closeDownloads(); }
    const std::string &loadPhase() const { return loadPhase_; }
    const std::string &hoveredLink() const { return hoveredLink_; }
    const std::string &zoomNotice() const { return zoomNotice_; }
    bool zoomNoticeActive() const { return !zoomNotice_.empty() && now() < zoomNoticeUntil_; }
    int loadProgress() const { return loadProgress_; }
    bool recentLoad() const { return loadFinishedAt_ > 0 && now() - loadFinishedAt_ < 3; }
    double lastLoadSeconds() const { return lastLoadSeconds_; }
    void recover();
    void snapshot();
    void escape() { if (engine_ && active()) engine_->escape(); }
    void cancelEscape() { overlayRequested_ = false; if (engine_) engine_->cancelEscape(); }
    bool takeOverlayRequest() { bool value = overlayRequested_; overlayRequested_ = false; return value; }
    bool takeSelectionRequest() {
        if (!selectionRequested_ || state_ != State::Idle) return false;
        selectionRequested_ = false;
        return true;
    }
    State state() const {
        return state_;
    }
    Mode displayMode() const { return state_ == State::Starting || state_ == State::Stopping || state_ == State::Settling ? target_ : mode_; }
    Mode mode() const {
        return mode_;
    }
    bool active() const {
        return state_ == State::Active;
    }
    bool loading() const {
        return loading_;
    }
    bool recovering() const { return restart_ && state_ != State::Active; }
    const std::string &message() const {
        return message_;
    }
    const std::string &error() const {
        return error_;
    }
    const History &history() const {
        return history_;
    }
    const MemorySample &sample() const {
        return sample_;
    }
    MemoryState memoryState() const {
        return memory_.state();
    }
    double loadingSeconds() const { return loading_ ? now() - requested_ : 0; }
    bool loadStarted() const { return loadStarted_ > 0; }
    pid_t pid() const {
        return engine_ ? engine_->pid() : 0;
    }
};
} // namespace zero
