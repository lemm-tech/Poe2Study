// End-to-end navigation policy tests on an isolated pipe and synthetic world.
// No DLL loading, injection, packets, or live-game memory access.
#include "../Common/AppConfig.h"
#include "../Monitor/NamedPipeServer.h"
#include "../Plug/Navigator.h"
#include "../Plug/NamedPipeClie.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>
namespace
{
int passed=0,failed=0;
#define CHECK(x) do { bool ok=!!(x); printf("[%s] %s\n",ok?"PASS":"FAIL",#x); ok?++passed:++failed; } while(0)
enum class Mode { Normal, Delay, Freeze, Wall, DropPosition, DropAck, BadAck, Deviation, BlackTwice };
struct World
{
    std::mutex mutex;
    Mode mode=Mode::Normal;
    double x=10,y=10,tx=10,ty=10;
    uint64_t tick=0,issued=0;
    int walks=0,drops=0,stops=0;
    bool displaced=false,unsafe=false;
    std::vector<nav::PixelPoint> targets;
    std::vector<uint64_t> callTimes;
    void Reset(Mode m)
    {
        std::lock_guard<std::mutex> lock(mutex);
        mode=m; x=tx=10; y=ty=10; tick=GetTickCount64(); issued=tick;
        walks=drops=stops=0; displaced=unsafe=false; targets.clear();callTimes.clear();
    }
    bool WallAt(int px,int py) const {return mode==Mode::Wall && px>=24&&px<=26&&py>=9&&py<=11;}
    void Advance()
    {
        uint64_t now=GetTickCount64();
        uint64_t from=tick;
        if(mode==Mode::Delay) from=(std::max)(from,issued+250);
        tick=now;
        if(mode==Mode::Freeze || from>=now) return;
        double dx=tx-x,dy=ty-y,d=std::hypot(dx,dy);
        if(d<1e-9)return;
        double move=(std::min)(d,(now-from)*0.3);
        double nx=x+dx*move/d,ny=y+dy*move/d;
        int samples=(std::max)(1,int(std::ceil(move*2)));
        double oldx=x,oldy=y;
        for(int i=1;i<=samples;++i)
        {
            double xx=oldx+(nx-oldx)*i/samples, yy=oldy+(ny-oldy)*i/samples;
            if(WallAt(int(std::lround(xx)),int(std::lround(yy))))break;
            x=xx;y=yy;
        }
    }
} world;
NamedPipeServer *server=nullptr;
void Handle(MessageType type,const void *body,size_t size,const managed::Route &route)
{
    if(type==MT_NavStop)
    {std::lock_guard<std::mutex> lock(world.mutex);++world.stops;}
    else if(type==MT_PositionReq)
    {
        NavStatus p{};bool drop=false;
        {
            std::lock_guard<std::mutex> lock(world.mutex);
            world.Advance();
            if(world.mode==Mode::DropPosition && world.walks==1 && world.drops==0)
            {++world.drops;drop=true;}
            if(!world.displaced && ((world.mode==Mode::Deviation&&world.walks==1) ||
                                     (world.mode==Mode::BlackTwice&&world.walks==2)))
            {
                world.displaced=true;
                world.x=world.tx=world.mode==Mode::BlackTwice?30:15;
                world.y=world.ty=world.mode==Mode::BlackTwice?15:35;
            }
            p.x=int32_t(std::lround(world.x));p.y=int32_t(std::lround(world.y));
        }
        if(drop)Sleep(2200); // Exceed the real 2s request deadline; late completion must be ignored.
        else server->Reply(route,MT_PositionReply,&p,sizeof(p));
    }
    else if(type==MT_WalkCall && size==sizeof(WalkCallReq))
    {
        WalkCallReq req{};memcpy(&req,body,size);WalkCallAck ack{};bool drop=false;
        {
            std::lock_guard<std::mutex> lock(world.mutex);
            world.Advance();world.tx=req.x;world.ty=req.y;
            world.issued=world.tick=GetTickCount64();++world.walks;
            world.targets.push_back({req.x,req.y});world.callTimes.push_back(world.issued);
            ack.targetX=req.x;ack.targetY=req.y;
            drop=world.mode==Mode::DropAck;
            if(world.mode==Mode::BadAck)++ack.targetX;
        }
        if(drop)Sleep(3200); // Real 3s acknowledgement timeout: never replay the executed call.
        else server->Reply(route,MT_WalkCallAck,&ack,sizeof(ack));
    }
}
mapdata::CostBuffer OpenMap()
{
    mapdata::CostBuffer map(mapdata::Geometry::FromGrid(3,3));
    map.Raw().assign(map.Size(),1);return map;
}
usecase::NavigateOptions Options(const mapdata::CostBuffer &map)
{
    usecase::NavigateOptions o;o.goalX=50;o.goalY=10;o.presetCost=&map;
    o.stepDistance=30;o.arriveTolerance=2;o.waypointTolerance=1;
    o.pollMs=20;o.minCallMs=100;o.startupMs=120;o.stepWaitMs=1000;
    o.replanMs=150;o.walkTimeoutSec=15;o.stallSec=10;return o;
}
int Run(Mode mode,const mapdata::CostBuffer &map,usecase::NavTelemetry &t,
        const usecase::NavigateOptions *overrideOptions=nullptr)
{
    world.Reset(mode);auto o=overrideOptions?*overrideOptions:Options(map);
    if(mode==Mode::Delay)o.startupMs=350;
    if(mode==Mode::Freeze)o.recoveryLimit=2;
    int rc=usecase::Navigate(o,&t);
    printf("CASE mode=%u rc=%d arrived=%d calls=%d plans=%u note=%s\n",unsigned(mode),rc,t.arrived,world.walks,t.replans,t.note.c_str());
    return rc;
}
}
int main()
{
    app::Config().pipeName="Poe2NavigationFeedback_"+std::to_string(GetCurrentProcessId());
    auto name=app::Config().PipeFullName();
    NamedPipeServer instance(name,Handle);server=&instance;
    if(!instance.Initialize() || !pipe::StartManagedConnections(name))return 2;
    auto map=OpenMap();usecase::NavTelemetry t;
    CHECK(Run(Mode::Normal,map,t)==0 && t.arrived && t.replans==1);
    CHECK(world.walks>=2);
    bool cadence=true;
    for(size_t i=1;i<world.callTimes.size();++i)cadence=cadence&&world.callTimes[i]-world.callTimes[i-1]>=100;
    CHECK(cadence);
    CHECK(Run(Mode::Delay,map,t)==0 && t.replans==1);
    uint64_t timeoutAt=GetTickCount64();
    CHECK(Run(Mode::DropPosition,map,t)==0 && world.drops==1 && t.arrived);
    CHECK(GetTickCount64()-timeoutAt>=2000);
    CHECK(Run(Mode::Deviation,map,t)==0 && t.replans>=2);
    // An unadvertised obstacle must produce feedback/replanning, not endless identical calls.
    auto before=map.Raw();
    CHECK(Run(Mode::Wall,map,t)==4 && !t.arrived && t.replans>=2 && world.walks<=40);
    CHECK(map.Raw()==before);
    CHECK(Run(Mode::Freeze,map,t)==4 && !t.arrived && world.walks<=12);
    CHECK(map.Raw()==before);
    timeoutAt=GetTickCount64();
    CHECK(Run(Mode::DropAck,map,t)==4 && world.walks==1);
    CHECK(GetTickCount64()-timeoutAt>=2900);
    CHECK(Run(Mode::BadAck,map,t)==4 && world.walks==1);
    auto black=OpenMap();black.Set(10,10,0);black.Set(30,15,0);
    // pollMs=20 < minCallMs=100: cadence-only samples cannot exhaust recovery.
    CHECK(Run(Mode::BlackTwice,black,t)==0 && t.arrived);
    CHECK(world.displaced && world.walks<=6);
    CHECK(black.At(10,10)==0 && black.At(30,15)==0);
    // U-shaped, one-pixel corridor: commands must preserve the two corners.
    auto bend=OpenMap();bend.Raw().assign(bend.Size(),0);
    for(int yy=10;yy<=50;++yy){bend.Set(10,yy,1);bend.Set(50,yy,1);}
    for(int xx=10;xx<=50;++xx)bend.Set(xx,50,1);
    auto bendOptions=Options(bend);
    // This fixture models the real WalkCall's short-range steering: it can turn
    // between two passable A* targets even when their direct chord crosses black.
    bendOptions.deviateDistance=1000;
    CHECK(Run(Mode::Normal,bend,t,&bendOptions)==0 && t.arrived && t.replans>=1);
    nav::PixelMap pixels;pixels.BuildFrom(bend);bool safe=true;
    for(auto target:world.targets)
        safe=safe&&pixels.Passable(target.x,target.y)&&nav::PixelDistance({10,10},target)<=1000;
    CHECK(safe && world.targets.size()<=5);
    // Euclidean proximity across a wall is NOT arrival.
    auto nearWall=OpenMap();nearWall.Raw().assign(nearWall.Size(),0);
    for(int yy=10;yy<=30;++yy){nearWall.Set(10,yy,1);nearWall.Set(20,yy,1);}
    for(int xx=10;xx<=20;++xx)nearWall.Set(xx,30,1);
    world.Reset(Mode::Normal);auto nearOptions=Options(nearWall);
    nearOptions.goalX=20;nearOptions.arriveTolerance=13;nearOptions.deviateDistance=1000;
    CHECK(usecase::Navigate(nearOptions,&t)==0 && world.walks>0 && t.replans>=1 &&
          (world.targets.front().x!=20 || world.targets.front().y!=10));
    // Cancellation and dry-run cannot issue additional movement commands.
    world.Reset(Mode::Freeze);auto options=Options(map);
    std::atomic_bool stop{false};
    std::atomic<usecase::NavStopReason> reason{usecase::NavStopReason::User};
    auto job=std::async(std::launch::async,[&]{return usecase::Navigate(options,&t,&stop,&reason);});
    Sleep(180);stop=true;
    CHECK(job.get()==3 && t.note=="cancelled by user");
    int count=world.walks;Sleep(150);CHECK(world.walks==count);
    world.Reset(Mode::Normal);options.dryRun=true;
    CHECK(usecase::Navigate(options,&t)==0 && world.walks==0 && world.stops==0 && !t.arrived);
    // Ritual opt-in: exact coordinates stay frozen across many short WalkCalls.
    // One recovery stops navigation first, uses the current steering direction,
    // then discards the old route and continues from a fresh position.
    world.Reset(Mode::Freeze); options=Options(map);
    options.recoveryLimit=100; options.stallLimit=100;
    usecase::StationaryRecovery stationary;
    int rolls=0; bool direction=false, stoppedFirst=false;
    stationary.perform=[&](pipe::NamedPipeClie&,nav::PixelPoint from,nav::PixelPoint toward) {
        ++rolls; direction=toward.x>from.x && toward.y==from.y;
        std::lock_guard<std::mutex> lock(world.mutex);
        stoppedFirst=world.stops>=2; world.mode=Mode::Normal; world.tick=GetTickCount64();
        return usecase::NavControl{};
    };
    options.stationaryRecovery=&stationary; timeoutAt=GetTickCount64();
    CHECK(usecase::Navigate(options,&t)==0 && t.arrived && rolls==1);
    CHECK(GetTickCount64()-timeoutAt>=5000 && direction && stoppedFirst && t.replans>=2);
    // Monster chasing retargets every 1.5s. Those Navigate calls must share the
    // same stationary clock instead of forever restarting below five seconds.
    world.Reset(Mode::Freeze); stationary.timer.Reset(); rolls=0; int legs=0;
    timeoutAt=GetTickCount64();
    for (;legs<8 && !rolls;++legs) {
        const auto legStarted=GetTickCount64();
        options.control=[&](nav::PixelPoint) {
            return !rolls && GetTickCount64()-legStarted>=1500
                ?usecase::NavControl{usecase::NavDirective::Arrived,"retarget"}:usecase::NavControl{};
        };
        if (usecase::Navigate(options,&t)) break;
    }
    CHECK(rolls==1 && legs>=4 && GetTickCount64()-timeoutAt>=5000);
    CHECK(t.arrived && world.x>=48);
    options.control={};
    // Dry-run never arms or executes the opt-in callback.
    world.Reset(Mode::Normal); options.dryRun=true; rolls=0;
    CHECK(usecase::Navigate(options,&t)==0 && rolls==0 && !stationary.timer.Active());
    pipe::StopManagedConnections();instance.Uninitialize();
    printf("RESULT passed=%d failed=%d\n",passed,failed);return failed?1:0;
}
