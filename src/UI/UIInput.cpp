#include "UIManager.h"
#include <algorithm>

namespace zero {
std::vector<std::string> UIManager::suggestions(const BrowserManager *b) const {
    std::vector<std::string> result;
    if (!b) return result;
    for (auto it = b->history().visits().rbegin(); it != b->history().visits().rend() && result.size() < (b->mode() != Mode::Lite ? 4u : 5u); ++it) {
        if ((address_.empty() || it->url.find(address_) != std::string::npos) &&
            std::find(result.begin(), result.end(), it->url) == result.end()) result.push_back(it->url);
    }
    return result;
}
void UIManager::address(const std::string &a) {
    suggestion_ = -1;
    view_ = View::Address;
    address_ = a;
    cursor_ = a.size();
    selectAll_ = true;
}
void UIManager::escape() {
    if (view_ == View::Settings) { view_ = View::Select; selected_ = 2; }
    else if (view_ == View::Keybinds) { view_ = View::Settings; settingsRow_ = 4; }
    else if (view_ == View::CleanupConfirm) { view_ = View::Settings; settingsRow_ = 3; }
    else if (view_ == View::Web) view_ = View::Menu;
    else if (view_ == View::Menu || view_ == View::Address)
        view_ = View::Web;
    if (view_ != View::Select) selected_ = 0;
}
Action UIManager::key(const Key &k, const BrowserManager *b) {
    // A held ENTER must never re-submit an URL (or select a mode) through
    // evdev auto-repeat. Navigation actions only accept the initial key-down.
    if (k.value == 2 && (k.code == KEY_ENTER || k.code == KEY_KPENTER)) return {};
    if (k.value == 0 || k.code == KEY_ESC)
        return {};
    if (view_ == View::Address) {
        // The URL editor uses bare F/Z/X/C as text. Fn+Z/C move the
        // insertion caret horizontally; Fn+F/X jump to start/end because
        // this is a single-line field (there is no vertical caret movement).
        // Never interpret these combinations as browser back/forward.
        if (k.fn) {
            if (k.code == KEY_Z || k.code == KEY_LEFT) {
                if (cursor_) --cursor_;
                selectAll_ = false;
                suggestion_ = -1;
                return {};
            }
            if (k.code == KEY_C || k.code == KEY_RIGHT) {
                if (cursor_ < address_.size()) ++cursor_;
                selectAll_ = false;
                suggestion_ = -1;
                return {};
            }
            if (k.code == KEY_F || k.code == KEY_UP) {
                cursor_ = 0;
                selectAll_ = false;
                suggestion_ = -1;
                return {};
            }
            if (k.code == KEY_X || k.code == KEY_DOWN) {
                cursor_ = address_.size();
                selectAll_ = false;
                suggestion_ = -1;
                return {};
            }
        }
        if (k.code == KEY_TAB) {
            auto options = suggestions(b);
            suggestion_ = options.empty() ? -1 : (suggestion_ + 1) % static_cast<int>(options.size());
            selectAll_ = false;
            return {};
        }
        if (k.code == KEY_ENTER || k.code == KEY_KPENTER) {
            auto options = suggestions(b);
            return {Action::Open, suggestion_ >= 0 && suggestion_ < static_cast<int>(options.size())
                ? options[suggestion_] : address_};
        }
        if (k.code != KEY_LEFT && k.code != KEY_RIGHT && k.code != KEY_HOME && k.code != KEY_END) suggestion_ = -1;
        if (k.code == KEY_A && (k.modifiers & 4)) {
            selectAll_ = true;
            return {};
        }
        if (k.code == KEY_LEFT) {
            if (cursor_)
                cursor_--;
            selectAll_ = false;
        } else if (k.code == KEY_RIGHT) {
            if (cursor_ < address_.size())
                cursor_++;
            selectAll_ = false;
        } else if (k.code == KEY_HOME) {
            cursor_ = 0;
            selectAll_ = false;
        } else if (k.code == KEY_END) {
            cursor_ = address_.size();
            selectAll_ = false;
        } else if (k.code == KEY_BACKSPACE) {
            if (selectAll_) {
                address_.clear();
                cursor_ = 0;
            } else if (cursor_)
                address_.erase(--cursor_, 1);
            selectAll_ = false;
        } else if (k.code == KEY_DELETE) {
            if (selectAll_) {
                address_.clear();
                cursor_ = 0;
            } else if (cursor_ < address_.size())
                address_.erase(cursor_, 1);
            selectAll_ = false;
        } else if (k.text && !(k.modifiers & (4 | 8 | 64)) && address_.size() < 4096) {
            // Opening the address bar selects the URL for one-key deletion,
            // but typing should append to the existing address, not replace it.
            if (selectAll_) cursor_ = address_.size();
            address_.insert(cursor_++, 1, k.text);
            selectAll_ = false;
        }
        return {};
    }
    bool up = k.code == KEY_UP || k.code == KEY_F, down = k.code == KEY_DOWN || k.code == KEY_X;
    bool left = k.code == KEY_LEFT || k.code == KEY_Z, right = k.code == KEY_RIGHT || k.code == KEY_C;
    bool ok = k.code == KEY_ENTER || k.code == KEY_KPENTER;
    if (view_ == View::Select) {
        if (up) selected_ = selected_ == 2 ? 0 : selected_;
        if (down || k.code == KEY_TAB) selected_ = 2;
        if (left && selected_ == 1) selected_ = 0;
        if (right) selected_ = selected_ == 0 ? 1 : selected_ == 1 ? 0 : 2;
        if (ok) {
            if (selected_ == 2) { openStartupSettings(); return {}; }
            return {selected_ == 0 ? Action::QtWebKit : Action::Lite, {}};
        }
        return {};
    }
    if (view_ == View::Keybinds) {
        if (left || up) keybindPage_ = (keybindPage_ + 2) % 3;
        if (right || down || k.code == KEY_TAB || ok) keybindPage_ = (keybindPage_ + 1) % 3;
        return {};
    }
    if (view_ == View::Menu) {
        if (b && b->mode() == Mode::Lite) {
            // Lite native GTK already exposes back/forward/reload. The overlay only
            // adds a Go action, an editable address field and history.
            if (left || right || k.code == KEY_TAB)
                selected_ = (selected_ + 1) % 2;
            if (ok) {
                if (selected_ == 0) { address(b->history().current()); return {}; }
                return {Action::Open, b->history().current()};
            }
            return {};
        }
        // Full overlay is address-first. Back/Forward remain available via
        // hardware shortcuts, but the decorative arrows never steal ENTER.
        if (ok || k.code == KEY_TAB) {
            address(b ? b->history().current() : "");
            return {};
        }
        if (k.code == KEY_R) return {Action::Reload, {}};
    } else if (view_ == View::Settings) {
        if (up) settingsRow_ = (settingsRow_ + 5) % 6;
        if (down || k.code == KEY_TAB) settingsRow_ = (settingsRow_ + 1) % 6;
        if (ok) {
            if (settingsRow_ == 0) clearCache_ = !clearCache_;
            if (settingsRow_ == 1) clearCookies_ = !clearCookies_;
            if (settingsRow_ == 2) clearHistory_ = !clearHistory_;
            if (settingsRow_ == 3 && (clearCache_ || clearCookies_ || clearHistory_)) { view_ = View::CleanupConfirm; settingsRow_ = 0; }
            if (settingsRow_ == 4) { openKeybinds(); return {}; }
            if (settingsRow_ == 5) { view_ = View::Select; selected_ = 2; }
        }
    } else if (view_ == View::CleanupConfirm) {
        if (left || right || up || down || k.code == KEY_TAB) settingsRow_ = settingsRow_ == 0 ? 1 : 0;
        if (ok) {
            if (settingsRow_ == 0) {
                std::string flags;
                if (clearCache_) flags += 'C';
                if (clearCookies_) flags += 'K';
                if (clearHistory_) flags += 'H';
                view_ = View::Select; selected_ = 2;
                return {Action::ClearBrowserData, flags};
            }
            view_ = View::Settings; settingsRow_ = 3;
        }
    }
    return {};
}
} // namespace zero
