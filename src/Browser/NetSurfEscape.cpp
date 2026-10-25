#include "NetSurfEscape.h"
#include "Utils/Process.h"
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <sys/socket.h>
#include <unistd.h>
namespace zero {
namespace {
const Toplevel *activeWindow(const std::vector<Toplevel> &windows, const char *id) {
    const Toplevel *result = nullptr;
    for (const auto &w : windows) {
        if (!w.complete || !w.active) continue;
        if (result || w.appId != id) return nullptr;
        result = &w;
    }
    return result;
}
bool safeSecondary(const ZeroGtkState &s) {
    return s.kind == ZERO_GTK_DOWNLOADS || s.kind == ZERO_GTK_HISTORY || s.kind == ZERO_GTK_SETTINGS;
}
}
EscapeAction decideEscape(const ZeroGtkState &s, const std::vector<Toplevel> &windows, bool safeVersion) {
    if (s.version != ZERO_GTK_PROTOCOL || !s.app_id[0] || !activeWindow(windows, s.app_id))
        return EscapeAction::Native;
    if (s.popup) return EscapeAction::Native;
    if (s.modal) return EscapeAction::NativeThenCheck;
    if (s.kind == ZERO_GTK_MAIN) return EscapeAction::Overlay;
    if (safeVersion && safeSecondary(s) && s.main_id[0] && strcmp(s.main_id, s.app_id) &&
        std::any_of(windows.begin(), windows.end(), [&](const auto &w) {
            return w.complete && w.appId == s.main_id;
        })) return EscapeAction::Close;
    return EscapeAction::Native;
}
NetSurfEscape::~NetSurfEscape() {
    if (fd_ >= 0) ::close(fd_);
    if (child_ >= 0) ::close(child_);
}
void NetSurfEscape::prepare(std::map<std::string, std::string> &environment) {
    std::error_code ec;
    auto exe = fs::read_symlink("/proc/self/exe", ec);
    auto module = exe.parent_path() / "libzero-netsurf-gtk.so";
    if (!fs::is_regular_file(module)) module = "/usr/lib/zero-browser/libzero-netsurf-gtk.so";
    if (!fs::is_regular_file(module)) { log("escape.fallback", "GTK module unavailable"); return; }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair)) {
        log("escape.fallback", "GTK channel unavailable"); return;
    }
    fd_ = pair[0]; child_ = pair[1];
    // Only this descriptor crosses exec; close the parent's copy immediately
    // after Process::start. The module sets CLOEXEC before NetSurf spawns anything.
    auto modules = environment.count("GTK3_MODULES") ? environment.at("GTK3_MODULES") : env("GTK3_MODULES");
    environment["GTK3_MODULES"] = module.string() + (modules.empty() ? "" : ":" + modules);
    environment["ZERO_BROWSER_GTK_FD"] = std::to_string(child_);
    const auto version = command({"dpkg-query", "-W", "-f=${Version}", "netsurf-gtk"}, 2);
    auto value = version.output;
    auto colon = value.find(':');
    if (colon != std::string::npos) value.erase(0, colon + 1);
    safeVersion_ = version.status == 0 && value.rfind("3.11-", 0) == 0;
    log("escape.init", safeVersion_ ? "NetSurf 3.11 close handlers verified" : "native ESC only for unknown secondary close handlers");
    fcntl(child_, F_SETFD, 0);
}
void NetSurfEscape::childStarted() {
    if (child_ >= 0) ::close(child_);
    child_ = -1;
}
bool NetSurfEscape::query(Phase phase) {
    ZeroGtkQuery q{ZERO_GTK_PROTOCOL, ++serial_};
    if (fd_ < 0 || send(fd_, &q, sizeof q, MSG_DONTWAIT | MSG_NOSIGNAL) != sizeof q) return false;
    phase_ = phase;
    deadline_ = now() + .8;
    return true;
}
void NetSurfEscape::native() {
    input_.tap(KEY_ESC);
    log("escape.native");
}
void NetSurfEscape::press() {
    if (busy()) return; // repeats/rapid presses cannot stack delayed actions
    if (!query(Phase::Query)) native();
}
void NetSurfEscape::close(const ZeroGtkState &s) {
    auto windows = input_.toplevels();
    auto active = activeWindow(windows, s.app_id);
    auto check = s;
    check.modal = 0;
    if (!active || decideEscape(check, windows, safeVersion_) != EscapeAction::Close) {
        phase_ = Phase::Idle; return;
    }
    target_ = s; targetHandle_ = active->id;
    if (input_.closeToplevel(targetHandle_, s.app_id)) {
        log("escape.close", "handle=" + std::to_string(targetHandle_) + " kind=" + std::to_string(s.kind));
        phase_ = Phase::Closing;
        deadline_ = now() + 1;
    } else phase_ = Phase::Idle;
}
bool NetSurfEscape::tick() {
    if (fd_ < 0) return false;
    if (phase_ == Phase::Closing) {
        const auto windows = input_.toplevels();
        bool exists = std::any_of(windows.begin(), windows.end(), [&](const auto &w) { return w.id == targetHandle_; });
        if (!exists) {
            // Do not steal focus from a newly opened confirmation/dialog.
            bool another = std::any_of(windows.begin(), windows.end(), [&](const auto &w) {
                return w.active && w.appId != target_.main_id;
            });
            if (!another) input_.activateToplevel(target_.main_id);
            log("escape.closed"); phase_ = Phase::Idle;
        } else if (now() >= deadline_) { log("escape.close_unconfirmed"); phase_ = Phase::Idle; }
        return false;
    }
    if (phase_ == Phase::NativeWait) {
        if (now() >= deadline_ && !query(Phase::Verify)) phase_ = Phase::Idle;
        return false;
    }
    ZeroGtkState s{};
    for (int i = 0; i < 8; ++i) {
        auto n = recv(fd_, &s, sizeof s, MSG_DONTWAIT | MSG_TRUNC);
        if (n <= 0) break;
        if (n != sizeof s || s.version != ZERO_GTK_PROTOCOL) continue;
        if (s.serial == ZERO_GTK_OPEN_SEARCHBAR && s.kind == ZERO_GTK_MAIN) {
            log("gtk.open_searchbar");
            return true;
        }
        if (phase_ == Phase::Idle || s.serial != serial_ ||
            !memchr(s.app_id, 0, sizeof s.app_id) || !memchr(s.main_id, 0, sizeof s.main_id)) continue;
        auto phase = phase_;
        phase_ = Phase::Idle;
        if (phase == Phase::Query && s.kind == ZERO_GTK_DOWNLOADS_HIDDEN) {
            // GTK already hid Downloads on its own event loop. Re-focus the
            // browser without dispatching a second ESC or opening the overlay.
            if (s.main_id[0]) input_.activateToplevel(s.main_id);
            log("escape.downloads_hidden");
            return false;
        }
        if (phase == Phase::Verify) {
            auto windows = input_.toplevels();
            auto active = activeWindow(windows, s.app_id);
            if (active && active->id == targetHandle_ && !strcmp(s.app_id, target_.app_id) &&
                s.kind == target_.kind && !s.popup && safeSecondary(s)) close(s);
            else log("escape.native_completed");
            return false; // never overlay after cancelling a dialog
        }
        auto decision = decideEscape(s, input_.toplevels(), safeVersion_);
        log("escape.route", "kind=" + std::to_string(s.kind) + " modal=" + std::to_string(s.modal) +
            " popup=" + std::to_string(s.popup) + " app=" + s.app_id);
        if (decision == EscapeAction::Overlay) { log("escape.overlay"); return true; }
        if (decision == EscapeAction::Close) close(s);
        else {
            native();
            if (decision == EscapeAction::NativeThenCheck) {
                const auto windows = input_.toplevels();
                const auto active = activeWindow(windows, s.app_id);
                targetHandle_ = active ? active->id : 0;
                target_ = s; phase_ = Phase::NativeWait; deadline_ = now() + .25;
            }
        }
        return false;
    }
    if (phase_ != Phase::Idle && now() >= deadline_) {
        // Missing module, stalled GTK, missing protocol: pass ESC through once.
        if (phase_ == Phase::Query) native();
        log("escape.query_timeout"); phase_ = Phase::Idle;
    }
    return false;
}
}
