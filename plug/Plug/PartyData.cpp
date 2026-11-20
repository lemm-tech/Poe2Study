#include "PartyData.h"
#include "SkillSlots.h"
#include <array>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace poe {
bool PartyMember::operator==(const PartyMember& b) const {
    return name == b.name && account == b.account && mapName == b.mapName && mapId == b.mapId &&
        areaKey == b.areaKey && mapStatus == b.mapStatus && self == b.self;
}
namespace {
using Bounds = std::array<uint64_t, 3>;
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("队伍名称包含无效字符");
    std::string result(size_t(n), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), n, nullptr, nullptr))
        throw std::runtime_error("队伍名称转换失败");
    return result;
}
bool SameText(const std::wstring& a, const std::wstring& b) {
    return a.empty() || b.empty() ? a == b :
        CompareStringOrdinal(a.data(), int(a.size()), b.data(), int(b.size()), TRUE) == CSTR_EQUAL;
}
template<class T> T Field(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) throw std::runtime_error("队伍记录字段越界");
    T value{}; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value;
}
struct Reader {
    const mem::GameMemory& memory;
    const std::atomic_bool* stop;
    uint64_t deadline;
    void Check() const {
        if (stop && stop->load()) throw std::runtime_error("队友列表读取已取消");
        if (GetTickCount64() >= deadline) throw std::runtime_error("队友列表读取超时，请稍后重新获取");
    }
    void Raw(uint64_t address, void* out, size_t size) const {
        Check();
        if (!size) return;
        if (size > 16 * 1024 * 1024 || !memory.IsPlausible(address) ||
            !memory.IsPlausible(address + size - 1) || !memory.ReadInto(address, out, size))
            throw std::runtime_error("队伍数据暂时不可读，请进入场景后重新获取");
    }
    template<class T> T Read(uint64_t address) const { T value{}; Raw(address, &value, sizeof(value)); return value; }
    uint64_t Pointer(uint64_t slot) const {
        const auto p = Read<uint64_t>(slot);
        if (!memory.IsPlausible(p)) throw std::runtime_error("队伍数据指针不可用，请稍后重新获取");
        return p;
    }
    Bounds Header(uint64_t address, uint64_t stride, size_t limit) const {
        const auto h = Read<Bounds>(address);
        if (h == Bounds{}) return h;
        if (!memory.IsPlausible(h[0]) || h[0] > h[1] || h[1] > h[2] || !memory.IsPlausible(h[2]) ||
            (h[1] - h[0]) % stride || (h[2] - h[0]) % stride || (h[2] - h[0]) / stride > limit)
            throw std::runtime_error("队伍数据容器校验失败，请稍后重新获取");
        return h;
    }
    std::vector<uint8_t> Bytes(const Bounds& h) const {
        std::vector<uint8_t> data(size_t(h[1] - h[0])); Raw(h[0], data.data(), data.size()); return data;
    }
    std::wstring WString(const std::vector<uint8_t>& data, size_t offset) const {
        const auto length = Field<uint64_t>(data, offset + kStdStringLengthOff);
        const auto capacity = Field<uint64_t>(data, offset + kStdStringMaxLenOff);
        if (length > 256 || capacity < length || capacity > 1024 * 1024)
            throw std::runtime_error("队伍名称字符串校验失败");
        std::wstring text(size_t(length), L'\0');
        if (capacity > kStdStringSsoLimit) Raw(Field<uint64_t>(data, offset + kStdStringDataOff), text.data(), length * sizeof(wchar_t));
        else {
            if (offset > data.size() || length * sizeof(wchar_t) > data.size() - offset)
                throw std::runtime_error("队伍名称字符串越界");
            if (length) std::memcpy(text.data(), data.data() + offset, length * sizeof(wchar_t));
        }
        for (auto ch : text) if (ch < 0x20) throw std::runtime_error("队伍名称包含控制字符");
        return text;
    }
    std::wstring Text(uint64_t address) const {
        std::wstring result;
        for (size_t i = 0; i < 256; ++i) {
            const auto ch = Read<wchar_t>(address + i * sizeof(wchar_t));
            if (!ch) {
                if (result.empty()) throw std::runtime_error("地图名称为空");
                return result;
            }
            if (ch < 0x20) throw std::runtime_error("地图名称包含控制字符");
            result += ch;
        }
        throw std::runtime_error("地图名称长度超限");
    }
};
struct Sample {
    uint64_t owner = 0, state = 0, world = 0, table = 0, storage = 0;
    Bounds roster{}, profiles{}, rows{}, lookup{};
    PartySnapshot value;
    bool Same(const Sample& b) const {
        return owner == b.owner && state == b.state && world == b.world && table == b.table && storage == b.storage &&
            roster == b.roster && profiles == b.profiles && rows == b.rows && lookup == b.lookup &&
            value.selfName == b.value.selfName && value.members == b.value.members;
    }
};
Sample ReadSample(const Reader& r, uint64_t base) {
    Sample s;
    const auto global = r.Pointer(base + BaseOffset);
    s.owner = r.Pointer(global + kSkillStopChainOff1);
    s.state = r.Pointer(r.Pointer(s.owner + kSkillStopChainOff2));
    if (r.Pointer(s.state + kPacketProtocolOff) != s.owner) throw std::runtime_error("游戏会话已变化，请重新获取");
    std::string nameError;
    r.Check();
    if (!ReadCharacterName(r.memory, base, s.value.selfName, nameError))
        throw std::runtime_error("本地角色名称读取失败：" + nameError);
    s.roster = r.Header(s.owner + kPartyRosterBeginOff, kPartyRosterStride, 64);
    s.profiles = r.Header(s.owner + kPartyProfilesBeginOff, kPartyProfileStride, 4096);
    const auto roster = r.Bytes(s.roster), profiles = r.Bytes(s.profiles);
    s.world = r.Pointer(s.owner + kPacketWorldOff);
    s.table = r.Pointer(s.world + kWorldAreaTableOff);
    if (r.Text(r.Pointer(s.table + kWorldAreaTableNameOff)) != L"Data/Balance/WorldAreas.dat")
        throw std::runtime_error("地图数据表校验失败");
    s.storage = r.Pointer(s.table + kWorldAreaStorageOff);
    s.rows = r.Header(s.storage + kWorldAreaRowsBeginOff, kWorldAreaRowStride, 16384);
    s.lookup = r.Header(s.storage + kWorldAreaLookupBeginOff, kWorldAreaLookupStride, 16384);
    const auto lookup = r.Bytes(s.lookup);
    std::unordered_map<uint16_t, uint64_t> areas;
    for (size_t i = 0; i < lookup.size(); i += kWorldAreaLookupStride) {
        const auto key = Field<uint16_t>(lookup, i + kWorldAreaLookupKeyOff);
        const auto row = Field<uint64_t>(lookup, i + kWorldAreaLookupRowOff);
        if (row < s.rows[0] || row >= s.rows[1] || (row - s.rows[0]) % kWorldAreaRowStride ||
            r.Read<uint16_t>(row + kWorldAreaNumericIdOff) != key || !areas.emplace(key, row).second)
            throw std::runtime_error("地图编号索引校验失败");
    }
    struct Profile { std::wstring account, name; uint16_t key; };
    std::vector<Profile> contacts;
    for (size_t i = 0; i < profiles.size(); i += kPartyProfileStride) {
        r.Check();
        contacts.push_back({r.WString(profiles, i + kPartyProfileAccountOff), r.WString(profiles, i + kPartyProfileNameOff),
            Field<uint16_t>(profiles, i + kPartyProfileAreaKeyOff)});
    }
    for (size_t i = 0; i < roster.size(); i += kPartyRosterStride) {
        const auto name = r.WString(roster, i + kPartyRosterNameOff), account = r.WString(roster, i + kPartyRosterAccountOff);
        PartyMember member;
        member.name = Utf8(name); member.account = Utf8(account); member.self = member.name == s.value.selfName;
        if (member.name.empty() || member.account.empty()) throw std::runtime_error("队伍成员身份尚未就绪");
        size_t matches = 0;
        for (const auto& profile : contacts) if (SameText(profile.name, name) && SameText(profile.account, account)) {
            ++matches; member.areaKey = profile.key;
        }
        if (matches == 1) {
            const auto row = areas.find(member.areaKey);
            member.mapStatus = PartyMapStatus::AreaUnavailable;
            if (member.areaKey && row != areas.end()) {
                member.mapId = Utf8(r.Text(r.Pointer(row->second + kWorldAreaIdOff)));
                member.mapName = Utf8(r.Text(r.Pointer(row->second + kWorldAreaNameOff)));
                for (auto ch : member.mapId) if (static_cast<unsigned char>(ch) < 0x21 || static_cast<unsigned char>(ch) > 0x7E)
                    throw std::runtime_error("地图 ID 格式校验失败");
                member.mapStatus = PartyMapStatus::Resolved;
            }
        } else {
            member.areaKey = 0;
            member.mapStatus = matches ? PartyMapStatus::ProfileAmbiguous : PartyMapStatus::ProfileMissing;
        }
        s.value.members.push_back(std::move(member));
    }
    // Validate each container again before accepting this sample.
    if (r.Pointer(r.Pointer(base + BaseOffset) + kSkillStopChainOff1) != s.owner ||
        r.Pointer(r.Pointer(s.owner + kSkillStopChainOff2)) != s.state ||
        r.Pointer(s.state + kPacketProtocolOff) != s.owner || r.Pointer(s.owner + kPacketWorldOff) != s.world ||
        r.Pointer(s.world + kWorldAreaTableOff) != s.table || r.Pointer(s.table + kWorldAreaStorageOff) != s.storage ||
        r.Read<Bounds>(s.owner + kPartyRosterBeginOff) != s.roster || r.Read<Bounds>(s.owner + kPartyProfilesBeginOff) != s.profiles ||
        r.Read<Bounds>(s.storage + kWorldAreaRowsBeginOff) != s.rows || r.Read<Bounds>(s.storage + kWorldAreaLookupBeginOff) != s.lookup)
        throw std::runtime_error("队伍或地图数据正在变化，请重新获取");
    return s;
}
} // namespace

bool ReadParty(const mem::GameMemory& memory, uint64_t base,
               PartySnapshot& out, std::string& error, const std::atomic_bool* stop) {
    out = {}; error.clear();
    try {
        const Reader r{memory, stop, GetTickCount64() + 8000};
        auto previous = ReadSample(r, base);
        for (int attempt = 0; attempt < 3; ++attempt) {
            auto current = ReadSample(r, base);
            if (previous.Same(current)) {
                r.Check();
                current.value.pid = memory.Driver().Pid(); current.value.sampledAt = GetTickCount64();
                out = std::move(current.value); return true;
            }
            previous = std::move(current);
        }
        error = "队伍或地图数据正在变化，请稍后重新获取";
    } catch (const std::exception& e) { error = e.what(); }
    return false;
}
} // namespace poe
