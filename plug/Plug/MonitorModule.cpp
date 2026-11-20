#include "MonitorModule.h"
#include <tlhelp32.h>
#include <cstring>
#include <cstdio>
#include <limits>
#include <vector>

namespace monitorimage {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct View {
    void* value = nullptr;
    ~View() { if (value) UnmapViewOfFile(value); }
};

std::string Utf8(const std::wstring& value)
{
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), int(value.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), int(value.size()), result.data(), bytes, nullptr, nullptr);
    return result;
}

class Reader {
public:
    Reader(HANDLE process, Identity& identity) : process_(process), image_(identity) {}
    bool Fail(const char* stage, DWORD error = ERROR_BAD_EXE_FORMAT)
    {
        // Keep the earliest failed read/validation, before another API overwrites LastError.
        if (image_.stage.empty()) { image_.stage = stage; image_.error = error; }
        return false;
    }
    bool Read(uint64_t rva, void* output, size_t bytes, const char* stage)
    {
        const uint64_t limit = image_.headers ? image_.imageSize : 0x100000;
        if (rva > limit || bytes > limit - rva ||
            image_.base > (std::numeric_limits<uintptr_t>::max)() - rva)
            return Fail(stage);
        SIZE_T got = 0;
        if (!ReadProcessMemory(process_, reinterpret_cast<const void*>(image_.base + rva), output, bytes, &got))
            return Fail(stage, GetLastError());
        return got == bytes || Fail(stage, ERROR_PARTIAL_COPY);
    }
    template<class T> bool Read(uint64_t rva, T& output, const char* stage)
    { return Read(rva, &output, sizeof(output), stage); }

    void Inspect()
    {
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!Read(0, dos, "read-dos")) return;
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000) {
            Fail("validate-dos"); return;
        }
        if (!Read(dos.e_lfanew, nt, "read-nt")) return;
        if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
            nt.OptionalHeader.SizeOfImage < uint64_t(dos.e_lfanew) + sizeof(nt) ||
            nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG) {
            Fail("validate-nt64"); return;
        }
        image_.headers = true;
        image_.timestamp = nt.FileHeader.TimeDateStamp;
        image_.imageSize = nt.OptionalHeader.SizeOfImage;
        InspectExports(nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]);
        InspectCodeView(nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG]);
    }
private:
    void InspectExports(const IMAGE_DATA_DIRECTORY& directory)
    {
        if (!directory.VirtualAddress || !directory.Size) { image_.exports = true; return; }
        IMAGE_EXPORT_DIRECTORY table{};
        if (!Read(directory.VirtualAddress, table, "read-exports")) return;
        if (directory.Size < sizeof(table) || uint64_t(directory.VirtualAddress) + directory.Size > image_.imageSize ||
            table.NumberOfNames > 4096 || table.NumberOfFunctions > 4096 ||
            table.NumberOfNames > table.NumberOfFunctions) { Fail("validate-exports"); return; }
        std::vector<DWORD> names(table.NumberOfNames), functions(table.NumberOfFunctions);
        std::vector<WORD> ordinals(table.NumberOfNames);
        if ((!names.empty() && (!Read(table.AddressOfNames, names.data(), names.size() * sizeof(DWORD), "read-export-names") ||
             !Read(table.AddressOfNameOrdinals, ordinals.data(), ordinals.size() * sizeof(WORD), "read-export-ordinals"))) ||
            (!functions.empty() && !Read(table.AddressOfFunctions, functions.data(), functions.size() * sizeof(DWORD), "read-export-functions"))) return;
        for (size_t index = 0; index < names.size(); ++index) {
            char name[128]{};
            size_t length = 0;
            for (; length < sizeof(name); ++length) {
                if (!Read(uint64_t(names[index]) + length, name[length], "read-export-name")) return;
                if (!name[length]) break;
            }
            if (length == sizeof(name) || ordinals[index] >= functions.size()) { Fail("validate-export-name"); return; }
            const DWORD rva = functions[ordinals[index]];
            const bool isInterface = std::strcmp(name, monitorabi::kExportName) == 0;
            const bool isShutdown = std::strcmp(name, "MonitorShutdown") == 0;
            if (!isInterface && !isShutdown) continue;
            if (!rva || rva >= image_.imageSize ||
                (rva >= directory.VirtualAddress && uint64_t(rva) < uint64_t(directory.VirtualAddress) + directory.Size)) {
                Fail("validate-export-rva"); return;
            }
            if (isInterface) {
                image_.interfaceFound = true;
                if (!Read(rva, image_.interfaceInfo, "read-interface")) return;
                image_.interfaceRead = true;
            }
            if (isShutdown) image_.shutdownRva = rva;
        }
        image_.exports = true;
    }
    void InspectCodeView(const IMAGE_DATA_DIRECTORY& directory)
    {
        if (!directory.VirtualAddress || !directory.Size) return;
        if (directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY) || directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY) > 128) {
            Fail("validate-debug-directory"); return;
        }
        struct CodeView { DWORD signature; GUID guid; DWORD age; };
        static_assert(sizeof(CodeView) == 24, "RSDS layout");
        for (uint32_t offset = 0; offset < directory.Size; offset += sizeof(IMAGE_DEBUG_DIRECTORY)) {
            IMAGE_DEBUG_DIRECTORY debug{};
            if (!Read(uint64_t(directory.VirtualAddress) + offset, debug, "read-debug-directory")) return;
            if (debug.Type != IMAGE_DEBUG_TYPE_CODEVIEW || debug.SizeOfData < sizeof(CodeView)) continue;
            CodeView code{};
            if (!Read(debug.AddressOfRawData, code, "read-codeview")) return;
            if (code.signature != 0x53445352) continue;
            const GUID empty{};
            if (!code.age || std::memcmp(&code.guid, &empty, sizeof(GUID)) == 0) continue;
            image_.codeView = true;
            image_.pdbGuid = code.guid;
            image_.pdbAge = code.age;
            return;
        }
    }
    HANDLE process_;
    Identity& image_;
};
} // namespace

Module Find(DWORD pid, const wchar_t* fileName)
{
    Module result;
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)};
    if (snapshot.value == INVALID_HANDLE_VALUE) { result.error = GetLastError(); return result; }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.value, &entry)) { result.error = GetLastError(); return result; }
    do {
        if (_wcsicmp(entry.szModule, fileName) != 0) continue;
        ++result.matches;
        if (!result.base) { result.base = reinterpret_cast<uint64_t>(entry.modBaseAddr); result.path = entry.szExePath; }
    } while (Module32NextW(snapshot.value, &entry));
    const DWORD error = GetLastError();
    if (error != ERROR_NO_MORE_FILES) result.error = error;
    return result;
}

Identity ReadLocal(const std::wstring& path)
{
    Identity result;
    result.path = path;
    Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) { result.stage = "open-local-file"; result.error = GetLastError(); return result; }
    // Map the requested file directly, without loader module-name reuse or executing DllMain.
    Handle mapping{CreateFileMappingW(file.value, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr)};
    if (!mapping.value) { result.stage = "map-local-image"; result.error = GetLastError(); return result; }
    View view{MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, 0)};
    if (!view.value) { result.stage = "view-local-image"; result.error = GetLastError(); return result; }
    result.base = reinterpret_cast<uint64_t>(view.value);
    Reader(GetCurrentProcess(), result).Inspect();
    // The local mapping is transient; do not include its address in deduplicated diagnostics.
    result.base = 0;
    return result;
}

Identity ReadRemote(HANDLE process, const Module& module)
{
    Identity result;
    result.path = module.path;
    result.base = module.base;
    Reader(process, result).Inspect();
    return result;
}

std::string Describe(const Identity& image)
{
    char line[640];
    const auto& id = image.pdbGuid;
    const auto& info = image.interfaceInfo;
    std::snprintf(line, sizeof(line),
        " base=0x%llX PE=%s timestamp=0x%08X imageSize=0x%X CodeView=%s "
        "PDB=%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X age=%u "
        "interface=%s magic=0x%08X bytes=%u abi=%u managed=%u combo=%u caps=0x%X layout=0x%llX "
        "shutdownRva=0x%X stage=%s err=%lu",
        static_cast<unsigned long long>(image.base), image.headers ? "ok" : "unreadable", image.timestamp, image.imageSize,
        image.codeView ? "ok" : "missing", static_cast<unsigned>(id.Data1), id.Data2, id.Data3,
        id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3], id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7], image.pdbAge,
        image.interfaceRead ? "read" : image.interfaceFound ? "unreadable" : "missing",
        info.magic, info.bytes, info.abiVersion, info.managedVersion, info.comboVersion, info.capabilities,
        static_cast<unsigned long long>(info.wireLayout), image.shutdownRva, image.stage.empty() ? "ok" : image.stage.c_str(), image.error);
    return "path=\"" + Utf8(image.path) + "\"" + line;
}

Check Compare(const Identity& local, const Identity& remote)
{
    Check result;
    result.sameBuild = local.headers && remote.headers && local.codeView && remote.codeView &&
        local.timestamp == remote.timestamp && local.imageSize == remote.imageSize &&
        local.pdbAge == remote.pdbAge && std::memcmp(&local.pdbGuid, &remote.pdbGuid, sizeof(GUID)) == 0;
    if (!remote.headers || !remote.exports) {
        result.reason = "remote-image-unreadable";
    } else if (remote.interfaceFound) {
        if (!remote.interfaceRead) result.reason = "remote-interface-unreadable";
        else if (monitorabi::Compatible(remote.interfaceInfo)) {
            result.compatibility = Compatibility::Compatible;
            result.reason = result.sameBuild ? "interface-compatible/same-build" : "interface-compatible/different-build";
        } else {
            result.compatibility = Compatibility::Incompatible;
            result.reason = "interface-incompatible";
        }
    } else if (result.sameBuild && local.exports && !local.interfaceFound) {
        // Legacy modules have no ABI export: only the same CodeView build may reconnect.
        result.compatibility = Compatibility::Compatible;
        result.reason = "legacy/exact-build-fallback";
    } else if (!local.headers || !local.codeView || !remote.codeView) {
        result.reason = "legacy-build-identity-unavailable";
    } else {
        result.compatibility = Compatibility::Incompatible;
        result.reason = "legacy-different-build/no-interface";
    }
    const monitorabi::Info expected{};
    char expectation[192];
    std::snprintf(expectation, sizeof(expectation),
        "expected abi=%u managed=%u combo=%u caps=0x%X layout=0x%llX sameBuild=%s result=%s",
        expected.abiVersion, expected.managedVersion, expected.comboVersion, expected.capabilities,
        static_cast<unsigned long long>(expected.wireLayout), result.sameBuild ? "yes" : "no", result.reason.c_str());
    result.diagnostic = std::string(expectation) + "\nlocal " + Describe(local) + "\nremote " + Describe(remote);
    return result;
}
} // namespace monitorimage
