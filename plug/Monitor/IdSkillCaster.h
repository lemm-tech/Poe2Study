#pragma once
#include "../Common/ComboProtocol.h"
#include "../Common/ManagedProtocol.h"

namespace navskill {
namespace detail {
// Keep the native argument order testable without entering a game process.
template<class Call, class Position>
void InvokeIdSkill(Call call, uint64_t context, uint32_t id, Position* position, bool dodge) {
    // Native object +0x28 packs flags, flagA, flagB in that byte order.
    // Manual Space uses 00 04 00 FF: 0xFF belongs to argument seven.
    call(context, id, uint32_t(0), position, uint16_t(0x400),
         uint8_t(0), uint8_t(dodge ? 0xFF : 0));
}
} // namespace detail

// All methods run under g_navMutex. Holding a skill never holds that mutex.
class IdSkillCaster {
public:
    combo::CastReply Handle(const combo::CastRequest&, const managed::Route&);
    void Tick(bool ownerConnected);
    void Cancel();
    bool Active() const { return active_; }
    const managed::Route& Owner() const { return owner_; }
private:
    combo::CastReply Release(uint64_t token);
    bool active_ = false;
    managed::Route owner_{};
    uint64_t token_ = 0, context_ = 0, entity_ = 0, world_ = 0, player_ = 0, deadline_ = 0;
    uint64_t pressedAt_ = 0, releasedAt_ = 0;
    uint32_t skillId_ = 0;
    combo::CastReply lastRelease_{};
};
} // namespace navskill
