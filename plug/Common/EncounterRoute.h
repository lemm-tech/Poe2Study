#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace encounterroute {
enum class Kind { Ritual, Elite, BossEntrance, MapBoss };
inline bool Mixed(Kind kind, bool elites) { return kind == Kind::Ritual || (elites && kind == Kind::Elite); }
inline const char* Text(Kind kind) {
    switch (kind) {
    case Kind::Ritual: return "祭祀";
    case Kind::Elite: return "精英怪";
    case Kind::BossEntrance: return "Boss 入口";
    default: return "终极 Boss";
    }
}

constexpr double kInfinity = std::numeric_limits<double>::infinity();
// Exact open-route optimization for ordinary ritual counts. Larger lists use
// a bounded nearest-neighbor fallback rather than factorial enumeration.
inline std::vector<size_t> VisitOrder(const std::vector<std::vector<double>>& distance,
                               const std::vector<double>& start, const std::vector<double>& tail) {
    const size_t count = start.size();
    if (!count || distance.size() != count || tail.size() != count) return {};
    for (const auto& row : distance) if (row.size() != count) return {};
    if (count <= 12) {
        const size_t states = size_t(1) << count;
        std::vector<double> best(states * count, kInfinity);
        std::vector<int> previous(states * count, -1);
        for (size_t i = 0; i < count; ++i) best[(size_t(1) << i) * count + i] = start[i];
        for (size_t mask = 1; mask < states; ++mask) for (size_t i = 0; i < count; ++i) {
            if (!(mask & (size_t(1) << i))) continue;
            const auto value = best[mask * count + i];
            if (!std::isfinite(value)) continue;
            for (size_t next = 0; next < count; ++next) if (!(mask & (size_t(1) << next))) {
                const auto target = (mask | (size_t(1) << next)) * count + next;
                const double candidate = value + distance[i][next];
                if (candidate < best[target]) { best[target] = candidate; previous[target] = int(i); }
            }
        }
        size_t mask = states - 1; int last = -1; double length = kInfinity;
        for (size_t i = 0; i < count; ++i) if (best[mask * count + i] + tail[i] < length) {
            length = best[mask * count + i] + tail[i]; last = int(i);
        }
        if (last < 0) return {};
        std::vector<size_t> result;
        while (last >= 0) {
            result.push_back(size_t(last));
            const int before = previous[mask * count + size_t(last)];
            mask &= ~(size_t(1) << size_t(last)); last = before;
        }
        std::reverse(result.begin(), result.end()); return result;
    }
    std::vector<size_t> result;
    std::vector<bool> used(count, false);
    while (result.size() < count) {
        size_t nearest = count; double length = kInfinity;
        for (size_t i = 0; i < count; ++i) if (!used[i]) {
            const double value = result.empty() ? start[i] : distance[result.back()][i];
            if (value < length) { length = value; nearest = i; }
        }
        if (nearest == count) return {};
        used[nearest] = true; result.push_back(nearest);
    }
    // Bounded 2-opt includes both the start and fixed final boss/entrance cost.
    // The distance matrix is symmetric; reversing the inner segment leaves its
    // internal edge cost unchanged.
    for (unsigned pass = 0; pass < 4; ++pass) {
        bool changed = false;
        for (size_t i = 0; i < count; ++i) for (size_t j = i + 1; j < count; ++j) {
            const auto a = result[i], b = result[j];
            const double before = (i ? distance[result[i - 1]][a] : start[a]) +
                (j + 1 < count ? distance[b][result[j + 1]] : tail[b]);
            const double after = (i ? distance[result[i - 1]][b] : start[b]) +
                (j + 1 < count ? distance[a][result[j + 1]] : tail[a]);
            if (after + 0.001 < before) {
                std::reverse(result.begin() + i, result.begin() + j + 1); changed = true;
            }
        }
        if (!changed) break;
    }
    if (!std::isfinite(tail[result.back()])) return {};
    return result;
}
} // namespace encounterroute
