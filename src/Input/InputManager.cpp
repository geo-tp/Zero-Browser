#include "InputManager.h"
#include "Utils/Util.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>
namespace zero {
// Cardputer Zero TCA8418 driver translates the Fn layer *before* evdev.
// Keep these translations at the input boundary (not in UI widgets).
static uint16_t cardputerPhysicalFnCode(uint16_t code) {
    switch (code) {
    case KEY_INSERT: return KEY_B;
    case KEY_VOLUMEDOWN: return KEY_S;
    case KEY_END: return KEY_N;
    case KEY_PREVIOUSSONG: return KEY_W;
    case KEY_NEXTSONG: return KEY_E;
    case KEY_HOME: return KEY_K;
    case KEY_PAGEUP: return KEY_L;
    case KEY_PAGEDOWN: return KEY_M;
    case KEY_SYSRQ: return KEY_J;
    default: return code;
    }
}
char InputManager::character(uint16_t c, uint32_t mods, bool caps) {
    const std::pair<int, char> letters[] = {
        {KEY_Q, 'q'}, {KEY_W, 'w'}, {KEY_E, 'e'}, {KEY_R, 'r'}, {KEY_T, 't'}, {KEY_Y, 'y'}, {KEY_U, 'u'},
        {KEY_I, 'i'}, {KEY_O, 'o'}, {KEY_P, 'p'}, {KEY_A, 'a'}, {KEY_S, 's'}, {KEY_D, 'd'}, {KEY_F, 'f'},
        {KEY_G, 'g'}, {KEY_H, 'h'}, {KEY_J, 'j'}, {KEY_K, 'k'}, {KEY_L, 'l'}, {KEY_Z, 'z'}, {KEY_X, 'x'},
        {KEY_C, 'c'}, {KEY_V, 'v'}, {KEY_B, 'b'}, {KEY_N, 'n'}, {KEY_M, 'm'}};
    bool shift = mods & 1;
    // The Fn modifier explicitly types lowercase F/X/Z/C, including in the URL bar.
    if ((mods & 128u) && (c == KEY_F || c == KEY_X || c == KEY_Z || c == KEY_C)) {
        switch (c) {
        case KEY_F: return 'f';
        case KEY_X: return 'x';
        case KEY_Z: return 'z';
        default: return 'c';
        }
    }
    // Cardputer SYM+ B is represented by its dedicated modifier (KEY_DVD / Fn).
    if ((mods & (128u | 1u)) && c == KEY_B) return '.';
    if ((mods & 128u) && c == KEY_S) return ':';
    if ((mods & 128u) && c == KEY_N) return '/';
    for (auto [k, ch] : letters)
        if (c == k)
            return shift != caps ? ch - 'a' + 'A' : ch;
    if (c >= KEY_1 && c <= KEY_0)
        return (shift ? "!@#$%^&*()" : "1234567890")[c - KEY_1];
    const std::pair<int, const char *> symbols[] = {
        {KEY_SPACE, "  "},      {KEY_MINUS, "-_"},      {KEY_EQUAL, "=+"},     {KEY_LEFTBRACE, "[{"},
        {KEY_RIGHTBRACE, "]}"}, {KEY_BACKSLASH, "\\|"}, {KEY_SEMICOLON, ";:"}, {KEY_APOSTROPHE, "'\""},
        {KEY_GRAVE, "`~"},      {KEY_COMMA, ",<"},      {KEY_DOT, ".>"},       {KEY_SLASH, "/?"}};
    for (auto [k, s] : symbols)
        if (c == k)
            return s[shift];
    return 0;
}
InputManager::~InputManager() {
    for (auto &d : devices_)
        if (d.fd >= 0) {
            if (d.keyboard) ioctl(d.fd, EVIOCGRAB, 0);
            close(d.fd);
        }
}
void InputManager::open(const std::string &requested) {
    std::vector<std::string> candidates;
    if (!requested.empty())
        candidates.push_back(requested);
    else
        for (int i = 0; i < 64; ++i)
            candidates.push_back("/dev/input/event" + std::to_string(i));
    for (auto &path : candidates) {
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        unsigned char keys[(KEY_MAX + 8) / 8]{};
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
        auto has = [&](int k) { return (keys[k / 8] >> (k % 8)) & 1; };
        const bool keyboard = !requested.empty() || (has(KEY_A) && has(KEY_ENTER));
        const bool pointer = has(BTN_LEFT) || has(BTN_TOUCH) || has(BTN_TOOL_FINGER);
        const bool buttons = has(KEY_POWER) || has(KEY_VOLUMEUP) || has(KEY_VOLUMEDOWN) ||
                             has(BTN_GAMEPAD) || has(BTN_JOYSTICK);
        if (!keyboard && !pointer && !buttons) {
            close(fd);
            continue;
        }
        char name[256]{};
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        std::string n = name;
        if (keyboard && ioctl(fd, EVIOCGRAB, 1) < 0) {
            close(fd);
            if (!requested.empty())
                throw std::runtime_error("Cannot acquire keyboard exclusively");
            continue;
        }
        Device d;
        d.fd = fd;
        d.keyboard = keyboard;
        d.cardputer = n.find("tca8418") != std::string::npos || n.find("cardputer") != std::string::npos;
        devices_.push_back(d);
        log("input.device", path + " " + n);
    }
    if (std::none_of(devices_.begin(), devices_.end(), [](const Device &d) { return d.keyboard; }))
        throw std::runtime_error("No accessible keyboard; check input group membership");
}
bool InputManager::held() const {
    for (const auto &device : devices_)
        if (std::any_of(device.keys.begin(), device.keys.end(), [](bool down) { return down; }))
            return true;
    return false;
}
bool InputManager::isActivity(const input_event &e) {
    if (e.type == EV_KEY) return e.code <= KEY_MAX && e.value >= 0 && e.value <= 2;
    if (e.type == EV_REL) return e.code <= REL_MAX && e.value != 0;
    return (e.type == EV_ABS && e.code <= ABS_MAX) || (e.type == EV_SW && e.code <= SW_MAX);
}
double InputManager::escapeHeld() const {
    return escSince_ < 0 ? 0 : now() - escSince_;
}
void InputManager::poll(const std::function<void(const Key &)> &callback, bool repeatUi,
                        const std::function<void()> &activity) {
    for (auto &d : devices_) {
        if (d.fd < 0)
            continue;
        input_event e{};
        ssize_t size;
        while ((size = read(d.fd, &e, sizeof e)) == sizeof e) {
            if (e.type == EV_SYN && e.code == SYN_DROPPED) {
                d.dropped = true;
                d.keys.fill(false);
                if (d.keyboard) {
                    escSince_ = -1;
                    repeatDevice_ = -1;
                    callback({0, -1, 0, false, 0});
                }
                continue;
            }
            if (d.dropped) {
                if (e.type == EV_SYN && e.code == SYN_REPORT)
                    d.dropped = false;
                continue;
            }
            // Count raw input before shortcut filtering or repeat suppression.
            // SYN/MSC bookkeeping must never keep an unattended screen awake.
            if (isActivity(e) && activity) activity();
            if (!d.keyboard) {
                if (e.type == EV_KEY && isActivity(e)) d.keys[e.code] = e.value != 0;
                continue; // Observe mouse/touch/gamepad activity without grabbing it.
            }
            if (e.type == EV_MSC && e.code == MSC_SCAN) {
                d.scan = e.value;
                continue;
            }
            if (e.type == EV_SYN) {
                d.scan = -1;
                continue;
            }
            if (e.type != EV_KEY || e.code > KEY_MAX || e.value < 0 || e.value > 2)
                continue;
            if (e.code == KEY_DVD && d.scan == 0x42)
                d.cardputer = true;
            const int scan = d.scan;
            d.scan = -1;
            if (e.value == 2)
                continue; // deterministic repeat below; Wayland handles browser repeat.
            d.keys[e.code] = e.value != 0;
            uint32_t mods = 0;
            bool fn = false, escape = false;
            for (auto &device : devices_) {
                auto &k = device.keys;
                if (k[KEY_LEFTSHIFT] || k[KEY_RIGHTSHIFT])
                    mods |= 1;
                if (k[KEY_LEFTCTRL] || k[KEY_RIGHTCTRL])
                    mods |= 4;
                if (k[KEY_LEFTALT] || k[KEY_RIGHTALT])
                    mods |= 8;
                if (k[KEY_LEFTMETA] || k[KEY_RIGHTMETA])
                    mods |= 64;
                fn |= k[KEY_FN] || (device.cardputer && k[KEY_DVD]);
                escape |= k[KEY_ESC];
            }
            if (escape) {
                if (escSince_ < 0)
                    escSince_ = now();
            } else
                escSince_ = -1;
            if (e.code == KEY_CAPSLOCK && e.value == 1)
                caps_ = !caps_;
            // KEY_INSERT / KEY_VOLUMEDOWN / KEY_END are not letters:
            // they are the kernel's translated Fn+B / Fn+S / Fn+N layer.
            // MSC_SCAN distinguishes these from genuine non-Cardputer keys.
            const bool translated = d.cardputer &&
                ((e.code == KEY_INSERT && (scan == 0x36 || fn)) ||
                 (e.code == KEY_VOLUMEDOWN && (scan == 0x21 || fn)) ||
                 (e.code == KEY_END && (scan == 0x37 || fn)) ||
                 (e.code == KEY_PREVIOUSSONG && (scan == 0x11 || fn)) ||
                 (e.code == KEY_NEXTSONG && (scan == 0x12 || fn)) ||
                 (e.code == KEY_HOME && (scan == 0x27 || fn)) ||
                 (e.code == KEY_PAGEUP && (scan == 0x28 || fn)) ||
                 (e.code == KEY_PAGEDOWN && (scan == 0x38 || fn)) ||
                 (e.code == KEY_SYSRQ && (scan == 0x26 || fn)));
            // Some Cardputer firmware/keyboard maps Fn+F/X/Z/C to evdev
            // directional keys before delivery. Restore the original letters
            // before applying Zero Browser's pointer routing. Fn+letter must
            // type into the focused input, never move its caret.
            uint16_t logical = translated ? cardputerPhysicalFnCode(e.code) : e.code;
            if (fn && d.cardputer) {
                switch (logical) {
                case KEY_UP: logical = KEY_F; break;
                case KEY_DOWN: logical = KEY_X; break;
                case KEY_LEFT: logical = KEY_Z; break;
                case KEY_RIGHT: logical = KEY_C; break;
                default: break;
                }
            }
            fn |= translated;
            Key key{logical, e.value, mods, fn, character(logical, mods | (fn ? 128u : 0u), caps_)};
            if (e.value == 1 && e.code != KEY_ESC && e.code != KEY_ENTER && e.code != KEY_FN &&
                e.code != KEY_DVD) {
                repeat_ = key;
                repeatAt_ = now() + .35;
                repeatDevice_ = d.fd;
            }
            if (e.value == 0 && repeat_.code == logical && repeatDevice_ == d.fd)
                repeatDevice_ = -1;
            callback(key);
        }
        if (size == 0 || (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            d.keys.fill(false);
            if (d.keyboard) {
                escSince_ = -1;
                repeatDevice_ = -1;
                callback({0, -1, 0, false, 0});
            }
            close(d.fd);
            d.fd = -1;
        }
    }
    if (repeatUi && repeatDevice_ >= 0 && now() >= repeatAt_) {
        repeatAt_ = now() + .09;
        repeat_.value = 2;
        callback(repeat_);
    }
}
} // namespace zero
