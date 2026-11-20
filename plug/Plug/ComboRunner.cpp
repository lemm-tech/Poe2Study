#include "ComboRunner.h"
#include "MyOLAFunction.h"
#include "NamedPipeClie.h"
#include "SkillSlots.h"
#include "Surroundings.h"
#include "../Common/ComboProtocol.h"
#include "../Common/Logger.h"
#include "../Common/RunDiagnostics.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace combo {
namespace {
uint64_t NextToken() {
    static std::atomic<uint64_t> serial{0};
    return (uint64_t(GetCurrentProcessId()) << 32) | (++serial & 0xFFFFFFFFULL);
}
struct World {
    uint64_t entity = 0, mapMarker = 0;
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
};
bool ReadWorld(const GameSession& session, World& out) {
    const auto& m = session.memory;
    auto entity = m.Follow(session.moduleBase + BaseOffset);
    for (int i = 0; entity && i < 7; ++i) entity = m.Follow(entity + kMapObjectChain[i]);
    if (!entity) return false;
    // Use the live pointer at player+0x78 as the map/session marker. Do not
    // derive a map name or a deeper map identity from the map object.
    const auto mapMarker = m.Follow(entity + 0x78);
    if (!mapMarker) return false;
    uint32_t w = 0, h = 0;
    if (!m.ReadInto(mapMarker + 0x8F8 + MAP_OFF_GRID_W, &w, sizeof(w)) ||
        !m.ReadInto(mapMarker + 0x8F8 + MAP_OFF_GRID_H, &h, sizeof(h)) ||
        !w || !h || w > kMaxGridDimension || h > kMaxGridDimension) return false;
    poe::Entity player(m, entity);
    auto pos = player.GetAs<poe::PositionComponent>("Positioned");
    if (!pos) return false;
    const auto point = pos->GetPosition();
    if (point.x < 0 || point.y < 0 || uint32_t(point.x) >= w * CELL_SIZE || uint32_t(point.y) >= h * CELL_SIZE)
        return false;
    out = {entity, mapMarker, point.x, point.y, w * CELL_SIZE, h * CELL_SIZE};
    return true;
}
bool Exchange(pipe::NamedPipeClie& client, const CastRequest& request, CastReply& reply) {
    CastMessage message(request); std::vector<uint8_t> body;
    if (!client.Request(&message, MT_ComboCastAck, body, 4000) || body.size() != sizeof(reply)) return false;
    std::memcpy(&reply, body.data(), sizeof(reply));
    reply.detail[sizeof(reply.detail) - 1] = 0;
    return reply.version == kWireVersion && reply.token == request.token;
}
bool StopGameSkill(pipe::NamedPipeClie& client) {
    // Token release may already be a no-op (0ms presses, or navigation replacing
    // the pending action). End the game-side skill explicitly at the end of a run.
    // The client is bound to the original Controller/session; never reconnect here.
    SkillStopReq request{};
    SkillStopMessage message(request);
    std::vector<uint8_t> body;
    if (!client.Request(&message, MT_SkillStopAck, body, 3000) || body.size() != sizeof(SkillStopAck)) {
        LOGW("[Combo] final skill stop acknowledgement missing");
        return false;
    }
    SkillStopAck reply{};
    std::memcpy(&reply, body.data(), sizeof(reply));
    LOGI("[Combo] final skill stop result=%d context=0x%llX gate=0x%02X->0x%02X",
         reply.result, reply.stopThis, unsigned(reply.gateBefore), unsigned(reply.gateAfter));
    return reply.result == 0;
}
uint32_t Resolve(const Row& row, const poe::SkillSlotsSnapshot& skills) {
    for (const auto& s : skills.slots)
        if (s.occupied && !s.movement && s.slotId == row.slotHint && s.internalId == row.internalId) return s.skillId;
    for (const auto& s : skills.slots)
        if (s.occupied && !s.movement && s.internalId == row.internalId) return s.skillId;
    return 0;
}
} // namespace

Runner& MainRunner() { static Runner runner; return runner; }
bool DodgeRollOnce(pipe::NamedPipeClie& client, uint64_t entity, uint64_t world,
                   int32_t x, int32_t y, std::string& error) {
    CastRequest request{}; request.operation = Operation::DodgeRoll;
    request.token = NextToken(); request.expectedEntity = entity; request.expectedWorld = world;
    request.skillId = kDodgeRollSkillId; request.x = x; request.y = y;
    CastReply reply{};
    const bool received = Exchange(client, request, reply);
    // Release is idempotent, including after a lost press acknowledgement. It
    // cannot issue another roll and never releases a different token/owner.
    CastRequest release{}; release.operation = Operation::Release; release.token = request.token;
    CastReply released{};
    const bool confirmed = Exchange(client, release, released) && released.result == Result::Ok;
    if (!confirmed) StopGameSkill(client);
    if (!received || reply.result != Result::Ok || reply.invoked != 1 || !confirmed ||
        !reply.pressedAt || !released.releasedAt) {
        error = !received ? "翻滚请求未确认，已请求松开；未自动重发" :
            !confirmed ? "翻滚松开未确认，已请求停止技能；祭祀任务停止" :
            std::string("翻滚请求失败：") + reply.detail;
        return false;
    }
    LOGI("[RitualRoll] id=0x%08X target=(%d,%d) token=%llu released=1", request.skillId, x, y, request.token);
    error.clear(); return true;
}
Runner::~Runner() { Stop(); Join(); }
bool Runner::Start(std::vector<Row> rows, std::string& error, StartOptions options) {
    error.clear();
    if (options.cancel && options.cancel->load()) { error = "导航测试已取消"; return false; }
    if (running_) { error = "连招已经运行"; return false; }
    if (rows.empty() || rows.size() > kMaxRows) { error = "请先添加技能"; return false; }
    for (const auto& row : rows) {
        if (!row.key || row.internalId.empty() || row.intervalMs > kMaxTimeMs || row.chargeMs > kMaxTimeMs) {
            error = "连招配置无效"; return false;
        }
    }
    if (!pipe::GetConnectionSnapshot().ready) {
        error = "通信未就绪，请先注入新版 Monitor 并关闭其他控制端"; return false;
    }
    Join();
    cancel_ = options.cancel;
    singleRound_ = options.singleRound;
    diagnostics_ = options.diagnostics;
    if (cancel_ && cancel_->load()) { error = "导航测试已取消"; return false; }
    if (options.resetSingleUse) {
        used_.clear(); ledgerMapMarker_ = ledgerEntity_ = 0; ledgerPid_ = 0;
        LOGI("[Combo] new manual test: reset single-use ledger");
    }
    stop_ = false; running_ = true;
    { std::lock_guard<std::mutex> lock(mutex_); status_ = {true, 0, "正在启动连招"}; }
    try { thread_ = std::thread(&Runner::Run, this, std::move(rows)); }
    catch (const std::exception& e) {
        running_ = false; error = std::string("无法启动连招线程：") + e.what();
        SetStatus(error); return false;
    }
    return true;
}
void Runner::Stop() {
    if (!stop_.exchange(true) && running_.load()) LOGI("[Combo] stop requested");
    wake_.notify_all();
}
bool Runner::StopRequested() const { return stop_.load() || (cancel_ && cancel_->load()); }
void Runner::Join() { if (thread_.joinable()) thread_.join(); }
RunnerStatus Runner::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto value = status_; value.running = running_.load(); return value;
}
void Runner::SetStatus(const std::string& text) {
    if (diagnostics_) diagnostics_->Changed("combo_status", text);
    std::lock_guard<std::mutex> lock(mutex_); status_.text = text;
}
void Runner::Wait(uint32_t ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return StopRequested(); });
}
void Runner::Run(std::vector<Row> rows) {
    if (!diagnostics_) diagnostics_ = std::make_shared<poelog::RunDiagnostics>(app::Config().RuntimeFileW(L"combo_logs"), L"combo");
    diagnostics_->Write("combo_begin", "rows=" + std::to_string(rows.size()) + " single_round=" + std::to_string(singleRound_));
    uint64_t heldToken = 0;
    bool pressAttempted = false;
    std::unique_ptr<pipe::NamedPipeClie> client;
    uint64_t releaseAt = 0;
    std::string endText = "连招已停止";
    auto release = [&] {
        if (!heldToken || !client) return true;
        CastRequest request{}; request.operation = Operation::Release; request.token = heldToken;
        CastReply reply{};
        const bool ok = Exchange(*client, request, reply) && reply.result == Result::Ok;
        diagnostics_->Write("combo_release", "token=" + std::to_string(heldToken) +
            " confirmed=" + std::to_string(ok) + " result=" + std::to_string(unsigned(reply.result)) +
            " released_at=" + std::to_string(reply.releasedAt));
        releaseAt = reply.releasedAt ? reply.releasedAt : GetTickCount64();
        heldToken = 0;
        return ok;
    };
    try {
        const auto connection = pipe::GetConnectionSnapshot();
        GameSession session; std::string error;
        if (!InitializeSession(session, &error)) throw std::runtime_error(error);
        if (session.game.pid != connection.gamePid) throw std::runtime_error("游戏 PID 与通信端不一致");
        client = std::make_unique<pipe::NamedPipeClie>(app::Config().PipeFullName(), nullptr);
        if (!client->Initialize() || client->SessionId() != connection.sessionId)
            throw std::runtime_error("连招通信初始化失败");
        Sequence sequence;
        bool attached = false;
        uint32_t worldReadFailures = 0;
        int32_t previousX = 0, previousY = 0;
        double headingX = 1, headingY = 0;
        while (!StopRequested()) {
            if (!connection.Same(pipe::GetConnectionSnapshot()))
                throw std::runtime_error("通信会话已变化，连招停止；不会自动重发旧请求");
            World world;
            if (!ReadWorld(session, world)) {
                if (worldReadFailures < 10) ++worldReadFailures;
                if (worldReadFailures < 10) {
                    SetStatus("地图状态暂时读不到 " + std::to_string(worldReadFailures) + "/10；保留连招进度");
                    Wait(100); continue;
                }
                if (!release()) throw std::runtime_error("离开地图时松开未确认，连招停止");
                attached = false;
                SetStatus("地图状态连续 10 次读取失败，等待进入地图"); Wait(100); continue;
            }
            worldReadFailures = 0;
            if (!attached || world.mapMarker != ledgerMapMarker_ || world.entity != ledgerEntity_ ||
                ledgerPid_ != connection.gamePid) {
                if (!release()) throw std::runtime_error("旧技能松开未确认，连招停止");
                if (world.mapMarker != ledgerMapMarker_ || world.entity != ledgerEntity_ || ledgerPid_ != connection.gamePid) {
                    used_.clear(); ledgerMapMarker_ = world.mapMarker; ledgerEntity_ = world.entity;
                    ledgerPid_ = connection.gamePid;
                    LOGI("[Combo] new game/map entity=0x%llX marker=0x%llX", world.entity, world.mapMarker);
                }
                sequence.Reset(rows, used_, singleRound_ ? 1U : 0U); attached = true;
                previousX = world.x; previousY = world.y;
            }
            const double dx = double(world.x) - previousX, dy = double(world.y) - previousY;
            if (dx * dx + dy * dy >= 4) {
                const double distance = std::sqrt(dx * dx + dy * dy);
                headingX = dx / distance; headingY = dy / distance;
            }
            previousX = world.x; previousY = world.y;
            const auto action = sequence.Poll(GetTickCount64());
            if (action == Sequence::Action::Complete) {
                endText = "本轮技能循环已完成";
                { std::lock_guard<std::mutex> lock(mutex_); status_.roundCompleted = true; }
                break;
            }
            if (action == Sequence::Action::Wait) { Wait(10); continue; }
            if (action == Sequence::Action::Release) {
                if (!release()) throw std::runtime_error("松开技能未确认，连招停止");
                sequence.Released(releaseAt);
                SetStatus("已松开：" + sequence.Current().name + "；等待间隔 " +
                          std::to_string(sequence.Current().intervalMs) + "ms");
                continue;
            }
            const auto& row = sequence.Current();
            Target target{};
            target.x = std::clamp(world.x + int32_t(std::lround(headingX * 60)), 0, int32_t(world.width) - 1);
            target.y = std::clamp(world.y + int32_t(std::lround(headingY * 60)), 0, int32_t(world.height) - 1);
            if (row.environment == Environment::Encounter) {
                poe::SurroundingsTraverser scan(session.memory); std::vector<poe::EntitySnapshot> entities;
                std::vector<Target> targets;
                poe::EntityScanInfo info;
                const bool scanned = scan.ListMonsters(session.moduleBase, entities, info, &stop_);
                diagnostics_->Changed("combo_scan", poe::ScanSummary(info, entities.size()));
                if (!scanned) {
                    SetStatus("怪物树扫描暂不可读，等待重扫：" + info.error);
                    Wait(100); continue;
                }
                {
                    for (const auto& e : entities)
                        if (e.kind == poe::EntityKind::Monster && e.hp && e.x >= 0 && e.y >= 0 &&
                            uint32_t(e.x) < world.width && uint32_t(e.y) < world.height)
                            targets.push_back({e.key, e.x, e.y});
                }
                if (!Nearest(world.x, world.y, row.distance, targets, target)) {
                    SetStatus("第 " + std::to_string(sequence.Index() + 1) + " 项：范围内无已确认存活怪，跳过 " + row.name +
                        "；读取失效实体 " + std::to_string(info.unreadable));
                    sequence.Skip(GetTickCount64()); continue;
                }
            }
            poe::SkillSlotsSnapshot skills;
            if (!poe::ReadSkillSlots(session.memory, session.moduleBase, skills, error) || skills.playerEntity != world.entity) {
                SetStatus("技能列表暂不可读，等待刷新"); Wait(100); continue;
            }
            const auto id = Resolve(row, skills);
            if (!id) {
                SetStatus("当前技能槽找不到：" + row.name + "，跳过");
                sequence.Skip(GetTickCount64()); continue;
            }
            if (StopRequested()) break;
            CastRequest request{};
            request.token = NextToken();
            request.expectedEntity = world.entity; request.expectedWorld = world.mapMarker;
            request.skillId = id; request.chargeMs = row.chargeMs; request.x = target.x; request.y = target.y;
            heldToken = request.token;
            CastReply reply{};
            pressAttempted = true; // A missing reply must still be followed by a stop.
            if (!Exchange(*client, request, reply)) {
                // Completion is unknown: don't replay within this run/map ledger.
                if (row.usage == Usage::Once) used_.insert(row.key);
                throw std::runtime_error("技能请求未确认，请确认已注入新版 Monitor；不自动重试");
            }
            diagnostics_->Write("combo_reply", "row=" + std::to_string(sequence.Index() + 1) +
                " key=" + std::to_string(row.key) + " name=" + row.name + " skill_id=" + std::to_string(id) +
                " token=" + std::to_string(request.token) + " target=" + std::to_string(target.id) +
                " xy=" + std::to_string(target.x) + "," + std::to_string(target.y) +
                " result=" + std::to_string(unsigned(reply.result)) + " invoked=" + std::to_string(reply.invoked) +
                " pressed_at=" + std::to_string(reply.pressedAt) + " released_at=" + std::to_string(reply.releasedAt) +
                " detail=" + reply.detail);
            if (reply.invoked) {
                sequence.Issued(reply.pressedAt ? reply.pressedAt : GetTickCount64()); used_ = sequence.Used();
                { std::lock_guard<std::mutex> lock(mutex_); ++status_.casts; }
                LOGI("[Combo] row=%zu key=%llu name=%s id=0x%08X charge=%u interval=%u target=%u (%d,%d)",
                     sequence.Index() + 1, row.key, row.name.c_str(), id, row.chargeMs,
                     row.intervalMs, target.id, target.x, target.y);
                SetStatus("第 " + std::to_string(sequence.Index() + 1) + " 项：" + row.name +
                          "，蓄力 " + std::to_string(row.chargeMs) + "ms");
            }
            if (reply.result != Result::Ok) {
                if (!reply.invoked && (reply.result == Result::UnknownSkill || reply.result == Result::ContextChanged)) {
                    heldToken = 0; sequence.Skip(GetTickCount64()); Wait(50); continue;
                }
                throw std::runtime_error(std::string("技能通道错误：") + reply.detail);
            }
        }
    } catch (const std::exception& e) {
        endText = e.what(); LOGW("[Combo] %s", endText.c_str());
    } catch (...) {
        endText = "连招出现未知异常"; LOGW("[Combo] %s", endText.c_str());
    }
    // Clear the Monitor token first, then explicitly stop the in-game skill even
    // when the token was already released during an interval. No Begin is sent here.
    bool stopped = !pressAttempted;
    try {
        if (!release()) LOGW("[Combo] token release unconfirmed; requesting final skill stop");
    } catch (const std::exception& e) {
        LOGW("[Combo] token release exception: %s; requesting final skill stop", e.what());
    } catch (...) {
        LOGW("[Combo] token release unknown exception; requesting final skill stop");
    }
    try {
        if (pressAttempted && client) {
            SetStatus("正在停止游戏内技能，等待回执…");
            stopped = StopGameSkill(*client);
        }
    } catch (const std::exception& e) {
        LOGW("[Combo] final stop exception: %s", e.what());
    } catch (...) {
        LOGW("[Combo] final stop unknown exception");
    }
    if (!stopped) endText += "；游戏内停止未确认，请检查通信与 Monitor 日志";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.text = endText; status_.stopConfirmed = stopped;
    }
    LOGI("[Combo] finished final_stop_confirmed=%u", stopped ? 1U : 0U);
    diagnostics_->Write("combo_end", "casts=" + std::to_string(Status().casts) +
        " stop_confirmed=" + std::to_string(stopped) + " text=" + endText);
    diagnostics_.reset();
    running_ = false;
}
} // namespace combo
