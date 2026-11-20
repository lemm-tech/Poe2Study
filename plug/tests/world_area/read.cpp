#include "../../Plug/WorldArea.h"
#include "../../Common/GameBuild.h"
#include <cstdio>
#include <cstdlib>

// Read-only diagnostic: no Monitor connection, injection, input or game call.
int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "Usage: ReadWorldArea <game-pid>\n"); return 2; }
    char* end = nullptr;
    const auto pid = std::strtoul(argv[1], &end, 10);
    if (!pid || !end || *end) return 2;
    ola::OlaDriver driver;
    if (!driver.InitializeLocal(static_cast<DWORD>(pid))) return 1;
    const mem::GameMemory memory(driver);
    const auto base = memory.ModuleBase(app::Config().gameModuleName);
    std::string error;
    if (!base || !gamebuild::Verify(base, [&memory](uint64_t address, void* data, size_t size) {
            return memory.ReadInto(address, data, size);
        }, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    poe::WorldAreaSnapshot area;
    if (!poe::ReadWorldArea(memory, base, area, error)) {
        std::fprintf(stderr, "%s\n", error.c_str()); return 1;
    }
    std::printf("id=%s\nname=%s\nworld=0x%llX\nrow=0x%llX\n", area.id.c_str(), area.name.c_str(),
                static_cast<unsigned long long>(area.world), static_cast<unsigned long long>(area.record));
    return 0;
}
