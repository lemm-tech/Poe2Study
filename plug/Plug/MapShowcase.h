#pragma once
// ============================================================
// MapShowcase.h —— 代价表 -> 灰度像素（着色策略）
//
// 为什么单独拆出来：
//   MapRenderer::SaveBmp 原来同时干"代价值怎么变黑白"和"BMP 怎么存"。
//   想改成按代价分级着色，得动存盘函数，风险高。
//
// 现在：IMapShowcase 只管 Cost -> 灰度 一个字节，BmpWriter 只管格式。
//
// 实测代价分布（1656x1656 藏身处地图）：
//   0x00 : 2,215,119  不可通行
//   0x01 :    17,386
//   0x02 :    14,768
//   0x03 :    14,614
//   0x04 :    14,725
//   0xFF :   465,724  最高可通行档（原 nibble 5）
//
// 原来的 BinaryShowcase 把 0x01~0xFF 全画成白色，
// 1/2/3/4 这几档地形完全被浪费。
// ============================================================
#include <cstdint>
#include <string>

#include "../Common/MapFrame.h"

namespace mapshow {

class IMapShowcase {
public:
    virtual ~IMapShowcase() = default;

    virtual const char* Name() const = 0;

    // 代价 -> 灰度（0=黑, 255=白）
    virtual uint8_t ToGray(mapdata::Cost cost) const = 0;
};

// ------------------------------------------------------------
// 1) BinaryShowcase —— 原来的行为：>0 白，==0 黑
//    保留它，保证"改动前后出图逐字节一致"这条基线不破。
// ------------------------------------------------------------
class BinaryShowcase final : public IMapShowcase {
public:
    const char* Name() const override { return "binary"; }
    uint8_t ToGray(mapdata::Cost cost) const override
    {
        return cost > 0 ? 0xFF : 0x00;
    }
};

// ------------------------------------------------------------
// 2) GradeShowcase —— 按代价值分档上灰度
//    0x00 -> 黑（不可通行）
//    0x01 -> 0x40
//    0x02 -> 0x80
//    0x03 -> 0xB0
//    0x04 -> 0xD0
//    0xFF -> 0xFF（最亮，最高可通行档）
//    其他 -> 0xC0（未知值，防止被当成背景）
// ------------------------------------------------------------
class GradeShowcase final : public IMapShowcase {
public:
    const char* Name() const override { return "grade"; }
    uint8_t ToGray(mapdata::Cost cost) const override
    {
        switch (cost) {
        case 0x00: return 0x00;
        case 0x01: return 0x40;
        case 0x02: return 0x80;
        case 0x03: return 0xB0;
        case 0x04: return 0xD0;
        case 0xFF: return 0xFF;
        default:   return 0xC0;
        }
    }
};

// ------------------------------------------------------------
// 3) WalkableOnlyShowcase —— 只画可通行区域，其余全黑
//    适合"只看路径"的场景：1~0xFF 统一白，0 黑。
//    与 Binary 的区别：这里显式把"未知非零值"也算可通行。
// ------------------------------------------------------------
class WalkableOnlyShowcase final : public IMapShowcase {
public:
    const char* Name() const override { return "walkable"; }
    uint8_t ToGray(mapdata::Cost cost) const override
    {
        return cost != 0 ? 0xFF : 0x00;
    }
};

// ------------------------------------------------------------
// 工厂：按名字造，不认识就返回 binary
// ------------------------------------------------------------
inline const IMapShowcase& GetShowcase(const std::string& name)
{
    static const BinaryShowcase      kBinary;
    static const GradeShowcase       kGrade;
    static const WalkableOnlyShowcase kWalk;

    if (name == "grade")    return kGrade;
    if (name == "walkable") return kWalk;
    return kBinary;
}

// 可用策略名（help 输出用）
inline const char* const* AvailableShowcaseNames()
{
    static const char* const kNames[] = { "binary", "grade", "walkable", nullptr };
    return kNames;
}

} // namespace mapshow
