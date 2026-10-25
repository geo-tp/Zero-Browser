#include "WaylandSession.h"
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace zero {
namespace {
struct RuntimeDirectory {
    fs::path path;
    explicit RuntimeDirectory(const fs::path &parent) {
        std::string pattern = (parent / "wl.XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back(0);
        if (!mkdtemp(name.data()))
            throw std::runtime_error("Cannot create private Wayland runtime directory");
        path = name.data(); // mkdtemp creates mode 0700, unique on every attempt.
    }
    ~RuntimeDirectory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
bool isSocket(const fs::path &path) {
    std::error_code ec;
    return fs::is_socket(path, ec);
}
} // namespace

struct WaylandSession::Attempt {
    // Destroy processes before removing their runtime directory and sockets.
    RuntimeDirectory runtime;
    std::map<std::string, std::string> env;
    Process compositor, bus;
    bool startupGpuFailure = false;
    std::string gpuDiagnostic;
    bool startupValidation = true;
    void inspectCompositor(const std::string &output) {
        if (!startupValidation || output.empty()) return;
        // A failed headless swapchain is a hard failure for this attempt.
        // A single transient GBM warning is not sufficient to abort.
        if (output.find("Swapchain for output") != std::string::npos &&
            output.find("failed test") != std::string::npos) {
            startupGpuFailure = true;
            log("wayland.gpu_allocation_failed", "headless output swapchain unavailable; aborting this attempt early");
            gpuDiagnostic = "headless GPU swapchain allocation failed";
        }
    }
    void checkGpuFailure() const {
        if (startupGpuFailure)
            throw std::runtime_error("Private labwc " + gpuDiagnostic +
                                     "; retrying with a fresh session");
    }


    explicit Attempt(const fs::path &parent) : runtime(parent) {
        const auto dir = runtime.path / "labwc";
        fs::create_directory(dir);
        atomicWrite(dir / "rc.xml",
            "<?xml version=\"1.0\"?><labwc_config><core><gap>0</gap><decoration>server</decoration></core><keyboard/>"
            "<windowRules><windowRule identifier=\"*\"><serverDecoration>no</serverDecoration><action "
            "name=\"ToggleFullscreen\"/></windowRule></windowRules></labwc_config>\n");
        atomicWrite(dir / "autostart", "#!/bin/sh\nexit 0\n");
        fs::permissions(dir / "autostart", fs::perms::owner_all);
        atomicWrite(dir / "environment", "");
        atomicWrite(dir / "menu.xml", "<?xml version=\"1.0\"?><openbox_menu/>\n");
        env = {{"XDG_RUNTIME_DIR", runtime.path.string()},
               {"DBUS_SESSION_BUS_ADDRESS", "unix:path=" + (runtime.path / "bus").string()},
               {"WAYLAND_DISPLAY", "wayland-0"},
               {"GDK_BACKEND", "wayland"},
               {"COG_PLATFORM_WL_VIEW_WIDTH", "320"},
               {"COG_PLATFORM_WL_VIEW_HEIGHT", "170"},
               {"COG_PLATFORM_WL_VIEW_FULLSCREEN", "1"}};
    }
    ~Attempt() {
        compositor.stop(.15);
        bus.stop(.15);
    }
    bool start(int number, const Progress &progress) {
        bus.start({"dbus-daemon", "--session", "--nofork",
                   "--address=" + env.at("DBUS_SESSION_BUS_ADDRESS")}, env);
        auto wlEnv = env;
        wlEnv["WLR_BACKENDS"] = "headless";
        wlEnv["WLR_HEADLESS_OUTPUTS"] = "1";
        wlEnv["WLR_RENDERER"] = "gles2";
        wlEnv["WLR_RENDER_DRM_DEVICE"] = "/dev/dri/renderD128";
        compositor.start({"labwc", "-C", (runtime.path / "labwc").string()}, wlEnv);
        const auto started = now();
        bool warned = false;
        std::string diagnostics;
        for (;;) {
            if (progress && !progress(number, now() - started)) return false;
            compositor.poll();
            bus.poll();
            const auto output = compositor.drain();
            if (!output.empty()) {
                log("labwc.startup", output);
                inspectCompositor(output);
                diagnostics += output;
                if (diagnostics.size() > 8192) diagnostics.erase(0, diagnostics.size() - 8192);
            }
            checkGpuFailure();
            const auto busOutput = bus.drain();
            if (!busOutput.empty()) log("dbus.startup", busOutput);
            if (!compositor.running())
                throw std::runtime_error("Private labwc exited before socket (status " +
                                         std::to_string(compositor.status()) + "): " + diagnostics);
            if (!bus.running())
                throw std::runtime_error("Private D-Bus exited during startup (status " +
                                         std::to_string(bus.status()) + "): " + busOutput);
            if (isSocket(runtime.path / "wayland-0")) break;
            const auto elapsed = now() - started;
            if (elapsed >= 15.0)
                throw std::runtime_error("Private Wayland socket timeout after 15s; labwc still running: " +
                                    diagnostics);
            if (!warned && elapsed >= 7.0) {
                warned = true;
                log("wayland.startup", "labwc still creating its private socket after 7 seconds");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        log("wayland.socket=READY", "private socket ready after " + std::to_string(now() - started) + " seconds");
        return true;
    }
};

WaylandSession::WaylandSession() = default;
WaylandSession::~WaylandSession() = default;
void WaylandSession::stop() { attempt_.reset(); }
bool WaylandSession::start(const Paths &paths, const Progress &progress) {
    stop();
    attempt_ = std::make_unique<Attempt>(paths.runtime);
    if (!attempt_->start(1, progress)) { stop(); return false; }
    return true;
}
void WaylandSession::validated() {
    if (attempt_) attempt_->startupValidation = false;
}
void WaylandSession::shutdown(const std::function<void()> &heartbeat) {
    if (!attempt_) return;
    attempt_->compositor.stop(.15);
    attempt_->bus.stop(.15);
    while (!attempt_->compositor.empty() || !attempt_->bus.empty()) {
        if (heartbeat) heartbeat();
        attempt_->compositor.drain();
        attempt_->bus.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    stop();
}

const std::map<std::string, std::string> &WaylandSession::environment() const {
    if (!attempt_) throw std::runtime_error("Private Wayland session is not started");
    return attempt_->env;
}
bool WaylandSession::alive() {
    if (!attempt_) return false;
    attempt_->compositor.poll();
    attempt_->bus.poll();
    const auto output = attempt_->compositor.drain();
    attempt_->inspectCompositor(output);
    if (!output.empty()) log("labwc", output);
    attempt_->checkGpuFailure();
    const auto busOutput = attempt_->bus.drain();
    if (!busOutput.empty()) log("dbus", busOutput);
    return attempt_->compositor.running() && attempt_->bus.running();
}
} // namespace zero
