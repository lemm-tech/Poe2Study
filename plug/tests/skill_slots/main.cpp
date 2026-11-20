#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../Common/GameBuild.h"
#include "ProbeOffsets.h"

static_assert(sizeof(void*) == 8 && sizeof(wchar_t) == 2, "Windows x64 required");

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

// Strict JSON escaping, including error strings and redirected Windows paths.
std::string Json(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') out << '\\' << ch;
        else if (ch < 0x20) out << "\\u00" << std::hex << std::setw(2)
                               << std::setfill('0') << static_cast<unsigned>(ch);
        else out << ch;
    }
    out << '"';
    return out.str();
}

struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

bool GameName(const wchar_t* name) {
    return _wcsicmp(name, L"PathOfExile.exe") == 0 ||
           _wcsicmp(name, L"PathOfExileSteam.exe") == 0 ||
           _wcsicmp(name, L"PathOfExile_x64.exe") == 0;
}

DWORD FindGame() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Process snapshot failed");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::vector<DWORD> found;
    if (Process32FirstW(snapshot.value, &entry)) do {
        if (GameName(entry.szExeFile)) found.push_back(entry.th32ProcessID);
    } while (Process32NextW(snapshot.value, &entry));
    if (found.empty()) throw std::runtime_error("Game not running; enter a character first");
    if (found.size() != 1) throw std::runtime_error("Multiple games found; select --pid explicitly");
    return found.front();
}

class Process {
    Handle handle;
public:
    DWORD pid;
    uint64_t base = 0;
    std::wstring path;
    explicit Process(DWORD id) : handle(OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, id)), pid(id) {
        if (!handle.value) throw std::runtime_error("OpenProcess failed, Win32=" +
            std::to_string(GetLastError()));
        std::array<wchar_t, 32768> filename{};
        DWORD length = static_cast<DWORD>(filename.size());
        if (!QueryFullProcessImageNameW(handle.value, 0, filename.data(), &length))
            throw std::runtime_error("QueryFullProcessImageName failed");
        path.assign(filename.data(), length);
        if (!GameName(std::filesystem::path(path).filename().c_str()))
            throw std::runtime_error("Selected PID is not a supported game executable");
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid));
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        if (snapshot.value == INVALID_HANDLE_VALUE || !Module32FirstW(snapshot.value, &module))
            throw std::runtime_error("Module snapshot failed");
        do {
            if (_wcsicmp(module.szExePath, path.c_str()) == 0) {
                base = reinterpret_cast<uint64_t>(module.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot.value, &module));
        if (!base) throw std::runtime_error("Main module not found");
        std::string error;
        if (!gamebuild::Verify(base, [this](uint64_t a, void* b, size_t n) {
            return ReadInto(a, b, n);
        }, error)) throw std::runtime_error(error);
        std::array<uint8_t, sizeof(probe::kIdInstruction)> code{};
        if (!ReadInto(base + probe::kIdInstructionRva, code.data(), code.size()) ||
            !std::equal(code.begin(), code.end(), std::begin(probe::kIdInstruction)))
            throw std::runtime_error("Runtime skill-ID instruction fingerprint mismatch");
    }
    bool ReadInto(uint64_t address, void* output, size_t size) const {
        if (!size || size > 1024 * 1024 || !IsPlausibleAddress(address) ||
            !IsPlausibleAddress(address + size - 1)) return false;
        SIZE_T done = 0;
        return ReadProcessMemory(handle.value, reinterpret_cast<LPCVOID>(address),
                                 output, size, &done) && done == size;
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
        auto length = Read<uint64_t>(address + 0x10);
        auto capacity = Read<uint64_t>(address + 0x18);
        if (!length || length > 256 || capacity < length || capacity > 1024 * 1024)
            throw std::runtime_error("Invalid character-name string");
        auto data = capacity > 7 ? Follow(address) : address;
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

Character ReadCharacter(const Process& p) {
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
        if (name == "Actor") result.actor = p.Follow(array + index * kComponentPtrSize);
        if (name == "Player") result.name = p.StdWideText(
            p.Follow(array + index * kComponentPtrSize) + kPlayer_NameOff);
    }
    if (!result.actor || result.name.empty())
        throw std::runtime_error("Local character/Actor unavailable; enter the game world");
    if (p.Follow(result.actor + probe::kComponentOwnerEntity) != result.entity)
        throw std::runtime_error("Actor does not belong to the local character");
    return result;
}

struct Slot {
    size_t index = 0;
    uint64_t object = 0, shared = 0, ptr = 0, rep = 0, owner = 0, ownerEntity = 0;
    uint64_t levelRow = 0, effectRow = 0, idText = 0;
    uint64_t activeRow = 0, activeCodeText = 0, localizedNameText = 0;
    uint32_t id = 0, uses = 0, weak = 0;
    std::string internalId, activeSkillCode, localizedName;
};

struct Snapshot {
    Character character;
    uint64_t global = 0, container = 0, begin = 0, end = 0, capacity = 0;
    std::vector<Slot> slots;
};

Snapshot Capture(const Process& p) {
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
        s.end - s.begin > probe::kMaxSlots * kSkillSlotStride ||
        s.capacity - s.begin > probe::kMaxSlots * kSkillSlotStride)
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
                slot.owner = p.Follow(slot.ptr + probe::kSkillOwner);
                slot.ownerEntity = p.Follow(slot.owner + probe::kOwnerEntity);
                if (slot.ownerEntity != s.character.entity)
                    throw std::runtime_error("Skill owner differs from current character entity");
                slot.id = p.Read<uint32_t>(slot.ptr + probe::kRuntimeSkillId);
                slot.uses = p.Read<uint32_t>(slot.rep + kRefUsesOff);
                slot.weak = p.Read<uint32_t>(slot.rep + kRefWeakOff);
                if (!slot.id || !slot.uses || slot.uses > 1000000 || !slot.weak)
                    throw std::runtime_error("Uninitialized skill ID/refcounts");
                slot.levelRow = p.Follow(slot.ptr + probe::kLevelRecord);
                slot.effectRow = p.Follow(slot.levelRow + probe::kGrantedEffectRecord);
                slot.idText = p.Follow(slot.effectRow + probe::kGrantedEffectIdText);
                slot.internalId = Utf8(p.WideText(slot.idText));
                if (!std::all_of(slot.internalId.begin(), slot.internalId.end(), [](char ch) {
                    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                           (ch >= '0' && ch <= '9') || ch == '_';
                })) throw std::runtime_error("Invalid GrantedEffects internal ID");
                slot.activeRow = p.Follow(slot.effectRow + probe::kActiveSkillRecord);
                slot.activeCodeText = p.Follow(slot.activeRow + probe::kActiveSkillCodeText);
                slot.localizedNameText = p.Follow(slot.activeRow + probe::kLocalizedNameText);
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
        << ':' << slot.internalId << ':' << slot.activeRow << ':' << slot.activeCodeText
        << ':' << slot.localizedNameText << ':' << slot.activeSkillCode << ':' << slot.localizedName;
    return out.str();
}

std::string Serialize(const Process& p, const Snapshot& s) {
    SYSTEMTIME time{}; GetSystemTime(&time);
    char stamp[40]{};
    std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02uT%02u:%02u:%02uZ",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    std::ostringstream out;
    out << "{\n  \"schemaVersion\": 2,\n  \"capturedAtUtc\": " << Json(stamp)
        << ",\n  \"success\": true,\n  \"readOnly\": true,\n  \"consistentDoubleRead\": true,"
        << "\n  \"pid\": " << p.pid << ",\n  \"executable\": " << Json(Utf8(p.path))
        << ",\n  \"moduleBase\": " << Json(Hex(p.base))
        << ",\n  \"buildVerified\": true,\n  \"gameTimeDateStamp\": " << Json(Hex(kGameTimeDateStamp))
        << ",\n  \"gameImageSize\": " << Json(Hex(kGameImageSize))
        << ",\n  \"runtimeIdSource\": \"uint32(skillPtr+0x40)\","
        << "\n  \"runtimeIdInstructionRva\": " << Json(Hex(probe::kIdInstructionRva))
        << ",\n  \"runtimeIdInstructionBytes\": \"8B 51 40 48 8B CE\","
        << "\n  \"localizedNameSource\": \"ActiveSkills.Name UTF16Z; follows skillPtr+0x58, +0, +0x4F, +8\","
        << "\n  \"localizedNameLocale\": \"current client\","
        << "\n  \"character\": {\"name\": " << Json(s.character.name)
        << ", \"entity\": " << Json(Hex(s.character.entity))
        << ", \"actor\": " << Json(Hex(s.character.actor)) << "},"
        << "\n  \"container\": {\"global\": " << Json(Hex(s.global))
        << ", \"owner\": " << Json(Hex(s.container)) << ", \"begin\": " << Json(Hex(s.begin))
        << ", \"end\": " << Json(Hex(s.end)) << ", \"capacity\": " << Json(Hex(s.capacity))
        << ", \"count\": " << s.slots.size() << "},\n  \"slots\": [\n";
    for (size_t i = 0; i < s.slots.size(); ++i) {
        const auto& slot = s.slots[i];
        out << "    {\"slot\": " << slot.index << ", \"state\": " << Json(slot.ptr ? "assigned" : "empty")
            << ", \"slotObject\": " << Json(Hex(slot.object)) << ", \"sharedAddress\": " << Json(Hex(slot.shared))
            << ", \"skillPtr\": " << Json(Hex(slot.ptr)) << ", \"skillRep\": " << Json(Hex(slot.rep));
        if (slot.ptr) out << ", \"skillId\": " << slot.id << ", \"skillIdHex\": " << Json(Hex(slot.id))
            << ", \"internalId\": " << Json(slot.internalId) << ", \"ownerObject\": " << Json(Hex(slot.owner))
            << ", \"ownerEntity\": " << Json(Hex(slot.ownerEntity))
            << ", \"uses\": " << slot.uses << ", \"weak\": " << slot.weak
            << ", \"levelRecord\": " << Json(Hex(slot.levelRow))
            << ", \"grantedEffectRecord\": " << Json(Hex(slot.effectRow))
            << ", \"internalIdAddress\": " << Json(Hex(slot.idText))
            << ", \"activeSkillRecord\": " << Json(Hex(slot.activeRow))
            << ", \"activeSkillCode\": " << Json(slot.activeSkillCode)
            << ", \"activeSkillCodeAddress\": " << Json(Hex(slot.activeCodeText))
            << ", \"localizedName\": " << Json(slot.localizedName)
            << ", \"localizedNameAddress\": " << Json(Hex(slot.localizedNameText));
        else out << ", \"skillId\": null, \"skillIdHex\": null, \"internalId\": null,"
                 << " \"activeSkillCode\": null, \"localizedName\": null";
        out << '}' << (i + 1 == s.slots.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    return out.str();
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        DWORD pid = 0;
        std::filesystem::path output;
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if (arg == L"--help") {
                std::cout << "SkillSlotsProbe.exe [--pid PID] [--out snapshot.json]\n"
                    << "Read-only skill slots, uint32 runtime IDs, internal IDs and client-localized names.\n"
                    << "No injection, game calls, key input or production-script integration.\n";
                return 0;
            }
            if ((arg != L"--pid" && arg != L"--out") || i + 1 >= argc)
                throw std::runtime_error("Unknown/incomplete option; use --help");
            std::wstring value = argv[++i];
            if (arg == L"--out") {
                if (value.empty()) throw std::runtime_error("Empty --out path");
                output = value;
            } else {
                if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
                    throw std::runtime_error("Invalid --pid");
                auto id = std::stoull(value);
                if (!id || id > MAXDWORD) throw std::runtime_error("Invalid --pid");
                pid = static_cast<DWORD>(id);
            }
        }
        Process process(pid ? pid : FindGame());
        Snapshot snapshot;
        bool stable = false;
        std::string error;
        for (int attempt = 0; attempt < 3 && !stable; ++attempt) {
            try {
                auto first = Capture(process);
                auto second = Capture(process);
                if (Identity(first) != Identity(second))
                    throw std::runtime_error("Skill slots/character changed while reading");
                snapshot = std::move(second);
                stable = true;
            } catch (const std::exception& ex) {
                error = ex.what();
                if (attempt < 2) Sleep(25);
            }
        }
        if (!stable) throw std::runtime_error(error);
        std::cout << "Character: " << snapshot.character.name << "  PID: " << process.pid
                  << "  slots: " << snapshot.slots.size() << "\n"
                  << "slot  skillId(dec)  skillId(hex)  internalId | localizedName\n";
        size_t assigned = 0;
        std::vector<uint64_t> unique;
        for (const auto& slot : snapshot.slots) {
            std::cout << std::left << std::setw(6) << slot.index;
            if (slot.ptr) {
                ++assigned;
                if (std::find(unique.begin(), unique.end(), slot.ptr) == unique.end()) unique.push_back(slot.ptr);
                std::cout << std::setw(14) << slot.id << std::setw(14) << Hex(slot.id)
                          << slot.internalId << " | " << slot.localizedName;
            } else std::cout << "<empty>";
            std::cout << '\n';
        }
        if (!output.empty()) {
            auto full = std::filesystem::absolute(output);
            if (std::filesystem::exists(full))
                throw std::runtime_error("Output already exists; select a new --out path");
            std::filesystem::create_directories(full.parent_path());
            std::ofstream file(full, std::ios::binary);
            file.exceptions(std::ios::badbit | std::ios::failbit);
            file << Serialize(process, snapshot);
            file.close();
            std::cout << "Snapshot: " << Utf8(full.wstring()) << '\n';
        }
        std::cout << "PASS: build + ID instruction + character ownership + localized names + stable double read; "
                  << assigned << " assigned slots, " << unique.size() << " unique skill objects.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: " << ex.what() << '\n';
        return 1;
    }
}
