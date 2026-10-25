#pragma once
#include "Startup.h"
#include "WaylandBridge.h"

namespace zero {
// Same readiness gates in both applications; no sleep added on a ready path.
inline void configureDisplay(Startup &startup, WaylandBridge &bridge,
                             const std::map<std::string, std::string> &env, int height,
                             const std::function<void()> &heartbeat) {
    startup.step("output", StartupDomain::Wayland, [&] {
        Process mode;
        mode.start({"wlr-randr", "--output", "HEADLESS-1", "--on", "--scale", "1",
                    "--transform", "normal", "--custom-mode", "320x" + std::to_string(height)}, env);
        std::string diagnostics;
        startup.wait(5, [&] {
            heartbeat();
            bridge.pump();
            diagnostics += mode.drain();
            if (diagnostics.size() > 8192) diagnostics.erase(0, diagnostics.size() - 8192);
            return mode.empty();
        }, "Display configuration command timed out");
        diagnostics += mode.drain();
        if (mode.status()) throw std::runtime_error("wlr-randr status=" +
                                                   std::to_string(mode.status()) + " " + diagnostics);
        startup.wait(5, [&] {
            heartbeat();
            bridge.pump();
            return bridge.outputReady(320, height);
        }, "No enabled 320x" + std::to_string(height) + " output at scale 1, normal transform");
        bridge.contentTop(170 - height);
        log("wayland.display=READY", "320x" + std::to_string(height));
    });
}

// tickApplication reports readiness or throws an application-domain failure.
// A black frame is valid: only protocol completion and geometry are checked.
inline void awaitApplication(Startup &startup, WaylandBridge &bridge, int height,
                             const std::function<void()> &heartbeat,
                             const std::function<bool()> &tickApplication) {
    auto applicationPulse = [&] {
        try { return tickApplication(); }
        catch (const StartupCancelled &) { throw; }
        catch (const StartupFailure &) { throw; }
        catch (const std::exception &e) {
            throw StartupFailure(StartupDomain::Application, "engine", e.what());
        }
    };
    auto displayPulse = [&] {
        try { heartbeat(); bridge.pump(); }
        catch (const StartupCancelled &) { throw; }
        catch (const StartupFailure &) { throw; }
        catch (const std::exception &e) {
            throw StartupFailure(StartupDomain::Wayland, "connection", e.what());
        }
    };
    startup.step("window", StartupDomain::Application, [&] {
        startup.wait(45, [&] {
            displayPulse();
            const bool ready = applicationPulse();
            for (const auto &window : bridge.toplevels())
                if (window.complete && window.mapped && ready) {
                    log("application.window=READY", "id=" + std::to_string(window.id));
                    return true;
                }
            return false;
        }, "Application did not expose a window / report readiness");
    });
    startup.step("first_frame", StartupDomain::Wayland, [&] {
        // Discard any request made before the window became visible. Its reply
        // could be a perfectly valid capture of the empty compositor.
        bridge.resetCapture();
        const auto before = bridge.capturedFrames();
        bridge.capture();
        startup.wait(4, [&] {
            displayPulse();
            applicationPulse();
            if (!bridge.error().empty()) throw std::runtime_error(bridge.error());
            if (bridge.capturedFrames() == before) return false;
            if (!bridge.outputReady(320, height) || !bridge.captureSize(320, height))
                throw std::runtime_error("Captured display has incorrect geometry");
            bool mapped = false;
            for (const auto &window : bridge.toplevels()) mapped |= window.complete && window.mapped;
            if (!mapped) throw StartupFailure(StartupDomain::Application, "window", "Application window closed");
            log("wayland.first_frame=READY");
            return true;
        }, "First application frame timed out");
    });
}
} // namespace zero
