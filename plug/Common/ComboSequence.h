#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>
#include <utility>

namespace combo {
constexpr uint32_t kMaxRows = 256, kMaxTimeMs = 600000;
enum class Usage : uint32_t { Once = 0, EveryLoop = 1 };
enum class Environment : uint32_t { Encounter = 0, Always = 1 };
struct Row {
    uint64_t key = 0;
    std::string internalId, name;
    uint32_t slotHint = 0;
    Usage usage = Usage::EveryLoop;
    Environment environment = Environment::Encounter;
    uint32_t intervalMs = 1000, chargeMs = 0, distance = 100;
};
struct Target { uint32_t id = 0; int32_t x = 0, y = 0; };
inline bool Nearest(int32_t x, int32_t y, uint32_t range,
                    const std::vector<Target>& targets, Target& out) {
    double best = std::numeric_limits<double>::infinity();
    bool found = false;
    const double limit = double(range) * range;
    for (const auto& t : targets) {
        const double dx = double(t.x) - x, dy = double(t.y) - y;
        const double d = dx * dx + dy * dy;
        if (d <= limit && (d < best || (d == best && t.id < out.id))) {
            out = t; best = d; found = true;
        }
    }
    return found;
}

// Pure scheduling rules; the caller supplies a monotonic clock and game I/O.
// A single-use row is consumed by Issued(), never by Skip().
class Sequence {
public:
    enum class Action { Wait, Evaluate, Release, Complete };
    void Reset(std::vector<Row> rows, std::set<uint64_t> used = {}, uint32_t maxRounds = 0) {
        rows_ = std::move(rows); used_ = std::move(used);
        index_ = 0; phase_ = Phase::Ready; deadline_ = 0; issuedInRound_ = false;
        rounds_ = 0; maxRounds_ = maxRounds;
    }
    Action Poll(uint64_t now) {
        if (maxRounds_ && rounds_ >= maxRounds_) return Action::Complete;
        if (rows_.empty() || now < deadline_) return Action::Wait;
        if (phase_ == Phase::Holding) return Action::Release;
        if (phase_ == Phase::Gap) { phase_ = Phase::Ready; Advance(now); }
        if (maxRounds_ && rounds_ >= maxRounds_) return Action::Complete;
        for (size_t n = 0; n < rows_.size() && now >= deadline_; ++n) {
            if (Current().usage != Usage::Once || !used_.count(Current().key))
                return Action::Evaluate;
            Advance(now);
            if (maxRounds_ && rounds_ >= maxRounds_) return Action::Complete;
        }
        return Action::Wait;
    }
    const Row& Current() const { return rows_[index_]; }
    size_t Index() const { return index_; }
    const std::set<uint64_t>& Used() const { return used_; }
    void Skip(uint64_t now) { Advance(now); }
    void Issued(uint64_t now) {
        if (Current().usage == Usage::Once) used_.insert(Current().key);
        issuedInRound_ = true; phase_ = Phase::Holding;
        deadline_ = now + Current().chargeMs;
    }
    void Released(uint64_t now) {
        phase_ = Phase::Gap; deadline_ = now + Current().intervalMs;
    }
private:
    enum class Phase { Ready, Holding, Gap };
    void Advance(uint64_t now) {
        if (++index_ == rows_.size()) {
            index_ = 0;
            ++rounds_;
            if (!issuedInRound_) deadline_ = now + 100;
            issuedInRound_ = false;
        }
    }
    std::vector<Row> rows_;
    std::set<uint64_t> used_;
    size_t index_ = 0;
    Phase phase_ = Phase::Ready;
    uint64_t deadline_ = 0;
    bool issuedInRound_ = false;
    uint32_t rounds_ = 0, maxRounds_ = 0;
};
} // namespace combo
