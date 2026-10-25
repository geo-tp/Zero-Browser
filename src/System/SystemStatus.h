#pragma once

namespace zero {
// Presentation data only; Linux probes and their cache live in SystemStatus.cpp.
struct SystemStatus {
    int batteryPercent = -1;
    bool wifiConnected = false;
    static const SystemStatus &sample();
};
} // namespace zero
