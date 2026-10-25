#pragma once
#include "Browser/BrowserManager.h"
#include "Input/InputManager.h"
#include "System/SystemStatus.h"
namespace zero {
struct Action {
    enum Type {
        None,
        Lite,
        QtWebKit,
        Back,
        Forward,
        Reload,
        StopLoading,
        Home,
        Downloads,
        Address,
        ZoomOut,
        ZoomIn,
        ToggleJavascript,
        Open,
        ClearBrowserData
    } type = None;
    std::string text;
};
class UIManager {
  public:
    static constexpr int ToolbarHeight = 20;
    // Values are exposed by the control socket. Keep 4 reserved for the retired History view.
    enum class View { Select = 0, Web = 1, Menu = 2, Address = 3, Settings = 5, CleanupConfirm = 6, Keybinds = 7, Transition = 8 };

  private:
    View view_ = View::Select;
    int selected_ = 0;
    int settingsRow_ = 0;
    int keybindPage_ = 0;
    bool clearCache_ = true, clearCookies_ = false, clearHistory_ = false;
    Mode transitionMode_ = Mode::Lite;
    std::string address_;
    size_t cursor_ = 0;
    bool selectAll_ = false;
    int suggestion_ = -1;
    std::vector<std::string> suggestions(const BrowserManager *) const;
    void rect(std::vector<uint32_t> &, int, int, int, int, uint32_t);
    void text(std::vector<uint32_t> &, int, int, const std::string &, uint32_t, bool large = false,
              int maxWidth = 300);

  public:
    View view() const {
        return view_;
    }
    void show(View v) {
        view_ = v;
        selected_ = 0;
    }
    void setTransitionMode(Mode mode) { transitionMode_ = mode; }
    void selectEngine(Mode mode) {
        view_ = View::Select;
        selected_ = mode == Mode::Lite ? 1 : 0;
        transitionMode_ = mode;
    }
    void address(const std::string &);
    void escape();
    void openStartupSettings() { view_ = View::Settings; settingsRow_ = 0; }
    void openKeybinds() { view_ = View::Keybinds; keybindPage_ = 0; }
    Action key(const Key &, const BrowserManager *);
    Action toolbarAction(int x, int y, const BrowserManager *) const;
    void draw(std::vector<uint32_t> &, const BrowserManager *, const SystemStatus &, double escapeHeld,
              bool pointer, int pointerX, int pointerY, const std::string &notice = "", bool clickHint = false, bool hideQtStatus = false);
};
} // namespace zero
