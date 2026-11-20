#pragma once
#include <cstdint>
#include <cstring>

// ============================================================
// Protocal.h - 通信协议定义（服务端与客户端共用）
// 管道名：\\.\pipe\<AppConfig.pipeName>（默认 Lmy）
//
// 数据包格式：
//   +----------------------+----------------------+
//   | PacketHeader (8字节) | payload (变长)       |
//   +----------------------+----------------------+
//   | m_MsgType : 4字节   |                      |
//   | m_len     : 4字节   | 长度由 m_len 指定     |
//   +----------------------+----------------------+
//
// 本文件被 Plug 与 Monitor 同时编译，改字段必须两边一起改，
// 且**只能往尾部追加**（老包长度兼容由接收端按 sizeof 判断）。
// ============================================================

// 包体最大长度
#define MSG_LEN 2048
// 包头固定长度（字节）
#define HEADER_LEN 8

// 批量航点包：包体 = NavBatchHeader + NavBatchChunk
#ifndef NAV_BATCH_HDR_LEN
#define NAV_BATCH_HDR_LEN 15
#endif

// 消息类型
enum MessageType {
	MT_Text,        // 文本消息
	MT_Ack,         // 确认响应
	MT_Exit,        // 退出通信
	MT_DataChunk,   // 二进制数据分块
	MT_MapReq,      // 客户端请求服务端扫描地图（客户端->服务端）
	MT_MapMeta,     // 地图代价表元信息（宽、高、行跨度、图层）
	MT_MapRow,      // 地图代价表一行数据（打包的 nibble 字节流）

	// ---- 自动寻路（Plug <-> Monitor）----
	MT_NavStatus,   // Monitor -> Plug：玩家当前坐标 + 状态（上行，高频）
	MT_NavGo,       // Plug -> Monitor：下发一个目标航点（世界坐标）
	// ---- 键盘驱动（WASD）----
	// 和 MT_NavGo 的区别：那个走的是游戏的动作系统 vtable，
	// 这个走的是 WASD 按键。按键方向已实测标定，更可靠。
	MT_NavKeyGo,
	MT_NavKeyStop,
	MT_NavStop,     // Plug -> Monitor：中止导航
	MT_NavAbort,    // Monitor -> Plug：导航异常终止（附原因码）

	// ---- 整条路径一次下发（GPS 重规划模式的主力通道）----
	// 一次寻路有几十~上百个航点，逐点发包在管道上来回太碎。
	// 这里按批发送，接收端按 idx 拼进同一张路径表。
	MT_NavBatch,    // Plug -> Monitor：一批航点

	MT_NavCfg,      // Plug -> Monitor：运行期改导航参数
									// Monitor -> Plug：回显当前生效值（同一结构）
	MT_ScanBmp,     // Plug -> Monitor：把当前代价表存成 BMP（诊断）

	// ---- 走路 call 槽位扫描（诊断，一次性用）----
	// 游戏更新后 vtable 里的走路函数换了槽位，靠静态分析定不下来，
	// 只能让游戏自己试：逐个 (对象, 槽位, 参数形态) 调一次，看玩家坐标有没有动。
	// 只用于定位，找到之后把 kActionWalkVtblOff 写死，这条消息不再用。
	MT_WalkScan,
	MT_WalkScanAck, // Monitor -> Plug：扫描结束（附找到的槽位，-1 = 没找到）

	// ---- 技能释放（新战斗模块的地基）----
	// 与走路 call 是两条独立通道：走路走 vtable、技能走固定 RVA 0x18C940。
	// 两者共用同一个动作系统对象（rcx），会抢同一个动作队列。
	MT_SkillCast,   // Plug -> Monitor：释放一次技能（带完整参数，可消融）
	MT_SkillAck,    // Monitor -> Plug：调用结果回执
	MT_SkillStop,      // 只停止施法（不释放），走 0x211F0A0
	MT_SkillStopAck,   // 停止结果回执

	// ---- 裸走路 call 标定（标定工具用）----
	// 与 MT_NavGo 的区别：MT_NavGo 走 NavDriver 的路径状态机（m_pts/m_idx、
	// 到位判定、卡住判定、续推），那是"走一条路"的语义。
	// 这一条是"只调一次走路 call + 立刻回当前坐标"，不带任何状态机，
	// 专门用来标定"call 一次能走多远 / 走多久 / 停在哪"。
	MT_WalkCall,      // Plug -> Monitor：裸调一次走路 call
	MT_WalkCallAck,   // Monitor -> Plug：调用结果 + 当前角色坐标 + 距目标距离
	MT_ManagedHello, MT_ManagedWelcome, MT_ManagedPacket,
	MT_PositionReq, MT_PositionReply, MT_RequestDone, MT_ManagedError,
	MT_Heartbeat, MT_Subscribe, MT_GameStatus, MT_LogEvent,
	MT_ComboCast, MT_ComboCastAck, // ID-based combo press/release; appended for compatibility.
	MT_PacketCommand, MT_PacketReply, MT_PacketCapture,
};

// 包头：8 字节 = 消息类型(4B) + 包体长度(4B)
class PacketHeader {
public:
	MessageType m_MsgType;
	uint32_t    m_len;
};
static_assert(sizeof(PacketHeader) == 8, "PacketHeader must be 8 bytes");

// 文本消息：包头 + 文本数据
class TextMessage : public PacketHeader {
public:
	TextMessage(const char* text, uint32_t len) {
		m_MsgType = MT_Text;
		if (len > MSG_LEN) len = MSG_LEN;
		memcpy(m_Data, text, len);
		m_len = len;
	}
	char m_Data[MSG_LEN] = { 0 };
};

// 二进制数据分块：包头 + 二进制数据
class DataChunkMessage : public PacketHeader {
public:
	DataChunkMessage(const void* data, uint32_t len) {
		m_MsgType = MT_DataChunk;
		if (len > MSG_LEN) len = MSG_LEN;
		memcpy(m_Data, data, len);
		m_len = len;
	}
	char m_Data[MSG_LEN] = { 0 };
};

// 地图代价表元信息（先发一条，随后按行发送 MT_MapRow）
#pragma pack(push, 1)
struct MapMeta {
	uint32_t width;      // 世界坐标宽（像素数，x 方向）
	uint32_t height;     // 世界坐标高（像素数，y 方向）
	uint32_t rowStride;  // 每行字节数（= width/2，nibble 打包）
	uint8_t  layer;      // 图层号（CostCall 的 r8 参数）
	uint8_t  reserved[3];
};
#pragma pack(pop)

class MapMetaMessage : public PacketHeader {
public:
	MapMetaMessage(const MapMeta& meta) {
		m_MsgType = MT_MapMeta;
		m_len = sizeof(MapMeta);
		memcpy(m_Data, &meta, sizeof(MapMeta));
	}
	char m_Data[sizeof(MapMeta)] = { 0 };
};

// 地图代价表一行数据：包头 + 该行的打包字节
// 约定：payload 前 12 字节为 MapRowHeader，其后为该行数据（可能跨多条消息分块发送）
#pragma pack(push, 1)
struct MapRowHeader {
	uint32_t rowIndex;   // 行号（y）
	uint32_t offset;     // 该包在本行内的字节偏移
	uint32_t rowStride;  // 本行总字节数
};
#pragma pack(pop)

class MapRowMessage : public PacketHeader {
public:
	// data/len 为「MapRowHeader + 行数据」整体
	MapRowMessage(const void* data, uint32_t len) {
		m_MsgType = MT_MapRow;
		if (len > MSG_LEN) len = MSG_LEN;
		memcpy(m_Data, data, len);
		m_len = len;
	}
	char m_Data[MSG_LEN] = { 0 };
};

// 地图扫描请求（无包体，客户端发给服务端触发扫图）
class MapReqMessage : public PacketHeader {
public:
	MapReqMessage() {
		m_MsgType = MT_MapReq;
		m_len = 0;
	}
};

// 确认响应（无包体）
class AckMessage : public PacketHeader {
public:
	AckMessage() {
		m_MsgType = MT_Ack;
		m_len = 0;
	}
};

// 退出消息（无包体）
class ExitMessage : public PacketHeader {
public:
	ExitMessage() {
		m_MsgType = MT_Exit;
		m_len = 0;
	}
};

// ============================================================
// 自动寻路消息
// ============================================================

// 导航状态（Monitor -> Plug）
//   每次走路 call 下发后上报一次，Plug 用它做进度跟踪和卡住检测
#pragma pack(push, 1)
struct NavStatus {
	int32_t  x;          // 玩家 Positioned+0x444 的 INT32 世界坐标 x
	int32_t  y;          // 玩家 Positioned+0x448 的 INT32 世界坐标 y
	int32_t  targetX;    // 当前目标航点 x
	int32_t  targetY;    // 当前目标航点 y
	uint32_t remaining;  // 还剩几个航点（Plug 侧只用来显示）
	uint8_t  state;      // 见 NavState
	uint8_t  reserved[3];
};
#pragma pack(pop)

// NavStatus.state 取值
enum NavState : uint8_t {
	NAV_IDLE     = 0,   // 没有导航任务
	NAV_MOVING   = 1,   // 正在朝目标点走
	NAV_REACHED  = 2,   // 已抵达当前目标航点
	NAV_ARRIVED  = 3,   // 全部航点走完
	NAV_STUCK    = 4,   // 卡住（位置没变化）
	NAV_FAILED   = 5    // 走路 call 调用失败 / 找不到动作系统
};

class NavStatusMessage : public PacketHeader {
public:
	NavStatusMessage(const NavStatus& st) {
		m_MsgType = MT_NavStatus;
		m_len = sizeof(NavStatus);
		memcpy(m_Data, &st, sizeof(NavStatus));
	}
	char m_Data[sizeof(NavStatus)] = { 0 };
};

// 下发一个目标航点（Plug -> Monitor）
#pragma pack(push, 1)
struct NavGo {
	int32_t x;           // 目标动画坐标 x
	int32_t y;           // 目标动画坐标 y
	uint16_t tolerance;  // 到达判定容差（世界像素），0 = 用默认值
	uint16_t speed;      // 预留：移动速度档位，0 = 默认
};
#pragma pack(pop)

class NavGoMessage : public PacketHeader {
public:
	NavGoMessage(const NavGo& go) {
		m_MsgType = MT_NavGo;
		m_len = sizeof(NavGo);
		memcpy(m_Data, &go, sizeof(NavGo));
	}
	char m_Data[sizeof(NavGo)] = { 0 };
};

// 键盘驱动下单点（Plug -> Monitor）
//
// 和 NavGoMessage 的包体完全一样（都是 NavGo），只是消息类型不同。
// Monitor 收到 MT_NavKeyGo 时走 WASD 按键驱动，收到 MT_NavGo 时
// 走动作系统 vtable —— 两条实现并行，方便对比和回退。
class NavKeyGoMessage : public PacketHeader {
public:
	NavKeyGoMessage(const NavGo& go) {
		m_MsgType = MT_NavKeyGo;
		m_len = sizeof(NavGo);
		memcpy(m_Data, &go, sizeof(NavGo));
	}
	char m_Data[sizeof(NavGo)] = { 0 };
};

// 中止导航（Plug -> Monitor，无包体）
class NavStopMessage : public PacketHeader {
public:
	NavStopMessage() {
		m_MsgType = MT_NavStop;
		m_len = 0;
	}
};

// ------------------------------------------------------------
// 批量航点（Plug -> Monitor）
//
// 包体布局：
//   [0]  NavBatchHeader（15 字节）
//   [15] NavBatchChunk × count（每个 8 字节：int32 x + int32 y）
//
// 为什么这么设计：
//   GPS 重规划模式每帧重算一次路径，但绝大多数帧路径"没实质变化"。
//   把整条路径按批推给 Monitor，Monitor 侧按 idx 对齐就地更新，
//   于是：
//     · 路径没变 -> 一个包都不用发（Plug 侧算好哈希比对）
//     · 路径变了一点 -> 只发变化的那一批
//   Monitor 永远拿到的是"当前完整路径"，不会出现半条路径的中间态。
// ------------------------------------------------------------
#pragma pack(push, 1)
struct NavBatchHeader {
	uint32_t seq;        // 这一轮路径的序号（新目标点会 +1）
	uint32_t total;      // 本轮路径总航点数
	uint32_t baseIdx;    // 本包第一个航点在整条路径里的下标
	int16_t  tolerance;  // 到达容差（世界像素），0 = 用默认
	uint8_t  flags;      // bit0 = 本轮最后一个包（Monitor 可据此裁掉多余尾巴）
};
#pragma pack(pop)

struct NavBatchChunk {
	int32_t x;
	int32_t y;
};
static_assert(sizeof(NavBatchChunk) == 8, "NavBatchChunk must be 8 bytes");

// 单包最多能带几个航点（留出包头余量）
#define NAV_BATCH_MAX_CHUNKS \
	((MSG_LEN - NAV_BATCH_HDR_LEN) / (int)sizeof(NavBatchChunk))

class NavBatchMessage : public PacketHeader {
public:
	// pts 指向 baseIdx 起的 count 个航点（{int32 x; int32 y;} 连续布局）
	NavBatchMessage(uint32_t seq, uint32_t total, uint32_t baseIdx,
	                int16_t tolerance, bool isLast,
	                const NavBatchChunk* pts, uint32_t count) {
		m_MsgType = MT_NavBatch;

		uint32_t n = count;
		const uint32_t cap = (uint32_t)NAV_BATCH_MAX_CHUNKS;
		if (n > cap) n = cap;

		const uint32_t body = (uint32_t)(NAV_BATCH_HDR_LEN + n * sizeof(NavBatchChunk));
		m_len = body;
		uint8_t* p = (uint8_t*)m_Data;

		NavBatchHeader h{};
		h.seq       = seq;
		h.total     = total;
		h.baseIdx   = baseIdx;
		h.tolerance = tolerance;
		h.flags     = isLast ? 1u : 0u;
		memcpy(p, &h, sizeof(h));
		if (n && pts) {
			memcpy(p + NAV_BATCH_HDR_LEN, pts, (size_t)n * sizeof(NavBatchChunk));
		}
	}
	uint8_t m_Data[MSG_LEN] = { 0 };
};

// ------------------------------------------------------------
// 运行期导航参数（Plug -> Monitor 下发，Monitor -> Plug 回显）
//
// 为什么存在：这些参数全是"实测调出来的手调值"，不同机器/不同
// 网络/不同地图都要微调。写死在 DLL 里每次都得重新注入，太难受。
// ------------------------------------------------------------
#pragma pack(push, 1)
struct NavCfg {
	int16_t  arrive        = 13;      // 到达容差（世界像素）
	int16_t  skipNear      = 30;     // 离得比这更近的航点直接跳过
	int16_t  stuckEps      = 2;      // 一 tick 内位移小于它就当没动
	uint16_t maxMsWaypoint = 12000;  // 单点超时（毫秒）
	uint16_t settleMs      = 120;    // 到点后静默期（毫秒）
	uint16_t reissueMs     = 1000;   // 下发后多久没动才补发
	uint16_t noProgressMs  = 2500;   // 距离多久没缩短算软卡死
	uint8_t  maxReissue    = 2;      // 单点最多补发几次
	uint8_t  backOff       = 15;     // 沿路径回退几格（防回头）
	uint8_t  flags         = 0;      // bit0=允许回头(一般关) bit1=按索引推进(默认开)
	uint8_t  reserved      = 0;
};
#pragma pack(pop)
static_assert(sizeof(NavCfg) == 18, "NavCfg 必须是 18 字节（两端共用）");

class NavCfgMessage : public PacketHeader {
public:
	NavCfgMessage(const NavCfg& c) {
		m_MsgType = MT_NavCfg;
		m_len = sizeof(NavCfg);
		memcpy(m_Data, &c, sizeof(NavCfg));
	}
	char m_Data[sizeof(NavCfg)] = { 0 };
};

// ------------------------------------------------------------
// 扫图落盘请求（Plug -> Monitor）
// Monitor 在注入端把当前代价表存成 BMP（游戏内视角，不经过管道）。
// 主要用来判断"是图错了还是寻路错了"。
// ------------------------------------------------------------
#pragma pack(push, 1)
struct ScanBmpReq {
	uint8_t layer   = 0;
	uint8_t style   = 0;   // 0=raw 1=grade 2=walkable
	uint8_t markStart = 1; // 是否把玩家位置画成红点
	uint8_t reserved = 0;
	int32_t markX   = -1;  // 额外标记点（-1 = 不用）
	int32_t markY   = -1;
};
#pragma pack(pop)

class ScanBmpMessage : public PacketHeader {
public:
	ScanBmpMessage(const ScanBmpReq& req) {
		m_MsgType = MT_ScanBmp;
		m_len = sizeof(ScanBmpReq);
		memcpy(m_Data, &req, sizeof(ScanBmpReq));
	}
	char m_Data[sizeof(ScanBmpReq)] = { 0 };
};

// ------------------------------------------------------------
// 走路 call 槽位扫描（Plug -> Monitor）
//
// 游戏更新后 kActionWalkVtblOff 失效：解出来的入口不是走路函数，
// 调用返回成功但角色不动。静态分析只能猜到 0x1A0/0x1A8 这类候选项，
// 实测都不对，所以改成让游戏自己挨个试。
//
// 判据：调一次 → 等 settleMs → 读玩家坐标，位移超过 minMove 就算命中。
// ------------------------------------------------------------
#pragma pack(push, 1)
struct WalkScanReq {
	uint32_t slotStart = 0x000;   // 起始槽位偏移（含）
	uint32_t slotEnd   = 0x300;   // 结束槽位偏移（不含）
	int32_t  tx        = 0;       // 目标世界像素（0,0 = 玩家右侧 3 格）
	int32_t  ty        = 0;
	uint8_t  argMode   = 0;       // 0=rdx:0x400+r8打包  1=rdx/r8都传指针  2=rdx:0 r8打包  3=rdx打包 r8:0
	uint8_t  objMode   = 0;       // 0=全局链对象  1=玩家 Actor 组件  2=玩家 Pathfinding 组件
	uint16_t settleMs  = 400;     // 每次调用后等多久再看坐标
	uint64_t objAddr   = 0;       // 非 0 = 直接用这个对象地址（压过 objMode）
};
#pragma pack(pop)

class WalkScanMessage : public PacketHeader {
public:
	WalkScanMessage(const WalkScanReq& req) {
		m_MsgType = MT_WalkScan;
		m_len = sizeof(WalkScanReq);
		memcpy(m_Data, &req, sizeof(WalkScanReq));
	}
	char m_Data[sizeof(WalkScanReq)] = { 0 };
};

// 扫描结果回执
#pragma pack(push, 1)
struct WalkScanAck {
	int32_t  slot     = -1;   // 命中的槽位偏移，-1 = 没找到
	uint8_t  argMode  = 0;
	uint8_t  objMode  = 0;
	int32_t  fromX    = 0;
	int32_t  fromY    = 0;
	int32_t  toX      = 0;
	int32_t  toY      = 0;
	int32_t  tested   = 0;    // 实际试了多少个槽位
};
#pragma pack(pop)

class WalkScanAckMessage : public PacketHeader {
public:
	WalkScanAckMessage(const WalkScanAck& ack) {
		m_MsgType = MT_WalkScanAck;
		m_len = sizeof(WalkScanAck);
		memcpy(m_Data, &ack, sizeof(WalkScanAck));
	}
	char m_Data[sizeof(WalkScanAck)] = { 0 };
};

// ============================================================
// 技能释放消息
// ============================================================
// 技能 call 的完整参数（语义见 GameOffsets.h 的"技能 call"一节）。
// 这里允许覆盖每一个参数，是为了让诊断阶段能做 A/B 消融：
// 一次只改一个字段，看技能到底有没有放出去。
#pragma pack(push, 1)
struct SkillCastReq {
	int32_t  slot      = 2;       // 技能栏下标（右键 = 2）
	uint64_t r8        = 0x401;   // 0x400（左键/键盘/翻滚） 0x401（右键技能）
	uint64_t stack28   = 0;       // 原反汇编里的 al（含义待定）
	uint64_t stack30   = 0x400;   // 原反汇编里的 r8d（含义待定）
	int32_t  posX      = 0;       // 释放坐标（世界像素）
	int32_t  posY      = 0;

	// 消融开关：
	//   rdxMode 0 = 地图对象本体（kMapObjectChain 末级 +0x8F8）
	//           1 = 实测链末尾 +0x78（不含 +0x8F8）
	//   r9Mode  0 = 静态链   1 = 先走懒加载虚调用再读静态链
	//   dryRun  1 = 只解链打日志，不动游戏（默认）
	uint8_t  rdxMode   = 0;
	uint8_t  r9Mode    = 0;
	uint8_t  dryRun    = 1;

	// 释放后自动停止（验证"停止施法"用）：>0 = Cast 成功后睡这么多毫秒，
	// 再调一次 0x211F0A0（停止发包 call）。0 = 不停止。
	uint32_t stopAfterMs = 0;

	// ---- 释放坐标模式 ----
	//   posMode 0 = 用上面的 posX/posY
	//           1 = 角色前方（Monitor 侧实时读玩家坐标自己算，忽略 posX/posY）
	//
	//   ★ 默认 1。实测把坐标设成"角色脚下"（pos 与玩家位置重合）时游戏偶尔卡死，
	//     设在正前方一段距离更接近真实操作（鼠标点前方地面）。
	uint8_t  posMode = 1;
	uint8_t  _pad2[3] = { 0, 0, 0 };

	// 前方距离（世界像素）。60 ≈ 2.6 格，和普通技能作用距离接近。
	int32_t  aheadDistance = 60;

	// ---- 会话卫生开关（修复"第二次必崩"）----
	//   bit0 = 调用后把 [r14+0xE8] 状态位写回调用前的值
	//   bit1 = 控制块引用计数增量还原
	//   bit2 = 调用前深度校验（[[_Ptr]+0x520] 必须是模块内函数起点）
	//   bit3 = 手动边走边放诊断通道（仅显式测试请求使用）
	//
	//   默认 0x03（前两条开）。bit2 关掉是有原因的：实测那版加固校验反而
	//   更容易失败，只作为诊断手段显式打开。想要 A/B 对比旧行为传 0。
	//
	//   ★ bit3：仅边走边放诊断面板的单次测试置位，用来绕过人工施法的静止
	//     门槛；技能方案序列和自动战斗功能已移除，不应由后台任务设置此位。
	uint8_t  hygiene   = 0x03;
};
#pragma pack(pop)

class SkillCastMessage : public PacketHeader {
public:
	SkillCastMessage(const SkillCastReq& req) {
		m_MsgType = MT_SkillCast;
		m_len = sizeof(SkillCastReq);
		memcpy(m_Data, &req, sizeof(SkillCastReq));
	}
	char m_Data[sizeof(SkillCastReq)] = { 0 };
};

#pragma pack(push, 1)
struct SkillAck {
	int32_t  result     = -1;   // 0=Success，其余见 navskill::SkillResult
	int32_t  retValue   = -1;   // 技能 call 的返回值
	uint64_t actionSys  = 0;    // rcx
	uint64_t mapObject  = 0;    // rdx（实际用的那个）
	uint64_t mapObjectA = 0;    // rdx 另一种候选（对照）
	uint64_t skillPtrAddr = 0;  // r9
	uint64_t skillPtr     = 0;  // [_Ptr]
	uint64_t skillRep     = 0;  // [_Rep]
	uint32_t skillRefCnt  = 0;
	int32_t  posX       = 0;
	int32_t  posY       = 0;
	uint32_t callCount  = 0;

	// ---- 会话卫生观测（修复"第二次必崩"时加的诊断口子）----
	// 引用计数调用前后的值。正常情况 usesAfter == usesBefore（已被还原）；
	// 若 usesAfter > usesBefore 说明还原失败（控制块不可写 / SEH），
	// 此时下一次 Cast 就该被拒绝，而不是硬上。
	uint32_t usesBefore = 0;
	uint32_t usesAfter  = 0;
	uint32_t weakBefore = 0;
	uint32_t weakAfter  = 0;
	uint8_t  stateRestored = 0;   // 状态位是否成功写回
	uint8_t  hygiene       = 0;   // 本次实际生效的卫生位（回显）
	uint8_t  _pad3[2]      = { 0, 0 };
	uint32_t budgetLeft    = 0;   // 本次注入还剩几次 Cast 额度
};
#pragma pack(pop)

class SkillAckMessage : public PacketHeader {
public:
	SkillAckMessage(const SkillAck& ack) {
		m_MsgType = MT_SkillAck;
		m_len = sizeof(SkillAck);
		memcpy(m_Data, &ack, sizeof(SkillAck));
	}
	char m_Data[sizeof(SkillAck)] = { 0 };
};

// ============================================================
// 只停止施法（不释放技能）
//
// 为什么单独做一个消息：
//   释放 call（0x18C940）的状态依赖太强，多数时候空转，反复调会让游戏
//   内存暴涨直至崩溃。而停止 call（0x211F0A0）结构简单、每次都能生效。
//
//   所以把"停止"独立出来：用户自己按技能键放技能，我们用这个 call 打断。
//   这样完全不碰释放逻辑，风险最低。
// ============================================================
struct SkillStopReq {
	// 保留字段，当前实现不需要任何参数（rcx 由 Monitor 内部解链得到）
	int32_t reserved = 0;
};

class SkillStopMessage : public PacketHeader {
public:
	SkillStopMessage(const SkillStopReq& req) {
		m_MsgType = MT_SkillStop;
		m_len = sizeof(SkillStopReq);
		memcpy(m_Data, &req, sizeof(SkillStopReq));
	}
	char m_Data[sizeof(SkillStopReq)] = { 0 };
};

struct SkillStopAck {
	int32_t  result = 0;		// 0 = 成功，非 0 = 失败
	int32_t  reserved = 0;
	uint64_t stopThis = 0;		// 解出的 rcx（诊断）
	uint8_t  gateBefore = 0;
	uint8_t  gateAfter  = 0;
	uint8_t  flagBefore = 0;
	uint8_t  flagAfter  = 0;
};

class SkillStopAckMessage : public PacketHeader {
public:
	SkillStopAckMessage(const SkillStopAck& ack) {
		m_MsgType = MT_SkillStopAck;
		m_len = sizeof(SkillStopAck);
		memcpy(m_Data, &ack, sizeof(SkillStopAck));
	}
	char m_Data[sizeof(SkillStopAck)] = { 0 };
};

// ============================================================
// 裸走路 call 标定（Plug -> Monitor -> Plug）
//
// 用途：一次性调走路 call，回当前角色坐标，让调用方自己算距离。
// 不带路径状态机 —— 现有 MT_NavGo 走的是 NavDriver 那套（m_pts/m_idx、
// 到位判定、自动续推），拿来标定"一次 call 走多远"会被状态机干扰：
// 它会自动跳过身边航点、自动续推、自动判卡住，测出来的不是裸 call 的行为。
//
// 这条通道**只做两件事**：
//   1) g_nav.Walker().WalkTo(x, y)  —— 真实调用一次，不经过任何判定
//   2) 立刻读一次玩家坐标返回
// 其余全交给调用方。
// ============================================================
#pragma pack(push, 1)
struct WalkCallReq {
	int32_t  x = 0;			// 目标世界坐标 x
	int32_t  y = 0;			// 目标世界坐标 y
	uint16_t traceRunId = 0;	// 0 = 非导航诊断调用；非 0 时与 Plug 的 nav_trace 文件关联
	uint16_t traceCallId = 0;	// 当前导航轮内 WalkCall 序号（0 = 未设置）
};

struct WalkCallAck {
	int32_t  ok = 0;		// 0 = 调用了；1 = 未就绪；2 = 调用失败（见 result）
	int32_t  result = 0;	// WalkResult 枚举值（0=Success）
	int32_t  playerX = 0;	// 调用后立刻读到的角色坐标
	int32_t  playerY = 0;
	int32_t  distBefore = -1;	// 调用**前**角色到目标的距离（世界坐标）
	int32_t  distAfter = -1;	// 调用**后**角色到目标的距离
	int32_t  targetX = 0;
	int32_t  targetY = 0;
	uint32_t elapsedMs = 0;	// WalkTo 这次调用本身花了多久
};
#pragma pack(pop)
static_assert(sizeof(WalkCallReq) == 12, "WalkCallReq must be 12 bytes");

class WalkCallMessage : public PacketHeader {
public:
	WalkCallMessage(const WalkCallReq& req) {
		m_MsgType = MT_WalkCall;
		m_len = sizeof(WalkCallReq);
		memcpy(m_Data, &req, sizeof(WalkCallReq));
	}
	char m_Data[sizeof(WalkCallReq)] = { 0 };
};

class WalkCallAckMessage : public PacketHeader {
public:
	WalkCallAckMessage(const WalkCallAck& ack) {
		m_MsgType = MT_WalkCallAck;
		m_len = sizeof(WalkCallAck);
		memcpy(m_Data, &ack, sizeof(WalkCallAck));
	}
	char m_Data[sizeof(WalkCallAck)] = { 0 };
};
