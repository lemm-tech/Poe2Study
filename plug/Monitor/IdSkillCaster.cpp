#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "IdSkillCaster.h"
#include "../Common/GameBuild.h"
#include "../Common/ComboSequence.h"

namespace {
template<class T> T Read(uint64_t address) {
    T v{}; SIZE_T done = 0;
    if (!IsPlausibleAddress(address) || !ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void*>(address), &v, sizeof(v), &done) || done != sizeof(v))
        throw std::runtime_error("Unreadable game state");
    return v;
}
uint64_t Pointer(uint64_t address) {
    auto v = Read<uint64_t>(address);
    if (!IsPlausibleAddress(v)) throw std::runtime_error("Game world unavailable");
    return v;
}
template<size_t N> bool Signature(uint64_t address, const uint8_t (&expected)[N]) {
    uint8_t value[N]{}; SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), value, N, &done) &&
        done == N && !std::memcmp(value, expected, N);
}
struct State { uint64_t context, actor, player, entity, world; };
State Current(uint64_t base) {
    State s{}; s.context = Pointer(base + BaseOffset);
    for (auto off : kPacketContextChain) s.context = Pointer(s.context + off);
    s.actor = Pointer(s.context + kIdSkillContextActorOff); s.player = Pointer(s.context + kSkillStopObj188);
    s.entity = Pointer(s.actor + skilllayout::kComponentOwnerEntity);
    s.world = Pointer(s.entity + kMapObjectChain[kLocalPlayerChainDepth]);
    if (Pointer(s.player + skilllayout::kComponentOwnerEntity) != s.entity ||
        Pointer(Pointer(s.context + kPacketProtocolOff) + kPacketWorldOff) != s.world)
        throw std::runtime_error("Player/context ownership mismatch");
    return s;
}
template<size_t N> bool TextIs(uint64_t address, const wchar_t (&text)[N]) {
    for (size_t i = 0; i < N; ++i)
        if (Read<wchar_t>(address + i * sizeof(wchar_t)) != text[i]) return false;
    return true;
}
bool HasSkill(const State& s, uint64_t base, uint32_t wanted, bool dodge) {
    if (Pointer(Pointer(s.actor) + kSkillManagerGetterVtblOff) != base + kSkillManagerGetterRva ||
        !Signature(base + kSkillManagerGetterRva, kSkillManagerGetterSignature))
        throw std::runtime_error("Skill manager fingerprint mismatch");
    const auto manager = s.actor + kSkillManagerInActorOff;
    if (Pointer(manager + skilllayout::kOwnerEntity) != s.entity) throw std::runtime_error("Skill owner mismatch");
    const auto begin = Pointer(manager + kSkillManagerVectorOff), end = Read<uint64_t>(manager + kSkillManagerVectorOff + 8);
    const auto capacity = Read<uint64_t>(manager + kSkillManagerVectorOff + 16);
    if (end <= begin || end - begin > 512 * 16 || (end - begin) % 16 || capacity < end ||
        capacity - begin > 512 * 16 || (capacity - begin) % 16)
        throw std::runtime_error("Invalid skill manager vector");
    bool found = false; uint32_t previous = 0;
    for (auto entry = begin; entry < end; entry += 16) {
        const auto ptr = Pointer(entry); const auto id = Read<uint32_t>(ptr + skilllayout::kRuntimeSkillId);
        if (id <= previous || Pointer(ptr + skilllayout::kSkillOwner) != manager)
            throw std::runtime_error("Skill manager changed during read");
        previous = id;
        if (id == wanted) {
            found = true;
            if (dodge) {
                const auto row = Pointer(ptr + skilllayout::kBaseSkillRecord);
                const auto effect = Pointer(Pointer(ptr + skilllayout::kLevelRecord) + skilllayout::kGrantedEffectRecord);
                found = TextIs(Pointer(row + skilllayout::kBaseSkillIdText), L"DodgeRoll") &&
                    TextIs(Pointer(effect + skilllayout::kGrantedEffectIdText), L"DodgeRollPlayer");
            }
        }
    }
    if (Read<uint64_t>(manager + kSkillManagerVectorOff) != begin ||
        Read<uint64_t>(manager + kSkillManagerVectorOff + 8) != end)
        throw std::runtime_error("Skill manager changed during read");
    return found;
}
struct Position { int32_t x, y; };
bool Cast(uint64_t base, uint64_t context, uint32_t id, int32_t x, int32_t y, bool dodge, DWORD& exception) {
    Position pos{x, y};
    using Fn = void (*)(uint64_t, uint32_t, uint32_t, Position*, uint16_t, uint8_t, uint8_t);
    __try { navskill::detail::InvokeIdSkill(reinterpret_cast<Fn>(base + kIdSkillCallRva), context, id, &pos, dodge); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { exception = GetExceptionCode(); return false; }
}
bool Stop(uint64_t base, uint64_t context, DWORD& exception) {
    __try { reinterpret_cast<void (*)(uint64_t)>(base + kSkillStopRva)(context); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { exception = GetExceptionCode(); return false; }
}
void Detail(combo::CastReply& reply, const char* text) {
    std::snprintf(reply.detail, sizeof(reply.detail), "%s", text);
}
} // namespace

namespace navskill {
combo::CastReply IdSkillCaster::Handle(const combo::CastRequest& r, const managed::Route& route) {
    combo::CastReply reply{}; reply.token = r.token;
    if (r.version != combo::kWireVersion || !r.token) { Detail(reply, "Invalid combo request"); return reply; }
    if (r.operation == combo::Operation::Release) {
        if (!active_ || !managed::Same(owner_, route) || token_ != r.token) {
            reply.result = combo::Result::Ok;
            if (managed::Same(owner_, route) && token_ == r.token) {
                return lastRelease_;
            }
            Detail(reply, "No matching held skill"); return reply;
        }
        return Release(r.token);
    }
    const bool dodge = r.operation == combo::Operation::DodgeRoll;
    if ((r.operation != combo::Operation::Begin && !dodge) || !r.skillId || r.chargeMs > combo::kMaxTimeMs ||
        (dodge && (r.skillId != kDodgeRollSkillId || r.chargeMs))) {
        Detail(reply, "Invalid combo press parameters"); return reply;
    }
    if (active_) { reply.result = combo::Result::Busy; Detail(reply, "Previous press not released"); return reply; }
    if (token_ == r.token && managed::Same(owner_, route)) {
        reply.result = combo::Result::BadRequest; Detail(reply, "Duplicate press token; no replay"); return reply;
    }
    try {
        const auto base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        std::string error;
        if (!gamebuild::VerifyLocal(base, error) || !Signature(base + kIdSkillCallRva, kIdSkillCallSignature)) {
            reply.result = combo::Result::UnsupportedBuild; Detail(reply, error.empty() ? "ID call changed" : error.c_str());
            return reply;
        }
        const auto s = Current(base);
        if (s.entity != r.expectedEntity || s.world != r.expectedWorld) {
            reply.result = combo::Result::ContextChanged; Detail(reply, "Map/player changed before press"); return reply;
        }
        const auto map = s.world + kMapObjectChain[kMapObjectChainDepth - 1];
        const auto w = Read<uint32_t>(map + MAP_OFF_GRID_W), h = Read<uint32_t>(map + MAP_OFF_GRID_H);
        if (!w || !h || w > kMaxGridDimension || h > kMaxGridDimension || r.x < 0 || r.y < 0 ||
            uint32_t(r.x) >= w * CELL_SIZE || uint32_t(r.y) >= h * CELL_SIZE) {
            Detail(reply, "Invalid target/map coordinates"); return reply;
        }
        if (!HasSkill(s, base, r.skillId, dodge)) {
            reply.result = combo::Result::UnknownSkill; Detail(reply, "Runtime ID is no longer owned by player"); return reply;
        }
        owner_ = route; token_ = r.token; context_ = s.context; entity_ = s.entity;
        world_ = s.world; player_ = s.player; skillId_ = r.skillId;
        pressedAt_ = GetTickCount64(); releasedAt_ = 0;
        lastRelease_ = {};
        deadline_ = pressedAt_ + r.chargeMs; active_ = true;
        DWORD exception = 0; reply.invoked = 1;
        const bool ok = Cast(base, context_, skillId_, r.x, r.y, dodge, exception);
        reply.pressedAt = pressedAt_;
        reply.exceptionCode = exception; reply.result = ok ? combo::Result::Ok : combo::Result::Exception;
        Detail(reply, ok ? "ID call issued; gameplay availability not checked" : "ID call raised SEH");
        if (!r.chargeMs || !ok) {
            const auto stopped = Release(r.token);
            reply.releasedAt = stopped.releasedAt;
            if (ok && stopped.result != combo::Result::Ok) {
                reply.result = stopped.result; reply.exceptionCode = stopped.exceptionCode;
                Detail(reply, stopped.detail);
            }
        }
    } catch (const std::exception& e) {
        reply.result = combo::Result::ContextChanged; Detail(reply, e.what());
    }
    return reply;
}
combo::CastReply IdSkillCaster::Release(uint64_t token) {
    combo::CastReply reply{}; reply.token = token; reply.result = combo::Result::Ok;
    reply.pressedAt = pressedAt_;
    if (!active_) return lastRelease_;
    auto finish = [&] { lastRelease_ = reply; return reply; };
    active_ = false;
    releasedAt_ = GetTickCount64(); reply.releasedAt = releasedAt_;
    try {
        const auto base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        const auto s = Current(base);
        if (s.context != context_ || s.entity != entity_ || s.world != world_ || s.player != player_) {
            Detail(reply, "Old map/player discarded; no call on stale context"); return finish();
        }
        // In server action mode (gate == 2), Cast sends 0x1A5 but skips the
        // Player+0x214 update. That field can be zero or belong to an older
        // action even immediately after our cast. Skipping Stop in that case
        // leaves the server repeating the skill until its charges run out.
        // Only the local action mode can use this field to protect a newer move.
        const auto gate = Read<uint8_t>(Pointer(context_ + kSkillStopObj1A0) + kSkillStopGate669);
        if (gate != 2 && Read<uint32_t>(player_ + kIdSkillCurrentIdOff) != skillId_) {
            Detail(reply, "Skill action already replaced; release complete"); return finish();
        }
        DWORD exception = 0; reply.invoked = 1;
        if (!Stop(base, context_, exception)) {
            reply.result = combo::Result::Exception; reply.exceptionCode = exception;
            Detail(reply, "Release call raised SEH");
        } else Detail(reply, gate == 2 ? "Skill stop sent; server action mode" : "Skill released");
        releasedAt_ = GetTickCount64(); reply.releasedAt = releasedAt_;
    } catch (const std::exception& e) {
        reply.result = combo::Result::ContextChanged; Detail(reply, e.what());
    }
    return finish();
}
void IdSkillCaster::Tick(bool ownerConnected) {
    if (active_ && (!ownerConnected || GetTickCount64() >= deadline_)) Release(token_);
}
void IdSkillCaster::Cancel() { if (active_) Release(token_); }
} // namespace navskill
