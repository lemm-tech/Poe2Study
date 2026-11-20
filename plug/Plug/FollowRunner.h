#pragma once
#include "../Common/FollowNavigation.h"
#include "Surroundings.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace follow {
std::wstring ConfigPath();
bool LoadConfig(Options&, std::string& error);
bool SaveConfig(const Options&, std::string& error);
std::wstring DiagnosticLogPath();
void LogDiagnostic(const char* format, ...);
struct PlayerListStatus {
    bool refreshing = false;
    uint64_t revision = 0, selfEntity = 0, selfPositioned = 0;
    std::vector<poe::PlayerCandidate> players;
    std::string text;
};
struct Status {
    bool running = false;
    Snapshot target;
    Feedback navigation;
    uint64_t samples = 0, sampleGapMs = 0;
    uint64_t selfEntity = 0, selfPositioned = 0, leaderPositioned = 0;
    std::string selfName, leaderName;
    nav::PixelPoint selfPosition{};
    bool selfPositionValid = false;
    std::string selfMap, leaderMap;
    uint32_t teleportAttempts = 0;
    std::string text = "未启动";
};
class Runner {
public:
    ~Runner();
    bool Start(const Options&, std::string& error);
    void Stop();
    void Join();
    bool Running() const { return running_.load(); }
    Status GetStatus() const;
    bool RefreshPlayers(std::string& error);
    PlayerListStatus GetPlayers() const;
    void StopPlayerScan();
    void JoinPlayerScan();
private:
    void Run(Options);
    void ScanPlayers();
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::atomic_bool stop_{false}, samplingStop_{false}, running_{false};
    Status status_;
    std::thread scanThread_;
    std::atomic_bool scanStop_{false}, scanning_{false};
    PlayerListStatus players_;
};
Runner& MainRunner();
} // namespace follow
