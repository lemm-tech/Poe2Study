// Isolated policy and transport tests. No game process, DLL loading or input.
#include "../Common/AppConfig.h"
#include "../Common/GameOffsets.h"
#include "../Common/MonitorInterface.h"
#include "../Common/StationaryRecovery.h"
#include "../Monitor/NamedPipeServer.h"
#include "../Plug/NamedPipeClie.h"
#include "../Plug/ComboRunner.h"
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {
int checks=0, failed=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; std::printf("FAIL %d: %s\n",__LINE__,#x); } } while(0)
NamedPipeServer* server=nullptr;
enum class Mode { Good, Reject, WrongToken, ReleaseFailed };
struct Mock {
    std::mutex mutex;
    Mode mode=Mode::Good;
    int presses=0, releases=0, stops=0;
    combo::CastRequest last{};
    uint64_t pressedAt=0;
} mock;
void Handle(MessageType type,const void* body,size_t size,const managed::Route& route) {
    std::lock_guard<std::mutex> lock(mock.mutex);
    if (type==MT_ComboCast && size==sizeof(combo::CastRequest)) {
        combo::CastRequest request{}; std::memcpy(&request,body,size);
        combo::CastReply reply{}; reply.token=request.token;
        if (request.operation==combo::Operation::DodgeRoll) {
            ++mock.presses; mock.last=request; mock.pressedAt=GetTickCount64();
            reply.result=mock.mode==Mode::Reject?combo::Result::UnknownSkill:combo::Result::Ok;
            reply.invoked=mock.mode==Mode::Reject?0:1;
            reply.pressedAt=mock.pressedAt; reply.releasedAt=mock.pressedAt+1;
            if (mock.mode==Mode::WrongToken) ++reply.token;
        } else if (request.operation==combo::Operation::Release) {
            ++mock.releases; reply.pressedAt=mock.pressedAt; reply.releasedAt=mock.pressedAt+1;
            reply.result=mock.mode==Mode::ReleaseFailed?combo::Result::Exception:combo::Result::Ok;
        } else { ++failed; }
        server->Reply(route,MT_ComboCastAck,&reply,sizeof(reply));
    } else if (type==MT_SkillStop) {
        ++mock.stops; SkillStopAck reply{};
        server->Reply(route,MT_SkillStopAck,&reply,sizeof(reply));
    }
}
}
int main() {
    nav::StationaryRecovery timer;
    CHECK(!timer.Due() && !timer.Active());
    timer.Observe(90000,{10,10}); CHECK(!timer.Due());
    timer.Begin(100,{10,10}); timer.Observe(5099,{10,10}); CHECK(!timer.Due());
    timer.Observe(5100,{10,10}); CHECK(timer.Due());
    timer.Reset(); timer.Begin(0,{10,10}); timer.Observe(4999,{11,10});
    timer.Observe(5000,{11,10}); CHECK(!timer.Due());
    timer.Observe(9999,{11,10}); CHECK(timer.Due());
    timer.Reset(); timer.Begin(0,{10,10}); timer.Observe(2000,{10,10});
    timer.Suspend(); CHECK(!timer.Due() && !timer.Active());
    timer.Observe(50000,{10,10}); timer.Begin(50000,{10,10});
    timer.Observe(52999,{10,10}); CHECK(!timer.Due());
    timer.Observe(53000,{10,10}); CHECK(timer.Due());
    // Target changes and short navigation slices do not reset stationary time.
    timer.Reset();
    for (uint64_t i=0;i<4;++i) {
        timer.Begin(i*2000,{10,10}); timer.Observe(i*2000+1500,{10,10}); timer.Suspend();
    }
    timer.Begin(8000,{10,10}); CHECK(timer.Due());
    // An intentional pause/attack/interaction starts a new five-second window.
    timer.Reset(); timer.Begin(100000,{10,10}); timer.Observe(104999,{10,10}); CHECK(!timer.Due());
    timer.Observe(105000,{10,10}); CHECK(timer.Due());
    timer.Reset(); timer.Begin(105000,{10,10}); timer.Observe(109999,{10,10}); CHECK(!timer.Due());
    timer.Observe(110000,{10,10}); CHECK(timer.Due());
    timer.Suspend(); timer.Begin(110001,{10,11}); CHECK(!timer.Due());
    timer.Observe(109000,{10,11}); CHECK(!timer.Due());
    monitorabi::Info info; CHECK(monitorabi::Compatible(info));
    info.capabilities&=~monitorabi::kStationaryDodgeRoll; CHECK(!monitorabi::Compatible(info));
    info = {}; info.capabilities &= ~monitorabi::kDodgeRollNativeFlags;
    CHECK(!(info.capabilities & monitorabi::kDodgeRollNativeFlags) && !monitorabi::Compatible(info));
    CHECK(kDodgeRollSkillId==0x80000040u && sizeof(combo::CastRequest)==48);

    app::Config().pipeName="Poe2RitualRoll_"+std::to_string(GetCurrentProcessId());
    const auto name=app::Config().PipeFullName(); NamedPipeServer instance(name,Handle); server=&instance;
    if (!instance.Initialize() || !pipe::StartManagedConnections(name)) return 2;
    pipe::NamedPipeClie client(name,nullptr); CHECK(client.Initialize());
    std::string error; uint64_t previousToken=0;
    for (auto mode:{Mode::Good,Mode::Reject,Mode::WrongToken,Mode::ReleaseFailed}) {
        { std::lock_guard<std::mutex> lock(mock.mutex); mock.mode=mode; mock.presses=mock.releases=mock.stops=0; }
        const bool ok=combo::DodgeRollOnce(client,123,456,40,25,error);
        CHECK(ok==(mode==Mode::Good));
        std::lock_guard<std::mutex> lock(mock.mutex);
        CHECK(mock.presses==1 && mock.releases==1); // No duplicate roll after malformed/failed replies.
        CHECK(mock.last.skillId==0x80000040u && mock.last.chargeMs==0);
        CHECK(mock.last.expectedEntity==123 && mock.last.expectedWorld==456 && mock.last.x==40 && mock.last.y==25);
        CHECK(mock.last.token && mock.last.token!=previousToken); previousToken=mock.last.token;
        CHECK(mock.stops==(mode==Mode::ReleaseFailed?1:0));
    }
    client.Uninitialize(); pipe::StopManagedConnections(); instance.Uninitialize();
    std::printf("Ritual roll tests: %d checks, %d failures. No game connections or injection.\n",checks,failed);
    return failed?1:0;
}
