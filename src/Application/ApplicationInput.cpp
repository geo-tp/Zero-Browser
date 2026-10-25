#include "Application.h"
#include <algorithm>
#include <cctype>

namespace zero {
namespace {
// GTK Downloads has its own scrollbar; keep wheel input on that surface.
bool downloadsFocused(const WaylandBridge &wayland) {
    for (const auto &w : wayland.toplevels()) {
        if (!w.complete || !w.active ||
            w.appId.rfind("org.cardputer.ZeroBrowser.NetSurf.", 0) != 0) continue;
        std::string title = w.title;
        std::transform(title.begin(), title.end(), title.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return title.find("download") != std::string::npos;
    }
    return false;
}
} // namespace
bool Application::qtDownloadsFocused(const WaylandBridge &wayland) {
    for (const auto &w : wayland.toplevels()) {
        if (!w.complete || !w.active) continue;
        std::string title = w.title;
        std::transform(title.begin(), title.end(), title.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        // Only used when the active engine is QtWebKit. The compositor may
        // expose an empty app-id for a transient QDialog.
        if (title.find("downloads") != std::string::npos ||
            title.find("confirm download") != std::string::npos) return true;
    }
    return false;
}
void Application::releaseMouse() {
    if (!leftMouseHeld_) return;
    display_.wayland.mouseButton(false);
    leftMouseHeld_ = false;
}
void Application::key(const Key &k) {
    if (k.value < 0) {
        releaseMouse();
        escapePressedAt_ = -1;
        if (browser_) browser_->cancelEscape();
        display_.wayland.releaseKeys();
        suppressed_.clear();
        shortcutHeld_.clear();
        return;
    }
    display_.activity(now());
    // F6/F9 activate NetSurf GTK modes which cannot be displayed by the
    // Cardputer framebuffer bridge. Consume press, repeat and release.
    if (browser_ && browser_->active() && browser_->mode() == Mode::Lite &&
        ui_.view() == UIManager::View::Web &&
        (k.code == KEY_F6 || k.code == KEY_F9)) return;
    if (k.code == KEY_ESC) {
        if (k.value == 1) releaseMouse();
        if (k.value == 1 && escapePressedAt_ < 0) escapePressedAt_ = now();
        if (k.value == 0 && escapePressedAt_ >= 0) {
            bool longPress = now() - escapePressedAt_ >= 3;
            escapePressedAt_ = -1;
            if (longPress) { quit_ = true; return; }
            if (browser_ && browser_->active() && browser_->mode() == Mode::QtWebKit &&
                qtDownloadsFocused(display_.wayland)) browser_->closeDownloads();
            else if (browser_ && browser_->active() && ui_.view() == UIManager::View::Web) {
                // NetSurf secondary windows and GTK menus need the GTK-aware
                // ESC router; a raw ESC cannot reliably hide Downloads.
                if (browser_->mode() == Mode::Lite) browser_->escape();
                else display_.wayland.tap(KEY_ESC);
            }
            else if (ui_.view() == UIManager::View::Settings ||
                     ui_.view() == UIManager::View::CleanupConfirm ||
                     ui_.view() == UIManager::View::Keybinds) {
                // Startup Settings is available before any browser process exists.
                // Do not route its ESC through toggleOverlay(), which requires browser_.
                ui_.escape();
            } else toggleOverlay();
        }
        return;
    }
    if (k.value == 1 && browser_) browser_->cancelEscape();
    if (k.code == KEY_FN || k.code == KEY_DVD)
        return;
    if ((k.code == KEY_F12 || k.code == KEY_MENU) && k.value == 1) {
        toggleOverlay();
        return;
    }
    // Fn+OK is a real Enter key for web forms; bare OK remains a pointer click.
    // Forward both press and release to avoid leaving Enter logically held.
    if (k.code == KEY_ENTER && (k.fn || shortcutHeld_.count(KEY_ENTER))) {
        if (ui_.view() == UIManager::View::Web && browser_ && browser_->active()) {
            if (k.value == 1) {
                shortcutHeld_.insert(KEY_ENTER);
                display_.wayland.key(KEY_ENTER, true, 0);
            } else if (k.value == 0) {
                display_.wayland.key(KEY_ENTER, false, 0);
                shortcutHeld_.erase(KEY_ENTER);
            }
        } else if (k.value == 1) {
            // In the overlay, Enter validates the address or selected action.
            action(ui_.key(k, browser_.get()));
            shortcutHeld_.insert(KEY_ENTER);
        } else if (k.value == 0) shortcutHeld_.erase(KEY_ENTER);
        return;
    }
    if (k.fn && k.code == KEY_SPACE && k.value == 1) {
        releaseMouse();
        pointer_ = !pointer_;
        display_.wayland.releaseKeys();
        return;
    }
    // A release can arrive after Fn itself is released; never leak that key
    // to the page as an unpaired release.
    if (shortcutHeld_.count(k.code)) {
        if (k.value == 0) {
            if (k.code == KEY_ENTER && ui_.view() == UIManager::View::Web && browser_ && browser_->active())
                display_.wayland.key(KEY_ENTER, false, 0);
            shortcutHeld_.erase(k.code);
        }
        return;
    }
    // Cardputer Zero navigation shortcuts, shared by the browser engines.
    // Consume both press and release so shortcut letters never leak into forms.
    if (k.fn && browser_ && browser_->active() && ui_.view() == UIManager::View::Web) {
        if (k.value == 1 && (k.code == KEY_W || k.code == KEY_E || k.code == KEY_K ||
                             k.code == KEY_L || k.code == KEY_M || k.code == KEY_J ||
                             k.code == KEY_R || k.code == KEY_T))
            shortcutHeld_.insert(k.code);
        switch (k.code) {
        case KEY_W:
            if (k.value == 1) action({Action::Back, {}});
            return;
        case KEY_E:
            if (k.value == 1) action({Action::Forward, {}});
            return;
        case KEY_K:
            if (k.value == 1) action({Action::Open, "https://lite.duckduckgo.com/lite/"});
            return;
        case KEY_J:
            // No reliable page-content inspection through the NetSurf GTK adapter.
            // Inform only: never infer JS requirements from a blank page, and never switch engines.
            if (browser_->mode() == Mode::Lite && k.value == 1) {
                notice_ = "NetSurf: JavaScript is disabled";
                noticeUntil_ = now() + 5;
            }
            return;
        case KEY_R:
        case KEY_T:
            if (k.value == 1) {
                if (browser_->mode() != Mode::Lite) {
                    action({k.code == KEY_R ? Action::ZoomOut : Action::ZoomIn, {}});
                    return;
                }
                // Standard GTK browser zoom shortcuts, independent of the
                // toolbar icons. Ctrl+minus = zoom out, Ctrl+plus = zoom in.
                if (k.code == KEY_R) display_.wayland.tap(KEY_MINUS, 4);
                else display_.wayland.tap(KEY_EQUAL, 4 | 1);
            }
            return;
        case KEY_L:
        case KEY_M:
            if (k.value == 1) {
                // Target the document rather than GTK's native toolbar.
                if (browser_->mode() == Mode::Lite && !downloadsFocused(display_.wayland)) display_.wayland.pointer(160, 100);
                display_.wayland.scroll(0, k.code == KEY_L ? -800 : 800);
                if (browser_->mode() == Mode::Lite && !downloadsFocused(display_.wayland)) display_.wayland.pointer(pointerX_, pointerY_);
            }
            return;
        default: break;
        }
    }
    if (ui_.view() == UIManager::View::Web && browser_ && browser_->active()) {
        bool up = k.code == KEY_F || k.code == KEY_UP;
        bool down = k.code == KEY_X || k.code == KEY_DOWN;
        bool left = k.code == KEY_Z || k.code == KEY_LEFT;
        bool right = k.code == KEY_C || k.code == KEY_RIGHT;
        // Special physical Cardputer keys are forwarded as unmodified symbols.
        // The driver emits INSERT / VOLUMEDOWN / END for Fn+B/S/N;
        // InputManager maps those events back to the logical letter + Fn.
        int symbol = -1;
        if (k.code == KEY_B && (k.fn || (k.modifiers & 1u))) symbol = KEY_DOT;
        if (k.fn && k.code == KEY_S) symbol = KEY_SEMICOLON;
        if (k.fn && k.code == KEY_N) symbol = KEY_SLASH;
        if (symbol != -1) {
            if (k.value != 2) {
                // Colon is Shift+semicolon in the US virtual keyboard layout.
                display_.wayland.key(symbol, k.value == 1, (symbol == KEY_SEMICOLON) ? 1u : 0u);
            }
            return;
        }
        // Bare F/X/Z/C moves the pointer; Fn types letters, Shift scrolls.
        // The actual arrow keys continue to move the pointer.
        // Check before pointer routing so these keys are not swallowed.
        if (k.fn &&
            (k.code == KEY_F || k.code == KEY_X || k.code == KEY_Z || k.code == KEY_C)) {
            if (k.value != 2) {
                // Do not forward Shift for this keystroke: it is a lowercase
                // text shortcut, even though Shift is physically held.
                display_.wayland.key(k.code, k.value == 1, k.modifiers & ~1u);
            }
            return;
        }
        if (pointer_ && (up || down || left || right) && !k.fn) {
            const bool pressed = k.value != 0;
            const bool shifted = (k.modifiers & 1u) != 0;
            auto &u = shifted ? heldScrollUp_ : heldUp_;
            auto &d = shifted ? heldScrollDown_ : heldDown_;
            auto &l = shifted ? heldScrollLeft_ : heldLeft_;
            auto &r = shifted ? heldScrollRight_ : heldRight_;
            if (up) { heldUp_ = heldScrollUp_ = false; if (pressed) u = true; }
            if (down) { heldDown_ = heldScrollDown_ = false; if (pressed) d = true; }
            if (left) { heldLeft_ = heldScrollLeft_ = false; if (pressed) l = true; }
            if (right) { heldRight_ = heldScrollRight_ = false; if (pressed) r = true; }
            if (k.value == 1 && !(heldUp_ && heldDown_) && !(heldLeft_ && heldRight_)) {
                pointerHeldSince_ = now();
                pointerRepeat_ = now() - .04;
            }
            if (!pressed && !(heldUp_ || heldDown_ || heldLeft_ || heldRight_)) {
                pointerHeldSince_ = 0;
                pointerFracX_ = pointerFracY_ = 0;
            }
            return;
        }
        if (k.code == KEY_ENTER && !k.fn) {
            if (k.value == 1) {
                if (k.modifiers & 1u) {
                    // Shift+OK remains an immediate right-click, not a drag.
                    releaseMouse();
                    click(true);
                } else if (!leftMouseHeld_) {
                    // Native Wayland press stays down while OK is held, so
                    // F/X/Z/C pointer movement selects text or drags content.
                    // The app toolbar consumes the click without grabbing.
                    if (browser_->mode() != Mode::Lite && pointerY_ < UIManager::ToolbarHeight) {
                        click(false);
                    } else {
                        display_.wayland.pointer(pointerX_, pointerY_);
                        display_.wayland.mouseButton(true);
                        leftMouseHeld_ = true;
                        clickHintUntil_ = now() + 12;
                    }
                }
            } else if (k.value == 0) {
                releaseMouse();
            }
            return;
        }
        if (suppressed_.count(k.code)) {
            if (k.value == 0)
                suppressed_.erase(k.code);
            return;
        }
        if (k.value != 2)
            display_.wayland.key(k.code, k.value == 1, k.modifiers);
    } else {
        if (k.value == 1)
            suppressed_.insert(k.code);
        else if (k.value == 0)
            suppressed_.erase(k.code);
        // ESC release already consumed the overlay toggle; do not swallow the
        // first typed character during the old 120ms UI debounce period.
        action(ui_.key(k, browser_.get()));
    }
}
void Application::updatePointer(double t) {
    if (ui_.view() != UIManager::View::Web || !browser_ || !browser_->active()) {
        pointerRepeat_ = t;
        return;
    }
    if (pointerRepeat_ == 0) pointerRepeat_ = t;
    double dt = std::clamp(t - pointerRepeat_, 0.0, .10);
    pointerRepeat_ = t;
    const int dx = static_cast<int>(heldRight_) - static_cast<int>(heldLeft_);
    const int dy = static_cast<int>(heldDown_) - static_cast<int>(heldUp_);
    if (dx || dy) {
        double duration = pointerHeldSince_ > 0 ? t - pointerHeldSince_ : 0;
        // Short taps stay precise; holding ramps from 85 to 440 px/s.
        double speed = duration < .18 ? 65.0 :
            85.0 + 355.0 * std::clamp((duration - .18) / 1.25, 0.0, 1.0);
        pointerFracX_ += dx * speed * dt;
        pointerFracY_ += dy * speed * dt;
        int mx = static_cast<int>(pointerFracX_), my = static_cast<int>(pointerFracY_);
        pointerFracX_ -= mx; pointerFracY_ -= my;
        if (mx || my) {
            pointerX_ = std::clamp(pointerX_ + mx, 0, 319);
            pointerY_ = std::clamp(pointerY_ + my, 0, 169);
            display_.wayland.pointer(pointerX_, pointerY_);
        }
    }
    const int sx = static_cast<int>(heldScrollRight_) - static_cast<int>(heldScrollLeft_);
    const int sy = static_cast<int>(heldScrollDown_) - static_cast<int>(heldScrollUp_);
    if (!(sx || sy)) scrollHeldSince_ = 0;
    else if (scrollHeldSince_ == 0) scrollHeldSince_ = t;
    if ((sx || sy) && dt > 0)
        {
        // Accumulate subpixel wheel deltas: integer truncation used to discard
        // every frame at 5-15 FPS (55 * dt < 1 in many cases).
        double held = scrollHeldSince_ > 0 ? t - scrollHeldSince_ : 0;
        double scrollSpeed = 90.0 + 140.0 * std::clamp(held / 1.7, 0.0, 1.0);
        scrollFracX_ += sx * scrollSpeed * dt;
        scrollFracY_ += sy * scrollSpeed * dt;
        int wx = static_cast<int>(scrollFracX_), wy = static_cast<int>(scrollFracY_);
        scrollFracX_ -= wx; scrollFracY_ -= wy;
        if (wx || wy) {
            // Wheel input normally goes to the surface beneath the pointer.
            // For NetSurf GTK, temporarily aim at the document, not the toolbar.
            if (browser_->mode() == Mode::Lite && !downloadsFocused(display_.wayland)) display_.wayland.pointer(160, 100);
            display_.wayland.scroll(wx, wy);
            if (browser_->mode() == Mode::Lite && !downloadsFocused(display_.wayland)) display_.wayland.pointer(pointerX_, pointerY_);
        }
    }
    // At the screen edge, continued pointer movement pans the page.
    if ((dx && ((dx < 0 && pointerX_ == 0) || (dx > 0 && pointerX_ == 319))) ||
        (dy && ((dy < 0 && pointerY_ == 0) || (dy > 0 && pointerY_ == 169)))) {
        const bool lite = browser_->mode() == Mode::Lite;
        edgeScrollX_ += dx * (lite ? 250.0 : 115.0) * dt;
        edgeScrollY_ += dy * (lite ? 250.0 : 115.0) * dt;
        int ex = static_cast<int>(edgeScrollX_), ey = static_cast<int>(edgeScrollY_);
        edgeScrollX_ -= ex; edgeScrollY_ -= ey;
        if (ex || ey) {
            if (browser_->mode() == Mode::Lite && !downloadsFocused(display_.wayland)) {
                // GTK toolbars/scrollbars can intercept wheel events at screen edges.
                // Temporarily target the document body, without moving our drawn cursor.
                display_.wayland.pointer(160, 100);
                display_.wayland.scroll(ex, ey);
                display_.wayland.pointer(pointerX_, pointerY_);
            } else if (browser_->mode() == Mode::QtWebKit) {
                // The top edge belongs to our framebuffer toolbar, not the web view.
                // Send wheel events inside the Qt content surface and restore the cursor.
                display_.wayland.pointer(160, 85);
                display_.wayland.scroll(ex, ey);
                display_.wayland.pointer(pointerX_, pointerY_);
            } else {
                display_.wayland.pointer(pointerX_, pointerY_);
                display_.wayland.scroll(ex, ey);
            }
        }
    } else { edgeScrollX_ = edgeScrollY_ = 0; }
}
void Application::click(bool right) {
    if (!browser_ || !browser_->active() || ui_.view() != UIManager::View::Web) return;
    if (!right && browser_->mode() != Mode::Lite && pointerY_ < UIManager::ToolbarHeight) {
        // Even a disabled toolbar button consumes the click, never the page below.
        action(ui_.toolbarAction(pointerX_, pointerY_, browser_.get()));
        return;
    }
    display_.wayland.pointer(pointerX_, pointerY_);
    display_.wayland.click(right);
    clickHintUntil_ = now() + 12;
}
} // namespace zero
