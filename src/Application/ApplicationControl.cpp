#include "Application.h"
#include <algorithm>
#include <cerrno>
#include <iomanip>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace zero {
std::string Application::execute(const std::string &line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "status") {
        std::ostringstream s;
        s << "state=" << (browser_ ? static_cast<int>(browser_->state()) : 0)
          << " view=" << static_cast<int>(ui_.view())
          << " mode=" << (browser_ ? modeName(browser_->mode()) : "NONE")
          << " requested=" << modeName(requestedMode_)
          << " pid=" << (browser_ ? browser_->pid() : 0)
          << " screen=" << display_.screenState()
          << " wayland=" << display_.wayland.connected()
          << " windows=" << display_.wayland.toplevels().size()
          << " frames=" << display_.wayland.capturedFrames()
          << " last_frame_age=" << (display_.wayland.lastFrameAt() > 0 ? now() - display_.wayland.lastFrameAt() : -1)
          << " capture_error=" << std::quoted(display_.wayland.error())
          << " startup_error=" << std::quoted(startupError_);
        if (browser_)
            s << " loading=" << browser_->loading() << " position=" << browser_->history().position()
              << " visits=" << browser_->history().visits().size()
              << " memory=" << memoryName(browser_->memoryState())
              << " available=" << browser_->sample().availableMb << " pss=" << browser_->sample().pssMb
              << " url=" << std::quoted(browser_->history().current())
              << " phase=" << std::quoted(browser_->loadPhase()) << " progress=" << browser_->loadProgress()
              << " viewport=320x" << display_.wayland.contentHeight()
              << " error=" << std::quoted(browser_->error());
        return s.str();
    }
    if (cmd == "quit") {
        quit_ = true;
        return "OK";
    }
    if (cmd == "windows") {
        std::ostringstream s;
        for (const auto &w : display_.wayland.toplevels())
            s << "id=" << w.id << " parent=" << w.parent << " active=" << w.active
              << " complete=" << w.complete << " app=" << std::quoted(w.appId)
              << " title=" << std::quoted(w.title) << '\n';
        return s.str();
    }
    if (cmd == "mode" || cmd == "overlay" || cmd == "ui-key" || cmd == "open" ||
        cmd == "back" || cmd == "forward" || cmd == "reload" || cmd == "recover" ||
        cmd == "tap" || cmd == "paste" || cmd == "pointer" || cmd == "click")
        display_.activity(now());
    if (cmd == "mode") {
        std::string mode;
        in >> mode;
        if (mode != "lite" && mode != "full" && mode != "qtwebkit")
            return "ERROR invalid mode";
        const auto target = mode == "lite" ? Mode::Lite : mode == "qtwebkit" ? Mode::QtWebKit : Mode::Full;
        if (!browser_ || browser_->mode() != target || browser_->state() == BrowserManager::State::Idle)
            start(target);
        return "OK";
    }
    if (cmd == "overlay") {
        toggleOverlay();
        return "OK";
    }
    if (cmd == "ui-key") {
        unsigned code, mods = 0;
        int value = -1;
        if (!(in >> code) || code > KEY_MAX)
            return "ERROR key";
        in >> mods;
        if (in >> value) {
            if (value < 0 || value > 2) return "ERROR key value";
            key({static_cast<uint16_t>(code), value, mods, false, InputManager::character(code, mods)});
        } else {
            key({static_cast<uint16_t>(code), 1, mods, false, InputManager::character(code, mods)});
            key({static_cast<uint16_t>(code), 0, 0, false, 0});
        }
        return "OK";
    }
    if (cmd == "screenshot") {
        render();
        std::string ppm = "P6\n320 170\n255\n";
        for (auto pixel : display_.frame) {
            ppm += static_cast<char>(pixel >> 16);
            ppm += static_cast<char>(pixel >> 8);
            ppm += static_cast<char>(pixel);
        }
        atomicWrite(paths_.state / "frame.ppm", ppm);
        return (paths_.state / "frame.ppm").string();
    }
    if (!browser_)
        return "ERROR no browser";
    if (cmd == "open") {
        std::string url;
        std::getline(in >> std::ws, url);
        return browser_->navigate(url) ? "OK" : "BUSY";
    }
    if (cmd == "back")
        return browser_->back() ? "OK" : "BUSY";
    if (cmd == "forward")
        return browser_->forward() ? "OK" : "BUSY";
    if (cmd == "reload")
        return browser_->reload() ? "OK" : "BUSY";
    if (cmd == "recover") {
        browser_->recover();
        ui_.show(UIManager::View::Transition);
        return "OK";
    }
    if (cmd == "tap") {
        unsigned code, mods = 0;
        if (!(in >> code) || code > KEY_MAX)
            return "ERROR key";
        in >> mods;
        display_.wayland.tap(code, mods);
        return "OK";
    }
    if (cmd == "paste") {
        std::string text;
        std::getline(in >> std::ws, text);
        display_.wayland.paste(text);
        return "OK";
    }
    if (cmd == "pointer") {
        int x, y;
        if (!(in >> x >> y))
            return "ERROR pointer";
        pointerX_ = std::clamp(x, 0, 319);
        pointerY_ = std::clamp(y, 0, 169);
        display_.wayland.pointer(pointerX_, pointerY_);
        return "OK";
    }
    if (cmd == "click") {
        click();
        return "OK";
    }
    return "ERROR unknown command";
}
void Application::control() {
    constexpr size_t maxClients = 8;
    constexpr double timeout = 2.0;
    auto retryable = [] { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; };

    // Accept at most one client per tick and never wait for socket readiness.
    // A connected client need not have sent its single request packet yet.
    if (!quit_ && controlClients_.size() < maxClients) {
        int fd = accept4(control_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd >= 0) {
            try {
                controlClients_.push_back({fd, now() + timeout, {}});
            } catch (...) {
                close(fd);
                throw;
            }
        } else if (!retryable()) {
            log("control.accept_error", std::to_string(errno));
        }
    }
    for (auto client = controlClients_.begin(); client != controlClients_.end();) {
        // Once quitting, only already executed commands may finish their replies.
        bool done = now() >= client->deadline || (quit_ && client->response.empty());
        if (!done && client->response.empty()) {
            char buf[8192];
            auto n = recv(client->fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
            if (n < 0) {
                done = !retryable();
            } else {
                try {
                    client->response = n > 0 ? execute(std::string(buf, n)) : "ERROR empty request";
                } catch (const std::exception &e) {
                    client->response = "ERROR " + std::string(e.what());
                }
                client->response += '\n';
                client->deadline = now() + timeout;
            }
        }
        if (!done && !client->response.empty()) {
            // SOCK_SEQPACKET sends one whole reply or fails; do not split it or
            // re-execute the command when a send must be retried next tick.
            auto n = send(client->fd, client->response.data(), client->response.size(),
                          MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n < 0) {
                done = !retryable();
            } else {
                if (static_cast<size_t>(n) != client->response.size())
                    log("control.short_reply");
                done = true;
            }
        }
        if (done) {
            close(client->fd);
            client = controlClients_.erase(client);
        } else {
            ++client;
        }
    }
}
} // namespace zero
