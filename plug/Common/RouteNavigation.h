#pragma once
// Navigation policy only: no packets, game calls, clocks, or mutable base map.
#include "PixelNavigation.h"

namespace nav
{
inline double PixelSegmentDistance(PixelPoint p, PixelPoint a, PixelPoint b)
{
    double dx = double(b.x) - a.x, dy = double(b.y) - a.y;
    double n = dx * dx + dy * dy;
    double t = n ? ((p.x - double(a.x)) * dx + (p.y - double(a.y)) * dy) / n : 0;
    t = (std::max)(0.0, (std::min)(1.0, t));
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}
inline bool PixelStepToward(const PixelMap &map, PixelPoint from, PixelPoint to, double limit,
                            PixelPoint &out, const PixelBlocked &blocked = {})
{
    double distance = PixelDistance(from, to);
    if (distance < 1e-9 || limit < 1 || !std::isfinite(limit)) return false;
    int n = (std::max)(std::abs(to.x - from.x), std::abs(to.y - from.y));
    int end = (std::min)(n, int(std::floor(n * (std::min)(1.0, limit / distance) + 1e-9)));
    // Test the actual integer chord after rounding; never exceed the call radius.
    for (int i = end; i > 0; --i)
    {
        PixelPoint p{int32_t(std::lround(from.x + double(to.x - from.x) * i / n)),
                     int32_t(std::lround(from.y + double(to.y - from.y) * i / n))};
        if (PixelDistance(from, p) <= limit + 1e-9 && PixelLineClear(map, from, p, blocked))
        { out = p; return true; }
    }
    return false;
}
class PixelRoute
{
    std::vector<PixelPoint> m_points;
    size_t m_index = 0;
public:
    void Clear() { m_points.clear(); m_index = 0; }
    bool Empty() const { return m_points.size() < 2; }
    size_t Index() const { return m_index; }
    const std::vector<PixelPoint> &Points() const { return m_points; }
    bool Build(const PixelMap &map, const std::vector<PixelPoint> &raw,
               const PixelBlocked &blocked = {}, const std::function<bool()> &cancel = {})
    {
        Clear();
        if (raw.empty()) return false;
        m_points.push_back(raw.front());
        for (size_t anchor = 0; anchor + 1 < raw.size();)
        {
            if (cancel && cancel()) { Clear(); return false; }
            size_t best = anchor + 1;
            if (!PixelLineClear(map, raw[anchor], raw[best], blocked)) { Clear(); return false; }
            // Collinear interior pixels cannot improve a shortcut: test corners/endpoints.
            for (size_t j = anchor + 2; j < raw.size(); ++j)
            {
                if ((j & 255) == 0 && cancel && cancel()) { Clear(); return false; }
                if (j + 1 < raw.size() && raw[j].x - raw[j-1].x == raw[j+1].x - raw[j].x &&
                    raw[j].y - raw[j-1].y == raw[j+1].y - raw[j].y) continue;
                if (!PixelLineClear(map, raw[anchor], raw[j], blocked)) break;
                best = j;
            }
            m_points.push_back(raw[best]);
            anchor = best;
        }
        return true;
    }
    void Advance(const PixelMap &map, PixelPoint p, double radius, const PixelBlocked &blocked = {})
    {
        while (m_index + 2 < m_points.size())
        {
            auto a = m_points[m_index], b = m_points[m_index + 1];
            const auto c = m_points[m_index + 2];
            bool passed = (double(p.x) - b.x) * (b.x - double(a.x)) +
                          (double(p.y) - b.y) * (b.y - double(a.y)) >= 0 &&
                          PixelSegmentDistance(p, a, b) <= radius;
            if (!passed && radius >= 0)
            {
                // WalkCall may cut across a corner and land on the outgoing
                // segment without ever entering the old segment's radius.
                // The old test only measured distance to [a,b], so a position
                // such as (333,582) on [b,c] kept m_index at the old corner
                // forever and Target() returned the same 1-pixel call target.
                // Recognize the outgoing segment as progress, but still move
                // only one segment per pass so the A* route remains ordered.
                const double outgoing = PixelSegmentDistance(p, b, c);
                const double cornerRadius = (std::max)(radius, 1.0);
                const double forward = (double(p.x) - b.x) * (c.x - double(b.x)) +
                                       (double(p.y) - b.y) * (c.y - double(b.y));
                const double outgoingLength2 = PixelDistance(b, c);
                if (outgoing <= cornerRadius && forward >= -cornerRadius * (outgoingLength2 + 1.0))
                    passed = true;
            }
            if (PixelDistance(p, b) > radius && !passed) break;
            // Advance one polyline segment at a time. The WalkCall itself has
            // short-range steering, so requiring a direct chord to the segment
            // after a corner would keep the cursor on the old segment and make
            // deviation detection fight the actual turn. We never skip more than
            // one reached waypoint here; the A* polyline remains authoritative.
            ++m_index;
        }
    }
    bool Target(const PixelMap &map, PixelPoint p, double step, double radius, PixelPoint &out,
                const PixelBlocked &blocked = {}, double lookAhead = 0)
    {
        if (Empty() || step <= 0) return false;
        Advance(map, p, radius, blocked);
        (void)lookAhead; // The lead window controls when this target is replaced.

        // WalkCall is a bounded absolute target with its own short-range steering.
        // Therefore a call should consume roughly `step` distance along the A*
        // polyline, not stop at every visibility corner. This lets one target lie
        // just beyond a turn and avoids the stop/turn/start cadence at corners.
        double budget = step;
        PixelPoint from = p;
        for (size_t i = m_index + 1; i < m_points.size(); ++i)
        {
            const PixelPoint to = m_points[i];
            const double segment = PixelDistance(from, to);
            if (segment < 0.5)
            {
                from = to;
                continue;
            }
            if (segment >= budget)
            {
                // Integer rounding can overshoot the Euclidean call cap by a
                // fraction of a pixel. Back off until the rounded target is safe.
                for (double d = budget; d >= 1.0; d -= 1.0)
                {
                    PixelPoint q{
                        int32_t(std::lround(from.x + (to.x - from.x) * d / segment)),
                        int32_t(std::lround(from.y + (to.y - from.y) * d / segment))};
                    if ((q.x == p.x && q.y == p.y) || PixelDistance(p, q) > step + 1e-6)
                        continue;
                    if (map.Passable(q.x, q.y) && (!blocked || !blocked(q)))
                    {
                        out = q;
                        return true;
                    }
                }
                return false;
            }
            budget -= segment;
            from = to;
        }

        // Less than one call remains: use the final route point. It is already
        // validated by A*/route construction and is allowed to be shorter.
        const PixelPoint goal = m_points.back();
        if ((goal.x == p.x && goal.y == p.y) || PixelDistance(p, goal) > step + 1e-6)
            return false;
        if (!map.Passable(goal.x, goal.y) || (blocked && blocked(goal))) return false;
        out = goal;
        return true;
    }
    double Deviation(PixelPoint p) const
    {
        double best = std::numeric_limits<double>::infinity();
        // Do not match an already traversed branch of a folded/intersecting route.
        for (size_t i = m_index; i + 1 < m_points.size() && i <= m_index + 1; ++i)
            best = (std::min)(best, PixelSegmentDistance(p, m_points[i], m_points[i+1]));
        return best;
    }
    double Remaining(PixelPoint p) const
    {
        if (Empty()) return 0;
        double d = PixelDistance(p, m_points[m_index + 1]);
        for (size_t i = m_index + 1; i + 1 < m_points.size(); ++i)
            d += PixelDistance(m_points[i], m_points[i+1]);
        return d;
    }
};
class TemporaryObstacles
{
    struct Block { PixelPoint center; int radius; uint64_t until; };
    std::vector<Block> m_blocks;
public:
    void Clear() { m_blocks.clear(); }
    void Expire(uint64_t now)
    {
        m_blocks.erase(std::remove_if(m_blocks.begin(), m_blocks.end(),
                       [=](const Block &b) { return now >= b.until; }), m_blocks.end());
    }
    bool Empty() const { return m_blocks.empty(); }
    bool Contains(PixelPoint p, uint64_t now) const
    {
        for (const auto &b : m_blocks)
            if (now < b.until && std::abs(p.x-b.center.x) <= b.radius &&
                std::abs(p.y-b.center.y) <= b.radius) return true;
        return false;
    }
    void Add(PixelPoint p, int radius, uint64_t until) { m_blocks.push_back({p, radius, until}); }
};
enum class MotionResult { Observing, Reached, Lead, Still, Timeout };
struct MotionOptions
{
    uint32_t leadMs = 120, startupMs = 350, waitMaxMs = 3000, minCallMs = 200, stillSamples = 3;
    double reachRadius = 3;
};
class MotionWindow
{
    PixelPoint m_previous{}, m_target{}, m_origin{};
    bool m_reached = false;
    uint64_t m_started = 0, m_previousAt = 0;
    uint32_t m_still = 0;
    double m_moved = 0, m_speed = 0;
public:
    void Begin(uint64_t at, PixelPoint p, PixelPoint target)
    { m_started = m_previousAt = at; m_origin = m_previous = p; m_target = target; m_still = 0; m_moved = m_speed = 0; m_reached = false; }
    double Moved() const { return m_moved; }
    double Speed() const { return m_speed; }
    MotionResult Sample(uint64_t at, PixelPoint p, const MotionOptions &o)
    {
        if (at <= m_previousAt) return MotionResult::Observing;
        double delta = PixelDistance(p, m_previous);
        double speed = delta * 1000 / double(at - m_previousAt);
        m_moved += delta;
        m_previous = p;
        m_previousAt = at;
        m_still = delta < 0.5 ? m_still + 1 : 0;
        if (delta >= 0.5) m_speed = speed;
        auto elapsed = at - m_started;
        double remain = PixelDistance(p, m_target);
        double along = (p.x-double(m_target.x))*(m_target.x-double(m_origin.x)) +
                       (p.y-double(m_target.y))*(m_target.y-double(m_origin.y));
        bool passed = along >= 0 && PixelSegmentDistance(m_target,m_origin,p) <= o.reachRadius;
        m_reached = m_reached || remain <= o.reachRadius || passed;
        // Observe arrival independently of rate limiting. The caller throttles actual
        // commands while continuing fresh position sampling, so a short target cannot
        // be missed during minCallMs and then waited on until the character stops.
        if (m_reached) return MotionResult::Reached;
        if (elapsed >= o.startupMs && m_still >= o.stillSamples) return MotionResult::Still;
        if (elapsed >= o.waitMaxMs) return MotionResult::Timeout;
        if (remain <= o.reachRadius) return MotionResult::Reached;
        if (o.leadMs && m_moved >= 1 && delta >= 0.5 && remain <= m_speed * o.leadMs / 1000)
            return MotionResult::Lead;
        return MotionResult::Observing;
    }
};
} // namespace nav
