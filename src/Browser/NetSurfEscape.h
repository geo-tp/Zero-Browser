#pragma once
#include "GtkEscapeProtocol.h"
#include "Display/WaylandBridge.h"
#include <functional>
#include <map>
namespace zero {
enum class EscapeAction { Native, Overlay, Close, NativeThenCheck };
EscapeAction decideEscape(const ZeroGtkState &, const std::vector<Toplevel> &, bool safeVersion);
class NetSurfEscape {
    WaylandBridge &input_;
    int fd_ = -1, child_ = -1;
    uint32_t serial_ = 0;
    enum class Phase { Idle, Query, NativeWait, Verify, Closing } phase_ = Phase::Idle;
    double deadline_ = 0;
    ZeroGtkState target_{};
    uint64_t targetHandle_ = 0;
    bool safeVersion_ = false;
    bool query(Phase);
    void native();
    void close(const ZeroGtkState &);
  public:
    explicit NetSurfEscape(WaylandBridge &input) : input_(input) {}
    ~NetSurfEscape();
    void prepare(std::map<std::string, std::string> &environment);
    void childStarted();
    void press();
    bool tick(); // true only when this press belongs to the Zero overlay
    void cancel() { phase_ = Phase::Idle; }
    bool busy() const { return phase_ != Phase::Idle; }
};
}
