#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <fstream>
#include "../Common/ComboSequence.h"
#include "../Common/ComboProtocol.h"
#include "../Common/ManagedProtocol.h"
#include "../Common/GameOffsets.h"
#include "../Plug/ComboConfig.h"
#include "../Monitor/IdSkillCaster.h"

int checks = 0, failed = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
using namespace combo;
Row Make(uint64_t key, Usage usage = Usage::EveryLoop, uint32_t charge = 0, uint32_t gap = 0) {
    Row r; r.key = key; r.internalId = "TornadoPlayer"; r.name = "龙卷风";
    r.usage = usage; r.chargeMs = charge; r.intervalMs = gap; return r;
}
int main() {
    using A = Sequence::Action;
    Sequence s;
    s.Reset({Make(1, Usage::Once, 800, 300), Make(2)});
    CHECK(s.Poll(1000) == A::Evaluate && s.Current().key == 1);
    s.Issued(1000);
    CHECK(s.Poll(1799) == A::Wait);
    CHECK(s.Poll(1800) == A::Release);
    s.Released(1800);
    CHECK(s.Poll(2099) == A::Wait);
    CHECK(s.Poll(2100) == A::Evaluate && s.Current().key == 2);
    s.Issued(2100); CHECK(s.Poll(2100) == A::Release); s.Released(2100);
    CHECK(s.Poll(2100) == A::Evaluate && s.Current().key == 2);
    CHECK(s.Used().count(1) && !s.Used().count(2));

    // Pause/restart and reorder preserve per-row single-use state.
    const auto ledger = s.Used();
    s.Reset({Make(2), Make(1, Usage::Once), Make(3, Usage::Once)}, ledger);
    CHECK(s.Poll(0) == A::Evaluate && s.Current().key == 2);
    s.Skip(0); CHECK(s.Poll(0) == A::Evaluate && s.Current().key == 3);
    s.Issued(0); CHECK(s.Used().count(3));
    s.Reset({Make(1, Usage::Once)}); CHECK(s.Poll(0) == A::Evaluate); // New map.

    // No enemy: skip without charge/gap or consuming a single-use row.
    s.Reset({Make(1, Usage::Once, 5000, 9000), Make(2, Usage::Once, 2000, 8000)});
    CHECK(s.Poll(10) == A::Evaluate); s.Skip(10);
    CHECK(s.Poll(10) == A::Evaluate && s.Current().key == 2);
    CHECK(s.Used().empty()); s.Skip(10);
    CHECK(s.Poll(109) == A::Wait);
    CHECK(s.Poll(110) == A::Evaluate && s.Current().key == 1);
    s.Reset({Make(1, Usage::Once)}, {1}); CHECK(s.Poll(0) == A::Wait);
    CHECK(s.Poll(99) == A::Wait); CHECK(s.Poll(100) == A::Wait);
    s.Reset({}); CHECK(s.Poll(0) == A::Wait);

    // Transport delay is not added to holding or the post-release interval.
    s.Reset({Make(21, Usage::EveryLoop, 800, 300), Make(22)});
    s.Issued(1000); // Backend press time; response arrives later.
    CHECK(s.Poll(1900) == A::Release);
    s.Released(1800); // Backend already released at its own deadline.
    CHECK(s.Poll(2099) == A::Wait);
    CHECK(s.Poll(2100) == A::Evaluate && s.Current().key == 22);

    // Same skill on two rows has independent usage bookkeeping.
    s.Reset({Make(11, Usage::Once), Make(12, Usage::Once)});
    s.Issued(0); s.Released(0);
    CHECK(s.Poll(0) == A::Evaluate && s.Current().key == 12);
    CHECK(s.Used().count(11) && !s.Used().count(12));

    Target target;
    CHECK(Nearest(100, 100, 10, {{3, 110, 100}, {2, 104, 103}, {1, 105, 100}}, target));
    CHECK(target.id == 1 && target.x == 105); // Stable ID tie-break.
    CHECK(!Nearest(0, 0, 9, {{1, 10, 0}}, target));
    CHECK(Nearest(0, 0, 10, {{1, 10, 0}}, target));
    CHECK(Nearest(0, 0, 0, {{1, 0, 0}}, target));

    CHECK(managed::Allowed(managed::Role::Controller, MT_ComboCast));
    CHECK(!managed::Allowed(managed::Role::Data, MT_ComboCast));
    CHECK(!managed::Allowed(managed::Role::Tracker, MT_ComboCast));
    CastRequest request; request.token = 123; request.skillId = 0x85070041;
    CastMessage message(request);
    CHECK(message.m_len == sizeof(request) && message.m_MsgType == MT_ComboCast);
    CastRequest decoded; std::memcpy(&decoded, message.body, sizeof(decoded));
    CHECK(decoded.skillId == 0x85070041 && decoded.token == 123);
    CastReply timing; timing.token = request.token; timing.pressedAt = 123456; timing.releasedAt = 123956;
    std::vector<uint8_t> wire(sizeof(timing)); std::memcpy(wire.data(), &timing, sizeof(timing));
    CastReply received; std::memcpy(&received, wire.data(), wire.size());
    CHECK(received.token == request.token && received.releasedAt - received.pressedAt == 500);

    // Native ABI regression: sixteen manual Space captures contained action
    // bytes 00 04 00 FF at +0x28 (docs/evidence/dodge-roll-20261119.json).
    // Capture the actual seven arguments used at the production call boundary.
    struct Point { int32_t x, y; } point{1199, 790};
    for (bool dodge : {false, true}) {
        uint32_t packedFlags = 0;
        navskill::detail::InvokeIdSkill([&](uint64_t context, uint32_t id, uint32_t target,
                                           Point* pos, uint16_t flags, uint8_t flagA, uint8_t flagB) {
            CHECK(context == 123 && id == kDodgeRollSkillId && target == 0 && pos == &point);
            packedFlags = uint32_t(flags) | (uint32_t(flagA) << 16) | (uint32_t(flagB) << 24);
        }, 123, kDodgeRollSkillId, &point, dodge);
        CHECK(packedFlags == (dodge ? 0xFF000400u : 0x00000400u));
    }

    // Backend rejection paths that never resolve a game or make any game call.
    navskill::IdSkillCaster caster;
    managed::Route route{1, 2, 3};
    request.version = 999;
    CHECK(caster.Handle(request, route).result == Result::BadRequest);
    request.version = kWireVersion; request.operation = Operation::Release;
    CHECK(caster.Handle(request, route).result == Result::Ok && !caster.Active());
    request.operation = Operation::Begin; request.chargeMs = kMaxTimeMs + 1;
    CHECK(caster.Handle(request, route).result == Result::BadRequest);
    request.operation = Operation::DodgeRoll; request.chargeMs = 0;
    request.skillId = 0x85070041;
    CHECK(caster.Handle(request, route).result == Result::BadRequest);
    request.skillId = kDodgeRollSkillId; request.chargeMs = 1;
    CHECK(caster.Handle(request, route).result == Result::BadRequest);
    request.chargeMs = 0;
    auto roll = caster.Handle(request, route);
    CHECK(roll.result == Result::UnsupportedBuild && !roll.invoked && !caster.Active());

    wchar_t folder[MAX_PATH]{}; GetTempPathW(MAX_PATH, folder);
    const auto path = std::wstring(folder) + L"poe2-combo-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini";
    std::vector<Row> original{Make(1, Usage::Once, 0, 300), Make(2, Usage::EveryLoop, 800, 0)};
    original[1].environment = Environment::Always;
    std::string error; std::vector<Row> loaded;
    CHECK(SaveConfig(path, original, error)); CHECK(LoadConfig(path, loaded, error));
    CHECK(loaded.size() == 2 && loaded[0].name == "龙卷风" && loaded[1].chargeMs == 800);
    CHECK(loaded[0].usage == Usage::Once && loaded[1].environment == Environment::Always);
    auto bad = original; bad[1].key = 1;
    CHECK(!SaveConfig(path, bad, error)); CHECK(LoadConfig(path, loaded, error) && loaded[1].key == 2);
    bad = original; bad[0].intervalMs = kMaxTimeMs + 1;
    CHECK(!SaveConfig(path, bad, error));
    WritePrivateProfileStringW(L"skill1", L"chargeMs", L"-1", path.c_str());
    CHECK(!LoadConfig(path, loaded, error));
    CHECK(loaded.size() == 2 && loaded[1].chargeMs == 800); // Failed load preserves caller state.
    DeleteFileW(path.c_str());
    std::printf("Combo tests: %d checks, %d failures. No game connections or injection.\n", checks, failed);
    return failed ? 1 : 0;
}
