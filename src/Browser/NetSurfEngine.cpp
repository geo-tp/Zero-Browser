#include "NetSurfEngine.h"
#include <linux/input-event-codes.h>
#include <sstream>
#include <algorithm>
#include <vector>
namespace zero {
namespace {
// NetSurf 3.11 serializes native GTK toolbar entries as slash-separated names.
// Keep the profile's existing layout while ensuring the native toolbar controls
// sit immediately after Reload (and before the location entry).
void ensureLiteToolbar(const fs::path &choices) {
    constexpr const char *prefix = "toolbar_items:";
    constexpr const char *defaultItems =
        "back/forward/reloadstop/home/downloads/zoomminus/zoomplus/zoomnormal/url_bar/openmenu";
    const std::string original = readFile(choices);
    std::istringstream lines(original);
    std::string line, updated, existing;
    bool found = false;
    while (std::getline(lines, line)) {
        if (line.rfind(prefix, 0) == 0) {
            if (!found) {
                existing = line.substr(std::char_traits<char>::length(prefix));
                if (!existing.empty() && existing.back() == '\r') existing.pop_back();
                found = true;
            }
            continue;
        }
        updated += line + '\n';
    }
    std::vector<std::string> items;
    std::istringstream entries(existing);
    std::string item;
    while (std::getline(entries, item, '/')) {
        if (!item.empty() && item != "zoomminus" && item != "zoomplus" && item != "zoomnormal" &&
            item != "home" && item != "history" && item != "downloads" && item != "fullscreen") items.push_back(item);
    }
    // Keep NetSurf's single native menu button, even with legacy custom layouts.
    bool seenMenu = false;
    items.erase(std::remove_if(items.begin(), items.end(), [&](const std::string &entry) {
        if (entry != "openmenu") return false;
        if (seenMenu) return true;
        seenMenu = true;
        return false;
    }), items.end());
    if (items.empty()) {
        updated += std::string(prefix) + defaultItems + "\n";
    } else {
        auto pos = std::find(items.begin(), items.end(), "reloadstop");
        if (pos == items.end()) pos = std::find(items.begin(), items.end(), "reload");
        if (pos != items.end()) ++pos;
        else {
            pos = std::find(items.begin(), items.end(), "url_bar");
            if (pos == items.end()) pos = items.end();
        }
        pos = items.insert(pos, "home");
        pos = items.insert(pos + 1, "downloads");
        pos = items.insert(pos + 1, "zoomminus");
        pos = items.insert(pos + 1, "zoomplus");
        pos = items.insert(pos + 1, "zoomnormal");

        updated += prefix;
        for (size_t i = 0; i < items.size(); ++i) {
            if (i) updated += '/';
            updated += items[i];
        }
        updated += '\n';
    }
    if (updated != original) atomicWrite(choices, updated);
}
// Keep scripting disabled for this comparison, including existing profiles.
// Preserve every unrelated preference; avoid accumulating duplicate keys.
void disableLiteJavaScript(const fs::path &choices) {
    constexpr const char *prefix = "enable_javascript:";
    const std::string original = readFile(choices);
    std::istringstream lines(original);
    std::string line, updated;
    while (std::getline(lines, line)) {
        if (line.rfind(prefix, 0) != 0) updated += line + '\n';
    }
    updated += std::string(prefix) + "0\n";
    if (updated != original) atomicWrite(choices, updated);
}
void setLiteHomepage(const fs::path &choices) {
    constexpr const char *prefix = "homepage_url:";
    constexpr const char *homepage = "https://lite.duckduckgo.com/lite/";
    const std::string original = readFile(choices);
    std::istringstream lines(original);
    std::string line, updated;
    while (std::getline(lines, line)) {
        if (line.rfind(prefix, 0) != 0) updated += line + '\n';
    }
    updated += std::string(prefix) + homepage + "\n";
    if (updated != original) atomicWrite(choices, updated);
}
// Only migrate the stock font sizes; preserve values explicitly chosen by the user.
void setCompactDefaultFont(const fs::path &choices) {
    const auto original = readFile(choices);
    std::istringstream lines(original);
    std::string line, updated;
    while (std::getline(lines, line)) {
        if (line == "font_size:100" || line == "font_size:90" || line == "font_size:80" || line == "font_size:70" || line == "font_size:65" || line == "font_size:60") line = "font_size:45";
        else if (line == "font_min_size:80" || line == "font_min_size:70" || line == "font_min_size:60" || line == "font_min_size:55" || line == "font_min_size:50" || line == "font_min_size:45") line = "font_min_size:35";
        updated += line + '\n';
    }
    if (updated != original) atomicWrite(choices, updated);
}
// NetSurf's native backing store performs its own eviction. The option is
// specified in bytes, not MiB; retain all other user preferences.
void setLiteDiskCacheLimit(const fs::path &choices) {
    constexpr const char *prefix = "disc_cache_size:";
    constexpr const char *value = "disc_cache_size:268435456\n"; // 256 MiB
    const auto original = readFile(choices);
    std::istringstream lines(original);
    std::string line, updated;
    while (std::getline(lines, line)) {
        if (line.rfind(prefix, 0) != 0) updated += line + '\n';
    }
    updated += value;
    if (updated != original) atomicWrite(choices, updated);
}
void setDefaultDownloads(const fs::path &choices) {
    constexpr const char *prefix = "downloads_directory:";
    const auto home = fs::path(env("HOME"));
    const auto contents = readFile(choices);
    std::istringstream lines(contents);
    std::string line, current, updated;
    while (std::getline(lines, line)) {
        if (line.rfind(prefix, 0) == 0) {
            current = line.substr(std::char_traits<char>::length(prefix));
            if (!current.empty() && current.back() == '\r') current.pop_back();
        } else updated += line + '\n';
    }
    // Migrate NetSurf's old HOME default, but retain an explicitly chosen folder.
    if (!current.empty() && fs::path(current).lexically_normal() != home.lexically_normal()) return;
    // Resolve the desktop setting BEFORE replacing XDG_CONFIG_HOME with the
    // isolated NetSurf profile; user-dirs.dirs belongs to the Linux desktop.
    auto result = command({"xdg-user-dir", "DOWNLOAD"}, 2);
    auto directory = result.output;
    while (!directory.empty() && (directory.back() == '\n' || directory.back() == '\r')) directory.pop_back();
    fs::path downloads = directory;
    if (result.status != 0 || !downloads.is_absolute() || directory.find_first_of("\r\n") != std::string::npos ||
        downloads.lexically_normal() == home.lexically_normal()) downloads = home / "Downloads";
    std::error_code ec;
    fs::create_directories(downloads, ec);
    if (ec) log("downloads.directory_error", ec.message());
    updated += prefix + downloads.string() + '\n';
    atomicWrite(choices, updated);
    log("downloads.default", downloads.string());
}
}
void NetSurfEngine::start(const std::string &url) {
    auto root = paths_.config / "lite";
    fs::create_directories(root / "netsurf");
    // Keep the native location bar: Ctrl+L is not guaranteed to focus a hidden widget.
    auto choices = root / "netsurf/Choices";
    if (!fs::exists(choices))
        atomicWrite(choices, "window_width:320\nwindow_height:170\nfont_size:45\nfont_min_size:35\nshow_"
                             "single_tab:0\nbar_show:tool\nbutton_type:2\nenable_javascript:0\n");
    setCompactDefaultFont(choices);
    disableLiteJavaScript(choices);
    ensureLiteToolbar(choices);
    setLiteHomepage(choices);
    setDefaultDownloads(choices);
    setLiteDiskCacheLimit(choices);
    environment_["XDG_CONFIG_HOME"] = root.string();
    environment_["XDG_CACHE_HOME"] = (paths_.cache / "lite").string();
    launched_ = now();
    readyAt_ = now() + 2;
    escape_.prepare(environment_);
    process_.start({"netsurf-gtk", url}, environment_);
    escape_.childStarted();
    log("engine.start", "LITE pid=" + std::to_string(process_.pid()));
}
bool NetSurfEngine::open(const std::string &url) {
    if (!ready_ || action_ || stopping_)
        return false;
    escape_.cancel();
    address_ = url;
    input_.tap(KEY_L, 4);
    action_ = 1;
    actionAt_ = now() + .12;
    return true;
}
bool NetSurfEngine::reload() {
    if (!ready_ || action_ || stopping_)
        return false;
    escape_.cancel();
    input_.tap(KEY_F5);
    emit_({EngineEvent::CommandDone, "", "reload", 0});
    return true;
}
void NetSurfEngine::snapshot() {
    if (!ready_ || action_ || stopping_)
        return;
    escape_.cancel();
    input_.tap(KEY_L, 4);
    action_ = 10;
    actionAt_ = now() + .15;
}
void NetSurfEngine::escape() {
    if (stopping_ || !ready_) return;
    // An address/snapshot keyboard transaction owns its key sequence. ESC
    // consumes this press; never close a window during that transaction.
    if (action_) return;
    escape_.press();
}
void NetSurfEngine::tick() {
    process_.poll();
    auto out = process_.drain();
    if (!out.empty())
        log("netsurf", out);
    if (!stopping_ && !process_.running() && !crashReported_) {
        crashReported_ = true;
        emit_({EngineEvent::Crashed, "", "NetSurf exited (" + std::to_string(process_.status()) + ")"});
    }
    if (!stopping_ && process_.running() && !ready_ && now() >= readyAt_) {
        ready_ = true;
        emit_({EngineEvent::Ready, "", "LITE (load completion unavailable)", now() - launched_});
    }
    if (!stopping_ && escape_.tick()) emit_({EngineEvent::OverlayRequested, "", ""});
    if (stopping_ || now() < actionAt_)
        return;
    // Clipboard delivery is asynchronous. Never strand navigation permanently
    // if the compositor does not provide a clipboard response.
    if (action_ == 12 && now() - actionAt_ > 2.0) {
        action_ = 0;
        input_.tap(KEY_ESC);
        log("netsurf.snapshot_timeout");
        return;
    }
    if (action_ == 1) {
        input_.tap(KEY_A, 4);
        input_.paste(address_);
        action_ = 2;
        actionAt_ = now() + .15;
    } else if (action_ == 2) {
        input_.tap(KEY_ENTER);
        action_ = 0;
        emit_({EngineEvent::CommandDone, address_, "open (native keyboard)", 0});
    } else if (action_ == 10) {
        input_.tap(KEY_C, 4);
        action_ = 11;
        actionAt_ = now() + .2;
    } else if (action_ == 11) {
        input_.requestClipboard();
        action_ = 12;
        actionAt_ = now() + .05;
    } else if (action_ == 12) {
        std::string url;
        if (input_.clipboardResult(url)) {
            action_ = 0;
            input_.tap(KEY_ESC);
            try {
                emit_({EngineEvent::UrlSnapshot, normalizeUrl(url), "native location bar"});
            } catch (...) {
                log("netsurf.url_unavailable");
            }
        }
    }
}
void NetSurfEngine::stop() {
    if (stopping_)
        return;
    escape_.cancel();
    input_.tap(KEY_Q, 4);
    BrowserEngine::stop();
}
} // namespace zero
