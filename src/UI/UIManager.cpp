#include "UIManager.h"
#include "UiFont.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
namespace zero {
namespace {
constexpr uint32_t bg = 0x15171c, panel = 0x292d35, border = 0x4a4f59, green = 0x79cc94, blue = 0xa8bde0,
                   white = 0xf4f5f7, muted = 0xaab0bb, amber = 0xd4c3a9;
constexpr int toolbarEdges[] = {0, 24, 48, 75, 102, 129, 147, 165, 192, 320};
constexpr Action::Type toolbarActions[] = {Action::Back, Action::Forward, Action::Reload, Action::Home,
                                          Action::Downloads, Action::ZoomOut, Action::ZoomIn, Action::ToggleJavascript, Action::Address};
int labelWidth(const std::string &label) {
    int width = 0;
    for (unsigned char c : label) width += platform::linux::ui_font::Small.glyphs[(c < 32 || c > 126 ? '?' : c) - 32].advance;
    return width;
}
}
Action UIManager::toolbarAction(int x, int y, const BrowserManager *b) const {
    if (view_ != View::Web || !b || !b->active() || b->mode() == Mode::Lite ||
        x < 0 || x >= 320 || y < 0 || y >= ToolbarHeight) return {};
    for (int i = 0; i < 9; ++i) {
        if (x >= toolbarEdges[i + 1]) continue;
        if ((i == 0 && !b->history().canBack()) || (i == 1 && !b->history().canForward())) return {};
        if (i == 2 && b->mode() == Mode::QtWebKit && b->loading())
            return {Action::StopLoading, {}};
        if (i == 7 && b->mode() != Mode::QtWebKit) return {};
        return {toolbarActions[i], {}};
    }
    return {};
}
void UIManager::rect(std::vector<uint32_t> &f, int x, int y, int w, int h, uint32_t c) {
    for (int j = std::max(y, 0); j < std::min(y + h, 170); ++j)
        for (int i = std::max(x, 0); i < std::min(x + w, 320); ++i)
            f[j * 320 + i] = c;
}
void UIManager::text(std::vector<uint32_t> &f, int x, int y, const std::string &s, uint32_t color, bool large,
                     int maxWidth) {
    const auto &font = large ? platform::linux::ui_font::Title : platform::linux::ui_font::Small;
    int start = x;
    for (unsigned char c : s) {
        if (c < 32 || c > 126)
            c = '?';
        auto &g = font.glyphs[c - 32];
        if (x + g.advance > start + maxWidth)
            break;
        for (int j = 0; j < font.height; ++j)
            for (int i = 0; i < g.width; ++i) {
                int px = x + i, py = y + j;
                if (px < 0 || px >= 320 || py < 0 || py >= 170)
                    continue;
                auto a = font.pixels[g.offset + j * g.width + i];
                if (!a)
                    continue;
                auto under = f[py * 320 + px];
                uint32_t result = 0;
                for (int shift : {0, 8, 16})
                    result |= ((((color >> shift) & 255) * a + ((under >> shift) & 255) * (255 - a)) / 255)
                              << shift;
                f[py * 320 + px] = result;
            }
        x += g.advance;
    }
}
void UIManager::draw(std::vector<uint32_t> &f, const BrowserManager *b, const SystemStatus &system, double hold,
                     bool pointerMode, int px, int py, const std::string &notice, bool clickHint, bool hideQtStatus) {
    auto centeredX = [](const std::string &label, bool large) {
        const auto &font = large ? platform::linux::ui_font::Title : platform::linux::ui_font::Small;
        int width = 0;
        for (unsigned char ch : label)
            if (ch >= 32 && ch <= 126) width += font.glyphs[ch - 32].advance;
        return std::max(0, (320 - width) / 2);
    };
    if (view_ == View::Transition) {
        // Match the light engine-selection screen while the engine starts.
        const uint32_t ink = 0x2b3740, subtle = 0x68747d, line = 0xd9dcdf;
        std::fill(f.begin(), f.end(), 0xf5f6f7);
        rect(f, 0, 0, 320, 29, 0xfdfdfd);
        rect(f, 0, 28, 320, 1, line);
        text(f, 10, 9, "ZERO BROWSER", ink);
        const bool restoring = b && b->recovering();
        const bool qt = (b ? b->displayMode() : transitionMode_) == Mode::QtWebKit;
        const bool full = (b ? b->displayMode() : transitionMode_) == Mode::Full;
        const uint32_t accent = qt ? 0x356cae : 0x25856a;
        text(f, qt ? 249 : 270, 9, qt ? "WEBKIT" : full ? "COG" : "HTML", accent, false, 70);
        // Keep the QtWebKit safety tips visible during both display recovery
        // and engine startup. Previously the early-return recovery screen hid them.
        auto drawQtWarning = [&](int y, bool retrying) {
            // Display recovery can be caused by unavailable graphics buffers.
            // Keep the explanation informational, not an unproven diagnosis.
            if (!qt && !retrying) return;
            const int warningCount = qt ? (retrying ? 4 : 3) : 1;
            const int warningIndex = static_cast<int>(now() / 3.5) % warningCount;
            const int tipIndex = retrying ? warningIndex - 1 : warningIndex;
            const std::string warning = retrying && warningIndex == 0
                ? "Graphics memory may be busy" :
                tipIndex == 0 ? "Legacy WebKit: NOT secure" :
                tipIndex == 1 ? "Avoid logins / untrusted sites" :
                "Experimental: RAM limits heavy JS";
            text(f, centeredX(warning, false), y, warning, 0x78838b, false, 310);
        };
        const auto separator = notice.find('|');
        if (separator != std::string::npos) {
            const std::string title = "Starting browser";
            text(f, centeredX(title, true), 52, title, ink, true, 310);
            // Technical startup stages remain in the logs, not on the 320x170 LCD.
            const auto second = notice.find('|', separator + 1);
            const std::string attempt = notice.substr(separator + 1, second - separator - 1);
            const bool retrying = attempt.find("Attempt 1/") == std::string::npos;
            const std::string status = retrying ? "Preparing display  /  " + attempt : "Preparing display...";
            text(f, centeredX(status, false), 81, status, subtle, false, 310);
            drawQtWarning(107, retrying);
            const int position = static_cast<int>(now() * 70) % 200;
            rect(f, 52, 127, 216, 4, 0xe3e5e7);
            rect(f, 52 + position, 127, 16, 4, accent);
            const std::string hint = hold > 0 ? "Exiting... keep holding ESC" : "Hold ESC 3s to exit";
            text(f, centeredX(hint, false), 142, hint, subtle, false, 310);
            return;
        }
        const std::string title = restoring ? "Restoring session" : "Starting browser";
        text(f, centeredX(title, true), 57, title, ink, true, 310);
        const std::string stage = !b && !notice.empty() ? notice :
            qt ? "QtWebKit / JavaScript" : full ? "Cog / WebKit" : "NetSurf / HTML";
        text(f, centeredX(stage, false), 83, stage, subtle, false, 310);
        if (qt) {
            drawQtWarning(101, false);
        } else {
            const std::string message = restoring ? "Restoring previous page" : "Preparing rendering engine...";
            text(f, centeredX(message, false), 101, message, subtle, false, 310);
        }
        const int position = static_cast<int>(now() * 70) % 200;
        rect(f, 52, 127, 216, 5, 0xe3e5e7);
        rect(f, 52 + position, 127, 16, 5, accent);
        if (!b) {
            const std::string hint = hold > 0 ? "Exiting... keep holding ESC" : "Hold ESC 3s to exit";
            text(f, centeredX(hint, false), 142, hint, subtle, false, 310);
        }
        return;
    }
    if (view_ == View::Web) {
        if (b && b->mode() != Mode::Lite) {
            rect(f, 0, 0, 320, ToolbarHeight, 0xf7f8f9);
            rect(f, 0, ToolbarHeight - 1, 320, 1, 0xd3d6d9);
            auto host = b->history().current();
            auto scheme = host.find("://");
            if (scheme != std::string::npos) host.erase(0, scheme + 3);
            host = host.substr(0, host.find('/'));
            std::string labels[] = {"<", ">", "", "", "", "-", "+", "", host};
            for (int i = 0; i < 9; ++i) {
                const int x = toolbarEdges[i], width = toolbarEdges[i + 1] - x;
                const bool enabled = b->active() && (i != 0 || b->history().canBack()) &&
                                     (i != 1 || b->history().canForward());
                const auto color = enabled ? 0x283441u : 0xa4adb5u;
                if (enabled && pointerMode && py < ToolbarHeight && px >= x && px < x + width)
                    rect(f, x + 1, 1, width - 2, ToolbarHeight - 2, 0xe7e9eb);
                // JS and the address read as a single combined control, no vertical divider.
                if (i != 7 && i != 8)
                    rect(f, x + width - 1, 3, 1, ToolbarHeight - 7, 0xcbd1d5);
                if (i == 2) {
                    if (b->mode() == Mode::QtWebKit && b->loading()) {
                        // Solid stop square while the current page is loading.
                        rect(f, x + (width - 9) / 2, 6, 9, 9, color);
                    } else {
                        // Thin nearly-complete circular refresh arrow.
                        const int cx = x + width / 2, cy = 10;
                        for (int angle = 38; angle < 330; angle += 5) {
                            const double a = angle * 3.141592653589793 / 180.0;
                            rect(f, cx + static_cast<int>(6 * std::cos(a)),
                                 cy + static_cast<int>(6 * std::sin(a)), 1, 1, color);
                        }
                        rect(f, cx + 4, 4, 3, 1, color);
                        rect(f, cx + 6, 4, 1, 4, color);
                    }
                } else if (i == 3) {
                    const int cx = x + width / 2;
                    for (int step = 0; step < 7; ++step) {
                        rect(f, cx - step, 4 + step, 1, 1, color);
                        rect(f, cx + step, 4 + step, 1, 1, color);
                    }
                    rect(f, cx - 4, 10, 1, 6, color); rect(f, cx + 4, 10, 1, 6, color);
                    rect(f, cx - 4, 15, 9, 1, color); rect(f, cx - 1, 12, 3, 4, color);
                } else if (i == 4) {
                    // Downloads: tray with downward arrow.
                    const int cx = x + width / 2;
                    rect(f, cx - 6, 13, 13, 2, color);
                    rect(f, cx - 4, 15, 9, 1, color);
                    rect(f, cx - 6, 10, 2, 5, color);
                    rect(f, cx + 5, 10, 2, 5, color);
                    rect(f, cx, 4, 2, 7, color);
                    rect(f, cx - 3, 8, 3, 2, color);
                    rect(f, cx + 2, 8, 3, 2, color);
                    rect(f, cx - 2, 10, 5, 2, color);
                } else if (i == 7) {
                    const bool jsOn = b->javascriptEnabled();
                    rect(f, x + 1, 3, width - 2, 14, jsOn ? 0xdcecdf : 0xf0dddd);
                    const uint32_t jsColor = jsOn ? 0x275b3a : 0x9a3d3d;
                    text(f, x + 2, 5, "JS", jsColor, false, width - 3);
                    // Distinct check/cross glyph instead of +/- next to the zoom buttons.
                    // Draw directly so the icon works with the tiny built-in font.
                    const int iconX = x + width - 10;
                    if (jsOn) {
                        for (int step = 0; step < 3; ++step)
                            rect(f, iconX + step, 10 + step, 1, 1, jsColor);
                        for (int step = 0; step < 5; ++step)
                            rect(f, iconX + 2 + step, 12 - step, 1, 1, jsColor);
                    } else {
                        for (int step = 0; step < 7; ++step) {
                            rect(f, iconX + step, 7 + step, 1, 1, jsColor);
                            rect(f, iconX + step, 13 - step, 1, 1, jsColor);
                        }
                    }
                } else {
                    const int offset = i == 8 ? 3 : std::max(2, (width - labelWidth(labels[i])) / 2);
                    text(f, x + offset, i == 8 ? 6 : 4, labels[i], i == 8 ? 0x43647e : color, false, width - offset - 3);
                }
            }
            const bool qt = b->mode() == Mode::QtWebKit;
            const bool downloadsWindow = qt && (b->downloadsVisible() || hideQtStatus);
            std::string label;
            bool busy = b->loading();
            if (busy) {
                const auto t = b->loadingSeconds();
                if (qt) label = b->loadPhase() + " " + std::to_string(b->loadProgress()) + "%";
                else if (!b->loadStarted()) label = "Waiting for WebKit";
                else label = t > 80 ? "Slow page - ESC for options" : "Loading";
                label += "  " + std::to_string(static_cast<int>(std::min(t, 999.0))) + "s";
            } else if (b->recentLoad()) {
                std::ostringstream status;
                status << b->loadPhase();
                if (b->loadPhase() == "Loaded") status << "  " << std::fixed << std::setprecision(1) << b->lastLoadSeconds() << "s";
                label = status.str();
            } else if (!qt && clickHint) {
                label = "Awaiting response";
                busy = true;
            }
            // Zoom warnings have their own timeout and override transient hover/load labels.
            if (qt && b->zoomNoticeActive()) {
                label = b->zoomNotice();
                busy = false;
            }
            // Keep the indicator clear of the right and bottom (7px) scrollbars.
            const bool linkPreview = qt && !b->zoomNoticeActive() && !busy && !b->hoveredLink().empty();
            if (linkPreview) {
                label = b->hoveredLink();
                while (!label.empty() && labelWidth(label) > 290) label.pop_back();
                if (label != b->hoveredLink() && label.size() > 3)
                    label.replace(label.size() - 3, 3, "...");
            }
            if (!label.empty() && !downloadsWindow) {
                const int width = std::min(302, labelWidth(label) + (busy ? 28 : 12));
                const int x = 312 - width;
                rect(f, x, 145, width, 15, 0x22262d);
                text(f, x + 5, 147, label, b->loadPhase() == "Failed" ? amber : white, false, width - (busy ? 24 : 10));
                if (busy) {
                    const int stage = static_cast<int>(now() * 6) % 8;
                    for (int i = 0; i < 8; ++i) {
                        const double a = i * 3.141592653589793 / 4;
                        rect(f, 302 + static_cast<int>(5 * std::cos(a)), 152 + static_cast<int>(5 * std::sin(a)),
                             2, 2, i == stage ? white : border);
                    }
                }
            }
        }
        if (pointerMode) {
            // Dark outline + bright center for contrast on both light and dark pages.
            rect(f, px - 7, py - 1, 15, 3, 0x000000);
            rect(f, px - 1, py - 7, 3, 15, 0x000000);
            rect(f, px - 6, py, 13, 1, 0xfdfdfd);
            rect(f, px, py - 6, 1, 13, 0xfdfdfd);
            rect(f, px - 1, py - 1, 3, 3, 0x000000);
        }
        // NetSurf's native GtkEntry remains visible in its toolbar. The
        // previous "ESC: Searchbar" framebuffer overlay obscured real GTK
        // widgets and was not itself interactive; rely on the native control.
        if (hold > .2) {
            rect(f, 65, 125, 190, 35, border);
            rect(f, 67, 127, 186, 31, panel);
            text(f, 81, 131, "Hold ESC to exit", white);
            rect(f, 73, 148, 174, 5, border);
            rect(f, 73, 148, static_cast<int>(174 * std::min(hold / 3, 1.0)), 5, amber);
        }
        return;
    }
    if (view_ == View::Select) {
        // Light engine selector: QtWebKit is the primary choice (60/40 split).
        std::fill(f.begin(), f.end(), 0xf5f6f7);
        rect(f, 0, 0, 320, 29, 0xfdfdfd);
        rect(f, 0, 28, 320, 1, 0xd9dcdf);
        text(f, 9, 9, "ZERO BROWSER", 0x243746);
        const int bat = system.batteryPercent;
        const bool wifi = system.wifiConnected;
        text(f, 227, 8, wifi ? "WiFi ON" : "WiFi --", wifi ? green : muted, false, 65);
        if (bat >= 0) text(f, 278, 8, std::to_string(bat) + "%", muted, false, 48);
        text(f, 12, 35, notice.empty() ? "Choose your rendering engine" : notice,
             notice.empty() ? 0x68747d : 0x9b6420, false, 296);
        const char *names[] = {"QTWEBKIT", "NETSURF"};
        const char *sub[] = {"WebKit / JavaScript", "HTML / No JS"};
        const char *detail[] = {"Advanced web rendering", "Basic page"};
        const uint32_t accents[] = {0x356cae, 0x25856a};
        for (int i = 0; i < 2; ++i) {
            const int x = i == 0 ? 8 : 193, y = 46, w = i == 0 ? 179 : 119, h = 80;
            const bool active = selected_ == i;
            rect(f, x, y, w, h, active ? accents[i] : 0xd2d5d9);
            rect(f, x + 2, y + 2, w - 4, h - 4, 0xfdfdfd);
            rect(f, x + 7, y + 9, 32, 32, active ? accents[i] : 0xe9ebed);
            text(f, x + 18, y + 16, i == 0 ? "Q" : "N", active ? 0xfdfdfd : accents[i], true, 18);
            text(f, x + 45, y + 9, names[i], 0x2b3740, false, 99);
            text(f, x + 45, y + 23, i == 0 ? "JS ON" : "NO JS", accents[i], false, 99);
            rect(f, x + 8, y + 48, w - 16, 1, 0xe4e6e8);
            text(f, x + 8, y + 54, sub[i], 0x344654, false, w - 16);
            text(f, x + 8, y + 65, detail[i], 0x667581, false, w - 16);
        }
        const std::string settingsLabel = "SETTINGS / CLEAR DATA";
        const bool settingsSelected = selected_ == 2;
        rect(f, 8, 130, 304, 20, settingsSelected ? 0x356cae : 0xdfe3e6);
        rect(f, 10, 132, 300, 16, settingsSelected ? 0x356cae : 0xfdfdfd);
        text(f, centeredX(settingsLabel, false), 136, settingsLabel,
             settingsSelected ? 0xffffff : 0x344654);
        rect(f, 0, 154, 320, 16, 0xfdfdfd);
        rect(f, 0, 154, 320, 1, 0xd9dcdf);
        const std::string footer = "Z/C: engine   X: settings   ESC 3s: exit";
        text(f, centeredX(footer, false), 159, footer, 0x667581, false, 320);
        if (hold > .2) {
            rect(f, 0, 166, static_cast<int>(320 * std::min(hold / 3, 1.0)), 4, green);
        }
        return;
    }
    if (view_ == View::Keybinds) {
        const uint32_t ink = 0x2b3740, blueInk = 0x356cae, soft = 0x667581;
        std::fill(f.begin(), f.end(), 0xf5f6f7);
        rect(f, 0, 0, 320, 25, 0xfdfdfd);
        rect(f, 0, 24, 320, 1, 0xd9dcdf);
        text(f, 10, 8, "KEYBOARD SHORTCUTS", ink);
        text(f, 264, 8, std::to_string(keybindPage_ + 1) + "/3", blueInk);
        auto row = [&](int y, const std::string &shortcut, const std::string &description) {
            rect(f, 8, y, 304, 18, 0xfdfdfd);
            rect(f, 8, y, 2, 18, 0x356cae);
            text(f, 14, y + 4, shortcut, blueInk, false, 119);
            text(f, 137, y + 4, description, ink, false, 172);
        };
        if (keybindPage_ == 0) {
            text(f, 10, 30, "POINTER AND TEXT INPUT", soft);
            row(45, "F / X / Z / C", "Move mouse pointer");
            row(66, "Shift + F/X/Z/C", "Scroll; pointer stays");
            row(87, "Fn + F/X/Z/C", "Web: type f/x/z/c");
            row(108, "OK / OK + move", "Click / select text");
            row(129, "Shift+OK / Fn+OK", "Right click / Enter");
        } else if (keybindPage_ == 1) {
            text(f, 10, 30, "PAGE NAVIGATION", soft);
            row(45, "Fn + L / M", "Page up / down");
            row(66, "Fn + W / E", "Back / forward");
            row(87, "Fn + R / T", "Zoom out / in");
            row(108, "Fn + K", "DuckDuckGo Lite");
            row(129, "Fn + Space", "Toggle pointer");
        } else {
            text(f, 10, 30, "BROWSER AND MENUS", soft);
            row(45, "ESC (short)", "Overlay / close menu");
            row(66, "ESC (hold 3s)", "Exit Zero Browser");
            row(87, "Refresh / Stop", "Qt: tap icon to stop load");
            row(108, "JS check / cross", "Qt: toggle JS + reload");
            row(129, "URL: Fn+F/X/Z/C", "Start/end/left/right");
        }
        rect(f, 0, 153, 320, 17, 0xfdfdfd);
        rect(f, 0, 153, 320, 1, 0xd9dcdf);
        text(f, 24, 158, "Z/C: pages    OK: next    ESC: back", soft);
        return;
    }
    if (view_ == View::Settings || view_ == View::CleanupConfirm) {
        const uint32_t ink = 0x2b3740, light = 0xfdfdfd, mutedInk = 0x67757f;
        std::fill(f.begin(), f.end(), 0xf5f6f7);
        rect(f, 0, 0, 320, 24, light);
        text(f, 10, 7, view_ == View::Settings ? "SETTINGS  /  CLEANUP" : "CONFIRM CLEANUP", ink);
        if (view_ == View::Settings) {
            const std::string rows[] = {
                std::string(clearCache_ ? "[x] " : "[ ] ") + "Cache (NetSurf + Qt)",
                std::string(clearCookies_ ? "[x] " : "[ ] ") + "Cookies (NetSurf)",
                std::string(clearHistory_ ? "[x] " : "[ ] ") + "History (Zero Browser)",
                "CLEAN DATA", "KEYBINDS / HELP", "BACK TO ENGINES"};
            for (int i = 0; i < 6; ++i) {
                int y = 27 + i * 20;
                const bool destructive = i == 3;
                const uint32_t fill = destructive ? (i == settingsRow_ ? 0xaa3540 : 0xfbecee)
                                                 : (i == settingsRow_ ? 0x356cae : light);
                rect(f, 8, y, 304, 18, fill);
                text(f, 14, y + 3, rows[i], i == settingsRow_ ? 0xffffff
                                  : (destructive ? 0x9e2532 : ink));
            }
            text(f, 8, 154, "F/X: move   OK: select   ESC: back", mutedInk, false, 305);
        } else {
            text(f, 9, 31, "Delete selected data for both engines?", ink, false, 307);
            text(f, 9, 48, "Cache: " + std::string(clearCache_ ? "YES" : "NO"), ink);
            text(f, 9, 62, "NetSurf cookies: " + std::string(clearCookies_ ? "YES" : "NO"), ink);
            text(f, 9, 76, "App history: " + std::string(clearHistory_ ? "YES" : "NO"), ink);
            text(f, 9, 92, "This cannot be undone.", 0x9b4338);
            for (int i = 0; i < 2; ++i) {
                int x = 8 + i * 155;
                rect(f, x, 117, 149, 24, settingsRow_ == i ? 0x356cae : light);
                const std::string label = i ? "CANCEL" : "DELETE";
                text(f, x + (149 - labelWidth(label)) / 2, 124, label, settingsRow_ == i ? 0xffffff : ink);
            }
        }
        return;
    }
    // Single-row compact browser chrome; rounding comes from the frame mask.
    if (view_ == View::Menu || view_ == View::Address) {
        for (auto &pixel : f) {
            auto channel = [&](int shift) { return ((pixel >> shift) & 255u) * 58u / 100u; };
            pixel = (channel(16) << 16) | (channel(8) << 8) | channel(0);
        }
        auto rounded = [&](int x, int y, int w, int h, int r, uint32_t color) {
            for (int yy = y; yy < y + h; ++yy)
                for (int xx = x; xx < x + w; ++xx) {
                    int cx = std::clamp(xx, x + r, x + w - r - 1);
                    int cy = std::clamp(yy, y + r, y + h - r - 1);
                    if ((xx-cx)*(xx-cx) + (yy-cy)*(yy-cy) <= r*r)
                        rect(f, xx, yy, 1, 1, color);
                }
        };
        rounded(3, 5, 314, 38, 10, 0x101217);
        rounded(4, 6, 312, 36, 9, 0x555b65);
        rounded(5, 7, 310, 34, 8, 0x30343d);
        const bool lite = b && b->mode() == Mode::Lite;
        int inputX = 9;
        int inputW = 264;
        bool focused = view_ == View::Address || (view_ == View::Menu && selected_ == 0);
        rounded(inputX, 9, inputW, 29, 8, focused ? 0xb2c5e2 : 0x737986);
        rounded(inputX+2, 11, inputW-4, 25, 7, 0x20242b);
        std::string shown;
        int addressCaretX = -1;
        if (view_ == View::Address) {
            // Keep the caret visible, even when the previous URL is selected.
            // The full URL remains intact until typing or deletion.
            int visibleChars = std::max(1, (inputW - 18)/6);
            size_t from = selectAll_ ? 0 : (cursor_ > static_cast<size_t>(visibleChars - 1) ? cursor_ - (visibleChars - 1) : 0);
            shown = address_.substr(from, static_cast<size_t>(visibleChars));
            size_t caret = selectAll_ ? shown.size() : std::min(cursor_ - from, shown.size());
            // Paint caret separately so it cannot be mistaken for an ASCII l.
            int pixelOffset = 0;
            const auto &font = platform::linux::ui_font::Small;
            for (size_t i = 0; i < caret; ++i) {
                const unsigned char ch = static_cast<unsigned char>(shown[i]);
                if (ch >= 32 && ch <= 126) pixelOffset += font.glyphs[ch - 32].advance;
            }
            addressCaretX = std::min(inputX + 7 + pixelOffset, inputX + inputW - 9);
        } else shown = b ? b->history().current() : "";
        text(f, inputX + 7, 19, shown.empty() ? "Search or enter URL" : shown,
             white, false, inputW-14);
        if (addressCaretX >= 0) rect(f, addressCaretX, 17, 2, 14, 0x76c7ff);
        {
            rounded(278, 11, 33, 26, 7,
                    view_ == View::Menu && selected_ == 1 ? blue : 0x414650);
            const bool search = view_ == View::Address &&
                (address_.empty() || address_.find(' ') != std::string::npos);
            if (search) {
                for (int yy = -4; yy <= 4; ++yy)
                    for (int xx = -4; xx <= 4; ++xx) {
                        const int rr = xx * xx + yy * yy;
                        if (rr >= 10 && rr <= 19) rect(f, 292 + xx, 22 + yy, 1, 1, white);
                    }
                for (int i = 0; i < 5; ++i) rect(f, 296 + i, 26 + i, 1, 1, white);
            } else text(f, 289, 18, ">", white, false, 13);
        }
        if (view_ == View::Address) {
            auto options = suggestions(b);
            for (size_t i = 0; i < options.size(); ++i) {
                int y = 47 + static_cast<int>(i)*19;
                rounded(8, y, 304, 18, 4, suggestion_ == static_cast<int>(i) ? 0x586782 : 0x30343d);
                text(f, 15, y + 5, options[i], suggestion_ == static_cast<int>(i) ? white : muted, false, 290);
            }
        }
        // Compact, aligned resource dashboard. Leave suggestions and overlay height unchanged.
        if (!lite && b) {
            const auto &mem = b->sample();
            const auto amount = [](double value) {
                return value >= 0 ? std::to_string(static_cast<int>(value)) + "M" : "--";
            };
            const uint32_t criticalInk = b->memoryState() == MemoryState::Critical ? 0xf4bdac : 0xb9c5cf;
            rounded(10, 128, 300, 16, 5, 0x252b32);
            // Columns: free system RAM, current engine PSS, total used swap, memory state.
            const std::string metrics[] = {
                "RAM " + amount(mem.availableMb),
                std::string(b->mode() == Mode::QtWebKit ? "QT " : "COG ") + amount(mem.pssMb),
                "SWAP " + amount(mem.swapUsedMb),
                memoryName(b->memoryState())
            };
            const int positions[] = {16, 91, 164, 243};
            const int widths[] = {70, 69, 74, 61};
            for (int i = 0; i < 4; ++i) {
                if (i > 0) rect(f, positions[i] - 5, 132, 1, 8, 0x48525d);
                text(f, positions[i], 131, metrics[i], i == 3 ? criticalInk : 0xb9c5cf,
                     false, widths[i]);
            }
        }
        // Center the engine hint inside its existing pill.
        const int tipY = 148;
        rounded(16, tipY, 288, 18, 7, 0x1c1e22);
        const std::string hint = !b ? "Choose an engine to start browsing" : b->mode() == Mode::QtWebKit ?
            "Heavy pages? Try switching JavaScript OFF" :
            b->mode() == Mode::Lite ? "NetSurf: JavaScript disabled" : "Cog / WebKit";
        text(f, centeredX(hint, false), tipY + 4, hint, 0x828894, false, 288);
    }
    if (view_ != View::Menu && view_ != View::Address && view_ != View::Transition) rect(f, 0, 146, 320, 24, bg);
    std::string footer =
        !notice.empty() ? notice
                        : (b && !b->error().empty() ? b->error() : "F/X: move   ENTER: select   ESC: page");
    if (view_ != View::Transition && view_ != View::Menu && view_ != View::Address) text(f, 8, 150, footer, (!notice.empty() || (b && !b->error().empty())) ? amber : muted, false, 304);
    if (hold > .2)
        rect(f, 0, 167, static_cast<int>(320 * std::min(hold / 3, 1.0)), 3, green);
}
} // namespace zero
