#pragma once
#include "GameOffsets.h"
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

// Readers return false for short/unreadable reads. No game calls or writes.
namespace gamebuild {
template<class Read>
bool Verify(uint64_t moduleBase, Read read, std::string& err)
{
    err.clear();
    uint16_t mz = 0;
    uint32_t peOffset = 0;
    if (!IsPlausibleAddress(moduleBase) ||
        !read(moduleBase, &mz, sizeof(mz)) || mz != 0x5A4D ||
        !read(moduleBase + 0x3C, &peOffset, sizeof(peOffset)) ||
        peOffset < 0x40 || peOffset > 0x1000) {
        err = "Game PE header unreadable or invalid";
        return false;
    }
    uint8_t nt[0x54] = {};
    if (!read(moduleBase + peOffset, nt, sizeof(nt))) {
        err = "Game NT header unreadable";
        return false;
    }
    uint32_t signature = 0, timestamp = 0, imageSize = 0;
    uint16_t machine = 0, magic = 0;
    std::memcpy(&signature, nt, sizeof(signature));
    std::memcpy(&machine, nt + 4, sizeof(machine));
    std::memcpy(&timestamp, nt + 8, sizeof(timestamp));
    std::memcpy(&magic, nt + 0x18, sizeof(magic));
    std::memcpy(&imageSize, nt + 0x50, sizeof(imageSize));
    if (signature != 0x4550 || machine != 0x8664 || magic != 0x20B ||
        timestamp != kGameTimeDateStamp || imageSize != kGameImageSize) {
        char message[192];
        std::snprintf(message, sizeof(message),
            "Unsupported game build: PE timestamp=0x%08X size=0x%X; expected 0x%08X/0x%llX. Recalibrate offsets.",
            timestamp, imageSize, kGameTimeDateStamp,
            (unsigned long long)kGameImageSize);
        err = message;
        return false;
    }
    struct Entry { uint64_t rva; const uint8_t* bytes; size_t size; const char* name; };
    const Entry entries[] = {
        { CostCall_Offset, kCostCallSignature, sizeof(kCostCallSignature), "CostCall" },
        { kActionWalkEntryRva, kActionWalkSignature, sizeof(kActionWalkSignature), "Walk" },
        { kSkillCallRva, kSkillCallSignature, sizeof(kSkillCallSignature), "Skill" },
        { kSkillStopRva, kSkillStopSignature, sizeof(kSkillStopSignature), "StopSkill" },
        { kIdSkillCallRva, kIdSkillCallSignature, sizeof(kIdSkillCallSignature), "IdSkill" },
        { kSkillManagerGetterRva, kSkillManagerGetterSignature, sizeof(kSkillManagerGetterSignature), "SkillManager" }
    };
    for (const auto& entry : entries) {
        uint8_t bytes[32] = {};
        if (entry.size > sizeof(bytes) || entry.rva + entry.size > imageSize ||
            !read(moduleBase + entry.rva, bytes, entry.size) ||
            std::memcmp(bytes, entry.bytes, entry.size) != 0) {
            err = std::string("Game function fingerprint mismatch: ") + entry.name;
            return false;
        }
    }
    return true;
}

#ifdef _WIN32
inline bool VerifyLocal(uint64_t moduleBase, std::string& err)
{
    return Verify(moduleBase, [](uint64_t address, void* out, size_t size) {
        SIZE_T done = 0;
        return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)address,
                                 out, size, &done) && done == size;
    }, err);
}
#endif
} // namespace gamebuild
