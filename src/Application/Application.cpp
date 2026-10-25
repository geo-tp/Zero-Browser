#include "Application.h"
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
namespace zero {
namespace {
volatile sig_atomic_t interrupted = 0;
void signalHandler(int) {
    interrupted = 1;
}
} // namespace
Application::Application(Options o) : options_(std::move(o)), paths_(Paths::user()) {}
Application::~Application() {
    releaseMouse();
    display_.restoreScreen();
    browser_.reset();
    display_.stopSession();
    if (!sessionDir_.empty()) {
        std::error_code ec;
        fs::remove_all(sessionDir_, ec);
    }
    for (const auto &client : controlClients_)
        close(client.fd);
    if (control_ >= 0)
        close(control_);
    if (!socket_.empty())
        unlink(socket_.c_str());
    if (lock_ >= 0)
        close(lock_);
}
void Application::start(Mode mode) {
    releaseMouse();
    requestedMode_ = mode;
    startupError_.clear();
    notice_.clear();
    noticeUntil_ = 0;
    log("application.start", "requested=" + std::string(modeName(mode)));
    ui_.setTransitionMode(mode);
    ui_.show(UIManager::View::Transition);
    double nextDraw = 0;
    Startup startup([&](int attempt, const std::string &stage, bool recovering) {
        // No browser actions are forwarded until its window and frame are ready.
        input_.poll([](const Key &) {}, false, [this] { display_.activity(now()); });
        if (now() >= nextDraw) {
            const bool application = stage == "window" || stage == "engine" || stage == "first_frame";
            notice_ = std::string(application ? "Starting browser..." : "Initializing Wayland") + "|Attempt " +
                      std::to_string(attempt) + "/3|" + (recovering ? "Recovering..." : stage);
            noticeUntil_ = now() + 1;
            render();
            nextDraw = now() + .1;
        }
        return !interrupted && !quit_ && input_.escapeHeld() < 3;
    });
    auto cleanup = [&] {
        // Release proxies first, then stop only children tracked by this owner.
        display_.wayland.disconnect();
        if (browser_) {
            browser_->stop();
            while (!browser_->stopped()) {
                startup.service();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            browser_.reset();
        }
        display_.stopSession([&] { startup.service(); });
    };
    std::string url = options_.url;
    if (url.empty() && browser_ && browser_->active()) url = browser_->history().current();
    try {
        cleanup();
        const int height = mode == Mode::Lite ? 170 : 170 - UIManager::ToolbarHeight;
        startup.run([&] {
            display_.startSession(paths_, startup, height);
            startup.step("engine", StartupDomain::Application, [&] {
                browser_ = std::make_unique<BrowserManager>(config_, paths_, display_.wayland, display_.environment(),
                    [this](Mode m) { display_.contentTop(m == Mode::Lite ? 0 : UIManager::ToolbarHeight); });
                browser_->start(mode, url);
            });
            awaitApplication(startup, display_.wayland, height, [&] {
                startup.pulse();
                if (!display_.sessionAlive()) throw std::runtime_error("Private compositor or bus exited");
            }, [&] {
                browser_->tick();
                const auto state = browser_->state();
                if (state == BrowserManager::State::Error || state == BrowserManager::State::Stopping ||
                    state == BrowserManager::State::Idle)
                    throw StartupFailure(StartupDomain::Application, "engine", browser_->error());
                return browser_->active();
            });
        }, cleanup);
        options_.url.clear();
        notice_.clear();
        noticeUntil_ = 0;
        ui_.show(UIManager::View::Web);
    } catch (const StartupCancelled &) {
        log("wayland.startup", "display startup cancelled");
        quit_ = true;
    } catch (const std::exception &e) {
        startupError_ = cleanText(e.what());
        log("application.start_failed", "requested=" + std::string(modeName(mode)) + " reason=" + startupError_);
        cleanup();
        const auto *failure = dynamic_cast<const StartupFailure *>(&e);
        notice_ = failure ? std::string(failure->domain == StartupDomain::Wayland ? "Display " : "Browser ") +
                  failure->stage + " failed (" + std::to_string(startup.attempt()) + "/3)" : "Startup failed: " + startupError_;
        noticeUntil_ = now() + 3600;
        ui_.selectEngine(mode);
        render();
    }
}

void Application::toggleOverlay() {
    releaseMouse();
    if (!browser_)
        return;
    browser_->cancelEscape();
    display_.wayland.releaseKeys();
    heldUp_ = heldDown_ = heldLeft_ = heldRight_ = false;
    heldScrollUp_ = heldScrollDown_ = heldScrollLeft_ = heldScrollRight_ = false;
    if (ui_.view() == UIManager::View::Transition) return;
    // Drop delayed ESC release events immediately after URL submission.
    // ESC hold-to-exit is evaluated independently in the main loop.
    if (addressSubmittedAt_ >= 0 && now() - addressSubmittedAt_ < 0.7) return;
    bool entering = ui_.view() == UIManager::View::Web;
    ui_.escape();
    if (entering) {
        browser_->snapshot();
        ui_.address(browser_->history().current());
    }
}
void Application::action(const Action &a) {
    if (a.type == Action::None)
        return;
    if (a.type == Action::Lite) {
        start(Mode::Lite);
        return;
    }
    if (a.type == Action::QtWebKit) {
        start(Mode::QtWebKit);
        return;
    }
    if (a.type == Action::ClearBrowserData) {
        // Startup only: never mutate NetSurf's indexed cache while it is running.
        if (browser_ && browser_->state() != BrowserManager::State::Idle) {
            notice_ = "Close browser before cleanup"; noticeUntil_ = now() + 5; return;
        }
        std::error_code ec;
        if (a.text.find('C') != std::string::npos) {
            fs::remove_all(paths_.cache / "lite/netsurf", ec);
            if (!ec) fs::remove_all(paths_.cache / "qtwebkit/http", ec);
        }
        if (!ec && a.text.find('K') != std::string::npos) {
            // NetSurf's persistent cookie database; QtWebKit does not install
            // a persistent cookie jar in this application.
            fs::remove(paths_.config / "lite/netsurf/Cookies", ec);
        }
        if (!ec && a.text.find('H') != std::string::npos) {
            if (browser_) browser_->clearHistory();
            else {
                fs::remove(paths_.state / "history", ec);
                if (!ec) fs::remove(paths_.state / "last-success", ec);
                if (!ec) fs::remove(paths_.state / "last-full-success", ec);
            }
        }
        notice_ = ec ? "Cleanup failed: check permissions" : "Selected data cleared";
        noticeUntil_ = now() + 6;
        return;
    }
    if (!browser_)
        return;
    bool success = true;
    switch (a.type) {
    case Action::Back:
        success = browser_->back();
        break;
    case Action::Forward:
        success = browser_->forward();
        break;
    case Action::Reload:
        success = browser_->mode() == Mode::QtWebKit && browser_->loading()
            ? browser_->stopLoading() : browser_->reload();
        break;
    case Action::StopLoading:
        success = browser_->stopLoading();
        break;
    case Action::Home:
        success = browser_->navigate("https://lite.duckduckgo.com/lite/");
        break;
    case Action::Downloads:
        success = browser_->mode() == Mode::QtWebKit && browser_->showDownloads();
        break;
    case Action::Address:
        releaseMouse();
        display_.wayland.releaseKeys();
        heldUp_ = heldDown_ = heldLeft_ = heldRight_ = false;
        heldScrollUp_ = heldScrollDown_ = heldScrollLeft_ = heldScrollRight_ = false;
        browser_->snapshot();
        ui_.address(browser_->history().current());
        return;
    case Action::ToggleJavascript:
        success = browser_->toggleJavascript();
        break;
    case Action::ZoomOut:
    case Action::ZoomIn:
        success = browser_->zoom(a.type == Action::ZoomIn);
        break;
    case Action::Open:
        success = browser_->navigate(a.text);
        break;
    default:
        break;
    }
    if (success) {
        display_.wayland.releaseKeys();
        if (a.type == Action::Open) {
            // Committing an address is a one-way transition out of the editor.
            // In particular, memory recovery can put the engine into Stopping
            // immediately after navigate() returns: never leave the editor open.
            addressSubmittedAt_ = now();
            suppressed_.insert(KEY_ENTER);
        }
        ui_.show(browser_->state() == BrowserManager::State::Active
                     ? UIManager::View::Web : UIManager::View::Transition);
    } else {
        notice_ = "Not available yet. Try again.";
        noticeUntil_ = now() + 2;
    }
}
void Application::render() {
    if (display_.screenOff()) return;
    if (browser_ && (browser_->loading() || browser_->recentLoad())) clickHintUntil_ = 0;
    if (display_.wayland.connected() && ui_.view() != UIManager::View::Transition)
        display_.frame = display_.wayland.pixels();
    const auto system = ui_.view() == UIManager::View::Select ? SystemStatus::sample() : SystemStatus{};
    ui_.draw(display_.frame, browser_.get(), system, input_.escapeHeld(), true, pointerX_, pointerY_,
             now() < noticeUntil_ ? notice_ : "", now() < clickHintUntil_,
             browser_ && browser_->mode() == Mode::QtWebKit && qtDownloadsFocused(display_.wayland));
    display_.present();
}
int Application::run() {
    prctl(PR_SET_CHILD_SUBREAPER, 1);
    signal(SIGTERM, signalHandler);
    signal(SIGINT, signalHandler);
    signal(SIGPIPE, SIG_IGN);
    if (!options_.preview.empty()) {
        config_.load(paths_);
        display_.open(config_, options_.preview);
        render();
        return 0;
    }
    paths_.create();
    config_.load(paths_);
    config_.debug |= options_.debug;
    if (!options_.url.empty()) options_.url = normalizeUrl(options_.url);
    lock_ = ::open((paths_.runtime / "lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_ < 0 || flock(lock_, LOCK_EX | LOCK_NB) < 0)
        throw std::runtime_error("Zero-Browser is already running");
    openSessionLog(paths_.state / "session.log");
    log("application.session", "pid=" + std::to_string(getpid()));
    socket_ = paths_.runtime / "control";
    unlink(socket_.c_str());
    control_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_.string().size() >= sizeof address.sun_path)
        throw std::runtime_error("Runtime path too long");
    std::strcpy(address.sun_path, socket_.c_str());
    if (control_ < 0 || bind(control_, reinterpret_cast<sockaddr *>(&address), sizeof address) < 0 ||
        listen(control_, 4) < 0)
        throw std::runtime_error("Cannot create control socket");
    chmod(socket_.c_str(), 0600);
    std::string session = (paths_.runtime / "session.XXXXXX").string();
    std::vector<char> name(session.begin(), session.end());
    name.push_back(0);
    if (!mkdtemp(name.data()))
        throw std::runtime_error("Cannot create private session directory");
    sessionDir_ = name.data();
    paths_.runtime = sessionDir_;
    display_.open(config_, options_.frameFile);
    if (!options_.noInput)
        input_.open(config_.input);
    render();
    if (!options_.mode.empty())
        start(options_.mode == "full" ? Mode::Full : options_.mode == "qtwebkit" ? Mode::QtWebKit : Mode::Lite);
    double begun = now(), nextDraw = 0, nextAlive = 0;
    BrowserManager::State previous = BrowserManager::State::Idle;
    // A control-command quit may still have a nonblocking reply to flush.
    // Keep supervising/rendering during its bounded send deadline.
    while ((!quit_ || !controlClients_.empty()) && !interrupted) {
        try {
            input_.poll([this](const Key &k) { key(k); }, true,
                        [this] { display_.activity(now()); });
            if (input_.escapeHeld() >= 3 || (escapePressedAt_ >= 0 && now() - escapePressedAt_ >= 3)) {
                quit_ = true;
                break;
            }
            control();
            updatePointer(now());
            display_.idle(now(), input_.held());
            if (browser_) {
                display_.tick(config_.fps);
                browser_->tick();
                if (browser_->takeSelectionRequest()) {
                    releaseMouse();
                    notice_ = browser_->error();
                    noticeUntil_ = now() + 15;
                    ui_.selectEngine(browser_->mode());
                }
                if (browser_->takeOverlayRequest() && ui_.view() == UIManager::View::Web)
                    toggleOverlay();
                auto state = browser_->state();
                if (state == BrowserManager::State::Active &&
                    (ui_.view() == UIManager::View::Transition))
                    ui_.show(UIManager::View::Web);
                if (state != BrowserManager::State::Active) releaseMouse();
                if (state == BrowserManager::State::Stopping || state == BrowserManager::State::Settling ||
                    state == BrowserManager::State::Starting)
                    ui_.show(UIManager::View::Transition);
                if (state == BrowserManager::State::Error && previous != state) {
                    // Do not auto-open the address overlay on an engine error.
                    // That looks like a second ENTER is required and can start
                    // another engine. Expose the error and let ESC open the UI.
                    ui_.show(UIManager::View::Web);
                    notice_ = browser_->error().empty() ? "Browser error - ESC for options" : browser_->error();
                    noticeUntil_ = now() + 8;
                }
                previous = state;
                if (now() >= nextAlive) {
                    nextAlive = now() + 2;
                    if (!display_.sessionAlive())
                        throw std::runtime_error("Private graphical session stopped");
                }
            }
            if (now() >= nextDraw) {
                render();
                nextDraw = now() + 1.0 / config_.fps;
            }
        } catch (const std::exception &e) {
            log("application.error", e.what());
            notice_ = e.what();
            noticeUntil_ = now() + 5;
            if (browser_) ui_.show(UIManager::View::Menu);
            else ui_.selectEngine(requestedMode_);
            if (browser_ && !display_.sessionAlive()) {
                quit_ = true;
            }
        }
        if (options_.seconds > 0 && now() - begun >= options_.seconds)
            quit_ = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    releaseMouse();
    display_.restoreScreen();
    config_.save(paths_);
    if (browser_)
        browser_->stop();
    browser_.reset();
    log("application.exit");
    return 0;
}
} // namespace zero
