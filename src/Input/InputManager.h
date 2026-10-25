#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <linux/input.h>
#include <string>
#include <vector>
namespace zero {
struct Key {
    uint16_t code = 0;
    int value = 0;
    uint32_t modifiers = 0;
    bool fn = false;
    char text = 0;
};
class InputManager {
    struct Device {
        int fd = -1;
        bool cardputer = false, dropped = false, keyboard = true;
        int scan = -1;
        std::array<bool, KEY_MAX + 1> keys{};
    };
    std::vector<Device> devices_;
    double escSince_ = -1, repeatAt_ = 0;
    Key repeat_;
    int repeatDevice_ = -1;
    bool caps_ = false;

  public:
    ~InputManager();
    void open(const std::string &device = "");
    void poll(const std::function<void(const Key &)> &, bool repeatUi,
              const std::function<void()> &activity = {});
    bool held() const;
    static bool isActivity(const input_event &);
    double escapeHeld() const;
    static char character(uint16_t code, uint32_t modifiers, bool caps = false);
};
} // namespace zero
