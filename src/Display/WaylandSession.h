#pragma once
#include "Utils/Process.h"
#include <functional>
#include <memory>

namespace zero {
// Own the private compositor and bus separately from the Wayland client.
// A timed-out attempt must be destroyed before a fresh socket is created.
class WaylandSession {
    struct Attempt;
    std::unique_ptr<Attempt> attempt_;

  public:
    using Progress = std::function<bool(int attempt, double elapsed)>;
    WaylandSession();
    ~WaylandSession();
    bool start(const Paths &, const Progress & = {}); // false when cancelled
    void stop();
    void shutdown(const std::function<void()> &heartbeat = {});
    bool alive();
    void validated(); // Disable startup-only GPU failure detection after display is ready.
    const std::map<std::string, std::string> &environment() const;
};
} // namespace zero
