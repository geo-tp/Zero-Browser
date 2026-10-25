#include "BrowserManager.h"
#include "CogEngine.h"
#include "NetSurfEngine.h"
#include "QtWebKitEngine.h"
#include <thread>
namespace zero {
BrowserManager::BrowserManager(Config &c, const Paths &p, WaylandBridge &i,
                               const std::map<std::string, std::string> &env,
                               std::function<void(Mode)> prepareDisplay)
    : config_(c), paths_(p), input_(i), environment_(env), memory_(c), prepareDisplay_(std::move(prepareDisplay)) {
    history_.load(paths_.state / "history");
    // Do not restore a page that was never confirmed loaded.
    const auto confirmed = readFile(paths_.state / "last-success", 8192);
    if (confirmed.empty() || confirmed != history_.current())
        history_.navigate(config_.home, "HOME");
}
BrowserManager::~BrowserManager() {
    stop();
    double end = now() + config_.stopTimeout + 2;
    while (engine_ && !engine_->stopped() && now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
void BrowserManager::persist() {
    try {
        history_.save(paths_.state / "history");
    } catch (const std::exception &e) {
        log("history.save_error", e.what());
        error_ = "History could not be saved";
    }
}
void BrowserManager::launch() {
    if (engine_ && !engine_->stopped())
        throw std::runtime_error("Previous browser still owns processes");
    engine_.reset();
    mode_ = target_;
    error_.clear();
    message_ = "Starting " + std::string(modeName(mode_)) + "...";
    state_ = State::Starting;
    stateSince_ = now();
    requested_ = now();
    loadStarted_ = 0;
    loadPhase_ = "Fetching";
    hoveredLink_.clear();
    downloadsVisible_ = false;
    zoomNotice_.clear();
    zoomNoticeUntil_ = 0;
    loadProgress_ = 0;
    loadFinishedAt_ = 0;
    pendingNavigation_ = true;
    loading_ = mode_ != Mode::Lite;
    crashed_ = false;
    javascriptEnabled_ = true;
    auto cb = [this](const EngineEvent &e) { events_.push_back(e); };
    if (mode_ == Mode::Full)
        engine_ = std::make_unique<CogEngine>(config_, input_, paths_, environment_, cb);
    else if (mode_ == Mode::QtWebKit)
        engine_ = std::make_unique<QtWebKitEngine>(config_, input_, paths_, environment_, cb);
    else
        engine_ = std::make_unique<NetSurfEngine>(config_, input_, paths_, environment_, cb);
    if (history_.current().empty())
        history_.navigate(config_.home, modeName(mode_));
    try {
        if (prepareDisplay_) prepareDisplay_(mode_);
        engine_->start(history_.current());
    } catch (const std::exception &e) {
        handle({EngineEvent::Crashed, "", std::string(modeName(mode_)) + ": " + e.what()});
    }
    persist();
}
void BrowserManager::start(Mode mode, const std::string &url) {
    const auto initialUrl = url.empty() ? std::string{} : normalizeUrl(url);
    quitting_ = false;
    selectionRequested_ = false;
    if (!initialUrl.empty()) history_.navigate(initialUrl, modeName(mode));
    if (engine_) {
        transition(mode, false);
        return;
    }
    target_ = mode;
    // Fresh launches use the engine's homepage unless --url overrides it.
    // Engine switches and recovery retain the current page through transition().
    if (initialUrl.empty())
        history_.navigate(mode == Mode::Full ? "https://example.com/" : "https://lite.duckduckgo.com/lite/",
                          modeName(mode));
    launch();
}
bool BrowserManager::toggleJavascript() {
    if (mode_ != Mode::QtWebKit || !active() || !engine_) return false;
    if (!engine_->toggleJavascript()) return false;
    javascriptEnabled_ = !javascriptEnabled_;
    zoomNotice_ = javascriptEnabled_ ? "Reloading with JavaScript" : "Reloading without JavaScript";
    zoomNoticeUntil_ = now() + 3;
    hoveredLink_.clear();
    return true;
}
void BrowserManager::handle(const EngineEvent &e) {
    if (state_ == State::Stopping || state_ == State::Settling)
        return;
    // WebKit also reports LoadStarted for redirects, scripts and reloads.
    // Never recycle from a page event: doing so can replay the same script
    // forever (e.g. Reddit). Only explicit navigation and bounded crash
    // recovery may replace the Cog process.
    if (e.kind == EngineEvent::OverlayRequested) {
        overlayRequested_ = true;
    } else if (e.kind == EngineEvent::Ready) {
        state_ = State::Active;
        message_.clear();
        log("engine.ready", "mode=" + std::string(modeName(mode_)) + " seconds=" + std::to_string(e.seconds) +
            " windows=" + std::to_string(input_.toplevels().size()) +
            " captured_frames=" + std::to_string(input_.capturedFrames()));
        if (mode_ == Mode::Lite) {
            if (!history_.current().empty()) atomicWrite(paths_.state / "last-success", history_.current());
            loading_ = false;
            pendingNavigation_ = false;
        }
    } else if (e.kind == EngineEvent::CommandDone) {
        commandFailures_ = 0;
        log("command.done", e.message + " seconds=" + std::to_string(e.seconds));
        if (mode_ == Mode::Lite && e.message.rfind("open", 0) == 0)
            pendingNavigation_ = false;
        if (mode_ == Mode::QtWebKit && e.message.rfind("hover:", 0) == 0)
            hoveredLink_ = e.message.substr(6);
        if (mode_ == Mode::QtWebKit && e.message == "downloads:open") { downloadsVisible_ = true; hoveredLink_.clear(); }
        if (mode_ == Mode::QtWebKit && e.message == "downloads:closed") downloadsVisible_ = false;
        if (mode_ == Mode::QtWebKit && e.message == "rendered" && !loading_ && loadPhase_ == "Rendering") {
            loadPhase_ = "Loaded";
            loadFinishedAt_ = now();
        }
        if (mode_ == Mode::QtWebKit && e.message.rfind("LOW RAM:", 0) == 0) {
            zoomNotice_ = "LOW RAM: Zoom saved - Reload to apply";
            zoomNoticeUntil_ = now() + 7;
            hoveredLink_.clear();
        }
    } else if (e.kind == EngineEvent::LoadStarted) {
        hoveredLink_.clear();
        if (pendingNavigation_) {
            if (!e.url.empty())
                history_.redirect(e.url, modeName(mode_));
        } else if (!e.url.empty()) {
            history_.navigate(e.url, modeName(mode_));
            requested_ = now();
        }
        pendingNavigation_ = false;
        loading_ = true;
        loadPhase_ = "Fetching";
        loadProgress_ = 0;
        loadFinishedAt_ = 0;
        loadStarted_ = now();
        error_.clear();
        log("load.started", "dispatch_to_start=" + std::to_string(now() - requested_));
        persist();
    } else if (e.kind == EngineEvent::LoadProgress) {
        if (loading_ || (mode_ == Mode::QtWebKit && e.message == "Rendering")) {
            loadPhase_ = e.message;
            loadProgress_ = e.progress;
        }
    } else if (e.kind == EngineEvent::Redirected) {
        if (!e.url.empty()) {
            history_.redirect(e.url, modeName(mode_), mode_ == Mode::QtWebKit);
            persist();
        }
    } else if (e.kind == EngineEvent::Loaded) {
        if (!e.url.empty())
            history_.redirect(e.url, modeName(mode_));
        loading_ = false;
        loadPhase_ = "Loaded";
        loadProgress_ = 100;
        loadFinishedAt_ = now();
        lastLoadSeconds_ = mode_ == Mode::QtWebKit ? e.seconds : now() - requested_;
        pendingNavigation_ = false;
        commandFailures_ = 0;
        if (mode_ == Mode::Full && !history_.current().empty()) {
            atomicWrite(paths_.state / "last-success", history_.current());
            atomicWrite(paths_.state / "last-full-success", history_.current());
        }
        log("load.finished", "total=" + std::to_string(now() - requested_) + " webkit=" +
                                 (loadStarted_ > 0 ? std::to_string(now() - loadStarted_) : "unavailable"));
        persist();
    } else if (e.kind == EngineEvent::Failed) {
        commandFailures_++;
        loading_ = false;
        loadPhase_ = "Failed";
        loadFinishedAt_ = now();
        pendingNavigation_ = false;
        error_ = e.message;
        message_ = "Page unavailable. Open menu for options.";
        log("engine.error", e.message);
    } else if (e.kind == EngineEvent::Crashed) {
        crashed_ = true;
        state_ = State::Error;
        error_ = e.message;
        message_ = "Browser stopped. Choose Retry or Lite.";
        loading_ = false;
        persist();
        log("engine.crash", e.message);
        if (mode_ == Mode::QtWebKit || mode_ == Mode::Lite) {
            // Clean up before offering a retry or a different engine.
            stop();
            selectionRequested_ = true;
        }
    } else if (e.kind == EngineEvent::UrlSnapshot && !e.url.empty()) {
        if (e.url != history_.current()) {
            history_.navigate(e.url, modeName(mode_));
            persist();
        }
    }
}
void BrowserManager::transition(Mode target, bool restart) {
    downloadsVisible_ = false;
    cancelEscape();
    target_ = target;
    restart_ = restart;
    state_ = State::Stopping;
    stateSince_ = now();
    loading_ = false;
    message_ = restart ? "Recovering browser session..." : "Switching browser...";
    persist();
    input_.releaseKeys();
    if (engine_)
        engine_->stop();
}
void BrowserManager::stop() {
    quitting_ = true;
    if (engine_)
        transition(mode_, false);
    else
        state_ = State::Idle;
    persist();
}
bool BrowserManager::clearHistory() {
    if (state_ != State::Idle) return false;
    history_ = History{};
    history_.save(paths_.state / "history");
    fs::remove(paths_.state / "last-success");
    fs::remove(paths_.state / "last-full-success");
    return true;
}
bool BrowserManager::navigate(const std::string &raw) {
    auto url = normalizeUrl(raw);
    // Both engines use DuckDuckGo Lite as their search endpoint.
    if (!engine_ || (state_ != State::Active && state_ != State::Error))
        return false;
    // Full: one complete Cog process per requested navigation. This aborts
    // any previous in-flight request before starting the next URL.
    if (mode_ == Mode::Full) {
        history_.navigate(url, "FULL");
        requested_ = now();
        pendingNavigation_ = true;
        blocked_ = false;
        persist();
        log("navigation.recycle", "address: " + url);
        transition(mode_, true);
        return true;
    }
    if (state_ == State::Error) {
        history_.navigate(url, modeName(mode_));
        blocked_ = false;
        transition(mode_, false);
        return true;
    }
    if (!engine_->open(url)) return false;
    history_.navigate(url, modeName(mode_));
    requested_ = now();
    loadStarted_ = 0;
    pendingNavigation_ = true;
    loading_ = false;
    blocked_ = false;
    error_.clear();
    persist();
    return true;
}
bool BrowserManager::back() {
    if (!history_.canBack())
        return false;
    auto copy = history_;
    copy.back();
    if (state_ == State::Error) {
        history_ = copy;
        transition(mode_, false);
        return true;
    }
    if (!engine_ || !active()) return false;
    if (mode_ == Mode::QtWebKit ? !engine_->goBack() :
        (mode_ != Mode::Full && !engine_->open(copy.current()))) return false;
    history_ = copy;
    if (mode_ == Mode::Full) {
        persist();
        transition(mode_, true);
        return true;
    }
    requested_ = now();
    loadStarted_ = 0;
    pendingNavigation_ = true;
    loading_ = mode_ == Mode::Full;
    error_.clear();
    persist();
    return true;
}
bool BrowserManager::forward() {
    if (!history_.canForward())
        return false;
    auto copy = history_;
    copy.forward();
    if (!engine_ || !active()) return false;
    if (mode_ == Mode::QtWebKit ? !engine_->goForward() :
        (mode_ != Mode::Full && !engine_->open(copy.current()))) return false;
    history_ = copy;
    if (mode_ == Mode::Full) {
        persist();
        transition(mode_, true);
        return true;
    }
    requested_ = now();
    loadStarted_ = 0;
    pendingNavigation_ = true;
    loading_ = mode_ == Mode::Full;
    error_.clear();
    persist();
    return true;
}
bool BrowserManager::stopLoading() {
    if (mode_ != Mode::QtWebKit || !active() || !engine_ || !loading_) return false;
    if (!engine_->stopLoading()) return false;
    loading_ = false;
    pendingNavigation_ = false;
    loadPhase_ = "Stopped";
    loadFinishedAt_ = now();
    hoveredLink_.clear();
    return true;
}
bool BrowserManager::reload() {
    if (state_ == State::Error) {
        transition(mode_, false);
        return true;
    }
    if (!engine_ || !active()) return false;
    if (mode_ == Mode::Full) {
        transition(mode_, true);
        return true;
    }
    if (!engine_->reload()) return false;
    requested_ = now();
    loadStarted_ = 0;
    pendingNavigation_ = true;
    loading_ = mode_ == Mode::Full;
    error_.clear();
    return true;
}
void BrowserManager::recover() {
    if (engine_ && state_ != State::Stopping && state_ != State::Settling)
        transition(mode_, true);
}
void BrowserManager::snapshot() {
    if (engine_ && active())
        engine_->snapshot();
}
void BrowserManager::tick() {
    double t = now();
    if (engine_ && state_ != State::Stopping && state_ != State::Settling) {
        engine_->tick();
        auto events = std::move(events_);
        events_.clear();
        for (auto &e : events) {
            if (state_ == State::Stopping || state_ == State::Settling) break;
            handle(e);
        }
    }
    if (state_ == State::Stopping) {
        if (!engine_ || engine_->stopped()) {
            engine_.reset();
            state_ = quitting_ ? State::Idle : State::Settling;
            stateSince_ = t;
            log("engine.stopped");
        } else if (t - stateSince_ > config_.stopTimeout + 10) {
            error_ = "Engine has not released its processes";
            message_ = "Waiting for browser cleanup...";
        }
        return;
    }
    if (state_ == State::Settling) {
        if (t - stateSince_ >= 2) {
            memory_.resetPressure();
            launch();
        }
        return;
    }
    if (state_ == State::Starting && t - stateSince_ > 45) {
        if (mode_ == Mode::QtWebKit || mode_ == Mode::Lite) {
            handle({EngineEvent::Crashed, "", std::string(modeName(mode_)) + " startup timed out. See session.log"});
            return;
        }
        error_ = "Browser startup timed out";
        state_ = State::Error;
        engine_->stop();
    }
    if (loading_ && t - requested_ > (mode_ == Mode::Full ? 256.0 : config_.loadTimeout)) {
        loading_ = false;
        commandFailures_++;
        error_ = "Page load timed out. Retry, Back or Lite.";
        log("load.timeout", std::to_string(t - requested_));
    }
    if (t < nextSample_)
        return;
    nextSample_ = t + (memory_.state() == MemoryState::Normal ? 2 : 1);
    sample_ = MemoryManager::sample(engine_ ? engine_->pids() : std::vector<pid_t>{});
    if (config_.debug)
        log("memory",
            "available=" + std::to_string(sample_.availableMb) + " pss=" + std::to_string(sample_.pssMb) +
                " readable=" + std::to_string(sample_.pssReadable) + " processes=" +
                std::to_string(sample_.processes) + " psi_some=" + std::to_string(sample_.psiSome) +
                " psi_full=" + std::to_string(sample_.psiFull) + " pswpin=" + std::to_string(sample_.swapIn) +
                " pswpout=" + std::to_string(sample_.swapOut) +
                " zram=" + std::to_string(sample_.zramPhysicalMb));
    if (mode_ != Mode::Full || !engine_)
        return;
    auto before = memory_.state();
    memory_.update(sample_, commandFailures_ >= 2 || (loading_ && t - requested_ > 45));
    if (before != memory_.state())
        log("memory.state", memoryName(memory_.state()));
    // Navigations in Full already start a fresh Cog. Do not trigger an
    // additional memory-based restart during that fresh page load: doing so
    // previously caused duplicate starts and confusing UI transitions.
    if (crashed_ && config_.autoRecover && !blocked_) {
        if (memory_.allowRestart(t)) {
            memory_.restarted(t);
            crashed_ = false;
            transition(mode_, true);
        } else {
            blocked_ = true;
            log("recovery.blocked", "crash recovery rate limit");
        }
    }

}
} // namespace zero
