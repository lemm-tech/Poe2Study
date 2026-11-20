#pragma once
#include "GameOffsets.h"
#include "MonsterIdentity.h"
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace monster {
struct ConfiguredBoss { uint64_t row = 0; std::string metadata, name; };
// Memory supplies checked ReadInto/IsPlausible. Tests use the identical parser.
template<class Memory> class Reader {
    const Memory& memory;
    uint64_t base;
    std::map<uint64_t, std::vector<std::string>> definitions;
    using Components = std::map<std::string, uint64_t>;
    template<class T> T Read(uint64_t address) const {
        T value{};
        if (!memory.IsPlausible(address) || !memory.ReadInto(address, &value, sizeof(value)))
            throw std::runtime_error("怪物身份字段暂不可读");
        return value;
    }
    uint64_t Ptr(uint64_t address) const {
        const auto value = Read<uint64_t>(address);
        if (!memory.IsPlausible(value)) throw std::runtime_error("怪物身份指针无效");
        return value;
    }
    void Same(uint64_t address, uint64_t expected) const {
        if (Read<uint64_t>(address) != expected) throw std::runtime_error("怪物身份在读取期间变化");
    }
    std::string Ascii(uint64_t address) const {
        std::string value;
        for (unsigned i = 0; i < 96; ++i) {
            const char ch = Read<char>(address + i);
            if (!ch && !value.empty()) return value;
            if (ch < 0x20 || ch > 0x7E) break;
            value += ch;
        }
        throw std::runtime_error("怪物组件名称无效");
    }
    std::string Wide(uint64_t address) const {
        std::string value;
        for (unsigned i = 0; i < 512; ++i) {
            uint32_t cp = Read<uint16_t>(address + i * 2);
            if (!cp && !value.empty()) return value;
            if (cp < 0x20) break;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                const auto low = Read<uint16_t>(address + (++i) * 2);
                if (low < 0xDC00 || low > 0xDFFF) break;
                cp = 0x10000 + ((cp - 0xD800) << 10) + low - 0xDC00;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) break;
            if (cp < 0x80) value += char(cp);
            else if (cp < 0x800) { value += char(0xC0 | (cp >> 6)); value += char(0x80 | (cp & 63)); }
            else if (cp < 0x10000) {
                value += char(0xE0 | (cp >> 12)); value += char(0x80 | ((cp >> 6) & 63)); value += char(0x80 | (cp & 63));
            } else {
                value += char(0xF0 | (cp >> 18)); value += char(0x80 | ((cp >> 12) & 63));
                value += char(0x80 | ((cp >> 6) & 63)); value += char(0x80 | (cp & 63));
            }
        }
        throw std::runtime_error("怪物数据字符串无效");
    }
    void Table(uint64_t table, const char* expected) const {
        // The table stores a std::wstring at +8. Its first slot is a pointer
        // for these long, known table names (all exceed the SSO threshold).
        const auto object = table + monsterlayout::kTableName;
        const auto length = Read<uint64_t>(object + kStdStringLengthOff);
        const auto capacity = Read<uint64_t>(object + kStdStringMaxLenOff);
        if (length != std::strlen(expected) || capacity < length || capacity > 4096 || Wide(Ptr(object)) != expected)
            throw std::runtime_error("怪物数据表身份不匹配");
    }
    std::vector<uint64_t> Vector(uint64_t address, uint64_t stride, uint64_t limit) const {
        const auto begin = Read<uint64_t>(address), end = Read<uint64_t>(address + 8), cap = Read<uint64_t>(address + 16);
        if (!begin && !end && !cap) return {};
        if (!memory.IsPlausible(begin) || end < begin || cap < end || (end - begin) % stride ||
            (cap - begin) % stride || cap - begin > limit * stride) throw std::runtime_error("怪物词缀数组范围无效");
        std::vector<uint64_t> result;
        for (auto entry = begin; entry < end; entry += stride) result.push_back(entry);
        return result;
    }
    Components Parts(uint64_t entity) const {
        const auto info = Ptr(entity + kEntityPairsOff0), header = Ptr(info + kEntityPairsOff1);
        const auto begin = Ptr(header + kEntityPairsOff2), end = Read<uint64_t>(header + kEntityPairsEndOff);
        const auto array = Ptr(entity + kEntityComponentsOff), arrayEnd = Read<uint64_t>(entity + kEntityComponentsEndOff);
        if (end <= begin || (end - begin) % kComponentPairSize || (end - begin) / kComponentPairSize > kMaxComponentCount ||
            arrayEnd < array || arrayEnd - array != (end - begin) / kComponentPairSize * kComponentPtrSize)
            throw std::runtime_error("怪物组件表范围无效");
        Components out; std::set<uint32_t> indices;
        for (auto slot = begin; slot < end; slot += kComponentPairSize) {
            const auto name = Ascii(Ptr(slot)); const auto index = Read<uint32_t>(slot + kComponentPairIndexOff);
            if (index >= (end - begin) / kComponentPairSize || !indices.insert(index).second || out.count(name))
                throw std::runtime_error("怪物组件表有重复或无效索引");
            out.emplace(name, Ptr(array + index * kComponentPtrSize));
        }
        Same(entity + kEntityPairsOff0, info); Same(info + kEntityPairsOff1, header);
        Same(header + kEntityPairsOff2, begin); Same(header + kEntityPairsEndOff, end);
        Same(entity + kEntityComponentsOff, array); Same(entity + kEntityComponentsEndOff, arrayEnd);
        return out;
    }
    void Owners(uint64_t entity, const Components& parts, uint64_t shared = 0, const Components* owner = nullptr) const {
        for (const auto& part : parts) {
            const auto actual = Read<uint64_t>(part.second + monsterlayout::kComponentOwner);
            if (actual == entity) continue;
            if (part.first != "Positioned" && actual == shared && owner) {
                const auto it = owner->find(part.first);
                if (it != owner->end() && it->second == part.second) continue;
            }
            throw std::runtime_error("怪物组件归属不匹配");
        }
    }
    const std::vector<std::string>& Definition(uint64_t row, uint64_t table) {
        Table(table, "Data/Balance/Mods.dat");
        const auto found = definitions.find(row);
        if (found != definitions.end()) return found->second;
        std::vector<std::string> names; bool ended = false;
        for (unsigned i = 0; i < monsterlayout::kModStatSlots; ++i) {
            const auto slot = row + monsterlayout::kModStatRefs + i * monsterlayout::kReferenceStride;
            const auto stat = Read<uint64_t>(slot);
            if (!stat) { ended = true; continue; }
            if (ended || !memory.IsPlausible(stat)) throw std::runtime_error("怪物词缀属性引用无效");
            Table(Ptr(slot + 8), "Data/Balance/Stats.dat"); names.push_back(Wide(Ptr(stat)));
        }
        return definitions.emplace(row, std::move(names)).first->second;
    }
public:
    Reader(const Memory& mem, uint64_t module) : memory(mem), base(module) {}
    // A metadata directory also contains props/effects. Only a real Monster
    // component owned by this entity supplies combat HP; absence is not HP=0.
    Evidence Vitals(uint64_t entity) const {
        Evidence result{false};
        const auto parts = Parts(entity);
        const auto m = parts.find("Monster");
        if (m == parts.end()) return result;
        result.monster = true;
        const auto owner = Ptr(m->second + monsterlayout::kComponentOwner);
        result.isCombatOwner = owner == entity;
        if (!result.isCombatOwner) return result;
        const auto life = parts.find("Life");
        if (life == parts.end()) throw std::runtime_error("Monster 实体缺少 Life，可能正在销毁或卸载");
        Same(life->second + monsterlayout::kComponentOwner, entity);
        result.hp = Read<uint32_t>(life->second + kLife_HpOff);
        Same(m->second + monsterlayout::kComponentOwner, entity);
        Same(life->second + monsterlayout::kComponentOwner, entity);
        result.hpValid = true;
        return result;
    }
    std::vector<ConfiguredBoss> Catalog(uint64_t world, uint64_t row) const {
        Table(Ptr(world + kWorldAreaTableOff), "Data/Balance/WorldAreas.dat");
        Same(world + kWorldAreaRecordOff, row);
        const auto count = Read<uint64_t>(row + monsterlayout::kWorldBossCount);
        const auto data = Read<uint64_t>(row + monsterlayout::kWorldBossData);
        if (count > 64 || (count && !memory.IsPlausible(data))) throw std::runtime_error("地图 Boss 名单范围无效");
        std::vector<ConfiguredBoss> out;
        for (uint64_t i = 0; i < count; ++i) {
            const auto slot = data + i * monsterlayout::kReferenceStride;
            const auto variety = Ptr(slot); Table(Ptr(slot + 8), "Data/Balance/MonsterVarieties.dat");
            ConfiguredBoss item{variety, Wide(Ptr(variety)), Wide(Ptr(variety + monsterlayout::kVarietyName))};
            if (item.metadata.find("Metadata/Monsters/") != 0) throw std::runtime_error("地图 Boss 元数据无效");
            out.push_back(std::move(item));
        }
        Same(row + monsterlayout::kWorldBossCount, count); Same(row + monsterlayout::kWorldBossData, data);
        Same(world + kWorldAreaRecordOff, row); return out;
    }
    Evidence Identity(uint64_t entity, const std::string& metadata, const std::vector<ConfiguredBoss>& catalog,
                      uint64_t& owner, std::string& name) {
        Evidence evidence; const auto parts = Parts(entity);
        const auto m = parts.find("Monster"); evidence.monster = m != parts.end(); owner = entity;
        if (!evidence.monster) return evidence;
        owner = Ptr(m->second + monsterlayout::kComponentOwner);
        if (owner != entity) {
            const auto principal = Parts(owner); Owners(owner, principal);
            const auto it = principal.find("Monster");
            if (it == principal.end() || it->second != m->second) throw std::runtime_error("Boss 代理主体不匹配");
            Owners(entity, parts, owner, &principal); evidence.isCombatOwner = false;
            return evidence;
        }
        Owners(entity, parts);
        if (Read<uint64_t>(m->second) != base + monsterlayout::kMonsterVtable) throw std::runtime_error("Monster 版本不匹配");
        const auto data = Ptr(m->second + monsterlayout::kMonsterData), variety = Ptr(data + monsterlayout::kVarietyRow);
        Table(Ptr(data + monsterlayout::kVarietyTable), "Data/Balance/MonsterVarieties.dat");
        const auto id = Wide(Ptr(variety));
        if (Canonical(metadata) != id) throw std::runtime_error("怪物实体与品种身份不匹配");
        name = Wide(Ptr(variety + monsterlayout::kVarietyName));
        for (const auto& boss : catalog) if (boss.row == variety || boss.metadata == id) evidence.configuredForArea = true;
        const auto magic = parts.find("ObjectMagicProperties"), life = parts.find("Life");
        if (magic == parts.end() || life == parts.end()) { evidence.valid = false; return evidence; }
        if (Read<uint64_t>(magic->second) != base + monsterlayout::kMagicVtable) throw std::runtime_error("怪物稀有度版本不匹配");
        evidence.rarity = Read<uint32_t>(magic->second + monsterlayout::kMagicRarity);
        if (evidence.rarity > 3) throw std::runtime_error("怪物稀有度无效");
        evidence.hp = Read<uint32_t>(life->second + kLife_HpOff); evidence.hpValid = true;
        // Tier evidence is required for map bosses. Ordinary magic/rare mobs
        // need only rarity; parsing every affix would slow each combat scan.
        if (evidence.rarity == 3) {
            bool gotTier = false;
            const auto header = magic->second + monsterlayout::kRuntimeMods;
            const auto entries = Vector(header, monsterlayout::kRuntimeModStride, 256);
            for (const auto entry : entries) {
                const auto& stats = Definition(Ptr(entry + monsterlayout::kRuntimeModRow), Ptr(entry + monsterlayout::kRuntimeModTable));
                const auto values = Vector(entry, sizeof(int32_t), monsterlayout::kModStatSlots);
                if (values.size() != stats.size()) throw std::runtime_error("怪物词缀属性与数值数量不匹配");
                for (size_t i = 0; i < values.size(); ++i) {
                    const auto value = Read<int32_t>(values[i]);
                    if (stats[i] == "monster_rarity" && value != int32_t(evidence.rarity))
                        throw std::runtime_error("怪物稀有度与词缀不一致");
                    if (stats[i] == "i_am_boss_of_tier") {
                        if (gotTier && evidence.bossTier != value) throw std::runtime_error("怪物 Boss 等级冲突");
                        evidence.bossTier = value; gotTier = true;
                    }
                }
            }
            if (Vector(header, monsterlayout::kRuntimeModStride, 256) != entries) throw std::runtime_error("怪物词缀在读取期间变化");
        }
        Same(m->second + monsterlayout::kComponentOwner, entity);
        Same(magic->second + monsterlayout::kComponentOwner, entity);
        Same(life->second + monsterlayout::kComponentOwner, entity);
        return evidence;
    }
};
} // namespace monster
