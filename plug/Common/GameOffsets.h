#pragma once
// ============================================================
// GameOffsets.h —— 游戏内存布局的唯一共享定义处
//
// 两个工程共用：
//   Plug.exe    （外部进程，ReadProcessMemory 只读）
//   Monitor.dll （注入游戏进程，进程内直接读/调用）
//
// 归属划分（请保持这个结构，别把两边的东西混在一起）：
//   本文件         -> Monitor 也要用的东西：BaseOffset、地图对象
//                     链、CostCall 的 RVA、地图字段偏移、nibble
//                     互转、地址校验、实体/组件布局、坐标、走路 call。
//   背包红黑树链   -> 也在这里（下面是 InventoryChainOffsets）。
//
// 本文件被两个工程一起编译，统一存成 UTF-8 **带 BOM**：
// MSVC 认 BOM 就按 UTF-8 解析，中文注释不会乱码；
// 千万别存成「UTF-8 无 BOM」，那样会被按 936 误读，甚至吞掉下一行代码。
//
// 规矩：偏移一律改这里。下游代码一律不准硬编码偏移。
// ============================================================
#include <cstdint>
#include <cstring>

// Camera/minimap: 2026-11-19 live tests; see docs/视角缩放与小地图全显_20261119.md.
namespace viewlayout {
constexpr uint64_t kManagerRva = 0x45EF380, kManagerClient = 0x90;
constexpr uint64_t kClientCamera = 0x368, kClientMinimap = 0x4C0;
constexpr uint64_t kGlobalOwner = 0x348, kOwnerState = 0x48, kStateWorld = 0x1A0;
constexpr uint64_t kGlobalCamera = 0x1F8, kCameraWorld = 0x98;
constexpr uint64_t kZoomTarget = 0x528, kZoomCurrent = 0x520;
constexpr uint64_t kWorldMinimap = 0xE78, kMinimapRender = 0x40;
constexpr uint64_t kRevealWalkable = 0x60, kRevealFull = 0x61, kRevealReset = 0x62;
constexpr uint64_t kDescriptor = 0x2A8, kDimensions = 0, kWorldDimensions = 0x08;
constexpr uint64_t kPixels = 0x10, kPixelsEnd = 0x18, kPixelsCapacity = 0x20;
constexpr uint64_t kZoomInputRva = 0x1718DC, kRevealCpuRva = 0x121775C;
// Native wheel upper clamp: minss xmm1,[rip+disp32], original constant = 1.0f.
constexpr uint64_t kZoomClampRva = 0x1718FC;
constexpr uint8_t kZoomClampSignature[] = {0xF3,0x0F,0x5D,0x0D,0x64,0x15,0x4B,0x03};
constexpr uint64_t kRevealRenderRva = 0x121828B, kRevealClearRva = 0x1218883;
constexpr uint8_t kZoomSignature[] = {0x66,0x41,0x0F,0x6E,0x4E,0x18,0x0F,0x57,0xC0,0x0F,0x5B,0xC9};
constexpr uint8_t kRevealCpuSignature[] = {0x41,0xC7,0x04,0x82,0x00,0x00,0x80,0x3F};
constexpr uint8_t kRevealRenderSignature[] = {0x41,0x80,0x7F,0x61,0x00};
constexpr uint8_t kRevealClearSignature[] = {0x66,0x41,0xC7,0x47,0x60,0x00,0x00};
}
// 当前 PathOfExile.exe（2026-11-18）；证据见 根目录 docs/游戏更新偏移_20261118.md。
// 本次 SizeOfImage 未变，必须同时检查时间戳和函数指纹。
constexpr uint64_t kGameImageSize = 0x4CBB000ULL;
constexpr uint32_t kGameTimeDateStamp = 0x6AFB7637;
constexpr const char* kGameFileSha256 = "dfc10ef7e4e4411df7418779c5467d804def225e80814be6ee88f96cbd10dcca";

// Packet object layout from pickup assembly, checked against the same disk PE.
constexpr uint64_t kPacketBuildRva = 0x212F0F0, kPacketSendRva = 0x21A8560;
constexpr uint64_t kPacketWriteRva = 0x21A8400, kPacketReadRva = 0x21A82D0;
constexpr uint64_t kPacketActionVtableRva = 0x34FC3D8, kPacketEndVtableRva = 0x34FB548;
constexpr uint64_t kPacketSerializeRva = 0x1F47550;
constexpr uint64_t kPacketEndSerializeRva = 0x1F4C130;
// Native summon entry verified by five manual captures and one active call.
// RCX=current state; native checks and live sequence remain inside this function.
constexpr uint64_t kPortalSummonRva = 0x21236F0, kPortalSummonGateOff = 0x8C08;
constexpr uint8_t kPortalSummonSignature[] = {
    0x40,0x53,0x48,0x83,0xEC,0x40,0x80,0xB9,0x08,0x8C,0x00,0x00,0x00,0x48,0x8B,0xD9,
    0x75,0x75,0x48,0x8B,0x89,0x88,0x01,0x00,0x00,0x48,0x85,0xC9,0x74,0x69,0x48,0x8B,
    0x49,0x08,0xE8,0xA9,0x04,0x03,0xFE,0x48,0x85,0xC0,0x74,0x5B,0x48,0x8B,0x88,0x60,
    0x01,0x00,0x00,0xBA,0xF0,0x45,0x00,0x00,0x48,0x8B,0x01,0xFF,0x10,0x85,0xC0,0x75,
    0x46,0xB8,0xD0,0x00,0x00,0x00,0x48,0x8D,0x54,0x24,0x20,0x66,0x89,0x44,0x24,0x28,
    0x48,0x8D,0x05,0x01,0x7E,0x3D,0x01,0x48,0x89,0x44,0x24,0x20,0x0F,0xB6,0x83,0x81,
    0x01,0x00,0x00,0x48,0x8B,0x9B,0xA8,0x01,0x00,0x00,0x48,0x8B,0xCB,0x88,0x44,0x24,
    0x30,0xE8,0x8A,0xB9,0x00,0x00,0x48,0x8B,0x8B,0xF8,0x20,0x00,0x00,0x45,0x33,0xC0,
    0x33,0xD2,0xE8,0xE9,0x4D,0x08,0x00,0x48,0x83,0xC4,0x40,0x5B,0xC3
};
static_assert(sizeof(kPortalSummonSignature) == 0x8D, "whole native portal summon fingerprint");
// Party-name teleport: native B5 caller +0x212003B, object size 0x38.
constexpr uint64_t kPacketPartyVtableRva = 0x34FB5A8;
constexpr uint64_t kPacketPartySerializeRva = 0x1F467F0;
// Recovery 0x8C, from supplied ASM and current disk/live serializer.
constexpr uint64_t kPacketFlaskVtableRva = 0x30B5960;
constexpr uint64_t kPacketFlaskSerializeRva = 0x86FFD0;
constexpr uint8_t kPacketFlaskSerializeSignature[] = {
    0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x8B,0xFA,
    0x0F,0xB7,0x49,0x08,0xFF,0x15,0xB6,0xDF,0x62,0x02
};
constexpr uint64_t kPacketStateStreamOff = 0x4F8;
constexpr uint8_t kPacketPartySerializeSignature[] = {
    0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x8B,0xFA,
    0x0F,0xB7,0x49,0x08,0xFF,0x15,0x96,0x77,0xF5,0x00
};
constexpr uint64_t kPacketLockEnabledOff = 0x242, kPacketLockOff = 0x248;
constexpr uint64_t kPacketContextChain[] = {0x348, 0x48, 0};
constexpr uint64_t kPacketProtocolOff = 0x1A8, kPacketStreamOff = 0x20F8;
constexpr uint64_t kPacketSequenceOff = 0x181;
constexpr uint64_t kPacketWorldOff = 0x20F0;
constexpr uint64_t kPacketSendSizeOff = 0x258, kPacketSendConsumedOff = 0x260;
constexpr uint64_t kPacketSendBufferOff = 0x280;
constexpr uint64_t kPacketRecvErrorOff = 0x252;
constexpr uint64_t kPacketRecvPositionOff = 0x278;
constexpr uint8_t kPacketBuildSignature[] = {0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57};
constexpr uint8_t kPacketSendSignature[] = {0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x55,0x57,0x41,0x54,0x41,0x56};
constexpr uint8_t kPacketIoSignature[] = {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57};
constexpr uint8_t kPacketSerializeSignature[] = {0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x8B,0xFA};
constexpr uint8_t kPacketEndSerializeSignature[] = {0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x8B,0xFA};

// ------------------------------------------------------------
// 模块基址（全工程唯一一处）
// ------------------------------------------------------------
// 2026-11-18：主链入口 0x45EF370；更新前的 0x45EF2F8 已为 0。
#ifndef BaseOffset
#define BaseOffset              0x45EF370ULL
#endif


// ------------------------------------------------------------
// 地图对象链（Monitor 侧，含基址共 10 级）
// ------------------------------------------------------------
// 前 5 级与周围实体链共用（0x348/0x48/0x0/0x1A8/0x20F0），第 6 级起分叉。
// 2026-11-18：结构偏移不变，读取到 39x45 网格、897x1035 世界像素。
//
// ★ 2026-11-06 游戏更新后的实际值（运行时逐级验证过）：
//     *(exe+BaseOffset) = 0x44C9B191710
//     +0x348  -> 0x44C6CB0B000
//     +0x48   -> 0x44C6CB0B060
//     +0x0    -> 0x44C688DA000
//     +0x1A8  -> 0x44C6CB0B000
//     +0x20F0 -> 0x44C87119000        <- 与周围链同一终点
//     +0x5C0  -> 0x44C871195D8
//     +0x0    -> 0x44C6D56F280（玩家实体）
//     +0x78   -> 0x44C87119000（回到上面那个对象）
//     末级 +0x8F8（只加法，不解引用）= 地图对象
//
//   上一版本的 0x5B8 / 0x8D0 已失效：0x5B8 现在指向一个"首字段是 vtable"
//   的对象，再 +0x0 解引用会直接跳进模块地址，整条链走废。
//   0x5B8 -> 0x5C0（+8）、0x8D0 -> 0x8F8（+0x28）。
//
// ★ 末级语义提醒：这一级是 TailOp::Add，不是 Deref。
//   地图对象是"嵌在父对象里"的子结构，所以落点是"父对象 + 0x8F8"，
//   不是"对 0x8F8 处的指针再解一次"。
//   历史上把这一级写成 Deref 会得到一个模块内地址（0x7FF69C647580），
//   读出来的网格尺寸是天文数字 —— 这个坑踩过一次，别再犯。
//
// ★ 网格尺寸偏移 +0x18/+0x20 和代价数组 +0xD0/+0xD8 本次都没变，
//   证据在 CostCall 自己的汇编里：imul eax,[rcx+0x18],0x17 /
//   imul eax,[rcx+0x20],0x17 / mov r8,[r9+rcx*8+0xD0]。
constexpr uint64_t kMapObjectChain[] = {
    0x348, 0x48, 0x0, 0x1A8, 0x20F0, 0x5C0, 0x0, 0x78, 0x8F8
};
constexpr int kMapObjectChainDepth = 9;
// This prefix resolves the local controlled entity, never a player from the nearby tree.
constexpr int kLocalPlayerChainDepth = 7;

// Current world -> WorldAreas.dat row (2026-11-18, read-only live verification).
// Row+0/+8 are UTF-16Z pointers: MapSteamingSprings / localized display name.
constexpr int kWorldAreaWorldChainDepth = 5;
constexpr uint64_t kWorldAreaRecordOff = 0xA0;
constexpr uint64_t kWorldAreaTableOff = 0xA8;
constexpr uint64_t kWorldAreaIdOff = 0x00;
constexpr uint64_t kWorldAreaNameOff = 0x08;
constexpr uint64_t kWorldAreaTableNameOff = 0x08;

// Party data (2026-11-18): read-only profile lookup used by the game's party UI.
// Owner = Q(Q(exe+BaseOffset)+0x348); also Q(state+0x1A8).
// Roster identifies party membership; profiles also contain non-party contacts.
constexpr uint64_t kPartyRosterBeginOff = 0x2300;
constexpr uint64_t kPartyRosterStride = 0x50;
constexpr uint64_t kPartyRosterAccountOff = 0x08;
constexpr uint64_t kPartyRosterNameOff = 0x28;
constexpr uint64_t kPartyProfilesBeginOff = 0x27E0;
constexpr uint64_t kPartyProfileStride = 0x230;
constexpr uint64_t kPartyProfileAccountOff = 0x00;
constexpr uint64_t kPartyProfileNameOff = 0x40;
constexpr uint64_t kPartyProfileAreaKeyOff = 0x1F0;  // uint16_t, not a pointer/string
constexpr uint64_t kWorldAreaNumericIdOff = 0x2A;  // uint16_t, same key as profile
constexpr uint64_t kWorldAreaStorageOff = 0x28;
constexpr uint64_t kWorldAreaRowsBeginOff = 0x00;  // vector of packed data rows
constexpr uint64_t kWorldAreaRowStride = 0x2E0;
constexpr uint64_t kWorldAreaLookupBeginOff = 0x58;  // vector of key/row pairs
constexpr uint64_t kWorldAreaLookupStride = 0x10;
constexpr uint64_t kWorldAreaLookupKeyOff = 0x00;  // only the low uint16_t is valid
constexpr uint64_t kWorldAreaLookupRowOff = 0x08;

// ------------------------------------------------------------
// 地图对象 / CostCall
// ------------------------------------------------------------
// CostCall 相对 PathOfExile.exe 的 RVA（每次游戏更新都会变）。
//
// 2026-11-18 当前安装版及运行进程复核：CostCall 函数入口 RVA = 0x1D1AF50。
// 唯一命中指纹：44 8B 1A 4C 8B C9 45 85 DB 78 71；入口前是 CC 对齐填充。
// 当前 PathOfExile.exe 中该指纹唯一命中于 0x1D1AF50，确认是函数头而非函数内部。
// 进程内存中的入口字节与磁盘映像一致。
// 反汇编逐条比对后确认逻辑完全同构：读 [rdx]/[rdx+4] 取坐标、
// cmp r8b,4 校 layer、lea rcx,[rax+rax*2] 算 layer*0x18、
// mov r8,[r9+rcx*8+0xD0] 取代价数组。
#ifndef CostCall_Offset
#define CostCall_Offset   0x1D1AF50
#endif

// 世界像素数 = 网格数 * CELL_SIZE。
#ifndef CELL_SIZE
#define CELL_SIZE         0x17
#endif

// 地图对象内部的字段偏移。
#ifndef MAP_OFF_GRID_W
#define MAP_OFF_GRID_W    0x18   // grid width  (* CELL_SIZE = world width)
#endif
#ifndef MAP_OFF_GRID_H
#define MAP_OFF_GRID_H    0x20   // grid height (* CELL_SIZE = world height)
#endif

// 代价数组：map 对象里 +0xD0 处是【每层一个 vector，共 4 槽，每槽 0x18 字节】的数组。
//   第 layer 层的 vector.begin = *(map + 0xD0 + layer*0x18)
//   第 layer 层的 vector.end   = *(map + 0xD8 + layer*0x18)
//
// 证据（CostCall 内部汇编）：cmp r8b, 4 / lea rcx,[rax+rax*2] / mov r8,[r9+rcx*8+0xD0]
//
// 2026-11-16 进程内实测：layer 0~3 的字节数 = RowStride * WorldH。
// +0x130 是 CostCall 读取的行跨度（当前 449），不能当 layer 4 的 begin。
#ifndef MAP_OFF_ARY_BEGIN
#define MAP_OFF_ARY_BEGIN 0xD0   // layer 0 的 begin；层步长 0x18
#endif
#ifndef MAP_OFF_ARY_END
#define MAP_OFF_ARY_END   0xD8   // layer 0 的 end  （相对 begin 的偏移 0x08）
#endif
#ifndef MAP_LAYER_STRIDE
#define MAP_LAYER_STRIDE  0x18   // 每层 vector 占 0x18 字节
#endif
#ifndef MAP_LAYER_COUNT
#define MAP_LAYER_COUNT   4      // 实测只有 layer 0~3；+0x130 是行跨度，不是第 5 个 vector
#endif

#ifndef MAP_OFF_ROW_STRIDE
#define MAP_OFF_ROW_STRIDE 0x130 // CostCall: imul r10d,dword ptr [r9+0x130]
#endif

// 网格单边上限：超过这个值说明链解错/基址漂移，别拿去分配内存
constexpr uint32_t kMaxGridDimension = 0x1000;


// ------------------------------------------------------------
// 代价值 <-> 管道上 nibble 的互转
// ------------------------------------------------------------
// CostCall 返回 0~255，其中 0xFF 是「打包 nibble 值 5」的特殊标记。
// 管道线上每格只传一个 nibble，所以 0xFF 必须在发送端折回 5、在接收端
// 再展开，否则 0xFF & 0x0F 会悄悄变成 15，值就丢了。
constexpr uint8_t kCostSpecial       = 0xFF;
constexpr uint8_t kCostSpecialNibble = 0x05;

// 高 4 位 = x 为偶数，低 4 位 = x 为奇数（与游戏的打包方式一致）。
inline uint8_t CostToNibble(uint8_t cost)
{
    return (cost == kCostSpecial) ? kCostSpecialNibble
                                  : static_cast<uint8_t>(cost & 0x0F);
}

inline uint8_t NibbleToCost(uint8_t nibble)
{
    return (nibble == kCostSpecialNibble) ? kCostSpecial
                                          : static_cast<uint8_t>(nibble & 0x0F);
}

// 代价值的**通行性**语义（被实测推翻过，现在以这条为准）：
//   0    = 障碍 / 边界外
//   1~4  = 有代价的地形（能走但慢）
//   0xFF = 最高可通行档
// 注意与"越小越好"的直觉相反：这里 0 是最差，0xFF 是最好。
inline bool IsWalkableCost(uint8_t cost)
{
    return cost != 0;
}

// ------------------------------------------------------------
// 地址合法性校验（两个工程共用）
// ------------------------------------------------------------
// ★ 全工程唯一的地址范围定义。历史上这组数字被硬写了 28 处，
//   现在所有调用点都必须走 IsPlausibleAddress。
constexpr uint64_t kMinValidAddress = 0x1000ULL;
constexpr uint64_t kMaxValidAddress = 0x7FFFFFFFFFFFULL;

inline bool IsPlausibleAddress(uint64_t addr)
{
    return addr >= kMinValidAddress && addr <= kMaxValidAddress;
}


// ------------------------------------------------------------
// 实体 / 组件通用布局（Plug 与 Monitor 都可能用到）
// ------------------------------------------------------------
// 组件信息对数组：容器 +0x28 为 begin，+0x30 为 end。
constexpr uint64_t kEntityPairsOff0        = 0x08;
constexpr uint64_t kEntityPairsOff1        = 0x28;
constexpr uint64_t kEntityPairsOff2        = 0x28; // begin 指针
constexpr uint64_t kEntityPairsEndOff      = 0x30; // end 指针

// 组件对象指针数组起止：entity+0x10 / entity+0x18
constexpr uint64_t kEntityComponentsOff    = 0x10;
constexpr uint64_t kEntityComponentsEndOff = 0x18;

// 组件信息对每项大小（名称指针 + 索引）、组件对象指针大小
constexpr uint64_t kComponentPairSize      = 0x10;
constexpr uint64_t kComponentPairIndexOff  = 0x08;
constexpr uint64_t kComponentPtrSize       = 0x08;

// 组件数量防御上限（数据错位的实体会算出天文数字）
constexpr uint64_t kMaxComponentCount      = 0x400;

// StateMachine named state table; read-only three-phase capture on 2026-11-19.
// docs/祭祀交互状态调查_20261119.md records native and live evidence.
constexpr uint64_t kStateMachineDefinitionOff = 0x158;
constexpr uint64_t kStateMachineValuesBeginOff = 0x160, kStateMachineValuesEndOff = 0x168;
constexpr uint64_t kStateMachineKeysBeginOff = 0x10, kStateMachineKeysEndOff = 0x18;
constexpr uint64_t kStateMachineKeyStride = 0xC0, kStateMachineValueStride = 8;
constexpr uint64_t kStateMachineMaxStates = 256;
constexpr size_t kStateMachineMaxName = 127;

// 元数据路径：[[entity+0x8]+0x8]
constexpr uint64_t kMetadataPathOff0       = 0x08;
constexpr uint64_t kMetadataPathOff1       = 0x08;

// 红黑树节点布局（背包树 / 周围树共用）
constexpr uint64_t kNodeLeftOff            = 0x00;
constexpr uint64_t kNodeParentOff          = 0x08;
constexpr uint64_t kNodeRightOff           = 0x10;
constexpr uint64_t kNodeColorOff           = 0x18;
constexpr uint64_t kNodeFlagOff            = 0x19;   // flag == 1 => 哨兵，终止
constexpr uint64_t kNodeKeyOff             = 0x20;   // Surroundings: uint32_t; inventory uses its own entry layout.
constexpr uint64_t kNodeEntityOff          = 0x28;   // 背包需解两层，周围只解一层

// 红黑树数组（背包 / 仓库等容器所在）
constexpr uint64_t kInvTreeArrayOff        = 0x320;  // 1B8 + 168 同层相加
constexpr uint64_t kInvTreeArrayStride     = 0x18;   // 每个成员占 0x18
constexpr uint64_t kInvTreeArrayTreeOff    = 0x08;   // 成员内部的树对象指针
constexpr uint64_t kInvTreeSentinelOff     = 0x188;  // 树对象 -> 哨兵头节点

constexpr int      kInventoryChainDepth    = 7;

// 背包链（末级解引用，落点就是哨兵节点）
inline const uint64_t* InventoryChainOffsets()
{
    static const uint64_t kChain[kInventoryChainDepth] = {
        BaseOffset, 0x348, 0x48, 0x0, kInvTreeArrayOff,
        kInvTreeArrayTreeOff, kInvTreeSentinelOff
    };
    return kChain;
}

// 周围实体树链（前 6 级与背包一致，第 7 级分叉：0x718 vs 0x320 路径）
// 0x718 = 0x708 + 0x10（2026-11-06 游戏更新后实测；旧值 0x6F0/0x6F8 已失效）
//
// 运行时逐级验证（2026-11-06，游戏内 210 个实体、其中 1 个玩家）：
//   哨兵 = 0x44C68CD2B00，根节点 = 0x44C87610780
constexpr uint64_t kSurroundingsChain[]      = { BaseOffset, 0x348, 0x48, 0x0, 0x1A8, 0x20F0, 0x718 };
constexpr int      kSurroundingsChainDepth   = 7;

// ★ 下面这条“计数链”目前**没有任何代码在用**（生产路径只走 kSurroundingsChain，
//   实体数量靠遍历树自己数）。它按旧版 0x6F8 (=0x6F0+8) 类推成 0x720 (=0x718+8)，
//   但 2026-11-06 实测解出来是 0 —— 说明这个字段的位置变了，或者这版根本不再
//   单独维护计数。
//   留着只为“别丢掉这条历史线索”，谁要用之前必须先实测，别直接信。
constexpr uint64_t kSurroundingsCountChain[] = { BaseOffset, 0x348, 0x48, 0x0, 0x1A8, 0x20F0, 0x720 };


// ------------------------------------------------------------
// 组件字段偏移（全部集中在此，下游不准再出现裸数字）
// ------------------------------------------------------------
// 字符串对象（游戏中 std::string / std::wstring 的 MSVC 布局）：
//   +0x00  数据。SSO 时是内联缓冲的首地址，长串时是堆指针（要再解一次）
//   +0x18  maxLen / 容量。> 7 表示数据在堆上
//
// ★ 这里曾经写成 +0x10，那是错的：实测（背包物品名、前缀名、元数据路径
//   全部一致）数据指针就在对象起始的 +0x00。GameMemory.cpp 里的旧实现
//   一直用 +0x00 才是对的，那次「偏移迁移」没做实测，属于凭空改。
//   任何改动都必须先在游戏里读出一件带名字的物品验证。
constexpr uint64_t kStdStringDataOff       = 0x00;
constexpr uint64_t kStdStringMaxLenOff     = 0x18;
constexpr uint64_t kStdStringLengthOff     = 0x10;
constexpr uint32_t kStdStringSsoLimit      = 7;
constexpr uint32_t kStdNarrowStringSsoLimit = 15;

// Render 组件：显示名在 +0x160
constexpr uint64_t kRender_NameOff         = 0x160;

// ------------------------------------------------------------
// 实体坐标（玩家 / 怪物 / NPC / 掉落物，全部同一个读法）
// ------------------------------------------------------------
// ★ 生产坐标来源：+0x444（x）、+0x448（y），各是一个 int32，
//   值本身**就是世界像素**（与地图 BMP 同一单位），不需要任何换算。
//
//   实测确认：
//     · 玩家与周围遍历实体用的是同一对偏移、同一类型；
//     · 读出来直接是 (242, 891) 这种小整数，不是 2635.87 那种浮点；
//     · 走路 call 的 r8 打包、地图查找、半径判定全部用这个单位。
//
//   历史（为什么放弃 0x490）：
//     旧版玩家坐标读 +0x490 的打包 u64（低 32 位 x / 高 32 位 y，各为
//     float），世界像素 = 该 float / 10.88。这条路径在新版已废弃：
//     0x490 在周围实体上常读到 0 或明显偏离实际位置，而且多一层换算
//     就多一个出错点。0x490 现在只作为历史对照保留，禁止再用于寻路。
//
// 注意 +0x208 不是位置（值是移动目标/插值），禁止用于寻路。
constexpr uint64_t kPositioned_EntXOff     = 0x444;   // int32 x（世界像素）
constexpr uint64_t kPositioned_EntYOff     = 0x448;   // int32 y（世界像素）

// 玩家坐标用同一对偏移，语义完全一致；这两个别名只是让调用处读起来清楚。
constexpr uint64_t kPositioned_XOff        = kPositioned_EntXOff;
constexpr uint64_t kPositioned_YOff        = kPositioned_EntYOff;

// ---- 以下为旧打包坐标（0x490），仅保留给历史对照 / tools 实验程序 ----
constexpr uint64_t kPositioned_ValueOff    = 0x490;   // 旧：float 对打包 u64，已废弃
constexpr uint64_t kPositioned_AnimPosOff  = 0x23C;   // 旧偏移，早已失效
constexpr uint64_t kPositioned_PosOff      = kPositioned_ValueOff;   // 旧
constexpr uint64_t kPositioned_AltPosOff   = 0x208;   // 非位置坐标，禁止用于寻路

// 旧换算比例：0x490 的 float -> 世界像素。新坐标已是像素，**不要再用它**。
// 保留定义是因为 tools 里的历史对照程序还在引用。
constexpr double kPosUnitPerPixel = 10.88;

// ------------------------------------------------------------
// 动作系统 / 走路 call（自动寻路用）
// ------------------------------------------------------------
// 动作系统对象（走路 call 的 rcx = this）：
//   rcx = [[[exe + kActionSystemBaseOffset] + 0x2D0 + slot*8 + 0x18]
//           + kActionSystemObjOff]
//
// ★ 2026-11-11 当前游戏进程运行时复核：动作系统全链命中。
//
//   [exe + 0x444A0F8] = L0 0x3BDC0E52E10。
//   slot 3: [L0+0x300] = 0x3BDAA0CCE00; [+0x3E8] = 对象 0x3BE2F8CD980。
//           vtable RVA 0x2F91C20；vtable[0x198] = exe + 0x188D20。
//   slot 4: [L0+0x308] = 0x3BE2E69BA00; [+0x3E8] = 对象 0x3BE2F8CD480；
//           vtable 与 slot 3 相同，0x198 项同样命中 RVA 0x188D20。
//   当前走路调用点 RVA 0x181372 仍执行 call qword ptr [rax+0x198]。
//   slot 1 虽能解出对象，但其 vtable/入口不同；动作控制器仍选 slot 3。
constexpr uint64_t kActionSystemBaseOffset = 0x444A108;   // 2026-11-18 复核不变

// ★★★ 2026-11-08 实测：技能 call 的 this = 走路 call 的 this（同一个对象）★★★
//
//   0x18C940 的真实序言（进程内读到的原始机器码）：
//     +00  48 89 5C 24 10        mov  [rsp+10], rbx
//     +05  4C 89 4C 24 20        mov  [rsp+20], r9
//     +0A  66 44 89 44 24 18     mov  [rsp+18], r8w
//     +10  55 56 57 41 54 ...    push rbp/rsi/rdi/r12/r13/r14/r15
//     +1B  48 83 EC 50           sub  rsp, 0x50
//     +1F  49 8B F9              mov  rdi, r9          ; r9 = shared_ptr*
//     +22  4C 8B E2              mov  r12, rdx         ; rdx = 地图对象
//     +25  4C 8B E9              mov  r13, rcx         ; rcx = this
//     +28  48 8B 81 D0 01 00 00  mov  rax, [rcx+1D0]   ★ 真正被读的字段
//     +2F  4C 8B B0 F0 03 00 00  mov  r14, [rax+3F0]
//     +36  48 8B 98 E0 03 00 00  mov  rbx, [rax+3E0]
//     +3D  48 8B CB              mov  rcx, rbx
//     +40  E8 ...                call 0x19448A0
//
//   实测对照（skillrcx2.dll，slot 3）：
//     objMid            = 0x4B2A99F0000 -> [1D0] = 0x100000027F   ✗ 垃圾值
//     *(objMid + 0x3E8) = 0x4B2B29EDB80 -> [1D0] = 0x4B2A99F0000  ✓
//                                          [3E0] = 0x7FF64D17C170 ✓
//                                          [3F0] = 0x4B2A89BBB00  ✓
//
//   → objMid 与它的 +0x3E8 派生对象是一对，技能 call 要的是**派生对象**，
//     也就是走路 call 用的那个 this。两者相同。
//
//   ★ 排查中走过两个弯路（记在这里，别再犯）：
//     1) 用 [+0x390] 当判据 —— 那是**调用 0x18C940 的外层函数**的序言
//        （mov rcx,[rcx+390]），不是 0x18C940 自己的，于是选错了对象；
//     2) 随后误判成"要用 objMid 本身"，反而把本来正确的取值改坏。
//   正确的判据只有一条：[this+0x1D0] 可读，且其中 +0x3E0/+0x3F0 可读。
constexpr uint64_t kSkillThisOff1D0       = 0x1D0;   // 0x18C940 第一条真读的字段
constexpr uint64_t kSkillThisOff3E0       = 0x3E0;   // 内层 call 的 rcx 来源
constexpr uint64_t kSkillThisOff3F0       = 0x3F0;

// ------------------------------------------------------------
// 停止施法 call（2026-11-18 RVA 0x211F360；下方 11-11 为历史对照）
// ------------------------------------------------------------
// 来源：用户在 x64dbg 里按"释放技能"时 send 断下，继续运行后又断一次，
//   说明"停止释放"也会走一次发包；向上追到 0x211F0A0。
//
// 调用形态（2026-11-11 当前安装版反汇编复核）：
//   rcx = [[[[PathOfExile.exe+0x45EF338]+0x348]+0x48]]
//         ★ 注意是四层方括号，用户最初给的链少写了最后一层解引用
//   rdx/r8/r9/栈参数都不需要 —— 函数自己在栈上构造发包结构体：
//     +00A  mov  rbx,[rcx+0x1A8]
//     +011  lea  rdx,[rsp+0x20]
//     +016  mov  eax,0x1AC              <- 包类型码
//     +01E  mov  [rsp+0x28],ax
//     +022  lea  rax,[rip+0x13DC3EE]
//     +029  mov  [rsp+0x20],rax
//     +02E  movzx eax,byte [rcx+0x181]
//     +038  mov  [rsp+0x30],al
//     +03C  call 内部发包函数
//
// ★ 关键分支（决定它到底做不做本地中断）：
//     +053  mov  rax,[rdi+0x1A0]
//     +05A  cmp  byte [rax+0x669], 2
//     +060  je   +0x72          <- 等于 2 就跳过下面这段
//     +062  mov  rcx,[rdi+0x188]
//     +069  mov  rax,[rcx]
//     +06C  call [rax+0x118]    <- 真正的本地中断（虚调用）
//   实测当前 [[rcx+0x1A0]+0x669] == 2，所以只发包、不做本地中断。
//   若验证发现"发包不足以停下"，下一步就是直接调这个 [+0x118]。
constexpr uint64_t kSkillStopRva          = 0x211F360;
constexpr uint64_t kSkillStopChainOff1    = 0x348;   // [exe+BaseOffset]+0x348
constexpr uint64_t kSkillStopChainOff2    = 0x48;    // 再 +0x48
constexpr uint64_t kSkillStopFlagOff      = 0x181;   // 发包结构体里的那个字节
constexpr uint64_t kSkillStopObj1A8       = 0x1A8;   // mov rbx,[rcx+0x1A8]
constexpr uint64_t kSkillStopObj1A0       = 0x1A0;   // cmp byte [[rcx+1A0]+669],2
constexpr uint64_t kSkillStopGate669      = 0x669;   // 那个门闩字节
constexpr uint64_t kSkillStopObj188       = 0x188;   // 中断虚调用的宿主
constexpr uint64_t kSkillStopAbortVtblOff = 0x118;   // 中断虚函数的槽位

// "放一次就停"的默认停止延迟（毫秒）。
//   实测 300ms 时技能已进入可中断状态，0x211F0A0 能停住。
//   太短（<150）可能还在前摇初期，太长（>800）动作可能已自己播完。
constexpr uint32_t kDefaultStopAfterMs = 300;

// 0x18C940 内部实际执行的虚函数槽（从技能对象 vtable 取）：
//   +09A  mov rcx,[rax]
//   +09D  mov r8, [rcx+0x520]
//   +0AA  call r8
// ★ 这个槽**不保证是函数指针**（实测出现过 0x2100650063 这种数据），
//   所以调用前必须校验它可执行，否则会执行堆数据直接崩游戏。

// ============================================================
// ★★★ 已知风险（2026-11-08 多次实测归纳，务必先读这一节）★★★
// ============================================================
//
// 【风险 1】返回值 ret 不能用来判断成败
//
//   同一个技能 call（0x18C940），实测返回值出现过：
//     2            <- 走 A 分支，正常执行（多次复现）
//     3            <- 也是执行路径，正常
//     772211792    <- 0x2E0A...，观察到的实际值
//     1409746064   <- 0x540D0910，观察到的实际值
//     0            <- C 路，什么都不做
//
//   ★ 大数值那些不是"错误码"，而是**未初始化/残留的 eax**：
//     函数在 +0AD 才有 `mov ebx,eax` 收集返回，但 +0B1 的 C 路是
//     `xor ebx,ebx` —— 说明寄存器里本来就可能有旧值。
//     所以**绝不能**用 `ret == 2` 之类判断"放成功没有"。
//
//   ✓ 正确的判据：
//       · 调用有没有崩（SEH 捕获）
//       · 调用前后 [r14+0xE8] 是否变化
//       · 角色有没有动作（人工观察）
//
// ------------------------------------------------------------
//
// 【风险 2】_Ptr 有状态依赖，只有"对的时刻"才能成功
//
//   同一槽位（右键 slot=2）在不同时刻读到的 _Ptr 完全不同：
//     0x5B156626C10   -> 成功
//     0x5B22ACBEA10   -> 成功一次，随后崩
//     0x2483B876C10   -> [[_Ptr]+0x520] = 0x2100650063（数据），调用会崩
//
//   ★ 这些是堆对象，随游戏状态变化（切图、换装备、技能冷却都会换）。
//     读到的只是"那个瞬间"的值，不能缓存复用。
//   ★ 每次 Cast 都必须重新解链（现在的实现就是这么做的）。
//
// 【风险 3】[[_Ptr]+0x520] 不保证是函数指针
//
//   0x18C940 的 +09D 是 `mov r8,[rcx+0x520]`，+0AA 是 `call r8`。
//   实测该槽出现过 0x2100650063（UTF-16 字符串数据），call 它会
//   触发"执行型访问违规"。崩溃转储里 Rip 落在堆/未映射区就是这么来的。
//   → 真要稳定，调用前需要校验该槽可执行；但那会改变行为（试过，反而
//     更容易失败），所以当前版本（v15）**不做校验**，接受这个风险。
//
// 【风险 4】停止 call 的 gate669==2 时只发包，不做本地中断
//
//   但实测仍能把技能停住（靠发包之后的后续机制）。
//   若哪天发现停不住，下一步是直接调被跳过的那段：
//     +062 mov rcx,[rdi+0x188] / +069 mov rax,[rcx] / +06C call [rax+0x118]
//
// ============================================================
constexpr uint64_t kSkillAbortVtblOff = 0x520;
constexpr uint64_t kActionSystemOuterOff   = 0x2D0;       // 数组对象偏移
constexpr uint64_t kActionSystemInnerOff   = 0x18;        // 数组头长度，元素从 +0x18 开始
constexpr uint64_t kActionSystemSlotIndex  = 3;           // 动作控制器所在下标（实测）
constexpr uint64_t kActionSystemObjOff     = 0x3E8;       // 再一跳才是动作系统对象

// 走路 call 是 vtable 间接调用：rax = [rcx]，入口 = [rax + 0x198]。
// 所以它没有固定 RVA —— 每次游戏更新，入口地址跟着 vtable 走。
//
// ★★ 2026-11-06 更新后：槽位 0x198 已经实测确认没变、而且真的能走 ★★
//
//   实测记录（Plug.exe walkscan -objaddr=0x56BC55DE900 -from=0x198 -to=0x1A0）：
//     [Scan] 开始：obj=0x56BC55DE900 argMode=0 槽位 0x198~0x1A0 目标(1304,592) 从(1235,569)
//     [Scan]   slot 0x198 entry=0x7FF69A698D20 Ok -> 位移(64,13)
//     [Scan] *** 命中 slot=0x198 ***
//   角色真的走了 (64,13) 像素，不是"返回成功但不干活"。
//   该入口 RVA 是 0x188D20（一个调整栈参数的转发桩，jmp 到真正的实现）。
//
//   上一版把 0x198 判成"取值器"是误判：那次读的是别的对象的 vtable
//   （基址 0x4713350 解出来的实体类对象），它的 [+0x198] 是 0x1A2850，
//   那个才是 getter。换成正确基址 0x444A0F8 后 0x198 就是走路。
//
//   kActionWalkEntryRva 只是"解对了没有"的校验用常量：
//   ResolveWalkEntry 解出来的入口 RVA 等于它才算真的命中，不等就说明游戏又更新了。
constexpr uint64_t kActionWalkVtblOff      = 0x198;
constexpr uint64_t kActionWalkEntryRva     = 0x188D20;   // 仅作校验，不参与调用

// ------------------------------------------------------------
// 走路 call 的参数（★ 2026-11-05 进程内三组消融实测，确定语义）
// ------------------------------------------------------------
//     rcx = 动作系统对象（this）
//     rdx = 0x400（**常数**，不是指针！）
//     r8  = 打包坐标 (y << 32) | x
//     r9  = 0
//     [rsp+0x20] = 0, [rsp+0x28] = 0x400
//
// 实测证据（walkasm_probe.dll；起始 (241,891)，目标 (361,891)）：
//
//   用例 A: rdx=0x400, r8=(y<<32)|x    -> 3 秒移动 121 像素，精确到达 (361,890) ✅
//   用例 B: rdx=&pos,  r8=&pos（指针） -> 位移 0                              ❌
//   用例 C: rdx=&pos,  r8=&pos         -> 位移 0                              ❌
//
// ★ 教训：手写汇编里的 `mov rdx,400` 是**十六进制常数 0x400**，
//   不是"把某个地址放进 rdx"。曾把它误读成指针语义，结果是
//   走路 call 永远返回成功、日志全绿、角色纹丝不动 ——
//   这是最难查的一类 bug：不崩溃、不报错、就是不干活。
//
// ★ 打包顺序：r8 = (y << 32) | x，即**高位 y、低位 x**。
//   (x<<32)|y 是错的，写反了角色朝镜像方向跑。
struct WalkPos {
    int32_t x;
    int32_t y;
};
static_assert(sizeof(WalkPos) == 8, "WalkPos 必须是 8 字节");

constexpr uint64_t kActionWalkArgRdx       = 0x400;
constexpr uint64_t kActionWalkArgR9        = 0;
constexpr uint64_t kActionWalkArgStack20   = 0;
constexpr uint64_t kActionWalkArgStack40   = 0x400;

// 走路 call 的 r8 打包：高位 y、低位 x
inline uint64_t PackWalkPos(int32_t x, int32_t y)
{
    return ((uint64_t)(uint32_t)y << 32) | (uint64_t)(uint32_t)x;
}

// ------------------------------------------------------------
// 【旧】0x490 的 8 字节原始值 -> 世界像素
// ------------------------------------------------------------
// 新代码一律读 +0x444 / +0x448 的 int32，值就是世界像素，直接用。
// 下面这三个函数只留给 tools 里的历史对照程序，生产路径不得调用。
inline int32_t ReadAnimPosX(uint64_t raw)
{
    float f = 0.0f;
    memcpy(&f, &raw, 4);
    return (int32_t)(f / (float)kPosUnitPerPixel);
}

inline int32_t ReadAnimPosY(uint64_t raw)
{
    float f = 0.0f;
    memcpy(&f, ((const unsigned char*)(&raw)) + 4, 4);
    return (int32_t)(f / (float)kPosUnitPerPixel);
}

// 兼容旧名字
inline uint64_t PackAnimPos(int32_t x, int32_t y) { return PackWalkPos(x, y); }
inline int32_t  UnpackAnimX(uint64_t packed)      { return ReadAnimPosX(packed); }
inline int32_t  UnpackAnimY(uint64_t packed)      { return ReadAnimPosY(packed); }

// ------------------------------------------------------------
// 走路 call 的 x64 调用桩（机器码）
// ------------------------------------------------------------
// 为什么必须用机器码：vtable 槽是"Microsoft x64 成员函数"约定 ——
// 参数从 rcx 开始，而"this" 就是 rcx 里的动作系统对象。C++ 函数指针
// 无法干净地表达"this 显式当第一个参数 + 额外 2 个栈参数"，所以直接
// 拼一段机器码，语义一目了然，也不会被编译器按 thunk 优化。
//
// 目标调用形态：
//     rcx = this   (动作系统对象)
//     rdx = 坐标指针
//     r8  = 坐标指针（或 0，见消融模式）
//     r9  = 0
//     [rsp+0x20] = 0        (第 5 个参数)
//     [rsp+0x28] = 0x400    (第 6 个参数)
//     call [ [rcx] + 0x198 ]
//     ret
//
// 机器码逐条对应（注释即拆解，改的时候对照着看）：
//   48 83 EC 38              sub  rsp, 0x38        分配 shadow space
//   48 C7 44 24 20 00 00 00 00   mov qword [rsp+0x20], 0
//   48 C7 44 24 28 00 04 00 00   mov qword [rsp+0x28], 0x400
//   48 8B 01                 mov  rax, [rcx]       rax = vtable
//   FF 90 98 01 00 00        call qword [rax+0x198]
//   48 83 C4 38              add  rsp, 0x38
//   C3                       ret
//
// rcx / rdx / r8 / r9 由调用方（C++ 侧）直接按 x64 约定放好，
// 桩本身不碰它们，所以参数变化时只改 C++ 侧即可。
constexpr uint8_t kActionWalkStub[] = {
    0x48, 0x83, 0xEC, 0x38,                                     // sub rsp, 0x38
    0x48, 0xC7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00,       // mov qword [rsp+0x20], 0
    0x48, 0xC7, 0x44, 0x24, 0x28, 0x00, 0x04, 0x00, 0x00,       // mov qword [rsp+0x28], 0x400
    0x48, 0x8B, 0x01,                                           // mov rax, [rcx]
    0xFF, 0x90, 0x98, 0x01, 0x00, 0x00,                         // call qword [rax+0x198]
    0x48, 0x83, 0xC4, 0x38,                                     // add rsp, 0x38
    0xC3                                                        // ret
};

// 调用走路 call 的函数指针类型（与上面机器码一致）
using ActionWalkFn = void (*)(uint64_t actionSys, uint64_t rdx, uint64_t r8, uint64_t r9);


// ============================================================
// 技能 call（释放技能）
// ============================================================
// ★ 这一节是**新战斗模块**的地基，和上面的走路 call 是两条独立通道：
//     走路 call -> vtable [[动作系统]+0x198]，入口随 vtable 变
//     技能 call -> 固定 RVA 0x18C940 的具名函数
//   两者共用同一个 rcx（动作系统对象），所以"互斥"关系仍然存在：
//   往同一个动作队列塞东西，同时推进会互相打断。
//
// 反汇编依据（2026-11-08，游戏 0x7FF648A50000 基址下实测）：
//
//   --- 调用点（RVA 0x18C940 正体，调用者序言）---
//   0x7FF648BD13BB  mov dword ptr [rsp+30], r8d          ; 第 4 个栈参数
//   0x7FF648BD13C0  mov byte  ptr [rsp+28], al           ; 第 3 个栈参数
//   0x7FF648BD13C4  lea  rax, [rbp-40]
//   0x7FF648BD13C8  mov  [rsp+20], rax                   ; 第 1 个栈参数 = 释放坐标指针
//   0x7FF648BD13CD  lea  r9,  [rbp-60]                   ; shared_ptr<skill> 地址
//   0x7FF648BD13D1  movzx r8d, word ptr [rsp+40]         ; 0x400 / 0x401
//   0x7FF648BD13D7  mov  rdx, [rbp+28]                   ; 地图对象
//   0x7FF648BD13DB  mov  rcx, rdi                        ; this = 动作系统对象
//   0x7FF648BD13DE  call r10                             ; -> 技能 call
//
//   --- 技能 shared_ptr 的静态来源（不依赖任何虚调用）---
//   0x7FF648BCB1E3  movzx ebx, al                        ; 技能栏下标
//   ...             [[[[exe+0x45F15D8]+0x578]+0x330]+slot*8]+0x2D8 = _Ptr
//                                                              +0x2E0 = _Rep
//
// ★ 为什么优先走静态链而不是那段 `call [rax+0x3F0]`：
//   那个虚调用的作用经推断是"懒加载槽位对象 + 引用计数++"。但只要槽位
//   已经被游戏引用过（玩家用过一次该技能），静态链解出来的就是现成的
//   有效 shared_ptr，不需要替游戏做引用计数。虚调用的风险高得多
//   （槽位没对上就是把任意函数当成员函数调），所以降级为"静态链失效
//   时的唤醒手段"。
//
// ★ r8 语义（用户实测）：左键技能 / 键盘技能 / 空格翻滚 = 0x400，
//   鼠标右键技能 = 0x401。0x401 = 0x400 | 1，最低位应当就是
//   "右键栏位"标记（待诊断确认），所以这里保留成可组合的位。
constexpr uint64_t kSkillCallRva           = 0x18C940;   // 固定 RVA（具名函数）
constexpr uint64_t kSkillCallEntryRva      = 0x18C940;   // 校验用（同上，留一个语义化别名）

// 技能栏容器基址链（拿到 shared_ptr 数组的那条）
//   [[[[<此处>]+0x578]+0x330]+slot*8]+0x2D8
// 这是模块 RVA，实际地址 = exe + kSkillContainerBase。
// 2026-11-18：入口 0x45F1618；槽对象/shared_ptr/所有者和技能文本链已复核。
constexpr uint64_t kSkillContainerBase     = 0x45F1618;
constexpr uint64_t kSkillContainerOff0     = 0x578;
constexpr uint64_t kSkillContainerOff1     = 0x330;
constexpr uint64_t kSkillSlotStride        = 0x08;       // 每槽 8 字节指针
// ★★★ 2026-11-08 进程内实测修正（skillscan.dll）★★★
//
//   原来以为 [[...[+0x330]+slot*8]+0x2D8 这一级就直接把 shared_ptr 拿出来了，
//   实测是错的：[[...[+0x330]] 的落点是一个**指针数组**（每 8 字节一个堆指针），
//   必须先解引用该元素才是"技能槽对象"，shared_ptr 在**那个对象**的 +0x2D8。
//
//   实测数据（L2 落点 0x20848DE9FF0，slot 0~3）：
//     L2+0x000 -> obj 0x208482B5E10 -> +0x2D8 = {_Ptr 0x2078265C810, _Rep 0x2078265C800, ref 4}
//     L2+0x008 -> obj 0x208482B6410 -> +0x2D8 = {_Ptr 0x20782653A10, _Rep 0x20782653A00, ref 4}
//     L2+0x010 -> obj 0x208482B6A10 -> +0x2D8 = {_Ptr 0x20782658810, _Rep 0x20782658800, ref 6}  <- 右键(slot 2)
//     L2+0x018 -> obj 0x208482B7010 -> +0x2D8 = {_Ptr 0x2078265D210, _Rep 0x2078265D200, ref 4}
//   槽位对象间距 0x600；+0x2D8 的引用计数是 4~6，说明已被游戏引用，可直接用。
//
//   完整链：
//     L2      = [[[exe+0x45F15D8]+0x578]+0x330]
//     slotObj = *(L2 + slot*8)                 <- 这一跳原来漏了
//     _Ptr    = *(slotObj + 0x2D8)
//     _Rep    = *(slotObj + 0x2E0)
// ★★★ r9 的取值是整套参数里最容易搞错的一个 ★★★
//
//   r9 必须 = **shared_ptr 结构本身的地址**，也就是 slotObj + 0x2D8，
//   而**不是** slotObj（槽位对象基址）。
//
//   为什么：0x18C940 序言里有 `mov [rsp+20], r9`，后面把它当
//   shared_ptr<skill>* 用（读 +0x00 拿 _Ptr、+0x08 拿 _Rep）。
//   传 slotObj 的话，它会把槽位对象开头的两个字段当成 _Ptr/_Rep ——
//   那是无关数据，进去必崩（表现为 EXCEPTION_ACCESS_VIOLATION）。
//
//   实测（skillveh.dll，2026-11-08）：
//     r9 = slotObj        (0x4B2A9927C10) -> 崩溃
//     r9 = slotObj + 0x2D8(0x4B2A9927EE8) -> 成功，返回 2/3
//
//   返回值语义（从 0x18C940 的分支解出来）：
//     0 = 内层检查既不是 1 也不是 2，直接返回（当前不能放）
//     1 = 走中间那条分支
//     2 = 走 +0x5B 那条路径（真正的执行路径）
//     3 = 也属于成功返回（实战观测到 3 时不崩、动作入队）
constexpr uint64_t kSkillSharedPtrOff      = 0x2D8;      // 槽位对象内 -> shared_ptr<skill>
constexpr uint64_t kSkillSharedPtrRepOff   = 0x2E0;      // _Rep（_Ptr + 0x08）

// 槽位懒加载用的虚调用（静态链失效时的唤醒手段，诊断用）
//   rcx = [动作系统对象 + 0x390]
//   rax = [rcx] ; call [rax+0x3F0]
constexpr uint64_t kSkillLazyLoadOuterOff  = 0x390;
constexpr uint64_t kSkillLazyLoadVtblOff   = 0x3F0;

// 常见栏位下标（用户实测：鼠标右键 = 2）
constexpr uint8_t  kSkillSlotRightMouse     = 2;

// ---- 技能 call 的参数（Microsoft x64：rcx/rdx/r8/r9 + 20/28/30 三个栈参）----
//   rcx = 动作系统对象（与走路 call 同一个）
//   rdx = 地图对象
//   r8  = 0x400（左键/键盘/翻滚）或 0x401（右键技能）
//   r9  = shared_ptr<skill>*（16 字节：_Ptr / _Rep）
//   [rsp+0x20] = 释放坐标指针（指向 {int32 x; int32 y;} 世界像素）
//   [rsp+0x28] = 原反汇编里的 al（含义待诊断，先照抄 0）
//   [rsp+0x30] = 原反汇编里的 r8d（含义待诊断，先照抄 r8）
constexpr uint64_t kSkillArgR8Default      = 0x400;
constexpr uint64_t kSkillArgR8RightMouse   = 0x401;
constexpr uint64_t kSkillArgStack28        = 0;
constexpr uint64_t kSkillArgStack30Default = 0x400;

// 一次调用的栈帧大小：shadow(0x20) + 3 个栈参数(0x18) + 对齐余量
// 用 0x48 是为了让 rsp 在 sub 之后仍然 16 字节对齐（调用点要求）。
constexpr uint32_t kSkillCallStubStackSize = 0x48;

// shared_ptr<skill> 的布局（MSVC）
struct SharedPtrSkill {
    uint64_t ptr;   // _Ptr：技能对象
    uint64_t rep;   // _Rep：控制块
};
static_assert(sizeof(SharedPtrSkill) == 16, "shared_ptr 必须是 16 字节");

// ============================================================
// 技能调用"会话卫生"（修复"注入一次只能放一次技能"）
// ============================================================
// 现象：注入 Monitor 后第一次 Cast + Stop 正常，第二次 Cast 游戏卡死，
//       必须重启游戏，一次注入只能放一次技能。
//
// 根因是三条互相独立的状态污染，缺一条都会复发：
//
//   1) 状态位不复位。
//      0x18C940 尾部有 `mov [r14+0xE8], ebx`（r14 = [[this+0x1D0]+0x3F0]），
//      把本次执行结果写进这个 uint32。正常输入路径下游戏会在动作播完后
//      自己把它清回 0；我们从外部直接调用绕过了那条链，于是它永远停在
//      "施法中"。第二次进入时那条分支假定状态干净，踩到半初始化结构。
//      → 修复：调用前记下原值，调用后写回（kSkillStateOffE8）。
//
//   2) r9 传的是游戏自己的 shared_ptr 地址（slotObj+0x2D8）。
//      函数内部对 r9 的任何写都直接落在游戏对象上，正常路径由游戏自己
//      收拾，外部调用没有。→ 修复：传自建副本（栈上 16 字节）。
//
//   3) 控制块引用计数被"扣光"（★ 方向与直觉相反，这是真凶）。
//      实测（2026-11-09，本机游戏线程内）：一次成功 Cast 前后
//        _Uses  2 -> 1     （掉了 1）
//        _Weaks 6 -> 6     （没动）
//      也就是说 0x18C940 **对传进去的 shared_ptr 做了一次 DecRef** ——
//      它把 r9 当成"移交所有权"的入参来消费。
//      于是第一次 2->1 相安无事，第二次 1->0 直接触发
//      _Destroy() + _Delete_this()，**技能对象被析构**；
//      游戏下一次访问那个技能槽 -> 访问已释放对象 -> 卡死/闪退。
//      这就是"注入一次只能放一次技能"的精确机制。
//      → 修复：调用前先替它"预支"一份引用（+1，保证调用期间绝不到 0），
//             调用后把 _Uses 拉回调用前的值（双向，涨了减、掉了加）。
//
// 另外 m_actionSys 绝不能再跨调用缓存：函数调用后对象可能已被游戏换掉
// （动作队列重建 / 切图 / 换装），第二次还往旧对象上写就是直接崩。
constexpr uint64_t kSkillStateOffE8         = 0xE8;   // r14 -> 状态位（uint32）

// 单次注入允许的真实 Cast 次数上限（兜底保险：根因没修干净的版本也不会
// 把用户反复试到崩游戏；重新注入 Monitor 即可重置）。
// 500 次约等于 30 秒一次连跑 4 小时，正常玩绝对够用，同时仍能挡住
// "脚本跑飞/死循环狂调"这类把游戏打崩的事故。
constexpr uint32_t kSkillMaxCastsPerInject  = 500;

// 技能槽 shared_ptr 的 _Uses 正常下限。
// 实测一个被游戏正常引用的技能槽是 2（槽位容器 + 当前持有者）。
// 低于这个值说明已经被外部调用扣过了；还原时一律补到这个下限，
// 宁可多留一份引用（代价只是这个技能对象在本局不释放），也绝不
// 让它掉到 0 触发析构。
constexpr uint32_t kSkillMinRefUses         = 2;

// MSVC shared_ptr 控制块（_Ref_count_base）布局，x64：
//   +0x00  vfptr
//   +0x08  _Uses   （_Atomic_counter_t，32 位）   ← 实测确认：值 2
//   +0x0C  _Weaks  （_Atomic_counter_t，32 位）   ← 实测确认：值 6
// ★ 旧注释把 +0xC 当成 _Uses，实际那是 _Weaks。这条错误注释间接导致了
//   "以为是泄漏、去减计数"的错误修法方向。
constexpr uint64_t kRefUsesOff              = 0x08;
constexpr uint64_t kRefWeakOff              = 0x0C;


// ------------------------------------------------------------
// 组件字段偏移（续）
// ------------------------------------------------------------
// Life 组件：当前血量 UINT32
constexpr uint64_t kLife_HpOff             = 0x1E0;
// Life resources: life / mana / energy shield. Resource +0x34 maximum, +0x38 current.
// Verified by the common resource methods and type getters (0 / 1 / 2).
constexpr uint64_t kLifeVtableRva = 0x34E01D8;
constexpr uint64_t kLifeResourceOff[] = {0x1A8, 0x200, 0x240};
constexpr uint64_t kLifeResourceVtableRva[] = {0x34DFB28, 0x34DF170, 0x34DEE20};
constexpr uint64_t kLifeResourceOwnerOff = 0x10;
constexpr uint64_t kLifeResourceMaximumOff = 0x34;
constexpr uint64_t kLifeResourceCurrentOff = 0x38;
constexpr uint64_t kLife_MaxHpOff = 0x1DC;
constexpr uint64_t kLife_MaxManaOff = 0x234, kLife_ManaOff = 0x238;
constexpr uint64_t kLife_MaxEnergyShieldOff = 0x274, kLife_EnergyShieldOff = 0x278;

// Player 组件：玩家名在 +0x1B0
constexpr uint64_t kPlayer_NameOff         = 0x1B0;

// WorldItem 组件：+0x28 指向掉落物物品实体
constexpr uint64_t kWorldItem_EntityOff    = 0x28;

// Base 组件：+0x10 -> 物品信息对象；物品信息 +0x30 = 名称字符串对象
constexpr uint64_t kBase_ItemInfoOff       = 0x10;
constexpr uint64_t kBase_NameOff           = 0x30;

// Stack 组件：+0x18 直接是堆叠数量（BYTE，不解引用）
constexpr uint64_t kStack_CountOff         = 0x18;

// Mods 组件：+0x18 为 shared_ptr 名称行 vector；金装通常2项，暗金只有1项。
// 行 +0x30 是 UTF-16Z 指针，不是 std::wstring；不可读取 vector.end 以外的后半。
constexpr uint64_t kMods_VectorOff         = 0x18;
constexpr uint64_t kMods_PrefixEntryStride = 0x10;
constexpr uint64_t kMods_MaxNameParts      = 2;
constexpr uint64_t kMods_PrefixFrontVecOff = 0x00;
constexpr uint64_t kMods_PrefixBackVecOff  = 0x10;
constexpr uint64_t kMods_PrefixNameOff     = 0x30;
constexpr uint64_t kMods_GradeOff          = 0x94;   // BYTE: 0白 1蓝 2金 3暗金



// 2026-11-18 入口指纹；PE 时间戳也必须匹配。
constexpr uint8_t kCostCallSignature[] = {
    0x44, 0x8B, 0x1A, 0x4C, 0x8B, 0xC9, 0x45, 0x85, 0xDB, 0x78, 0x71
};
constexpr uint8_t kActionWalkSignature[] = {
    0x0F, 0xB6, 0x44, 0x24, 0x28, 0x88, 0x44, 0x24, 0x28
};
constexpr uint8_t kSkillCallSignature[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x4C, 0x89, 0x4C, 0x24, 0x20,
    0x66, 0x44, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54
};
constexpr uint8_t kSkillStopSignature[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40,
    0x48, 0x8B, 0x99, 0xA8, 0x01, 0x00, 0x00, 0x48, 0x8D, 0x54,
    0x24, 0x20, 0xB8, 0xAC, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xF9
};

// ID-based skill loop. These RVAs must not be hardcoded in Monitor.
constexpr uint64_t kIdSkillCallRva = 0x211EF20;
constexpr uint64_t kSkillManagerGetterRva = 0x16DF8A0;
constexpr uint64_t kIdSkillContextActorOff = 0x198;
constexpr uint64_t kIdSkillCurrentIdOff = 0x214;
// Sixteen manual Space presses verified this complete runtime ID on 2026-11-19.
// The 0x80000041 variant exists but its activation conditions are unverified.
constexpr uint32_t kDodgeRollSkillId = 0x80000040;
constexpr uint64_t kSkillManagerGetterVtblOff = 0x168;
constexpr uint64_t kSkillManagerInActorOff = 0x9C0;
constexpr uint64_t kSkillManagerVectorOff = 0x148;
constexpr uint8_t kIdSkillCallSignature[] = {
    0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,
    0x55,0x57,0x41,0x56,0x48,0x8B,0xEC,0x48,0x81,0xEC,0x80,0,0,0
};
constexpr uint8_t kSkillManagerGetterSignature[] = {0x48,0x8D,0x81,0xC0,0x09,0,0,0xC3};

// Read-only skill-slot layout, live-verified 2026-11-18.
// Names are UTF-16Z; skill ID is a complete uint32_t.
namespace skilllayout {
constexpr uint64_t kVectorEnd = kSkillContainerOff1 + 8;
constexpr uint64_t kVectorCapacity = kSkillContainerOff1 + 16;
constexpr uint64_t kRuntimeSkillId = 0x40; // uint32_t, NOT uint16_t
// Built-in Move has no level/granted-effect row. Its UTF-16Z action code is
// Q(Q(skill + 0x48) + 0), live-verified 2026-11-18 without game calls.
constexpr uint64_t kBaseSkillRecord = 0x48;
constexpr uint64_t kBaseSkillIdText = 0;
constexpr uint64_t kLevelRecord = 0x58;
constexpr uint64_t kGrantedEffectRecord = 0;
constexpr uint64_t kGrantedEffectIdText = 0;
// Packed GrantedEffects -> ActiveSkills row; +0x4F is intentionally unaligned.
// The old skillPtr+0x378 hop is invalid in this build.
constexpr uint64_t kActiveSkillRecord = 0x4F;
constexpr uint64_t kActiveSkillCodeText = 0;
constexpr uint64_t kLocalizedNameText = 0x08;
constexpr uint64_t kSkillOwner = 0xB8;
constexpr uint64_t kOwnerEntity = 0x140;
constexpr uint64_t kComponentOwnerEntity = 0x08;
constexpr uint64_t kIdInstructionRva = 0x1857A1;
constexpr uint8_t kIdInstruction[] = {0x8B, 0x51, 0x40, 0x48, 0x8B, 0xCE};
constexpr uint64_t kMaxSlots = 64;
}

// Read-only monster identity, verified by boss_probe on 2026-11-19.
namespace monsterlayout {
constexpr uint64_t kWorldBossCount = 0x9C, kWorldBossData = 0xA4;
constexpr uint64_t kMonsterVtable = 0x34DF1E8, kMagicVtable = 0x34E7BB8;
constexpr uint64_t kComponentOwner = 0x08, kTableName = 0x08;
constexpr uint64_t kMonsterData = 0x18, kVarietyRow = 0x10, kVarietyTable = 0x18, kVarietyName = 0x110;
constexpr uint64_t kMagicRarity = 0x144, kRuntimeMods = 0x150, kRuntimeModStride = 0x40;
constexpr uint64_t kRuntimeModRow = 0x28, kRuntimeModTable = 0x30;
constexpr uint64_t kModStatRefs = 0x1E, kReferenceStride = 0x10;
constexpr unsigned kModStatSlots = 4;
}
