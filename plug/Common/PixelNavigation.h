#pragma once
// Raw white coordinates only. No coarse cells, erosion, cost thresholds or black A* nodes.
#include "MapFrame.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace nav
{
struct PixelPoint
{
    int32_t x = 0, y = 0;
};
class PixelMap
{
    struct Data
    {
        uint32_t w = 0, h = 0;
        std::vector<PixelPoint> white;
        std::unordered_map<uint64_t, uint32_t> ids;
    };
    std::shared_ptr<const Data> d;
    uint64_t Key(int32_t x, int32_t y) const
    {
        return uint64_t(uint32_t(y)) * d->w + uint32_t(x);
    }

  public:
    bool BuildFrom(const mapdata::CostBuffer &cost)
    {
        d.reset();
        if (cost.Empty())
            return false;
        auto n = std::make_shared<Data>();
        n->w = cost.Geo().WorldW();
        n->h = cost.Geo().WorldH();
        size_t count = cost.CountNonZero();
        if (!count || count >= (std::numeric_limits<uint32_t>::max)())
            return false;
        n->white.reserve(count);
        n->ids.reserve(count);
        for (uint32_t y = 0; y < n->h; ++y)
            for (uint32_t x = 0; x < n->w; ++x)
                if (cost.At(x, y) != 0)
                {
                    uint32_t id = static_cast<uint32_t>(n->white.size());
                    n->white.push_back({int32_t(x), int32_t(y)});
                    n->ids.emplace(uint64_t(y) * n->w + x, id);
                }
        d = n;
        return true;
    }
    bool Empty() const
    {
        return !d || d->white.empty();
    }
    uint32_t Width() const
    {
        return d ? d->w : 0;
    }
    uint32_t Height() const
    {
        return d ? d->h : 0;
    }
    size_t PassableCount() const
    {
        return d ? d->white.size() : 0;
    }
    int64_t Id(int32_t x, int32_t y) const
    {
        if (!d || x < 0 || y < 0 || uint32_t(x) >= d->w || uint32_t(y) >= d->h)
            return -1;
        auto i = d->ids.find(Key(x, y));
        return i == d->ids.end() ? -1 : int64_t(i->second);
    }
    bool Passable(int32_t x, int32_t y) const
    {
        return Id(x, y) >= 0;
    }
    PixelPoint Point(uint32_t i) const
    {
        return d->white.at(i);
    }
};
inline double PixelDistance(PixelPoint a, PixelPoint b)
{
    return std::hypot(double(a.x) - b.x, double(a.y) - b.y);
}
using PixelBlocked = std::function<bool(PixelPoint)>;
inline bool PixelLineClear(const PixelMap &m, PixelPoint a, PixelPoint b, const PixelBlocked &blocked = {})
{
    auto pass = [&](PixelPoint p) { return m.Passable(p.x, p.y) && (!blocked || !blocked(p)); };
    if (!pass(a) || !pass(b))
        return false;
    int dx = b.x - a.x, dy = b.y - a.y, n = (std::max)(std::abs(dx), std::abs(dy));
    PixelPoint last = a;
    for (int i = 1; i <= n; ++i)
    {
        PixelPoint p{int32_t(std::lround(a.x + double(dx) * i / n)), int32_t(std::lround(a.y + double(dy) * i / n))};
        if (!pass(p))
            return false;
        if (p.x != last.x && p.y != last.y && (!pass({p.x, last.y}) || !pass({last.x, p.y})))
            return false;
        last = p;
    }
    return true;
}
struct PixelPath
{
    std::vector<PixelPoint> points;
    size_t expanded = 0, nodeCount = 0;
    double length = 0;
    std::string error;
};
inline bool FindPixelPath(const PixelMap &m, PixelPoint s, PixelPoint goal, PixelPath &out,
                          const std::function<bool()> &cancel = {}, const PixelBlocked &blocked = {})
{
    out = PixelPath{};
    out.nodeCount = m.PassableCount();
    int64_t si = m.Id(s.x, s.y), gi = m.Id(goal.x, goal.y);
    if (si < 0 || gi < 0 || (blocked && (blocked(s) || blocked(goal))))
    {
        out.error = si < 0 ? "start is black/outside" : "goal is black/outside";
        return false;
    }
    if (si == gi)
    {
        out.points.push_back(s);
        return true;
    }
    struct Q
    {
        double f, g;
        uint32_t id;
        bool operator<(const Q &b) const
        {
            return f > b.f || (f == b.f && g < b.g);
        }
    };
    // Indexed exclusively by white-node IDs. A black coordinate has no ID or search slot.
    std::vector<double> dist(m.PassableCount(), std::numeric_limits<double>::infinity());
    std::vector<uint32_t> parent(m.PassableCount(), UINT32_MAX);
    std::vector<uint8_t> closed(m.PassableCount(), 0);
    std::priority_queue<Q> q;
    auto h = [&](PixelPoint p) {
        double x = std::abs(double(goal.x) - p.x), y = std::abs(double(goal.y) - p.y);
        return (std::max)(x, y) + (std::sqrt(2.) - 1.) * (std::min)(x, y);
    };
    dist[size_t(si)] = 0;
    q.push({h(s), 0, uint32_t(si)});
    const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    while (!q.empty())
    {
        Q cur = q.top();
        q.pop();
        if (closed[cur.id] || cur.g != dist[cur.id])
            continue;
        if ((out.expanded & 255) == 0 && cancel && cancel())
        {
            out.error = "cancelled";
            return false;
        }
        closed[cur.id] = 1;
        ++out.expanded;
        if (cur.id == uint32_t(gi))
        {
            for (uint32_t i = uint32_t(gi);; i = parent[i])
            {
                out.points.push_back(m.Point(i));
                if (i == uint32_t(si))
                    break;
            }
            std::reverse(out.points.begin(), out.points.end());
            out.length = cur.g;
            return true;
        }
        PixelPoint p = m.Point(cur.id);
        for (auto &v : dirs)
        {
            PixelPoint n{p.x + v[0], p.y + v[1]};
            int64_t id = m.Id(n.x, n.y);
            if (id < 0 || closed[size_t(id)] || (blocked && blocked(n)))
                continue;
            if (v[0] && v[1] && (!m.Passable(p.x + v[0], p.y) || !m.Passable(p.x, p.y + v[1]) ||
                (blocked && (blocked({p.x + v[0], p.y}) || blocked({p.x, p.y + v[1]})))))
                continue;
            double g = cur.g + ((v[0] && v[1]) ? std::sqrt(2.) : 1.);
            if (g < dist[size_t(id)])
            {
                dist[size_t(id)] = g;
                parent[size_t(id)] = cur.id;
                q.push({g + h(n), g, uint32_t(id)});
            }
        }
    }
    out.error = "no connected white path";
    return false;
}
// Only shortcut a path prefix when the complete direct chord is walkable.
// A white endpoint across a wall is not a valid movement target.
inline size_t PixelCallTargetIndex(const PixelMap &m, const std::vector<PixelPoint> &raw, size_t begin, double step,
                                   double arrive)
{
    if (begin >= raw.size() || !std::isfinite(step) || !std::isfinite(arrive) || step < 1 || arrive < 0)
        return begin;
    size_t best = begin;
    for (size_t j = begin + 1; j < raw.size(); ++j)
    {
        if (!m.Passable(raw[j].x, raw[j].y))
            return begin;
        const double direct = PixelDistance(raw[begin], raw[j]);
        if (direct > step + 1e-9 || !PixelLineClear(m, raw[begin], raw[j]))
            break;
        if (j + 1 == raw.size() || direct > arrive)
            best = j;
    }
    return best;
}
inline std::vector<PixelPoint> PixelWaypoints(const PixelMap &m, const std::vector<PixelPoint> &raw, double step)
{
    std::vector<PixelPoint> out;
    if (raw.empty() || !std::isfinite(step) || step < 1)
        return out;
    out.push_back(raw.front());
    size_t i = 0;
    while (i + 1 < raw.size())
    {
        size_t next = PixelCallTargetIndex(m, raw, i, step, 0);
        if (next == i)
            return {};
        out.push_back(raw[next]);
        i = next;
    }
    return out;
}
inline bool SelectPixelCallTarget(const PixelMap &m, const std::vector<PixelPoint> &raw, double step, double arrive,
                                  PixelPoint &target)
{
    if (raw.size() < 2)
        return false;
    size_t i = PixelCallTargetIndex(m, raw, 0, step, arrive);
    if (i == 0)
        return false;
    target = raw[i];
    return true;
}
} // namespace nav
