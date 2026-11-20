#pragma once
#include "PixelNavigation.h"
#include "RitualState.h"
#include "ComboSequence.h"

namespace ritualcombat {
// Repeated encounter skills determine how close combat needs to approach.
// Single-use rows and unconditional buffs must not shrink that distance.
inline uint32_t AttackRange(const std::vector<combo::Row>& rows) {
    uint32_t range = 0;
    for (const auto& row : rows)
        if (row.environment == combo::Environment::Encounter &&
            row.usage == combo::Usage::EveryLoop && row.distance)
            range = range ? (std::min)(range, row.distance) : row.distance;
    return range ? range : 8;
}
inline bool InAttackPosition(const nav::PixelMap& map, nav::PixelPoint player,
                             nav::PixelPoint target, nav::PixelPoint goal, uint32_t range) {
    return nav::PixelDistance(player, target) <= range && nav::PixelLineClear(map, player, goal);
}
enum class Phase { Waiting, Ready, Clearing, Fighting, Completed };
inline Phase Next(bool object, const ritualstate::Snapshot& state, bool interactable) {
    if (!object || !state.valid) return Phase::Waiting;
    if (state.Completed()) return Phase::Completed;
    if (state.Fighting()) return Phase::Fighting;
    if (interactable && state.CanStart()) return Phase::Ready;
    // A missing/unreadable flag is never evidence that monsters block an altar.
    if (state.current == 1 && state.hasInteraction && state.interaction == 0)
        return Phase::Clearing;
    return Phase::Waiting;
}
struct Circle {
    nav::PixelPoint center{};
    int32_t radius = 0;
    bool Contains(nav::PixelPoint point) const {
        const int64_t dx = int64_t(point.x) - center.x, dy = int64_t(point.y) - center.y;
        if (radius <= 0 || dx < -int64_t(radius) || dx > radius || dy < -int64_t(radius) || dy > radius)
            return false;
        return dx * dx + dy * dy <= int64_t(radius) * radius;
    }
};
inline void ClipCost(const mapdata::CostBuffer& source, const Circle& circle, mapdata::CostBuffer& out) {
    out.Resize(source.Geo());
    for (int dy = -circle.radius; dy <= circle.radius; ++dy)
        for (int dx = -circle.radius; dx <= circle.radius; ++dx) {
            const nav::PixelPoint point{circle.center.x + dx, circle.center.y + dy};
            if (circle.Contains(point)) out.Set(point.x, point.y, source.Get(point.x, point.y));
        }
}
// Select from the player's connected white component in a clipped arena.
// Prefer substantial moves and avoid the last few destinations; small pockets
// can still use a shorter move instead of waiting for an unreachable far point.
inline bool ChooseRoamGoal(const nav::PixelMap& map, const Circle& circle, nav::PixelPoint from,
                           uint64_t variation, nav::PixelPoint& goal,
                           const std::vector<nav::PixelPoint>& recent = {},
                           const std::function<bool()>& cancel = {}) {
    if (!circle.Contains(from) || map.Empty() || (cancel && cancel())) return false;
    nav::PixelPoint white = from;
    double nearest = std::numeric_limits<double>::infinity();
    for (int dy = -8; dy <= 8; ++dy) for (int dx = -8; dx <= 8; ++dx) {
        const nav::PixelPoint point{from.x + dx, from.y + dy};
        const auto distance = nav::PixelDistance(from, point);
        if (distance <= 8 && distance < nearest && circle.Contains(point) && map.Passable(point.x, point.y)) {
            nearest = distance; white = point;
        }
    }
    if (!std::isfinite(nearest)) return false;
    std::vector<uint8_t> connected(map.PassableCount(), 0);
    std::vector<uint32_t> pending{uint32_t(map.Id(white.x, white.y))};
    connected[pending.front()] = 1;
    const nav::PixelBlocked outside = [&](nav::PixelPoint point) { return !circle.Contains(point); };
    for (size_t i = 0; i < pending.size(); ++i) {
        if ((i & 255) == 0 && cancel && cancel()) return false;
        const auto current = map.Point(pending[i]);
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            const nav::PixelPoint next{current.x + dx, current.y + dy};
            const auto id = map.Id(next.x, next.y);
            if (id < 0 || connected[size_t(id)] || !circle.Contains(next) ||
                !nav::PixelLineClear(map, current, next, outside)) continue;
            connected[size_t(id)] = 1; pending.push_back(uint32_t(id));
        }
    }
    const auto preferred = (std::max)(2.0, (std::min)(25.0, double(circle.radius) / 3));
    const auto recentDistance = (std::min)(12.0, double(circle.radius) / 6);
    unsigned bestPriority = 4;
    std::vector<uint32_t> candidates;
    for (size_t i = 0; i < pending.size(); ++i) {
        if ((i & 255) == 0 && cancel && cancel()) return false;
        const auto point = map.Point(pending[i]);
        const auto distance = nav::PixelDistance(from, point);
        if (distance <= 1 || nav::PixelDistance(white, point) <= 1) continue;
        const bool visited = std::any_of(recent.begin(), recent.end(), [&](nav::PixelPoint previous) {
            return nav::PixelDistance(point, previous) <= recentDistance;
        });
        const unsigned priority = (distance < preferred ? 2u : 0u) + (visited ? 1u : 0u);
        if (priority < bestPriority) { bestPriority = priority; candidates.clear(); }
        if (priority == bestPriority) candidates.push_back(pending[i]);
    }
    if (candidates.empty() || (cancel && cancel())) return false;
    goal = map.Point(candidates[size_t(variation % candidates.size())]);
    return true;
}
template<class Entity>
bool AliveInside(const Entity& monster, const Circle& circle, const nav::PixelMap& map) {
    return monster.entity && monster.hp && monster.x >= 0 && monster.y >= 0 &&
        uint32_t(monster.x) < map.Width() && uint32_t(monster.y) < map.Height() &&
        circle.Contains({monster.x, monster.y});
}
// Prefer nearby monsters, but try alternate standing positions when the nearest
// white coordinate belongs to an unreachable component (e.g. across a wall).
template<class Entity>
bool ChooseTarget(const std::vector<Entity>& monsters, const Circle& circle, const nav::PixelMap& map,
                  nav::PixelPoint from, double standOff, Entity& target, nav::PixelPoint& goal,
                  const std::function<bool()>& cancel = {}) {
    target = {};
    if (!std::isfinite(standOff) || standOff < 0) return false;
    nav::PixelPoint white = from;
    double nearest = std::numeric_limits<double>::infinity();
    for (int dy = -8; dy <= 8; ++dy) for (int dx = -8; dx <= 8; ++dx) {
        const nav::PixelPoint point{from.x + dx, from.y + dy};
        const double distance = nav::PixelDistance(point, from);
        if (distance <= 8 && distance < nearest && map.Passable(point.x, point.y)) {
            nearest = distance; white = point;
        }
    }
    if (!std::isfinite(nearest)) return false;
    std::vector<const Entity*> alive;
    for (const auto& monster : monsters) if (AliveInside(monster, circle, map)) alive.push_back(&monster);
    std::sort(alive.begin(), alive.end(), [&](const Entity* a, const Entity* b) {
        const auto da = nav::PixelDistance(from, {a->x, a->y}), db = nav::PixelDistance(from, {b->x, b->y});
        return da != db ? da < db : a->key < b->key;
    });
    // If one goal is disconnected, label the player's component once rather
    // than running a full failed A* for every nearby standing coordinate.
    std::vector<uint8_t> connected;
    auto labelComponent = [&]() {
        connected.assign(map.PassableCount(), 0);
        std::vector<uint32_t> pending{uint32_t(map.Id(white.x, white.y))};
        connected[pending.front()] = 1;
        for (size_t i = 0; i < pending.size(); ++i) {
            if ((i & 4095) == 0 && cancel && cancel()) return false;
            const auto from = map.Point(pending[i]);
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const nav::PixelPoint point{from.x + dx, from.y + dy};
                const auto id = map.Id(point.x, point.y);
                if (id < 0 || connected[size_t(id)] || !nav::PixelLineClear(map, from, point)) continue;
                connected[size_t(id)] = 1; pending.push_back(uint32_t(id));
            }
        }
        return true;
    };
    for (const auto* monster : alive) {
        if (cancel && cancel()) return false;
        // The player's current white coordinate is already a reachable goal.
        // Long-range skills should not enumerate thousands of alternatives
        // before deciding to stand still and cast.
        if (nav::PixelDistance(white, {monster->x, monster->y}) <= standOff) {
            target = *monster; goal = white; return true;
        }
        std::vector<nav::PixelPoint> goals;
        const int64_t radius = int64_t(std::ceil((std::min)(standOff,
            std::hypot(double(map.Width()), double(map.Height())))));
        const int64_t left = (std::max)(int64_t(0), int64_t(monster->x) - radius),
            right = (std::min)(int64_t(map.Width()) - 1, int64_t(monster->x) + radius),
            top = (std::max)(int64_t(0), int64_t(monster->y) - radius),
            bottom = (std::min)(int64_t(map.Height()) - 1, int64_t(monster->y) + radius);
        auto addGoal = [&](nav::PixelPoint point) {
            if (nav::PixelDistance({monster->x, monster->y}, point) <= standOff && map.Passable(point.x, point.y))
                goals.push_back(point);
        };
        // Bound work by the actual map, even for the maximum configured range.
        if (uint64_t(right - left + 1) * uint64_t(bottom - top + 1) > map.PassableCount()) {
            for (size_t i = 0; i < map.PassableCount(); ++i) {
                if ((i & 255) == 0 && cancel && cancel()) return false;
                addGoal(map.Point(uint32_t(i)));
            }
        } else {
            for (int64_t y = top; y <= bottom; ++y) {
                if (cancel && cancel()) return false;
                for (int64_t x = left; x <= right; ++x) addGoal({int32_t(x), int32_t(y)});
            }
        }
        std::sort(goals.begin(), goals.end(), [&](nav::PixelPoint a, nav::PixelPoint b) {
            return nav::PixelDistance(from, a) < nav::PixelDistance(from, b);
        });
        for (const auto point : goals) {
            if (cancel && cancel()) return false;
            if (!connected.empty() && !connected[size_t(map.Id(point.x, point.y))]) continue;
            nav::PixelPath path;
            if (nav::FindPixelPath(map, white, point, path, cancel)) {
                target = *monster; goal = point; return true;
            }
            if (connected.empty() && !labelComponent()) return false;
        }
    }
    return false;
}
} // namespace ritualcombat
