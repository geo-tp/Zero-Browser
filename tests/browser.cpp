#include "Browser/QtWebKitEngine.h"
#include "Browser/BrowserManager.h"
#include "Browser/NetSurfEscape.h"
#include "UI/UIManager.h"
#include <csignal>
#include <unistd.h>
#include <cstring>
#include <thread>
using namespace zero;
extern void require(bool, const char *);
void browserTests(const fs::path &root) {
    ZeroGtkState s{};
    s.version = ZERO_GTK_PROTOCOL;
    s.kind = ZERO_GTK_MAIN;
    strcpy(s.app_id, "browser"); strcpy(s.main_id, "browser");
    std::vector<Toplevel> windows{{1, 0, "Downloads - NetSurf", "browser", true, true}};
    require(decideEscape(s, windows, true) == EscapeAction::Overlay, "Page title cannot impersonate downloads");
    s.popup = 1;
    require(decideEscape(s, windows, true) == EscapeAction::Native, "Dropdown consumes ESC without overlay");
    s.popup = 0; s.kind = ZERO_GTK_DOWNLOADS;
    require(decideEscape(s, windows, true) == EscapeAction::Native, "Never close main identity even if GTK role conflicts");
    windows[0].active = false;
    windows.push_back({2, 1, "Localized downloads", "secondary", true, true});
    strcpy(s.app_id, "secondary");
    require(decideEscape(s, windows, true) == EscapeAction::Close, "Audited secondary uses identity, not translated title");
    require(decideEscape(s, windows, false) == EscapeAction::Native, "Unknown NetSurf version never closes secondary");
    windows[1].complete = false;
    require(decideEscape(s, windows, true) == EscapeAction::Native, "Incomplete Wayland state cannot close");
    windows[1].complete = true;
    windows[0].active = true;
    require(decideEscape(s, windows, true) == EscapeAction::Native, "Ambiguous focus cannot close");
    windows[0].active = false;
    s.modal = 1;
    require(decideEscape(s, windows, true) == EscapeAction::NativeThenCheck, "Modal cancellation precedes close");
    s.modal = 0; s.kind = ZERO_GTK_UNKNOWN;
    require(decideEscape(s, windows, true) == EscapeAction::Native, "Unknown window stays safe");
    s.kind = ZERO_GTK_SETTINGS;
    windows.erase(windows.begin());
    require(decideEscape(s, windows, true) == EscapeAction::Native, "No close without live main window");

    auto dir = root / "browser";
    Paths paths{dir/"config", dir/"state", dir/"cache", dir/"run"};
    paths.create();
    // Real supervised child/log parsing: replay what script-driven websites
    // report after onload. The old manager restarted on the second event.
    atomicWrite(dir/"cog", "#!/bin/sh\nprintf 'Cog: <https://example.com/> Load started.\\nCog: <https://example.com/> Loaded successfully.\\n'\nsleep .1\ni=0\nwhile [ $i -lt 8 ]; do\nprintf 'Cog: <https://www.reddit.com/> Load started.\\nCog: <https://www.reddit.com/> Loading...\\nCog: <https://www.reddit.com/> Loaded successfully.\\n'\ni=$((i+1))\nsleep .04\ndone\nsleep 60\n");
    atomicWrite(dir/"cogctl", "#!/bin/sh\nexit 0\n");
    fs::permissions(dir/"cog", fs::perms::owner_all);
    fs::permissions(dir/"cogctl", fs::perms::owner_all);
    Config config;
    config.stopTimeout = .1;
    WaylandBridge input;
    BrowserManager browser(config, paths, input, {{"PATH", dir.string()+":"+env("PATH")}});
    browser.start(Mode::Full);
    auto pid = browser.pid();
    double until = now() + 1;
    while (now() < until) { browser.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    require(browser.active() && browser.pid() == pid && !browser.loading(), "Repeated script navigations do not recycle Cog");
    require(browser.history().current() == "https://www.reddit.com/", "Script navigation still updates current URL");
    require(browser.navigate("example.org") && browser.state() == BrowserManager::State::Stopping,
            "Explicit address retains controlled process replacement");
    browser.stop();

    UIManager selector;
    Key enter{KEY_ENTER, 1, 0, false, 0}, right{KEY_RIGHT, 1, 0, false, 0};
    require(selector.key(enter, nullptr).type == Action::QtWebKit, "QtWebKit is default");
    selector.key(right, nullptr);
    require(selector.key(enter, nullptr).type == Action::Lite, "NetSurf is second choice");
    selector.key(right, nullptr);
    require(selector.key(enter, nullptr).type == Action::QtWebKit, "Two-engine navigation wraps");
    for (const auto mode : {Mode::Lite, Mode::QtWebKit}) {
        selector.show(UIManager::View::Transition);
        selector.selectEngine(mode);
        require(selector.view() == UIManager::View::Select && selector.key(enter, nullptr).type ==
                (mode == Mode::Lite ? Action::Lite : Action::QtWebKit),
                "Startup failure retains the requested engine for OK-to-retry");
        BrowserManager failed(config, paths, input, {}, [](Mode) {
            throw std::runtime_error("injected viewport configuration failure");
        });
        failed.start(mode);
        failed.tick();
        require(failed.state() == BrowserManager::State::Idle && failed.pid() == 0 &&
                failed.takeSelectionRequest() && failed.mode() == mode &&
                failed.error().find("injected viewport configuration failure") != std::string::npos,
                "Both engine adapters return to selection after display preparation failure");
    }

    // Keybind help lives inside Settings, not on the engine selector.
    UIManager help;
    Key down{KEY_DOWN, 1, 0, false, 0};
    help.key(down, nullptr);
    help.key(enter, nullptr);
    require(help.view() == UIManager::View::Settings, "Settings opens from selector");
    for (int i = 0; i < 4; ++i) help.key(down, nullptr);
    help.key(enter, nullptr);
    require(help.view() == UIManager::View::Keybinds, "Keybind help opens from Settings");
    help.escape();
    require(help.view() == UIManager::View::Settings, "Keybind ESC returns to Settings");
    help.escape();
    require(help.view() == UIManager::View::Select, "Settings ESC returns to selector");

    EngineEvent event{};
    require(QtWebKitEngine::parseLine("ZERO_QT_EVENT loaded 1.25 \"https://example.org/\" \"\"", event) &&
        event.kind == EngineEvent::Loaded && event.seconds == 1.25, "Qt load timing parser");
    require(!QtWebKitEngine::parseLine("qt.qpa.plugin: cannot initialize wayland", event),
        "Qt plugin errors are diagnostics, not readiness");
    require(!QtWebKitEngine::parseLine("ZERO_QT_EVENT url 0 \"file:///etc/passwd\" \"\"", event),
        "Qt URL protocol remains restricted");
    require(QtWebKitEngine::parseLine("ZERO_QT_EVENT url 0 \"https://en.wikipedia.org/wiki/Test\" \"\"", event) &&
        event.kind == EngineEvent::Redirected,
        "Qt URL changes retain the existing event protocol");

    require(QtWebKitEngine::parseLine("ZERO_QT_EVENT progress 0 \"\" \"Fetching\" 42", event) &&
        event.kind == EngineEvent::LoadProgress && event.progress == 42, "Qt resource progress parser");
    require(!QtWebKitEngine::parseLine("ZERO_QT_EVENT progress 0 \"\" \"Fetching\" 142", event),
        "Reject invalid Qt progress");

    // Exercise an actual child exiting before display readiness; the manager
    // must reap it before returning to selection and allow a subsequent launch.
    atomicWrite(dir/"qt-fail", "#!/bin/sh\necho 'qt.qpa.plugin: failed to initialize wayland' >&2\nexit 134\n");
    fs::permissions(dir/"qt-fail", fs::perms::owner_all);
    setenv("ZERO_BROWSER_QTWEBKIT_BINARY", (dir/"qt-fail").c_str(), 1);
    BrowserManager qt(config, paths, input, {});
    qt.start(Mode::QtWebKit);
    auto failedPid = qt.pid();
    until = now() + 3;
    while (qt.state() != BrowserManager::State::Idle && now() < until) {
        qt.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(qt.takeSelectionRequest() && !qt.error().empty() && qt.pid() == 0,
        "Qt display failure returns to selector after process cleanup");
    require(kill(failedPid, 0) == -1, "Failed Qt helper is reaped");
    atomicWrite(dir/"qt-fail", "#!/bin/sh\n"
        "printf 'ZERO_QT_EVENT ready 0.1 \"\" \"\"\\n"
        "ZERO_QT_EVENT loaded 0.2 \"\" \"\"\\n"
        "ZERO_QT_EVENT rendering 0.2 \"\" \"\"\\n'\n"
        "sleep .3\n"
        "printf 'ZERO_QT_EVENT rendered 0.3 \"\" \"\"\\n"
        "ZERO_QT_EVENT notice 0 \"\" \"LOW RAM: zoom queued. Reload page to apply.\"\\n'\n"
        "sleep 60\n");
    fs::permissions(dir/"qt-fail", fs::perms::owner_all);
    qt.start(Mode::QtWebKit);
    until = now() + 2;
    while (!qt.active() && now() < until) {
        qt.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(qt.active() && qt.history().current() == "https://lite.duckduckgo.com/lite/",
        "Qt retry starts DuckDuckGo with a new process");
    require(qt.loadPhase() == "Rendering" && !qt.loading(), "Qt awaits first paint after page load");
    until = now() + 2;
    while (qt.loadPhase() != "Loaded" && now() < until) {
        qt.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(qt.loadPhase() == "Loaded" && qt.recentLoad(), "Qt paint completion restores Loaded status");
    require(qt.zoomNoticeActive() && qt.loadPhase() == "Loaded", "Low RAM zoom notice does not replace load status");
    require(!qt.clearHistory() && !qt.history().visits().empty(), "Cleanup cannot erase an active session");
    UIManager toolbar;
    toolbar.show(UIManager::View::Web);
    require(toolbar.toolbarAction(40, 10, &qt).type == Action::None,
        "Disabled forward consumes no navigation action");
    require(toolbar.toolbarAction(72, 10, &qt).type == Action::Reload &&
            toolbar.toolbarAction(94, 10, &qt).type == Action::Home &&
            toolbar.toolbarAction(120, 10, &qt).type == Action::Downloads &&
            toolbar.toolbarAction(139, 10, &qt).type == Action::ZoomOut &&
            toolbar.toolbarAction(156, 10, &qt).type == Action::ZoomIn &&
            toolbar.toolbarAction(179, 10, &qt).type == Action::ToggleJavascript &&
            toolbar.toolbarAction(230, 10, &qt).type == Action::Address,
        "Toolbar hits match the visible controls");
    require(toolbar.toolbarAction(180, 20, &qt).type == Action::None &&
            toolbar.toolbarAction(-1, 10, &qt).type == Action::None,
        "Page pixels and outside coordinates are not toolbar clicks");
    toolbar.address("https://example.org/");
    require(toolbar.toolbarAction(72, 10, &qt).type == Action::None,
        "Address overlay cannot activate hidden toolbar");
    qt.selectEngine();
    until = now() + 2;
    while (qt.state() != BrowserManager::State::Idle && now() < until) {
        qt.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(qt.takeSelectionRequest() && qt.pid() == 0, "Returning from Qt cleans up the child tree");
    atomicWrite(paths.state / "last-success", "https://example.org/");
    atomicWrite(paths.state / "last-full-success", "https://example.org/");
    require(qt.clearHistory() && qt.history().visits().empty(), "Cleanup clears in-memory history");
    qt.stop(); // A subsequent save must not restore the deleted visits.
    History cleared;
    cleared.load(paths.state / "history");
    require(cleared.visits().empty() && cleared.current().empty() &&
        !fs::exists(paths.state / "last-success") && !fs::exists(paths.state / "last-full-success"),
        "History and recovery markers stay cleared after later saves");
    qt.start(Mode::QtWebKit, "example.org/initial");
    require(qt.history().current() == "https://example.org/initial" &&
        config.home == "https://lite.duckduckgo.com/lite/", "Explicit startup URL overrides home for this launch only");
    qt.stop();

    // Simulate clicking a search result: QtWebKit's loadStarted can have no
    // requested URL, but urlChanged still announces the actual destination.
    // Both links must survive a save/reload and reach address suggestions.
    atomicWrite(dir/"qt-history-helper", R"SH(#!/bin/sh
printf '%s\n' \
'ZERO_QT_EVENT ready 0 "" ""' \
'ZERO_QT_EVENT start 0 "https://lite.duckduckgo.com/lite/" ""' \
'ZERO_QT_EVENT loaded 0.1 "https://lite.duckduckgo.com/lite/" ""' \
'ZERO_QT_EVENT start 0 "" ""' \
'ZERO_QT_EVENT url 0 "https://lite.duckduckgo.com/lite/?q=test" ""' \
'ZERO_QT_EVENT loaded 0.1 "https://lite.duckduckgo.com/lite/?q=test" ""' \
'ZERO_QT_EVENT start 0 "" ""' \
'ZERO_QT_EVENT url 0 "https://en.wikipedia.org/wiki/Test" ""' \
'ZERO_QT_EVENT loaded 0.1 "https://en.wikipedia.org/wiki/Test" ""'
sleep 60
)SH");
    fs::permissions(dir/"qt-history-helper", fs::perms::owner_all);
    setenv("ZERO_BROWSER_QTWEBKIT_BINARY", (dir/"qt-history-helper").c_str(), 1);
    auto historyDir = dir / "qt-history-state";
    Paths historyPaths{historyDir/"config", historyDir/"state", historyDir/"cache", historyDir/"run"};
    historyPaths.create();
    BrowserManager qtHistory(config, historyPaths, input, {});
    qtHistory.start(Mode::QtWebKit);
    until = now() + 3;
    while (qtHistory.history().visits().size() < 3 && now() < until) {
        qtHistory.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(qtHistory.history().visits().size() == 3 && qtHistory.history().stack().size() == 1,
        "Qt internal links record visits without changing native Back/Forward behavior");
    History onDisk;
    onDisk.load(historyPaths.state / "history");
    require(onDisk.visits().size() == 3 &&
            onDisk.visits()[1].url == "https://lite.duckduckgo.com/lite/?q=test" &&
            onDisk.visits()[2].url == "https://en.wikipedia.org/wiki/Test" &&
            onDisk.visits()[2].engine == "QTWEBKIT",
        "Qt link destinations persist on disk with their source engine");
    UIManager qtSuggestions;
    qtSuggestions.address("wikipedia.org/wiki/Test");
    Key tab{KEY_TAB, 1, 0, false, 0};
    qtSuggestions.key(tab, &qtHistory);
    require(qtSuggestions.key(enter, &qtHistory).text == "https://en.wikipedia.org/wiki/Test",
        "Qt visits appear in the address overlay suggestions");
    qtHistory.stop();

    // Switch through every adapter using owned fake child processes. The
    // compositor/renderers remain hardware checks; lifecycle/history are real.
    atomicWrite(dir/"qt-control", R"PY(#!/usr/bin/env python3
import os
import socket
import sys

channel = socket.socket(fileno=int(os.environ['ZERO_QT_CONTROL_FD']))
channel.setblocking(True)
print('ZERO_QT_EVENT ready 0 "" ""', flush=True)
print('ZERO_QT_EVENT loaded 0 "' + sys.argv[1] + '" ""', flush=True)
while True:
    packet = channel.recv(8192)
    if not packet or packet == b'quit':
        break
    with open(os.environ['ZERO_TEST_QT_COMMANDS'], 'ab') as commands:
        commands.write(packet + b'\n')
)PY");
    fs::permissions(dir/"qt-control", fs::perms::owner_all);
    setenv("ZERO_BROWSER_QTWEBKIT_BINARY", (dir/"qt-control").c_str(), 1);
    const auto qtCommands = dir/"qt-commands";
    setenv("ZERO_TEST_QT_COMMANDS", qtCommands.c_str(), 1);
    atomicWrite(dir/"netsurf-gtk", "#!/bin/sh\nexec sleep 60\n");
    atomicWrite(dir/"xdg-user-dir", "#!/bin/sh\nprintf '%s\\n' \"$ZERO_TEST_DOWNLOADS\"\n");
    fs::permissions(dir/"netsurf-gtk", fs::perms::owner_all);
    fs::permissions(dir/"xdg-user-dir", fs::perms::owner_all);
    const auto oldPath = env("PATH");
    setenv("PATH", (dir.string() + ":" + oldPath).c_str(), 1);
    setenv("ZERO_TEST_DOWNLOADS", (dir/"downloads").c_str(), 1);
    BrowserManager switching(config, paths, input, {});
    auto waitActive = [&] {
        const double deadline = now() + 7;
        while (!switching.active() && now() < deadline) {
            switching.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(switching.active(), "Switched engine becomes active");
    };
    switching.start(Mode::QtWebKit, "example.org/switching");
    waitActive();
    require(switching.toggleJavascript() && !switching.javascriptEnabled(),
            "Qt JavaScript toggle updates the supervisor state to disabled");
    require(switching.toggleJavascript() && switching.javascriptEnabled(),
            "Qt JavaScript toggle restores the enabled state");
    until = now() + 2;
    while (readFile(qtCommands) != "toggle-js\ntoggle-js\n" && now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(readFile(qtCommands) == "toggle-js\ntoggle-js\n",
            "Both JavaScript toggles reach the helper using the unchanged IPC command");
    require(switching.navigate("example.net/next"), "Qt navigation command accepted");
    require(switching.back() && switching.history().current() == "https://example.org/switching",
            "Qt back command preserves supervisor history position");
    require(switching.forward() && switching.history().current() == "https://example.net/next",
            "Qt forward command preserves supervisor history position");
    for (const auto target : {Mode::Full, Mode::Lite, Mode::QtWebKit}) {
        const auto previousPid = switching.pid();
        switching.start(target);
        require(switching.state() == BrowserManager::State::Stopping,
                "Engine switch goes through owned-process shutdown");
        waitActive();
        require(switching.mode() == target && switching.pid() != previousPid && kill(previousPid, 0) == -1,
                "Engine switch reaps the previous child before using the new adapter");
        if (target != Mode::QtWebKit)
            require(!switching.toggleJavascript() && switching.javascriptEnabled(),
                    "Other engines reject JavaScript toggles without changing supervisor state");
    }
    switching.stop();
    require(!switching.toggleJavascript(), "Stopping Qt rejects JavaScript toggles");
    setenv("PATH", oldPath.c_str(), 1);
    unsetenv("ZERO_TEST_DOWNLOADS");
    unsetenv("ZERO_BROWSER_QTWEBKIT_BINARY");
    unsetenv("ZERO_TEST_QT_COMMANDS");
}
