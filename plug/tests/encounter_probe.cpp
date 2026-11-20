#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../Plug/MyOLAFunction.h"
#include "../Plug/Surroundings.h"
#include "../Common/MonsterReader.h"
#include <chrono>
#include <cstdio>

// Exercises production GameMemory, map identity and the complete tree scanner.
// No pipe, navigation, skill, injection or memory write API is linked here.
int main() {
    SetConsoleOutputCP(CP_UTF8);
    GameSession session; std::string error;
    if (!InitializeSession(session, &error)) { std::printf("FAIL: %s\n", error.c_str()); return 1; }
    poe::WorldAreaSnapshot area;
    if (!poe::ReadWorldArea(session.memory, session.moduleBase, area, error)) {
        std::printf("FAIL: %s\n", error.c_str()); return 1;
    }
    std::printf("PID=%lu area=%s (%s)\n", session.game.pid, area.name.c_str(), area.id.c_str());
    try {
        const auto catalog = monster::Reader<mem::GameMemory>(session.memory, session.moduleBase).Catalog(area.world, area.record);
        for (const auto& boss : catalog) std::printf("configured boss: %s | %s\n", boss.name.c_str(), boss.metadata.c_str());
    } catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); return 1; }
    for (int sample = 0; sample < 3; ++sample) {
        const auto start = std::chrono::steady_clock::now();
        poe::EntityScanInfo info; std::vector<poe::EntitySnapshot> entities;
        if (!poe::SurroundingsTraverser(session.memory).ListEncounterTargets(session.moduleBase, area, entities, info)) {
            std::printf("sample %d FAIL: %s\n", sample + 1, info.error.c_str()); return 1;
        }
        size_t elites = 0, bosses = 0, entrances = 0;
        for (const auto& item : entities) {
            if (monster::OptionalElite(item.combat) && item.hp) ++elites;
            if (monster::MapBoss(item.combat)) ++bosses;
            if (monster::BossEntrance(item.metadataPath)) ++entrances;
            if (monster::MapBoss(item.combat) || monster::BossEntrance(item.metadataPath))
                std::printf("  %s key=%X name=%s HP=%u xy=(%d,%d)\n",
                    monster::BossEntrance(item.metadataPath) ? "boss_entrance" : "map_boss",
                    item.key, item.name.c_str(), item.hp, item.x, item.y);
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        std::printf("sample %d OK visited=%zu owners=%zu live_elites=%zu bosses=%zu entrances=%zu elapsed=%lldms\n",
            sample + 1, info.visited, entities.size(), elites, bosses, entrances, static_cast<long long>(ms));
        std::printf("encounter_scan %s\n", poe::ScanSummary(info, entities.size()).c_str());
        if (!poe::SurroundingsTraverser(session.memory).ListMonsters(session.moduleBase, entities, info)) {
            std::printf("monster_scan FAIL %s\n", poe::ScanSummary(info, entities.size()).c_str()); return 1;
        }
        const auto living = std::count_if(entities.begin(), entities.end(), [](const auto& item) { return item.hp > 0; });
        std::printf("monster_scan living=%zu %s\n", size_t(living), poe::ScanSummary(info, entities.size()).c_str());
        Sleep(400);
    }
    return 0;
}
