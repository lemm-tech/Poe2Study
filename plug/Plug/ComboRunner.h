#pragma once
#include "../Common/ComboSequence.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <thread>
namespace pipe { class NamedPipeClie; }
namespace poelog { class RunDiagnostics; }

namespace combo {
// One native roll plus a matching release; ambiguous replies are never replayed.
bool DodgeRollOnce(pipe::NamedPipeClie&, uint64_t entity, uint64_t world,
                   int32_t x, int32_t y, std::string& error);
struct RunnerStatus {
    bool running = false;
    uint64_t casts = 0;
    std::string text = "未启动";
    bool stopConfirmed = true;
    bool roundCompleted = false;
};
struct StartOptions {
    // Manual test runs start from a fresh single-use ledger; game loops may retain it.
    bool resetSingleUse = false;
    const std::atomic_bool* cancel = nullptr;
    bool singleRound = false; // Finish after each configured row is evaluated once.
    std::shared_ptr<poelog::RunDiagnostics> diagnostics;
};
class Runner {
public:
    ~Runner();
    bool Start(std::vector<Row>, std::string& error, StartOptions options = {});
    void Stop();
    void Join();
    RunnerStatus Status() const;
private:
    void Run(std::vector<Row>);
    void SetStatus(const std::string&);
    void Wait(uint32_t ms);
    bool StopRequested() const;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::atomic_bool stop_{false}, running_{false};
    const std::atomic_bool* cancel_ = nullptr; // Supplied flag outlives Run().
    RunnerStatus status_;
    bool singleRound_ = false;
    std::shared_ptr<poelog::RunDiagnostics> diagnostics_;
    uint64_t ledgerMapMarker_ = 0, ledgerEntity_ = 0;
    uint32_t ledgerPid_ = 0;
    std::set<uint64_t> used_;
};
Runner& MainRunner();
} // namespace combo
