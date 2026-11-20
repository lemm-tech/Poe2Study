#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <algorithm>

#include "../Common/MapGeometry.h"
#include "../Common/PixelNavigation.h"
#include "../Common/RouteNavigation.h"


#include "../Common/MapFrame.h"
#include "../Common/Protocal.h"
#include "../Common/PollSource.h"
#include "../Common/GameBuild.h"

// ------------------------------------------------------------
// 极简测试框架
// ------------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;
static bool g_verbose = false;
static const char* g_suite = "";

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            ++g_pass;                                                        \
            if (g_verbose) {                                                 \
                std::printf("    [OK]   %s\n", #cond);                       \
            }                                                                \
        }                                                                    \
        else {                                                               \
            ++g_fail;                                                        \
            std::printf("    [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);\
            std::printf("           ");                                      \
            std::printf(__VA_ARGS__);                                        \
            std::printf("\n");                                               \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b, ...)                                                  \
    do {                                                                     \
        const auto _a = (a);                                                 \
        const auto _b = (b);                                                 \
        if (_a == _b) {                                                      \
            ++g_pass;                                                        \
            if (g_verbose) {                                                 \
                std::printf("    [OK]   %s == %s\n", #a, #b);                \
            }                                                                \
        }                                                                    \
        else {                                                               \
            ++g_fail;                                                        \
            std::printf("    [FAIL] %s:%d  %s == %s  (左=%lld 右=%lld)\n",   \
                        __FILE__, __LINE__, #a, #b,                          \
                        (long long)_a, (long long)_b);                       \
        }                                                                    \
    } while (0)

static void Suite(const char* name)
{
    g_suite = name;
    std::printf("\n== %s ==\n", name);
}

// ------------------------------------------------------------
// 1. Geometry
// ------------------------------------------------------------
static void TestGeometry()
{
    Suite("Geometry：网格/像素/nibble 换算");

    // 72x72 网格 -> 1656x1656 世界像素（CELL_SIZE = 23）
    auto g = mapdata::Geometry::FromGrid(72, 72);
    CHECK_EQ(g.WorldW(), 72 * 23, "72 格宽 = 1656 像素");
    CHECK_EQ(g.WorldH(), 72 * 23, "72 格高 = 1656 像素");
    CHECK_EQ(CELL_SIZE, 23, "CELL_SIZE 必须是 23");

    // 行跨度 = ceil(世界宽 / 2)，nibble 打包每字节 2 点
    CHECK_EQ(g.RowStride(), 828, "1656/2 = 828");
    CHECK_EQ(g.PackedBytes(), (uint64_t)828 * 1656, "打包字节数 = 行跨度 x 高");

    // 奇数宽度要向上取整
    auto odd = mapdata::Geometry::FromGrid(1, 1);
    CHECK_EQ(odd.WorldW(), 23, "1 格 = 23 像素");
    CHECK_EQ(odd.RowStride(), 12, "23 像素 -> 12 字节（向上取整）");

    // 合理性校验
    CHECK(g.Plausible(), "72x72 合理");
    CHECK(!mapdata::Geometry::FromGrid(0, 0).Plausible(), "0x0 不合理");
    CHECK(!mapdata::Geometry::FromGrid(99999, 8).Plausible(), "超大网格不合理");

    // 边界
    CHECK(g.Contains(0, 0), "左上角在内");
    CHECK(g.Contains(1655, 1655), "右下角在内");
    CHECK(!g.Contains(-1, 0), "负数在外");
    CHECK(!g.Contains(1656, 0), "右边界外");

    // 打包索引：idx = y * RowStride + x/2
    CHECK_EQ(g.PackedIndex(0, 0), 0ull, "原点索引 0");
    CHECK_EQ(g.PackedIndex(2, 0), 1ull, "x=2 -> 第 1 字节");
    CHECK_EQ(g.PackedIndex(0, 1), 828ull, "y=1 -> 偏移一行");
}

// ------------------------------------------------------------
// 2. nibble 编解码
// ------------------------------------------------------------
static void TestNibble()
{
    Suite("MapFrame：nibble 打包/解包");

    // 打包规则（MapFrame.h / GameOffsets.h）：
    //   x 偶数 -> 高 nibble，x 奇数 -> 低 nibble
    //   值 0xFF（kCostSpecial）用 nibble 5 表示，其它值直接用低 4 位
    CHECK_EQ((int)kCostSpecial, 0xFF, "特殊档位是 0xFF");
    CHECK_EQ((int)kCostSpecialNibble, 5, "0xFF 用 nibble 5 表示");

    // 单点换算往返
    CHECK_EQ((int)CostToNibble(0xFF), 5, "0xFF -> nibble 5");
    CHECK_EQ((int)NibbleToCost(5), 0xFF, "nibble 5 -> 0xFF");
    CHECK_EQ((int)CostToNibble(0), 0, "0 -> nibble 0");
    CHECK_EQ((int)CostToNibble(3), 3, "3 -> nibble 3");

    // 完整打包：4 像素一行
    auto geo = mapdata::Geometry::FromGrid(1, 1);   // 23x23 像素
    mapdata::CostBuffer buf;
    buf.Resize(geo);
    for (uint32_t y = 0; y < geo.WorldH(); ++y) {
        for (uint32_t x = 0; x < geo.WorldW(); ++x) {
            buf.At(x, y) = 0xFF;   // 默认全可走
        }
    }
    // 第一行前 4 个像素设成 0/1/2/3
    buf.At(0, 0) = 0;
    buf.At(1, 0) = 1;
    buf.At(2, 0) = 2;
    buf.At(3, 0) = 3;

    const mapdata::MapFrame frame = mapdata::ToFrame(buf, 0);

    // MapFrame 的打包字节是私有的，通过 SliceRow 拿第一行前 2 字节
    const mapdata::MapFrame::RowSlice s0 = frame.SliceRow(0, 0, 2);

    // 字节 0 = 像素 (0,0) 高 nibble + (1,0) 低 nibble
    //   0 -> 高 0，1 -> 低 1  =>  0x01
    CHECK_EQ((int)s0.data[0], 0x01, "x=0/1 打包到第 0 字节 = 0x01");

    // 字节 1 = 像素 (2,0) 和 (3,0)
    //   2 -> 高 2，3 -> 低 3  =>  0x23
    CHECK_EQ((int)s0.data[1], 0x23, "x=2/3 打包到第 1 字节 = 0x23");

    // 解包回来必须和原值一致
    mapdata::CostBuffer back;
    CHECK(frame.ToCost(back), "ToCost 成功");
    CHECK_EQ((int)back.At(0, 0), 0, "解包 (0,0) = 0");
    CHECK_EQ((int)back.At(1, 0), 1, "解包 (1,0) = 1");
    CHECK_EQ((int)back.At(2, 0), 2, "解包 (2,0) = 2");
    CHECK_EQ((int)back.At(3, 0), 3, "解包 (3,0) = 3");
    CHECK_EQ((int)back.At(4, 0), 0xFF, "解包 (4,0) = 0xFF（未改动的默认值）");
}

// ------------------------------------------------------------
// White-node A*, checked against a separate dense Dijkstra oracle on tiny fixtures.
static double Oracle(const mapdata::CostBuffer& c,nav::PixelPoint s,nav::PixelPoint g){
    if(!c.Get(s.x,s.y)||!c.Get(g.x,g.y))return -1;
    int w=(int)c.Geo().WorldW(),h=(int)c.Geo().WorldH();
    std::vector<double>d((size_t)w*h,1e100);
    using Q=std::pair<double,int>;std::priority_queue<Q,std::vector<Q>,std::greater<Q>>q;
    int si=s.y*w+s.x,gi=g.y*w+g.x;d[si]=0;q.push({0,si});
    while(!q.empty()){auto t=q.top();q.pop();if(t.first!=d[t.second])continue;if(t.second==gi)return t.first;
        int x=t.second%w,y=t.second/w;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){if(!dx&&!dy)continue;
            int nx=x+dx,ny=y+dy;if(!c.Get(nx,ny))continue;
            if(dx&&dy&&(!c.Get(nx,y)||!c.Get(x,ny)))continue;
            int ni=ny*w+nx;double nd=t.first+(dx&&dy?std::sqrt(2.):1.);
            if(nd<d[ni]){d[ni]=nd;q.push({nd,ni});}
        }
    }return -1;
}
static void TestPixelNavigation(){
    Suite("PixelNavigation: white-only nodes, topology, optimality, strict targets");
    mapdata::CostBuffer c(mapdata::Geometry::FromGrid(1,1));
    for(int x=2;x<=20;++x)c.Set(x,11,1);
    nav::PixelMap m;CHECK(m.BuildFrom(c),"single-coordinate-wide passage retained");
    CHECK_EQ(m.PassableCount(),size_t(19),"no black slots in search arrays");
    CHECK_EQ(m.Id(2,10),int64_t(-1),"black has no node ID");
    CHECK_EQ(m.Id(-1,11),int64_t(-1),"negative x rejected");
    nav::PixelPath p;CHECK(nav::FindPixelPath(m,{2,11},{20,11},p),"thin corridor remains connected");
    CHECK_EQ(p.points.size(),size_t(19),"exact pixel path");CHECK(std::abs(p.length-18)<1e-9,"world length");
    auto wp=nav::PixelWaypoints(m,p.points,7);CHECK_EQ(wp.size(),size_t(4),"world step not CELL_SIZE");
    CHECK(wp.back().x==20&&wp.back().y==11,"preserve exact goal");
    for(size_t i=1;i<wp.size();++i){CHECK(nav::PixelLineClear(m,wp[i-1],wp[i]),"no black crossing");CHECK(nav::PixelDistance(wp[i-1],wp[i])<=7.,"step bounded");}
    CHECK(!nav::FindPixelPath(m,{2,10},{20,11},p),"do not snap black start");
    CHECK(!nav::FindPixelPath(m,{2,11},{20,10},p),"do not snap black goal");
    CHECK(nav::FindPixelPath(m,{2,11},{2,11},p)&&p.points.size()==1,"same point");
    CHECK(nav::PixelWaypoints(m,p.points,0).empty(),"zero step rejected");
    c.Set(12,11,0);CHECK(m.BuildFrom(c),"rebuild replaces topology");CHECK(!nav::FindPixelPath(m,{2,11},{20,11},p),"disconnected corridor");
    c.Raw().assign(c.Size(),0);c.Set(3,3,1);c.Set(4,4,1);m.BuildFrom(c);
    CHECK(!nav::FindPixelPath(m,{3,3},{4,4},p),"cannot cut black diagonal corner");
    CHECK(!nav::PixelLineClear(m,{3,3},{4,4}),"target line cannot cut corner");
    c.Set(4,3,1);c.Set(3,4,255);m.BuildFrom(c);
    CHECK(nav::FindPixelPath(m,{3,3},{4,4},p)&&std::abs(p.length-std::sqrt(2.))<1e-9,"valid diagonal");
    CHECK(nav::PixelWaypoints(m,p.points,1).empty(),"diagonal cannot exceed step cap 1");
    CHECK(!nav::FindPixelPath(m,{3,3},{4,4},p,[]{return true;}),"cancel checked inside A*");
    nav::PixelMap copy=m;c.Raw().assign(c.Size(),0);CHECK(!m.BuildFrom(c),"empty rebuild clears previous map");
    CHECK(!m.Passable(3,3)&&copy.Passable(3,3),"immutable copied snapshot survives rebuild");
    // Reproduction: a tiny visible corner target is inside T4 and gets ignored by the engine.
    c.Raw().assign(c.Size(),0);
    for(int x=2;x<=18;++x)c.Set(x,5,1);
    for(int y=5;y<=19;++y)c.Set(18,y,1);
    m.BuildFrom(c);nav::FindPixelPath(m,{2,5},{18,19},p);nav::PixelPoint target{};
    CHECK(nav::SelectPixelCallTarget(m,p.points,30,13,target),"look past tiny corner targets");
    CHECK(nav::PixelDistance({2,5},target)>13,"intermediate call outside arrival radius");
    CHECK(m.Passable(target.x,target.y),"detour endpoint must be white");
    CHECK(!nav::SelectPixelCallTarget(m,p.points,10,13,target),"do not emit nonprogress target");
    {
        mapdata::CostBuffer bend(mapdata::Geometry(mapdata::GridSize{6,4}));
        bend.Raw().assign(bend.Size(),0);std::vector<nav::PixelPoint> raw;
        for(int y=10;y<=70;++y){bend.Set(10,y,1);raw.push_back({10,y});}
        for(int x=11;x<=110;++x){bend.Set(x,70,1);raw.push_back({x,70});}
        for(int y=69;y>=10;--y){bend.Set(110,y,1);raw.push_back({110,y});}
        nav::PixelMap b;CHECK(b.BuildFrom(bend),"engine routing fixture");
        nav::PixelPoint selected{};
        CHECK(nav::SelectPixelCallTarget(b,raw,126,13,selected),"no extra detour budget");
        CHECK(selected.x==10&&selected.y==70,"preserve the first necessary corner");
        CHECK(nav::PixelLineClear(b,raw.front(),selected),"never hand obstacle routing back to the engine");
        nav::PixelRoute route; CHECK(route.Build(b,raw),"build cached route");
        CHECK(route.Points().size()==4,"U corridor keeps both corners");
        CHECK(route.Target(b,{10,10},126,3,selected)&&selected.x==76&&selected.y==70,"call target consumes path distance across first corner");
        // WalkCall can cut the first corner and report a position on the outgoing
        // segment. The route cursor must advance instead of returning the same
        // short target forever.
        CHECK(route.Target(b,{76,70},126,3,selected)&&route.Index()>=1&&selected.x==110&&selected.y==10,
              "advance when position is already on outgoing segment after corner");
        CHECK(route.Target(b,{10,70},126,3,selected)&&selected.x==110&&selected.y==44,"call target continues beyond second segment corner");
        size_t progress=route.Index();
        route.Target(b,{10,69},126,3,selected);
        CHECK(route.Index()>=progress,"route pointer never moves backward");
        CHECK(route.Target(b,{110,70},126,3,selected)&&selected.x==110&&selected.y==10,"advance around second corner");
        CHECK(nav::PixelDistance({110,70},selected)<=126,"call radius bounded");
    }
    uint32_t seed=20261110;auto random=[&](){seed=seed*1664525u+1013904223u;return seed;};
    for(int fixture=0;fixture<80;++fixture){
        c.Raw().assign(c.Size(),0);for(int y=0;y<18;++y)for(int x=0;x<18;++x)c.Set(x,y,(random()%100)<72?1:0);
        nav::PixelPoint s{int32_t(random()%18),int32_t(random()%18)},g{int32_t(random()%18),int32_t(random()%18)};
        c.Set(s.x,s.y,1);c.Set(g.x,g.y,1);m.BuildFrom(c);double expected=Oracle(c,s,g);bool found=nav::FindPixelPath(m,s,g,p);
        CHECK(found==(expected>=0),"reachability equals independent Dijkstra");
        CHECK(!found||std::abs(p.length-expected)<1e-8,"optimality equals independent Dijkstra");
        CHECK_EQ(m.PassableCount(),c.CountNonZero(),"every white point, no black node");
        if(found){auto points=nav::PixelWaypoints(m,p.points,5);bool valid=!points.empty();
            for(size_t i=1;i<points.size();++i)valid=valid&&m.Passable(points[i].x,points[i].y)&&nav::PixelDistance(points[i-1],points[i])<=5.;
            CHECK(valid,"call endpoints are white and bounded in world distance");
            nav::PixelRoute route;CHECK(route.Build(m,p.points),"random route can be smoothed");
            bool clear=true;
            for(size_t i=1;i<route.Points().size();++i)
                clear=clear&&nav::PixelLineClear(m,route.Points()[i-1],route.Points()[i]);
            CHECK(clear,"all random smoothed chords preserve topology");}
    }
}

static void TestRouteFeedback()
{
    Suite("Route feedback: actual motion, bounded recovery, temporary obstacles");
    nav::MotionWindow window; nav::MotionOptions o;
    window.Begin(1000,{0,0},{100,0});
    CHECK(window.Sample(1080,{0,0},o)==nav::MotionResult::Observing,"startup is not a wall");
    CHECK(window.Sample(1240,{0,0},o)==nav::MotionResult::Observing,"startup grace retained");
    CHECK(window.Sample(1360,{0,0},o)==nav::MotionResult::Still,"confirmed still samples");
    window.Begin(2000,{0,0},{100,0});
    CHECK(window.Sample(2200,{80,0},o)==nav::MotionResult::Lead,"handoff follows observed speed");
    window.Begin(3000,{0,0},{100,0});
    CHECK(window.Sample(6200,{20,0},o)==nav::MotionResult::Timeout,"single step has finite deadline");
    // Replay the real short-target trace: reaching during cadence guard must not be lost.
    window.Begin(5390,{379,572},{375,573});
    CHECK(window.Sample(5515,{375,574},o)==nav::MotionResult::Reached,"short target reached before call cadence guard");
    CHECK(window.Sample(5625,{373,576},o)==nav::MotionResult::Reached,"overshoot retains reached latch");
    CHECK(window.Sample(5734,{370,577},o)==nav::MotionResult::Reached,"advanced route never waits for old target to become still");
    nav::TemporaryObstacles blocks; blocks.Add({5,5},1,1000);
    CHECK(blocks.Contains({6,5},999)&&!blocks.Contains({5,5},1000),"temporary blocks expire");
    blocks.Expire(1000); CHECK(blocks.Empty(),"expired overlay removed");
    mapdata::CostBuffer cost(mapdata::Geometry::FromGrid(1,1)); cost.Raw().assign(cost.Size(),1);
    nav::PixelMap map; map.BuildFrom(cost); nav::PixelPath path;
    nav::PixelBlocked blocked=[](nav::PixelPoint p){return p.x==10&&p.y>=8&&p.y<=12;};
    CHECK(nav::FindPixelPath(map,{2,10},{20,10},path,{},blocked),"overlay causes a detour");
    nav::PixelRoute route; CHECK(route.Build(map,path.points,blocked),"smoothing respects overlay");
    for(size_t i=1;i<route.Points().size();++i)
        CHECK(nav::PixelLineClear(map,route.Points()[i-1],route.Points()[i],blocked),"detour chord remains clear");
    CHECK(map.Passable(10,10)&&cost.At(10,10)==1,"recovery never mutates base map");
    nav::PixelPoint target{};
    CHECK(nav::PixelStepToward(map,{1,1},{20,20},7,target)&&nav::PixelDistance({1,1},target)<=7,"rounded targets respect distance cap");
}

static void TestProtocol()
{
    Suite("Protocal：消息包布局");

    CHECK_EQ(MSG_LEN, 2048, "包体长度 2048");
    CHECK_EQ(HEADER_LEN, 8, "包头 8 字节");

    // NavGo 包：类型 + 数据一致
    {
        NavGo go{};
        go.x = 1234;
        go.y = 5678;
        go.tolerance = 12;
        go.speed = 3;

        NavGoMessage m(go);
        CHECK_EQ((int)m.m_MsgType, (int)MT_NavGo, "类型是 MT_NavGo");
        CHECK_EQ((int)m.m_len, (int)sizeof(NavGo), "长度等于 NavGo 大小");

        NavGo back{};
        std::memcpy(&back, m.m_Data, sizeof(NavGo));
        CHECK_EQ(back.x, 1234, "x 往返一致");
        CHECK_EQ(back.y, 5678, "y 往返一致");
        CHECK_EQ(back.tolerance, 12, "tolerance 往返一致");
    }

    // WalkCall 诊断关联字段复用保留槽位，不改变现有 12 字节 wire layout。
    {
        WalkCallReq req{};
        req.x = 321;
        req.y = 654;
        req.traceRunId = 0x1234;
        req.traceCallId = 0x5678;
        WalkCallMessage m(req);
        WalkCallReq back{};
        std::memcpy(&back, m.m_Data, sizeof(back));
        CHECK_EQ(sizeof(WalkCallReq), (size_t)12, "WalkCallReq wire size remains stable");
        CHECK_EQ(back.traceRunId, 0x1234, "walk trace run id round-trips");
        CHECK_EQ(back.traceCallId, 0x5678, "walk trace call id round-trips");
    }

    // NavKeyGo 包：类型必须是 MT_NavKeyGo（Monitor 靠这个分流）
    {
        NavGo go{};
        go.x = 100;
        go.y = 200;
        NavKeyGoMessage m(go);
        CHECK_EQ((int)m.m_MsgType, (int)MT_NavKeyGo,
                 "键盘驱动用的是 MT_NavKeyGo，不能退回 MT_NavGo");
    }

    // 消息类型不重复（重复会导致分发到错的分支）
    {
        const int types[] = {
            MT_Text, MT_Ack, MT_Exit, MT_DataChunk, MT_MapReq,
            MT_MapMeta, MT_MapRow, MT_NavStatus, MT_NavGo,
            MT_NavKeyGo, MT_NavKeyStop, MT_NavStop, MT_NavAbort
        };
        bool unique = true;
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
            for (size_t j = i + 1; j < sizeof(types) / sizeof(types[0]); ++j) {
                if (types[i] == types[j]) { unique = false; break; }
            }
        }
        CHECK(unique, "所有消息类型编号不重复");
    }
}

// ------------------------------------------------------------
// 7. GameOffsets 里的关键常量（防止回归）
// ------------------------------------------------------------
static void TestOffsets()
{
    Suite("GameOffsets：关键常量");

    // 生产坐标：+0x444(x) / +0x448(y)，各一个 int32，值就是世界像素
    CHECK_EQ(kPositioned_EntXOff, 0x444ull, "生产坐标 x 必须用 0x444");
    CHECK_EQ(kPositioned_EntYOff, 0x448ull, "生产坐标 y 必须用 0x448");
    CHECK_EQ(kPositioned_XOff, kPositioned_EntXOff,
             "玩家与周围实体的 x 必须同一偏移");
    CHECK_EQ(kPositioned_YOff, kPositioned_EntYOff,
             "玩家与周围实体的 y 必须同一偏移");

    // 0x490 已废弃，只作为历史对照保留，禁止再用于寻路
    CHECK_EQ(kPositioned_ValueOff, 0x490ull,
             "kPositioned_ValueOff 是历史偏移，值不应被改动");
    CHECK_EQ(kPositioned_PosOff, kPositioned_ValueOff,
             "PosOff 仍是历史别名");

    // r8 打包：必须 (y<<32)|x，由 hook 实测确定
    {
        const int32_t x = 1234;
        const int32_t y = 5678;
        const uint64_t packed = PackWalkPos(x, y);
        CHECK_EQ((int32_t)(uint32_t)(packed >> 32), y,
                 "r8 高 32 位是 y");
        CHECK_EQ((int32_t)(uint32_t)(packed & 0xFFFFFFFFULL), x,
                 "r8 低 32 位是 x");
    }
}

static void TestGameBuild()
{
    Suite("GameBuild: reject changed builds before pointer reads or calls");
    const uint64_t base = 0x10000000;
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> regions;
    regions.push_back({0, std::vector<uint8_t>(0x200, 0)});
    auto put = [&](size_t offset, auto value) {
        std::memcpy(regions[0].second.data() + offset, &value, sizeof(value));
    };
    put(0, uint16_t(0x5A4D));
    put(0x3C, uint32_t(0x80));
    put(0x80, uint32_t(0x4550));
    put(0x84, uint16_t(0x8664));
    put(0x88, kGameTimeDateStamp);
    put(0x98, uint16_t(0x20B));
    put(0xD0, uint32_t(kGameImageSize));
    auto add = [&](uint64_t rva, const auto& bytes) {
        regions.push_back({rva, std::vector<uint8_t>(bytes, bytes + sizeof(bytes))});
    };
    add(CostCall_Offset, kCostCallSignature);
    add(kActionWalkEntryRva, kActionWalkSignature);
    add(kSkillCallRva, kSkillCallSignature);
    add(kSkillStopRva, kSkillStopSignature);
    add(kIdSkillCallRva, kIdSkillCallSignature);
    add(kSkillManagerGetterRva, kSkillManagerGetterSignature);
    auto read = [&](uint64_t address, void* out, size_t size) {
        if (address < base) return false;
        const uint64_t offset = address - base;
        for (const auto& region : regions) {
            if (offset >= region.first && offset - region.first <= region.second.size() &&
                size <= region.second.size() - size_t(offset - region.first)) {
                std::memcpy(out, region.second.data() + size_t(offset - region.first), size);
                return true;
            }
        }
        return false;
    };
    std::string err;
    CHECK(gamebuild::Verify(base, read, err), "matching build must pass");
    put(0x88, kGameTimeDateStamp + 1);
    CHECK(!gamebuild::Verify(base, read, err), "same SizeOfImage with changed timestamp must fail");
    put(0x88, kGameTimeDateStamp);
    put(0xD0, uint32_t(kGameImageSize - 0x1000));
    CHECK(!gamebuild::Verify(base, read, err), "changed SizeOfImage must fail");
    put(0xD0, uint32_t(kGameImageSize));
    for (size_t i = 1; i < regions.size(); ++i) {
        regions[i].second[0] ^= 1;
        CHECK(!gamebuild::Verify(base, read, err), "corrupt entry fingerprint must fail");
        regions[i].second[0] ^= 1;
    }
    const auto stopRva = regions[4].first;
    regions[4].first += 0x70;
    CHECK(!gamebuild::Verify(base, read, err), "relocated stop entry must fail closed");
    regions[4].first = stopRva;
    regions[0].second.resize(0xC0);
    CHECK(!gamebuild::Verify(base, read, err), "short PE header read must fail");
    CHECK_EQ(MAP_LAYER_COUNT * MAP_LAYER_STRIDE + MAP_OFF_ARY_BEGIN,
             MAP_OFF_ROW_STRIDE, "layer vectors must stop before row stride");
}

static void TestPollSource()
{
    Suite("PollSource：定时采样与双缓冲");

    poll::Source<std::function<bool(poll::Snapshot&)>> src;
    src.SetMaxConsecutiveFail(3);

    int  fetchCount = 0;
    bool fetchOk    = true;

    auto fetch = [&](poll::Snapshot& s) -> bool {
        ++fetchCount;
        if (!fetchOk) {
            return false;
        }
        s.Add(10, 20, 1);
        s.Add(30, 40, 2);
        return true;
    };

    // ---- 周期节流 ----
    CHECK(src.Tick(0, 100, fetch), "第一次 tick 应该采样");
    CHECK_EQ(fetchCount, 1, "第一次采样一次");
    CHECK(src.NeverSampled() == false, "采样成功后不应是 NeverSampled");
    CHECK_EQ(src.Peek().count, (size_t)2, "快照里应有 2 个点");

    CHECK(!src.Tick(50, 100, fetch), "未到周期不采样");
    CHECK_EQ(fetchCount, 1, "未到周期不应调用 fetch");

    CHECK(src.Tick(100, 100, fetch), "满周期应采样");
    CHECK_EQ(fetchCount, 2, "满周期采样一次");

    // ---- 失败不覆盖快照 ----
    fetchOk = false;
    CHECK(src.Tick(200, 100, fetch), "失败的 tick 仍然算作一次采样");
    CHECK_EQ(src.Peek().count, (size_t)2, "失败时必须保留上一份有效快照");
    CHECK(src.Peek().valid, "快照仍标记为有效");
    CHECK_EQ(src.FailStreak(), 1u, "失败计数 +1");

    // ---- 连续失败到降级 ----
    CHECK(src.Tick(300, 100, fetch), "第二次失败");
    CHECK(!src.Degraded(), "2 次失败还没到阈值 3");
    CHECK(src.Tick(400, 100, fetch), "第三次失败");
    CHECK(src.Degraded(), "连续 3 次失败应降级");
    CHECK(src.Peek().valid, "降级后快照仍保留（但上层据此停止行为）");

    // ---- 恢复 ----
    fetchOk = true;
    CHECK(src.Tick(500, 100, fetch), "恢复后重新采样");
    CHECK(!src.Degraded(), "成功一次应解除降级");
    CHECK_EQ(src.FailStreak(), 0u, "失败计数清零");

    // ---- Reset ----
    src.Reset();
    CHECK(src.NeverSampled(), "Reset 后回到未采样状态");

    // ---- 满载保护 ----
    {
        poll::Source<std::function<bool(poll::Snapshot&)>> s2;
        auto full = [](poll::Snapshot& out) {
            for (int i = 0; i < (int)poll::Snapshot::kMaxPoints + 50; ++i) {
                out.Add(i, i, (uint64_t)i);
            }
            return true;
        };
        s2.Tick(0, 1, full);
        CHECK_EQ(s2.Peek().count, poll::Snapshot::kMaxPoints,
                 "超过上限要截断，不能越界写");
    }
}

// ------------------------------------------------------------
int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0) g_verbose = true;
    }

    std::printf("===== 核心逻辑测试 =====\n");

    TestGeometry();
    TestNibble();
    TestPixelNavigation();
    TestRouteFeedback();
    TestProtocol();
    TestOffsets();
    TestGameBuild();
    TestPollSource();

    std::printf("\n===== 结果 =====\n");
    std::printf("  通过 %d\n", g_pass);
    std::printf("  失败 %d\n", g_fail);
    if (g_fail == 0) {
        std::printf("  全部通过\n");
        return 0;
    }
    std::printf("  有失败项，见上面的 [FAIL]\n");
    return 1;
}
