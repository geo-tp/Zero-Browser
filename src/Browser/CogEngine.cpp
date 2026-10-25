#include "CogEngine.h"
#include <sstream>
#include <linux/input-event-codes.h>
namespace zero {
void CogEngine::start(const std::string &url) {
    environment_["COG_PLATFORM_WL_VIEW_HEIGHT"] = std::to_string(input_.contentHeight());
    auto args = config_.cogArguments();
    args.push_back(url);
    launched_ = now();
    process_.start(args, environment_);
    log("engine.start", "FULL pid=" + std::to_string(process_.pid()));
}
bool CogEngine::control(const std::string &name, const std::string &arg) {
    if (!command_.empty() || stopping_)
        return false;
    std::vector<std::string> args = {"cogctl", "--appid=org.cardputer.ZeroBrowser.Cog", name};
    if (!arg.empty())
        args.push_back(arg);
    commandStarted_ = now();
    commandName_ = name;
    command_.start(args, environment_);
    return true;
}
bool CogEngine::open(const std::string &url) {
    return control("open", url);
}
bool CogEngine::reload() {
    return control("reload");
}
bool CogEngine::zoom(bool in) {
    if (!ready_ || stopping_) return false;
    // Cog's Wayland binding is Ctrl+= (without Shift), unlike NetSurf GTK.
    input_.tap(in ? KEY_EQUAL : KEY_MINUS, 4);
    return true;
}
bool CogEngine::parseLine(const std::string &line, EngineEvent &e) {
    auto a = line.find('<'), b = line.find("> ", a);
    if (a == std::string::npos || b == std::string::npos)
        return false;
    e.url = line.substr(a + 1, b - a - 1);
    auto text = line.substr(b + 2);
    if (text.rfind("Load started.", 0) == 0)
        e.kind = EngineEvent::LoadStarted;
    else if (text.rfind("Loading...", 0) == 0)
        e.kind = EngineEvent::Redirected;
    else if (text.rfind("Loaded successfully.", 0) == 0)
        e.kind = EngineEvent::Loaded;
    else if (text.rfind("Load failed", 0) == 0) {
        e.kind = EngineEvent::Failed;
        e.message = text;
    } else
        return false;
    try {
        e.url = normalizeUrl(e.url);
    } catch (...) {
        return false;
    }
    return true;
}
void CogEngine::tick() {
    process_.poll();
    auto out = process_.drain();
    partial_ += out;
    if (partial_.size() > 131072)
        partial_.erase(0, partial_.size() - 65536);
    size_t pos;
    while ((pos = partial_.find('\n')) != std::string::npos) {
        auto line = partial_.substr(0, pos);
        partial_.erase(0, pos + 1);
        EngineEvent e{};
        if (parseLine(line, e)) {
            if (!ready_) {
                ready_ = true;
                emit_({EngineEvent::Ready, "", "FULL", now() - launched_});
            }
            emit_(e);
        } else if (config_.debug || line.find("ERROR") != std::string::npos ||
                   line.find("WARNING") != std::string::npos)
            log("cog", line);
    }
    if (!stopping_ && !process_.running() && !crashReported_) {
        crashReported_ = true;
        emit_({EngineEvent::Crashed, "", "Cog exited (" + std::to_string(process_.status()) + ")"});
    }
    // The first WebKit load event marks readiness. Process::poll() detects
    // crashes; polling cogctl ping is unnecessary and could generate false
    // navigation errors or interfere with real user commands.
    pollCommand();
}
void CogEngine::stop() {
    if (stopping_)
        return;
    // Native quit is attempted first; the manager then enforces the bounded stop deadline.
    if (command_.empty())
        control("quit");
    stopping_ = true;
    process_.stop(config_.stopTimeout);
}
} // namespace zero
