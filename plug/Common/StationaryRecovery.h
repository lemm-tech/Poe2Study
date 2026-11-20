#pragma once
#include "PixelNavigation.h"
#include <cstdint>

namespace nav {
// One recovery per five seconds of observed, unchanged coordinates while walking.
// Suspend preserves walking time across short retarget/replan calls, but excludes
// time spent outside Navigate. Intentional waits/combat/pause explicitly Reset it.
class StationaryRecovery {
public:
    static constexpr uint64_t kDelayMs = 5000;
    void Reset() { *this = {}; }
    void Begin(uint64_t now, PixelPoint point) {
        if (!known_ || point.x != point_.x || point.y != point_.y) elapsed_ = 0;
        point_ = point; known_ = active_ = true; sampledAt_ = now;
    }
    void Observe(uint64_t now, PixelPoint point) {
        if (!active_) return;
        if (point.x != point_.x || point.y != point_.y || now < sampledAt_) elapsed_ = 0;
        else elapsed_ += now - sampledAt_;
        point_ = point; sampledAt_ = now;
    }
    void Suspend() { active_ = false; }
    bool Active() const { return active_; }
    bool Due() const { return active_ && elapsed_ >= kDelayMs; }
private:
    PixelPoint point_{};
    uint64_t sampledAt_ = 0, elapsed_ = 0;
    bool known_ = false, active_ = false;
};
} // namespace nav
