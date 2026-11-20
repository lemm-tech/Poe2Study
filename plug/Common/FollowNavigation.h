#pragma once
#include "PixelNavigation.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace follow {
enum class Mode : uint32_t { Story = 0, Atlas = 1 };
inline const char* ModeText(Mode mode) { return mode == Mode::Atlas ? "异界模式" : "剧情模式"; }
struct Options {
    std::string leaderName;
    std::string lootNames;
    Mode mode = Mode::Story;
    int32_t distance = 60;
    int32_t lootDistance = 200;
    uint32_t sampleMs = 100, replanMs = 250, searchMs = 500;
    int32_t moveThreshold = 15;
    uint32_t StaleMs() const { return (std::max)(300u, sampleMs * 3); }
    int32_t ResumeDistance() const { return distance + (std::max)(10, distance / 2); }
};
// Exact display-name matching; accept either comma and ignore empty/duplicate entries.
inline bool ParseLootNames(const std::string& text, std::vector<std::string>& names, std::string& error) {
    names.clear(); error.clear();
    if (text.size() > 4096 || text.find_first_of("\r\n=[]\"") != std::string::npos) {
        error = "拾取名单最多 4096 字节，请使用逗号分隔物品名称"; return false;
    }
    std::string value = text;
    for (size_t p = 0; (p = value.find("，", p)) != std::string::npos;) value.replace(p, 3, ",");
    for (size_t start = 0; start <= value.size();) {
        const auto end = value.find(',', start);
        auto name = value.substr(start, end == std::string::npos ? end : end - start);
        const auto first = name.find_first_not_of(" \t");
        name = first == std::string::npos ? "" : name.substr(first, name.find_last_not_of(" \t") - first + 1);
        if (name.size() > 384) { error = "单个物品名称过长"; return false; }
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(std::move(name));
        if (names.size() > 128) { error = "拾取名单最多填写 128 个名称"; return false; }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}
inline std::string JoinLootNames(const std::vector<std::string>& names) {
    std::string result;
    for (const auto& name : names) { if (!result.empty()) result += ','; result += name; }
    return result;
}
inline bool Validate(const Options& o, std::string& error) {
    error.clear();
    if (o.leaderName.empty() || o.leaderName.size() > 384 ||
        o.leaderName.find_first_of("\r\n=[]") != std::string::npos) error = "请填写有效的队长角色名称";
    else if (o.mode != Mode::Story && o.mode != Mode::Atlas) error = "请选择剧情或异界模式";
    else if (o.distance < 1 || o.distance > 94208) error = "保持距离请填写 1–94208 的整数";
    else if (o.lootDistance < 1 || o.lootDistance > 94208) error = "拾取距离请填写 1–94208 的整数（世界像素）";
    else if (o.sampleMs < 20 || o.sampleMs > 1000) error = "队长采样间隔请填写 20–1000ms";
    else if (o.replanMs < 100 || o.replanMs > 5000) error = "重规划最小间隔请填写 100–5000ms";
    else if (o.searchMs < o.sampleMs || o.searchMs > 10000) error = "失联搜索间隔需不小于采样间隔，且不超过 10000ms";
    else if (o.moveThreshold < 1 || o.moveThreshold > 1000) error = "目标位移阈值请填写 1–1000";
    std::vector<std::string> names;
    return error.empty() && ParseLootNames(o.lootNames, names, error);
}
enum class State { Searching, Planning, Following, Waiting, Lost, Stopped, Failed, LootApproach, LootWaiting, Travelling };
inline const char* StateText(State s) {
    switch (s) {
    case State::Searching: return "正在寻找队长";
    case State::Planning: return "正在规划路线";
    case State::Following: return "正在跟随";
    case State::Waiting: return "保持距离，等待队长";
    case State::Lost: return "队长位置失效，正在重新寻找";
    case State::Failed: return "跟随已停止：发生错误";
    case State::LootApproach: return "正在接近拾取物品";
    case State::LootWaiting: return "正在等待拾取确认";
    case State::Travelling: return "正在跨图跟随";
    default: return "已停止跟随";
    }
}
struct Snapshot {
    bool valid = false, terminal = false, sceneChanged = false, absent = false;
    nav::PixelPoint position{};
    uint64_t sampledAt = 0, sequence = 0, entity = 0, mapToken = 0;
    uint64_t searchedAt = 0; // Only a complete, unambiguous nearby scan may authorize travel.
    uint32_t width = 0, height = 0;
    std::string note;
};
inline bool Fresh(const Snapshot& s, uint64_t now, uint32_t staleMs) {
    return s.valid && !s.terminal && s.sampledAt && now >= s.sampledAt && now - s.sampledAt <= staleMs;
}
inline bool HoldDistance(double reachableDistance, bool waiting, const Options& o, double tolerance = 0) {
    return reachableDistance <= (waiting ? o.ResumeDistance() : o.distance + tolerance);
}
inline bool RetargetDue(nav::PixelPoint planned, nav::PixelPoint latest, uint64_t now,
                        uint64_t lastPlanAt, const Options& o) {
    return (!lastPlanAt || (now >= lastPlanAt && now - lastPlanAt >= o.replanMs)) &&
           nav::PixelDistance(planned, latest) >= o.moveThreshold;
}
struct Feedback {
    State state = State::Searching;
    nav::PixelPoint player{}, leader{};
    double distance = -1;
    uint32_t plans = 0, calls = 0;
    uint64_t planMs = 0;
    std::string note;
    std::string lootName;
    uint32_t lootRemaining = 0, lootResolved = 0, lootSkipped = 0;
};
struct LootSnapshot {
    bool active = false, waiting = false, terminal = false;
    uint32_t key = 0, remaining = 0, resolved = 0, skipped = 0;
    uint64_t entity = 0;
    nav::PixelPoint position{};
    std::string name, note;
};
enum class TravelAction { Continue, Wait, ReloadScene, Failed, Portal };
struct TravelSnapshot {
    TravelAction action = TravelAction::Continue;
    uint32_t key = 0;
    uint64_t entity = 0;
    nav::PixelPoint position{};
    std::string note;
};
struct Channel {
    Options options;
    // sample only copies a thread-safe snapshot; no memory I/O here.
    std::function<Snapshot()> sample;
    std::function<void(const Feedback&)> publish;
    // Invoked on the navigation thread, serially with WalkCall. May perform I/O.
    std::function<LootSnapshot(nav::PixelPoint)> loot;
    std::function<bool(uint32_t, uint64_t, std::string&)> pick;
    // Serial with movement/pick: false only checks scene health, true may select/send travel.
    std::function<TravelSnapshot(const Snapshot&, bool)> travel;
    std::function<TravelSnapshot(uint32_t, uint64_t)> enterPortal;
};
} // namespace follow
