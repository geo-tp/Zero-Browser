#pragma once
#include "Utils/Util.h"
namespace zero {
struct Visit {
    std::string url, engine;
    int64_t date = 0;
};
class History {
    std::vector<std::string> stack_;
    std::vector<Visit> visits_;
    int position_ = -1;

  public:
    void load(const fs::path &);
    void save(const fs::path &) const;
    void navigate(const std::string &url, const std::string &engine);
    void redirect(const std::string &url, const std::string &engine, bool recordVisit = false);
    bool back();
    bool forward();
    std::string current() const;
    int position() const {
        return position_;
    }
    bool canBack() const {
        return position_ > 0;
    }
    bool canForward() const {
        return position_ + 1 < static_cast<int>(stack_.size());
    }
    const std::vector<Visit> &visits() const {
        return visits_;
    }
    const std::vector<std::string> &stack() const {
        return stack_;
    }
};
} // namespace zero
