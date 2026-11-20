#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../Common/RitualCombat.h"
#include <cstdio>
#include <cstdlib>

namespace {
int passed = 0, failed = 0;
#define CHECK(value) do { if (value) ++passed; else { ++failed; std::printf("FAIL line %d: %s\n", __LINE__, #value); } } while (0)
using ritualcombat::Circle;
using ritualcombat::Phase;
struct Monster { uint64_t entity = 0; uint32_t hp = 0, key = 0; int32_t x = 0, y = 0; };
mapdata::CostBuffer OpenMap() {
    mapdata::CostBuffer cost(mapdata::Geometry::FromGrid(2, 2));
    std::fill(cost.Raw().begin(), cost.Raw().end(), 1);
    return cost;
}
void StateTransitions() {
    ritualstate::Snapshot state;
    state.valid = state.hasInteraction = state.hasNpc = true;
    state.current = 1; state.interaction = 0; state.disabledByNpc = 0;
    CHECK(ritualcombat::Next(true, state, true) == Phase::Clearing);
    // Circle becoming empty does not authorize a click; only the live state does.
    CHECK(ritualcombat::Next(true, state, true) == Phase::Clearing);
    state.interaction = 1;
    CHECK(ritualcombat::Next(true, state, true) == Phase::Ready);
    CHECK(ritualcombat::Next(true, state, false) == Phase::Waiting);
    state.current = 2; state.interaction = 0;
    CHECK(ritualcombat::Next(true, state, true) == Phase::Fighting);
    state.current = 3;
    CHECK(ritualcombat::Next(true, state, true) == Phase::Completed);
    state.current = 4;
    CHECK(ritualcombat::Next(true, state, true) == Phase::Waiting);
    state.current = 1; state.hasInteraction = false;
    CHECK(ritualcombat::Next(true, state, true) == Phase::Waiting);
    state.hasInteraction = true; state.valid = false;
    CHECK(ritualcombat::Next(true, state, true) == Phase::Waiting);
    state.valid = true;
    CHECK(ritualcombat::Next(false, state, true) == Phase::Waiting);
    state.interaction = 1; state.disabledByNpc = 1;
    CHECK(ritualcombat::Next(true, state, true) == Phase::Waiting);
    state.disabledByNpc = 0; state.hasNpc = false;
    CHECK(ritualcombat::Next(true, state, true) == Phase::Waiting);
}
void FixedCircle() {
    const Circle circle{{10, 10}, 5};
    CHECK(circle.Contains({10, 10}));
    CHECK(circle.Contains({13, 14})); // Exact circular boundary.
    CHECK(!circle.Contains({14, 14})); // Inside bounding square, outside circle.
    CHECK(!circle.Contains({16, 10}));
    CHECK(!circle.Contains({INT32_MAX, INT32_MIN}));
    const auto source = OpenMap(); mapdata::CostBuffer clipped;
    ritualcombat::ClipCost(source, circle, clipped);
    bool correct = true;
    for (int y = 0; y < 46; ++y) for (int x = 0; x < 46; ++x)
        if ((clipped.Get(x, y) != 0) != circle.Contains({x, y})) correct = false;
    CHECK(correct);
    // Rebuilding for another node must not retain the old circle's white pixels.
    ritualcombat::ClipCost(source, Circle{{30, 30}, 3}, clipped);
    CHECK(clipped.Get(10, 10) == 0 && clipped.Get(30, 30) != 0);
    ritualcombat::ClipCost(source, Circle{{0, 0}, 5}, clipped);
    CHECK(clipped.Get(3, 4) != 0 && clipped.Get(4, 4) == 0);
}
void TargetSelection() {
    const Circle circle{{15, 15}, 10};
    mapdata::CostBuffer clipped; ritualcombat::ClipCost(OpenMap(), circle, clipped);
    nav::PixelMap map; CHECK(map.BuildFrom(clipped));
    Monster target; nav::PixelPoint goal;
    std::vector<Monster> monsters{{1, 0, 1, 16, 15}, {2, 100, 2, 26, 15},
        {3, 100, 3, 22, 15}, {4, 100, 4, 18, 15}, {5, 100, 5, -1, 15}};
    CHECK(ritualcombat::ChooseTarget(monsters, circle, map, {15, 15}, 2, target, goal) && target.key == 4);
    CHECK(ritualcombat::ChooseTarget(monsters, circle, map, {23, 15}, 2, target, goal) && target.key == 3);
    // Player motion changes target priority, never the altar circle's center.
    CHECK(!ritualcombat::AliveInside(monsters[1], circle, map));
    CHECK(!ritualcombat::ChooseTarget(std::vector<Monster>{monsters[0], monsters[1], monsters[4]},
        circle, map, {23, 15}, 2, target, goal));
    CHECK(!ritualcombat::ChooseTarget(monsters, circle, map, {15, 15}, 2, target, goal, [] { return true; }));
    monsters = {{6, 100, 6, 25, 15}};
    CHECK(ritualcombat::ChooseTarget(monsters, circle, map, {20, 15}, 2, target, goal));
    monsters[0].x = 26;
    CHECK(!ritualcombat::ChooseTarget(monsters, circle, map, {20, 15}, 2, target, goal));
}
void BoundedPaths() {
    auto source = OpenMap();
    for (int y = 5; y <= 25; ++y) source.Set(15, y, 0);
    const Circle circle{{15, 15}, 10};
    mapdata::CostBuffer clipped; ritualcombat::ClipCost(source, circle, clipped);
    nav::PixelMap map; CHECK(map.BuildFrom(clipped));
    Monster target; nav::PixelPoint goal;
    std::vector<Monster> monsters{{1, 100, 1, 18, 15}, {2, 100, 2, 8, 18}};
    CHECK(ritualcombat::ChooseTarget(monsters, circle, map, {12, 15}, 1, target, goal) && target.key == 2);
    nav::PixelPath path;
    CHECK(nav::FindPixelPath(map, {12, 15}, goal, path));
    CHECK(std::all_of(path.points.begin(), path.points.end(), [&](auto p) { return circle.Contains(p); }));
    CHECK(!ritualcombat::ChooseTarget(std::vector<Monster>{monsters[0]}, circle, map, {12, 15}, 1, target, goal));
    // An isolated closest standing spot must not hide an accessible alternative.
    source.Resize(source.Geo());
    for (int y = 10; y <= 14; ++y) source.Set(2, y, 1);
    for (int x = 2; x <= 15; ++x) source.Set(x, 14, 1);
    source.Set(12, 10, 1);
    CHECK(map.BuildFrom(source));
    CHECK(ritualcombat::ChooseTarget(std::vector<Monster>{{3, 100, 3, 15, 10}}, Circle{{15, 15}, 20},
        map, {2, 10}, 4, target, goal) && goal.x == 15 && goal.y == 14);
}
void ConfiguredAttackRange() {
    combo::Row attack; attack.distance = 100;
    CHECK(ritualcombat::AttackRange({attack}) == 100);
    combo::Row melee = attack; melee.distance = 4;
    CHECK(ritualcombat::AttackRange({attack, melee}) == 4);
    combo::Row buff = melee; buff.environment = combo::Environment::Always;
    combo::Row once = melee; once.usage = combo::Usage::Once;
    CHECK(ritualcombat::AttackRange({buff, once, attack}) == 100);
    attack.distance = 0;
    CHECK(ritualcombat::AttackRange({attack, buff, once}) == 8);
    CHECK(ritualcombat::AttackRange({}) == 8);
}
void RitualDistanceReplay() {
    // All 20 target selections from ritual_20261119_084534_546_3676_5.jsonl.
    // Translate the recorded positions together onto an open test map: this
    // isolates the distance gate, without claiming to replay the live terrain.
    const int samples[][6] = {
        {424,666,419,674,422,669}, {424,666,424,688,424,682},
        {424,682,432,690,428,686}, {428,686,407,690,413,690},
        {413,689,413,697,413,691}, {412,674,421,675,415,675},
        {412,674,400,679,405,676}, {407,677,411,685,408,680},
        {407,677,400,691,403,686}, {341,825,327,816,332,819},
        {341,825,337,816,340,821}, {340,821,323,813,328,816},
        {340,821,335,807,338,812}, {352,814,354,844,354,838},
        {354,838,363,842,358,839}, {359,835,360,827,360,833},
        {364,830,360,827,364,830}, {364,830,360,827,364,830},
        {364,830,350,847,353,842}, {364,830,349,835,354,832}
    };
    mapdata::CostBuffer cost(mapdata::Geometry::FromGrid(4, 4));
    std::fill(cost.Raw().begin(), cost.Raw().end(), 1);
    nav::PixelMap map; CHECK(map.BuildFrom(cost));
    combo::Row attack; attack.distance = 100;
    const auto range = ritualcombat::AttackRange({attack});
    for (const auto& sample : samples) {
        const nav::PixelPoint player{44,44}, target{44+sample[2]-sample[0],44+sample[3]-sample[1]},
            goal{44+sample[4]-sample[0],44+sample[5]-sample[1]};
        CHECK(ritualcombat::InAttackPosition(map, player, target, goal, range));
    }
    CHECK(!ritualcombat::InAttackPosition(map, {10,10}, {30,10}, {15,10}, 19));
    cost.Set(12,10,0); CHECK(map.BuildFrom(cost));
    CHECK(!ritualcombat::InAttackPosition(map, {10,10}, {30,10}, {15,10}, 100));
}
void LongRangeSelection() {
    auto source = OpenMap(); nav::PixelMap map; CHECK(map.BuildFrom(source));
    const Circle circle{{23,23}, 40};
    const std::vector<Monster> monsters{{1,100,1,45,45}};
    Monster target; nav::PixelPoint goal;
    CHECK(ritualcombat::ChooseTarget(monsters, circle, map, {2,2}, 100 * 0.75, target, goal));
    CHECK(goal.x == 2 && goal.y == 2); // No approach when already within range.
    CHECK(ritualcombat::ChooseTarget(monsters, circle, map, {2,2}, (0x1000 * 23) * 0.75, target, goal));
    CHECK(goal.x == 2 && goal.y == 2);
    // Boundary clipping must still find a reachable approach outside stand-off.
    CHECK(ritualcombat::ChooseTarget(monsters, circle, map, {2,2}, 32, target, goal));
    CHECK(nav::PixelDistance(goal, {45,45}) <= 32 && map.Passable(goal.x, goal.y));
    nav::PixelPath path; CHECK(nav::FindPixelPath(map, {2,2}, goal, path));
    CHECK(!ritualcombat::ChooseTarget(monsters, circle, map, {2,2}, 32, target, goal, [] { return true; }));
    // Sparse maps use white nodes rather than enumerating a large empty square.
    source.Resize(source.Geo());
    for (int x = 0; x < 46; ++x) source.Set(x,2,1);
    CHECK(map.BuildFrom(source));
    CHECK(ritualcombat::ChooseTarget(monsters, circle, map, {2,2}, 44, target, goal));
    CHECK(goal.y == 2 && nav::PixelDistance(goal, {45,45}) <= 44);
}
void RoamingCircle80() {
    mapdata::CostBuffer source(mapdata::Geometry::FromGrid(8, 8));
    std::fill(source.Raw().begin(), source.Raw().end(), 1);
    const Circle circle{{92,92}, 80};
    CHECK(circle.Contains({172,92}) && !circle.Contains({173,92}));
    // A wall with a single gap exercises paths that must turn around obstacles.
    for (int y = 12; y <= 172; ++y) if (y < 103 || y > 108) source.Set(92,y,0);
    mapdata::CostBuffer clipped; ritualcombat::ClipCost(source, circle, clipped);
    nav::PixelMap map; CHECK(map.BuildFrom(clipped));
    nav::PixelPoint player{50,92}, goal;
    std::vector<nav::PixelPoint> recent;
    bool crossed = false;
    for (uint64_t i = 0; i < 24; ++i) {
        CHECK(ritualcombat::ChooseRoamGoal(map, circle, player, i * 7919, goal, recent));
        CHECK(circle.Contains(goal) && map.Passable(goal.x,goal.y));
        CHECK(nav::PixelDistance(player,goal) >= 25);
        CHECK(std::none_of(recent.begin(),recent.end(),[&](auto p) { return nav::PixelDistance(p,goal) <= 12; }));
        nav::PixelPath path;
        CHECK(nav::FindPixelPath(map,player,goal,path));
        CHECK(std::all_of(path.points.begin(),path.points.end(),[&](auto p) {
            return circle.Contains(p) && clipped.Get(p.x,p.y) != 0;
        }));
        crossed = crossed || goal.x > 92;
        recent.push_back(goal);
        if (recent.size() > 4) recent.erase(recent.begin());
        player = goal;
    }
    CHECK(crossed);
    // A complete wall splits the circle. Never pick a white point across it,
    // even though the original full map permits a detour outside the circle.
    for (int y = 12; y <= 172; ++y) source.Set(92,y,0);
    ritualcombat::ClipCost(source,circle,clipped); CHECK(map.BuildFrom(clipped));
    for (uint64_t i = 0; i < 12; ++i) {
        CHECK(ritualcombat::ChooseRoamGoal(map,circle,{50,92},i * 1237,goal));
        CHECK(goal.x < 92);
    }
    CHECK(!ritualcombat::ChooseRoamGoal(map,circle,{173,92},0,goal));
    CHECK(!ritualcombat::ChooseRoamGoal(map,circle,{50,92},0,goal,{},[] { return true; }));
    int checks = 0;
    CHECK(!ritualcombat::ChooseRoamGoal(map,circle,{50,92},0,goal,{},[&] { return ++checks >= 3; }));
}
void RoamingSmallPockets() {
    auto source = OpenMap(); source.Resize(source.Geo());
    const Circle circle{{20,20}, 80};
    for (int x = 10; x <= 14; ++x) source.Set(x,10,1);
    source.Set(20,20,1); // Isolated white point must never be chosen.
    nav::PixelMap map; CHECK(map.BuildFrom(source));
    nav::PixelPoint goal;
    CHECK(ritualcombat::ChooseRoamGoal(map,circle,{10,10},0,goal));
    CHECK(goal.y == 10 && goal.x >= 12 && goal.x <= 14);
    CHECK(ritualcombat::ChooseRoamGoal(map,circle,{10,10},0,goal,{{12,10},{13,10},{14,10}}));
    CHECK(goal.y == 10 && goal.x >= 12 && goal.x <= 14); // Revisit when no fresh space exists.
    CHECK(!ritualcombat::ChooseRoamGoal(map,circle,{20,20},0,goal));
    CHECK(!ritualcombat::ChooseRoamGoal(map,circle,{40,40},0,goal));
    CHECK(ritualcombat::ChooseRoamGoal(map,circle,{9,10},0,goal)); // Native black-start recovery.
    CHECK(goal.y == 10 && goal.x >= 12 && goal.x <= 14);
    // Diagonal contact alone cannot connect two floor regions through a corner.
    source.Resize(source.Geo()); source.Set(10,10,1); source.Set(11,11,1); source.Set(12,12,1);
    CHECK(map.BuildFrom(source));
    CHECK(!ritualcombat::ChooseRoamGoal(map,circle,{10,10},0,goal));
    CHECK(!ritualcombat::ChooseRoamGoal(nav::PixelMap{},circle,{10,10},0,goal));
}
} // namespace
int main() {
    StateTransitions(); FixedCircle(); TargetSelection(); BoundedPaths();
    ConfiguredAttackRange(); RitualDistanceReplay(); LongRangeSelection();
    RoamingCircle80(); RoamingSmallPockets();
    std::printf("ritual combat: %d passed, %d failed\n", passed, failed);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
