#include "History.h"
#include <ctime>
#include <iomanip>
#include <sstream>
namespace zero {
std::string History::current() const {
    return position_ >= 0 ? stack_.at(position_) : "";
}
void History::navigate(const std::string &url, const std::string &engine) {
    if (url == current())
        return;
    stack_.resize(position_ + 1);
    stack_.push_back(url);
    position_++;
    if (stack_.size() > 500) {
        stack_.erase(stack_.begin());
        position_--;
    }
    visits_.push_back({url, engine, std::time(nullptr)});
    if (visits_.size() > 2000)
        visits_.erase(visits_.begin());
}
void History::redirect(const std::string &url, const std::string &engine, bool recordVisit) {
    if (url == current())
        return;
    if (position_ < 0) {
        navigate(url, engine);
        return;
    }
    auto before = current();
    stack_[position_] = url;
    if (recordVisit) {
        // A QtWebKit urlChanged event may be a new page, not a redirect.
        // Keep the existing navigation stack semantics, but retain each
        // destination as a permanent visit for address suggestions.
        if (visits_.empty() || visits_.back().url != url) {
            visits_.push_back({url, engine, std::time(nullptr)});
            if (visits_.size() > 2000)
                visits_.erase(visits_.begin());
        }
    } else if (!visits_.empty() && visits_.back().url == before) {
        visits_.back().url = url;
        visits_.back().engine = engine;
    }
}
bool History::back() {
    if (!canBack())
        return false;
    --position_;
    return true;
}
bool History::forward() {
    if (!canForward())
        return false;
    ++position_;
    return true;
}
void History::save(const fs::path &p) const {
    std::ostringstream s;
    s << "ZERO-HISTORY 1\n" << position_ << " " << stack_.size() << " " << visits_.size() << "\n";
    for (auto &url : stack_)
        s << std::quoted(url) << "\n";
    for (auto &v : visits_)
        s << v.date << " " << std::quoted(v.engine) << " " << std::quoted(v.url) << "\n";
    atomicWrite(p, s.str());
}
void History::load(const fs::path &p) {
    auto contents = readFile(p, 10 * 1024 * 1024);
    if (contents.empty())
        return;
    std::istringstream in(contents);
    std::string magic;
    std::getline(in, magic);
    int pos;
    size_t ns, nv;
    if (magic != "ZERO-HISTORY 1" || !(in >> pos >> ns >> nv) || ns > 500 || nv > 2000 || pos < -1 ||
        pos >= static_cast<int>(ns) || (ns > 0 && pos < 0)) {
        log("history.invalid");
        return;
    }
    std::vector<std::string> stack;
    std::vector<Visit> visits;
    try {
        for (size_t i = 0; i < ns; ++i) {
            std::string url;
            if (!(in >> std::quoted(url)))
                return;
            stack.push_back(normalizeUrl(url));
        }
        for (size_t i = 0; i < nv; ++i) {
            Visit v;
            if (!(in >> v.date >> std::quoted(v.engine) >> std::quoted(v.url)))
                return;
            v.url = normalizeUrl(v.url);
            visits.push_back(v);
        }
    } catch (...) {
        log("history.invalid_url");
        return;
    }
    stack_ = std::move(stack);
    visits_ = std::move(visits);
    position_ = pos;
}
} // namespace zero
