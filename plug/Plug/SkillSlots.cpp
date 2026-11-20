#include "SkillSlots.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {
std::string Hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << value;
    return out.str();
}

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid UTF-16 text");
    std::string result(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr))
        throw std::runtime_error("UTF-16 conversion failed");
    return result;
}

// Unlike Read<T>, checked reads never confuse an unreadable pointer with an empty slot.
struct Reader {
    const mem::GameMemory& memory;
    uint64_t base;
    bool ReadInto(uint64_t address, void* output, size_t size) const {
        return size && size <= 1024 * 1024 && memory.IsPlausible(address) &&
            memory.IsPlausible(address + size - 1) && memory.ReadInto(address, output, size);
    }
    template<class T> T Read(uint64_t address) const {
        T value{};
        if (!ReadInto(address, &value, sizeof(value)))
            throw std::runtime_error("Unreadable memory at " + Hex(address));
        return value;
    }
    uint64_t Follow(uint64_t address) const {
        auto value = Read<uint64_t>(address);
        if (!IsPlausibleAddress(value))
            throw std::runtime_error("Invalid pointer at " + Hex(address) + ": " + Hex(value));
        return value;
    }
    std::wstring WideText(uint64_t address) const {
        std::wstring result;
        for (size_t i = 0; i < 256; ++i) {
            wchar_t ch = Read<wchar_t>(address + i * sizeof(wchar_t));
            if (!ch) {
                if (result.empty()) throw std::runtime_error("Empty text at " + Hex(address));
                return result;
            }
            if (ch < 0x20) throw std::runtime_error("Unexpected control character in text");
            result += ch;
        }
        throw std::runtime_error("Unterminated text at " + Hex(address));
    }
    std::string AsciiText(uint64_t address) const {
        std::string result;
        for (size_t i = 0; i < 96; ++i) {
            char ch = Read<char>(address + i);
            if (!ch) return result;
            if (ch < 0x20 || ch > 0x7E) throw std::runtime_error("Invalid component name");
            result += ch;
        }
        throw std::runtime_error("Unterminated component name");
    }
    std::string StdWideText(uint64_t address) const {
        auto length = Read<uint64_t>(address + kStdStringLengthOff);
        auto capacity = Read<uint64_t>(address + kStdStringMaxLenOff);
        if (!length || length > 256 || capacity < length || capacity > 1024 * 1024)
            throw std::runtime_error("Invalid character-name string");
        auto data = capacity > kStdStringSsoLimit ? Follow(address) : address;
        std::wstring value(static_cast<size_t>(length), L'\0');
        if (!ReadInto(data, value.data(), value.size() * sizeof(wchar_t)))
            throw std::runtime_error("Character name unreadable");
        return Utf8(value);
    }
};
struct Character {
    uint64_t entity = 0, actor = 0;
    std::string name;
};

Character ReadCharacter(const Reader& p, bool needActor = true) {
    Character result;
    result.entity = p.Follow(p.base + BaseOffset);
    // Shared map chain stops at player entity, before the parent/map tail.
    for (size_t i = 0; i < 7; ++i) result.entity = p.Follow(result.entity + kMapObjectChain[i]);
    auto header = p.Follow(p.Follow(result.entity + kEntityPairsOff0) + kEntityPairsOff1);
    auto begin = p.Follow(header + kEntityPairsOff2);
    auto end = p.Read<uint64_t>(header + kEntityPairsEndOff);
    auto array = p.Follow(result.entity + kEntityComponentsOff);
    auto arrayEnd = p.Read<uint64_t>(result.entity + kEntityComponentsEndOff);
    if (end <= begin || end - begin > 128 * kComponentPairSize ||
        (end - begin) % kComponentPairSize || arrayEnd < array ||
        arrayEnd - array != ((end - begin) / kComponentPairSize) * kComponentPtrSize)
        throw std::runtime_error("Invalid player component table");
    auto count = (end - begin) / kComponentPairSize;
    for (uint64_t i = 0; i < count; ++i) {
        auto pair = begin + i * kComponentPairSize;
        auto name = p.AsciiText(p.Follow(pair));
        auto index = p.Read<uint32_t>(pair + kComponentPairIndexOff);
        if (index >= count) throw std::runtime_error("Component index out of range");
        if (needActor && name == "Actor") result.actor = p.Follow(array + index * kComponentPtrSize);
        if (name == "Player") {
            result.name = p.StdWideText(p.Follow(array + index * kComponentPtrSize) + kPlayer_NameOff);
            if (!needActor) break;
        }
    }
    if ((needActor && !result.actor) || result.name.empty())
        throw std::runtime_error("Local character/Actor unavailable; enter the game world");
    if (needActor && p.Follow(result.actor + skilllayout::kComponentOwnerEntity) != result.entity)
        throw std::runtime_error("Actor does not belong to the local character");
    return result;
}

struct Slot {
    size_t index = 0;
    uint64_t object = 0, shared = 0, ptr = 0, rep = 0, owner = 0, ownerEntity = 0;
    uint64_t levelRow = 0, effectRow = 0, idText = 0;
    uint64_t baseRow = 0, baseIdText = 0;
    uint64_t activeRow = 0, activeCodeText = 0, localizedNameText = 0;
    uint32_t id = 0, uses = 0, weak = 0;
    bool movement = false;
    std::string internalId, activeSkillCode, localizedName;
};

struct Snapshot {
    Character character;
    uint64_t global = 0, container = 0, begin = 0, end = 0, capacity = 0;
    std::vector<Slot> slots;
};

Snapshot Capture(const Reader& p) {
    Snapshot s;
    s.character = ReadCharacter(p);
    s.global = p.Follow(p.base + kSkillContainerBase);
    s.container = p.Follow(s.global + kSkillContainerOff0);
    std::array<uint64_t, 3> vector{};
    if (!p.ReadInto(s.container + kSkillContainerOff1, vector.data(), sizeof(vector)))
        throw std::runtime_error("Skill-slot vector unreadable");
    s.begin = vector[0]; s.end = vector[1]; s.capacity = vector[2];
    if (!IsPlausibleAddress(s.begin) || s.end <= s.begin || s.capacity < s.end ||
        (s.end - s.begin) % kSkillSlotStride ||
        s.end - s.begin > skilllayout::kMaxSlots * kSkillSlotStride ||
        s.capacity - s.begin > skilllayout::kMaxSlots * kSkillSlotStride)
        throw std::runtime_error("Invalid skill-slot vector boundaries");
    const auto count = (s.end - s.begin) / kSkillSlotStride;
    for (size_t i = 0; i < count; ++i) {
        Slot slot;
        slot.index = i;
        slot.object = p.Read<uint64_t>(s.begin + i * kSkillSlotStride);
        if (slot.object) {
            if (!IsPlausibleAddress(slot.object)) throw std::runtime_error("Invalid slot object");
            slot.shared = slot.object + kSkillSharedPtrOff;
            std::array<uint64_t, 2> shared{};
            if (!p.ReadInto(slot.shared, shared.data(), sizeof(shared)))
                throw std::runtime_error("Slot shared_ptr unreadable");
            slot.ptr = shared[0]; slot.rep = shared[1];
            if ((slot.ptr == 0) != (slot.rep == 0))
                throw std::runtime_error("Inconsistent slot shared_ptr");
            if (slot.ptr) {
                if (!IsPlausibleAddress(slot.ptr) || !IsPlausibleAddress(slot.rep))
                    throw std::runtime_error("Invalid skill shared_ptr");
                slot.owner = p.Follow(slot.ptr + skilllayout::kSkillOwner);
                slot.ownerEntity = p.Follow(slot.owner + skilllayout::kOwnerEntity);
                if (slot.ownerEntity != s.character.entity)
                    throw std::runtime_error("Skill owner differs from current character entity");
                slot.id = p.Read<uint32_t>(slot.ptr + skilllayout::kRuntimeSkillId);
                slot.uses = p.Read<uint32_t>(slot.rep + kRefUsesOff);
                slot.weak = p.Read<uint32_t>(slot.rep + kRefWeakOff);
                if (!slot.id || !slot.uses || slot.uses > 1000000 || !slot.weak)
                    throw std::runtime_error("Uninitialized skill ID/refcounts");
                slot.levelRow = p.Read<uint64_t>(slot.ptr + skilllayout::kLevelRecord);
                if (!slot.levelRow) {
                    // Move has valid ownership/ID/refcounts, but no level or
                    // GrantedEffects chain. Identify its actual action code;
                    // never assume every slot zero (or every null row) is Move.
                    slot.baseRow = p.Follow(slot.ptr + skilllayout::kBaseSkillRecord);
                    slot.baseIdText = p.Follow(slot.baseRow + skilllayout::kBaseSkillIdText);
                    slot.internalId = Utf8(p.WideText(slot.baseIdText));
                    if (slot.internalId != "Move")
                        throw std::runtime_error("Missing level record in slot " + std::to_string(i) +
                            " (action " + slot.internalId + ")");
                    slot.movement = true;
                    slot.localizedName = "移動（内置动作）";
                    s.slots.push_back(std::move(slot));
                    continue;
                }
                if (!IsPlausibleAddress(slot.levelRow))
                    throw std::runtime_error("Invalid level-record pointer in slot " + std::to_string(i));
                slot.effectRow = p.Follow(slot.levelRow + skilllayout::kGrantedEffectRecord);
                slot.idText = p.Follow(slot.effectRow + skilllayout::kGrantedEffectIdText);
                slot.internalId = Utf8(p.WideText(slot.idText));
                if (!std::all_of(slot.internalId.begin(), slot.internalId.end(), [](char ch) {
                    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                           (ch >= '0' && ch <= '9') || ch == '_';
                })) throw std::runtime_error("Invalid GrantedEffects internal ID");
                slot.activeRow = p.Follow(slot.effectRow + skilllayout::kActiveSkillRecord);
                slot.activeCodeText = p.Follow(slot.activeRow + skilllayout::kActiveSkillCodeText);
                slot.localizedNameText = p.Follow(slot.activeRow + skilllayout::kLocalizedNameText);
                slot.activeSkillCode = Utf8(p.WideText(slot.activeCodeText));
                slot.localizedName = Utf8(p.WideText(slot.localizedNameText));
                if (!std::all_of(slot.activeSkillCode.begin(), slot.activeSkillCode.end(), [](char ch) {
                    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                           (ch >= '0' && ch <= '9') || ch == '_';
                })) throw std::runtime_error("Invalid ActiveSkills internal code");
            }
        }
        s.slots.push_back(slot);
    }
    return s;
}

// Ignore volatile reference counts but compare all ownership, identity and bounds.
std::string Identity(const Snapshot& s) {
    std::ostringstream out;
    out << s.character.entity << ':' << s.character.actor << ':' << s.character.name
        << ':' << s.global << ':' << s.container << ':' << s.begin << ':' << s.end
        << ':' << s.capacity;
    for (const auto& slot : s.slots) out << '|' << slot.index << ':' << slot.object
        << ':' << slot.ptr << ':' << slot.rep << ':' << slot.owner << ':' << slot.ownerEntity << ':' << slot.id
        << ':' << slot.levelRow << ':' << slot.effectRow << ':' << slot.idText
        << ':' << slot.baseRow << ':' << slot.baseIdText << ':' << slot.movement
        << ':' << slot.internalId << ':' << slot.activeRow << ':' << slot.activeCodeText
        << ':' << slot.localizedNameText << ':' << slot.activeSkillCode << ':' << slot.localizedName;
    return out.str();
}

} // namespace

namespace poe {
bool ReadCharacterName(const mem::GameMemory& memory, uint64_t moduleBase,
                       std::string& name, std::string& err)
{
    name.clear(); err.clear();
    try {
        const Reader reader{memory, moduleBase};
        const auto first = ReadCharacter(reader, false);
        const auto second = ReadCharacter(reader, false);
        if (first.entity != second.entity || first.name != second.name || second.name.empty()) {
            err = "Local character changed during name read"; return false;
        }
        for (const unsigned char ch : second.name) {
            if (ch < 0x20 || ch == 0x7F) { err = "Invalid character name"; return false; }
        }
        name = second.name;
        return true;
    } catch (const std::exception& e) {
        err = e.what(); return false;
    }
}

bool ReadSkillSlots(const mem::GameMemory& memory, uint64_t moduleBase,
                   SkillSlotsSnapshot& out, std::string& err)
{
    out = {};
    err.clear();
    const Reader reader{memory, moduleBase};
    std::array<uint8_t, sizeof(skilllayout::kIdInstruction)> code{};
    if (!reader.ReadInto(moduleBase + skilllayout::kIdInstructionRva, code.data(), code.size()) ||
        !std::equal(code.begin(), code.end(), std::begin(skilllayout::kIdInstruction))) {
        err = "技能 ID 指令指纹不匹配，请重新校准游戏版本";
        return false;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        try {
            const auto first = Capture(reader);
            const auto second = Capture(reader);
            if (Identity(first) != Identity(second)) {
                err = "技能槽或角色正在变化，请稍后刷新";
                continue;
            }
            SkillSlotsSnapshot result;
            result.pid = memory.Driver().Pid();
            result.playerEntity = second.character.entity;
            result.characterName = second.character.name;
            for (const auto& slot : second.slots) {
                result.slots.push_back({static_cast<uint32_t>(slot.index), slot.ptr != 0,
                    slot.id, slot.internalId, slot.localizedName, slot.movement});
            }
            out = std::move(result);
            err.clear();
            return true;
        } catch (const std::exception& e) {
            err = std::string("技能槽读取失败：") + e.what();
        }
    }
    return false;
}

std::string FormatSkillId(uint32_t skillId)
{
    char id[11];
    std::snprintf(id, sizeof(id), "0x%08X", skillId);
    return id;
}

std::string SkillSlotsToTsv(const SkillSlotsSnapshot& snapshot)
{
    std::string text = "技能槽ID\t技能内部标识\t繁体中文名\t技能ID\n";
    for (const auto& slot : snapshot.slots) {
        text += std::to_string(slot.slotId) + "\t";
        if (slot.occupied) {
            text += slot.internalId + "\t" + slot.localizedName + "\t" + FormatSkillId(slot.skillId);
        } else {
            text += "(空槽)\t\t";
        }
        text += '\n';
    }
    return text;
}
} // namespace poe
