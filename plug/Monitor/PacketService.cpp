#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "PacketService.h"
#include "ViewService.h"
#include "../Common/GameBuild.h"
#include "../Common/PlayerVitals.h"
#include "../Common/RitualState.h"
#include "../Common/PortalReturn.h"
#include "../third_party/minhook/include/MinHook.h"
#include "Log.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace {
using IoFn = uint64_t (*)(uint64_t, void*, uint64_t);
using SendFn = uint64_t (*)(uint64_t, uint32_t, uint32_t);
IoFn originalRead = nullptr;
SendFn originalSend = nullptr;
uint64_t moduleBase = 0;
bool hooksInstalled = false, ownHookRuntime = false;
bool sendHookCreated = false, readHookCreated = false;
std::atomic<uint32_t> captureMask{0}, activeHooks{0};
std::atomic<uint64_t> watchedStream{0}, captureGeneration{0}, captured{0}, dropped{0}, nextSequence{0};
std::mutex controlMutex, queueMutex;
managed::Route captureOwner{};
std::deque<packet::CaptureEvent> events;
constexpr size_t kQueueLimit = 256;
recovery::Config recoveryConfig{};
recovery::Snapshot recoveryState{};
managed::Route recoveryOwner{};
uint64_t lastFlaskAttempt[recovery::kFlasks]{}, nextRecoveryTick = 0;
std::string recoveryDetail = "自动恢复未启用";

bool ReadBytes(uint64_t address, void* out, size_t size) {
    SIZE_T done = 0;
    return IsPlausibleAddress(address) && ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void*>(address), out, size, &done) && done == size;
}
template<class T> T Read(uint64_t address) {
    T value{};
    if (!ReadBytes(address, &value, sizeof(value))) throw std::runtime_error("游戏上下文不可读");
    return value;
}
uint64_t Pointer(uint64_t address) {
    auto value = Read<uint64_t>(address);
    if (!IsPlausibleAddress(value)) throw std::runtime_error("游戏上下文尚未就绪");
    return value;
}
struct Context { uint64_t state, protocol, stream, world; uint8_t sequence; };
Context Current() {
    Context c{};
    c.state = Pointer(moduleBase + BaseOffset);
    for (auto offset : kPacketContextChain) c.state = Pointer(c.state + offset);
    c.protocol = Pointer(c.state + kPacketProtocolOff);
    c.stream = Pointer(c.protocol + kPacketStreamOff);
    c.world = Pointer(c.protocol + kPacketWorldOff);
    c.sequence = Read<uint8_t>(c.state + kPacketSequenceOff);
    return c;
}
template<size_t N> bool Signature(uint64_t rva, const uint8_t (&expected)[N]) {
    uint8_t bytes[N]{};
    return ReadBytes(moduleBase + rva, bytes, N) && !std::memcmp(bytes, expected, N);
}
bool Validate(std::string& error) {
    moduleBase = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    if (!gamebuild::VerifyLocal(moduleBase, error)) return false;
    if (!Signature(kPacketBuildRva, kPacketBuildSignature) ||
        !Signature(kPacketWriteRva, kPacketIoSignature) ||
        !Signature(kPacketSerializeRva, kPacketSerializeSignature) ||
        !Signature(kPacketEndSerializeRva, kPacketEndSerializeSignature) ||
        (!hooksInstalled && (!Signature(kPacketSendRva, kPacketSendSignature) ||
                             !Signature(kPacketReadRva, kPacketIoSignature))) ||
        Read<uint64_t>(moduleBase + kPacketActionVtableRva + sizeof(uint64_t)) != moduleBase + kPacketSerializeRva ||
        Read<uint64_t>(moduleBase + kPacketEndVtableRva + sizeof(uint64_t)) != moduleBase + kPacketEndSerializeRva) {
        error = "封包入口或虚表指纹不匹配"; return false;
    }
    return true;
}
bool ValidateParty(std::string& error) {
    if (!Signature(kPacketPartySerializeRva, kPacketPartySerializeSignature) ||
        Read<uint64_t>(moduleBase + kPacketPartyVtableRva + sizeof(uint64_t)) != moduleBase + kPacketPartySerializeRva) {
        error = "队友传送封包的序列化函数或虚表不匹配"; return false;
    }
    return true;
}
bool ValidateFlask(std::string& error) {
    if (!Signature(kPacketFlaskSerializeRva, kPacketFlaskSerializeSignature) ||
        Read<uint64_t>(moduleBase + kPacketFlaskVtableRva + sizeof(uint64_t)) != moduleBase + kPacketFlaskSerializeRva) {
        error = "药剂封包序列化函数或虚表不匹配"; return false;
    }
    return true;
}
bool SampleVitals(recovery::Vitals& vitals) {
    const bool valid = recovery::ReadVitals(moduleBase, ReadBytes, vitals);
    recoveryState.valid = valid ? 1 : 0;
    for (size_t i = 0; i < recovery::kResources; ++i) {
        recoveryState.current[i] = valid ? vitals.current[i] : 0;
        recoveryState.maximum[i] = valid ? vitals.maximum[i] : 0;
    }
    return valid;
}
void StopRecovery(const char* detail) {
    recoveryConfig.enabled = recoveryState.enabled = 0;
    recoveryOwner = {}; recoveryDetail = detail;
}
void Capture(uint64_t generation, uint32_t direction, const void* data, uint64_t length, uint64_t offset = 0) noexcept {
    try {
    if (!(captureMask.load() & direction) || !length || !data) return;
    packet::CaptureEvent event{};
    event.direction = direction; event.tick = GetTickCount64(); event.threadId = GetCurrentThreadId();
    event.streamOffset = offset;
    event.originalLength = uint32_t((std::min)(length, uint64_t(UINT32_MAX)));
    event.length = uint32_t((std::min)(length, uint64_t(packet::kMaxBytes)));
    event.flags = length > packet::kMaxBytes ? packet::Truncated : 0;
    if (!ReadBytes(reinterpret_cast<uint64_t>(data), event.bytes, event.length)) { ++dropped; return; }
    std::unique_lock<std::mutex> lock(queueMutex, std::try_to_lock);
    if (!lock || events.size() >= kQueueLimit) { ++dropped; return; }
    if (generation != captureGeneration.load() || !(captureMask.load() & direction)) return;
    // ReadBytes is invoked for individual fields. Merge contiguous reads on the
    // same game stream/thread; these are stream fragments, never inferred packets.
    if (direction == packet::Receive && !events.empty()) {
        auto& last = events.back();
        if (last.direction == direction && last.threadId == event.threadId && !last.flags && !event.flags &&
            last.streamOffset + last.length == offset && last.length + event.length <= packet::kMaxBytes) {
            std::memcpy(last.bytes + last.length, event.bytes, event.length);
            last.length += event.length; last.originalLength += event.originalLength; return;
        }
    }
    event.sequence = ++nextSequence;
    events.push_back(event); ++captured;
    } catch (...) { ++dropped; } // Never unwind through a game callback.
}
struct HookScope { HookScope() { ++activeHooks; } ~HookScope() { --activeHooks; } };
uint64_t SendHook(uint64_t stream, uint32_t flags, uint32_t force) {
    HookScope scope;
    const auto generation = captureGeneration.load();
    if (stream == watchedStream.load() && (captureMask.load() & packet::Send)) {
        // WriteBytes calls Send(force=1) while already holding this non-recursive
        // SRW lock. For normal sends, only sample if the buffer is available.
        uint8_t enabled = 0;
        auto* lock = reinterpret_cast<PSRWLOCK>(stream + kPacketLockOff);
        const bool needsLock = !force && ReadBytes(stream + kPacketLockEnabledOff, &enabled, 1) && enabled;
        const bool acquired = needsLock && TryAcquireSRWLockExclusive(lock);
        if (needsLock && !acquired) { ++dropped; return originalSend(stream, flags, force); }
        uint64_t size = 0, consumed = 0, buffer = 0;
        if (ReadBytes(stream + kPacketSendSizeOff, &size, sizeof(size)) &&
            ReadBytes(stream + kPacketSendConsumedOff, &consumed, sizeof(consumed)) && size >= consumed &&
            ReadBytes(stream + kPacketSendBufferOff, &buffer, sizeof(buffer)))
            Capture(generation, packet::Send, reinterpret_cast<void*>(buffer + consumed), size - consumed, consumed);
        if (acquired) ReleaseSRWLockExclusive(lock);
    }
    return originalSend(stream, flags, force);
}
uint64_t ReadHook(uint64_t stream, void* output, uint64_t length) {
    HookScope scope;
    const auto generation = captureGeneration.load();
    uint64_t before = 0;
    const bool watch = stream == watchedStream.load() && (captureMask.load() & packet::Receive) &&
        ReadBytes(stream + kPacketRecvPositionOff, &before, sizeof(before));
    const auto result = originalRead(stream, output, length);
    uint64_t after = 0; uint8_t failed = 1;
    if (watch && ReadBytes(stream + kPacketRecvErrorOff, &failed, sizeof(failed)) && !failed &&
        ReadBytes(stream + kPacketRecvPositionOff, &after, sizeof(after)) && after >= before && after - before == length)
        Capture(generation, packet::Receive, output, length, before);
    return result;
}
bool InstallHooks(std::string& error) {
    if (hooksInstalled) return true;
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        error = "无法初始化收发记录钩子"; return false;
    }
    ownHookRuntime = ownHookRuntime || status == MH_OK;
    void* send = reinterpret_cast<void*>(moduleBase + kPacketSendRva);
    void* read = reinterpret_cast<void*>(moduleBase + kPacketReadRva);
    if (!sendHookCreated) {
        status = MH_CreateHook(send, reinterpret_cast<void*>(&SendHook), reinterpret_cast<void**>(&originalSend));
        if (status != MH_OK) { error = "发送入口钩子创建失败"; return false; }
        sendHookCreated = true;
    }
    if (!readHookCreated) {
        status = MH_CreateHook(read, reinterpret_cast<void*>(&ReadHook), reinterpret_cast<void**>(&originalRead));
        if (status != MH_OK) { error = "接收入口钩子创建失败"; return false; }
        readHookCreated = true;
    }
    if (MH_QueueEnableHook(send) != MH_OK || MH_QueueEnableHook(read) != MH_OK || MH_ApplyQueued() != MH_OK) {
        MH_DisableHook(send); MH_DisableHook(read);
        error = "启用收发记录钩子失败"; return false;
    }
    hooksInstalled = true; return true;
}
void StopCapture() {
    captureMask = 0; watchedStream = 0; ++captureGeneration;
    captureOwner = {};
    std::lock_guard<std::mutex> lock(queueMutex); events.clear();
}
uint64_t FindTarget(const Context& c, uint32_t key, const wchar_t* metadataType, bool exactMetadata = false) {
    auto node = Pointer(c.world + kSurroundingsChain[kSurroundingsChainDepth - 1]);
    node = Pointer(node + kNodeParentOff);
    std::vector<uint64_t> stack{node}; std::unordered_set<uint64_t> visited;
    while (!stack.empty() && visited.size() < 8192) {
        node = stack.back(); stack.pop_back();
        if (!IsPlausibleAddress(node) || !visited.insert(node).second || Read<uint8_t>(node + kNodeFlagOff)) continue;
        if (Read<uint32_t>(node + kNodeKeyOff) == key) {
            const auto entity = Pointer(node + kNodeEntityOff);
            const auto holder = Pointer(entity + kMetadataPathOff0);
            const auto text = holder + kMetadataPathOff1;
            const auto length = Read<uint64_t>(text + kStdStringLengthOff);
            const auto capacity = Read<uint64_t>(text + kStdStringMaxLenOff);
            if (!length || length > 1024 || length > capacity) return 0;
            const auto chars = capacity > kStdStringSsoLimit ? Pointer(text) : text;
            std::wstring metadata(size_t(length), L'\0');
            if (!ReadBytes(chars, metadata.data(), size_t(length) * sizeof(wchar_t)) ||
                (exactMetadata ? metadata != metadataType : metadata.find(metadataType) == std::wstring::npos)) return 0;
            return entity;
        }
        stack.push_back(Read<uint64_t>(node + kNodeLeftOff));
        stack.push_back(Read<uint64_t>(node + kNodeRightOff));
    }
    return 0;
}
struct alignas(16) ActionObject {
    uint64_t vtable = 0;
    uint16_t opcode = packet::kActionOpcode; uint8_t pad0[6]{};
    uint8_t sequence = 0, pad1[7]{};
    uint32_t action = 0, target = 0;
    int32_t x = 0, y = 0;
    uint32_t flags = 0, pad2 = 0;
};
static_assert(sizeof(ActionObject) == 48 && offsetof(ActionObject, target) == 0x1C, "pickup object layout");
struct EndObject {
    uint64_t vtable = 0; uint16_t opcode = packet::kEndOpcode; uint8_t pad0[6]{};
    uint8_t sequence = 0, pad1[7]{};
};
struct FlaskObject {
    uint64_t vtable = 0;
    uint16_t opcode = recovery::kFlaskOpcode; uint8_t pad0[6]{};
    uint8_t sequence = 0, pad1[7]{};
    uint32_t flask = 0, pad2 = 0;
};
static_assert(sizeof(FlaskObject) == 0x20 && offsetof(FlaskObject, flask) == 0x18, "flask ASM object layout");
// Explicit game string layout: do not hand the game an STL object owned by our CRT.
struct PartyObject {
    uint64_t vtable = 0;
    uint16_t opcode = packet::kPartyTeleportOpcode; uint8_t pad0[6]{};
    uint8_t sequence = 0, pad1[7]{};
    union NameStorage {
        wchar_t small[8];
        const wchar_t* pointer;
        NameStorage() : small{} {}
    } name;
    uint64_t length = 0, capacity = 7;
    explicit PartyObject(const std::wstring& text) : length(text.size()) {
        if (text.size() <= 7) std::memcpy(name.small, text.data(), text.size() * sizeof(wchar_t));
        else { name.pointer = text.c_str(); capacity = text.size(); }
    }
};
static_assert(sizeof(wchar_t) == 2 && sizeof(PartyObject) == 0x38 &&
    offsetof(PartyObject, name) == 0x18 && offsetof(PartyObject, length) == 0x28 &&
    offsetof(PartyObject, capacity) == 0x30, "party teleport object layout");

std::wstring RosterText(const uint8_t* record, size_t offset) {
    uint64_t length = 0, capacity = 0, pointer = 0;
    std::memcpy(&length, record + offset + kStdStringLengthOff, sizeof(length));
    std::memcpy(&capacity, record + offset + kStdStringMaxLenOff, sizeof(capacity));
    if (length > 256 || length > capacity || capacity > 1024 * 1024)
        throw std::runtime_error("队友名称数据尚未稳定，请重新获取列表");
    std::wstring text(size_t(length), L'\0');
    if (capacity > kStdStringSsoLimit) {
        std::memcpy(&pointer, record + offset, sizeof(pointer));
        if (length && !ReadBytes(pointer, text.data(), size_t(length) * sizeof(wchar_t)))
            throw std::runtime_error("队友名称数据不可读，请重新获取列表");
    } else if (length) std::memcpy(text.data(), record + offset, size_t(length) * sizeof(wchar_t));
    for (const auto ch : text) if (ch < 0x20) throw std::runtime_error("队友名称包含无效字符");
    return text;
}
uint64_t FindPartyMember(const Context& c, const std::wstring& name, const std::wstring& account) {
    using Bounds = std::array<uint64_t, 3>;
    const auto h = Read<Bounds>(c.protocol + kPartyRosterBeginOff);
    if (h == Bounds{}) return 0;
    if (!IsPlausibleAddress(h[0]) || h[0] > h[1] || h[1] > h[2] || !IsPlausibleAddress(h[2]) ||
        (h[1] - h[0]) % kPartyRosterStride || (h[2] - h[0]) % kPartyRosterStride ||
        (h[2] - h[0]) / kPartyRosterStride > 64)
        throw std::runtime_error("队伍名单容器校验失败，请重新获取列表");
    std::vector<uint8_t> records(size_t(h[1] - h[0]));
    if (!records.empty() && !ReadBytes(h[0], records.data(), records.size()))
        throw std::runtime_error("队伍名单暂不可读，请重新获取列表");
    uint64_t found = 0;
    for (size_t i = 0; i < records.size(); i += kPartyRosterStride) {
        if (RosterText(records.data() + i, kPartyRosterNameOff) == name &&
            RosterText(records.data() + i, kPartyRosterAccountOff) == account) {
            if (found) throw std::runtime_error("队友身份存在歧义，未发送传送请求");
            found = h[0] + i;
        }
    }
    if (Read<Bounds>(c.protocol + kPartyRosterBeginOff) != h)
        throw std::runtime_error("队伍名单正在变化，请重新获取列表");
    return found;
}
bool InvokeObject(const Context& c, void* object, DWORD& exception, uint32_t& invoked) {
    __try {
        reinterpret_cast<void (*)(uint64_t, void*)>(moduleBase + kPacketBuildRva)(c.protocol, object);
        ++invoked;
        reinterpret_cast<SendFn>(moduleBase + kPacketSendRva)(c.stream, 0, 0);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { exception = GetExceptionCode(); return false; }
}
bool InvokeSummon(const Context& c, DWORD& exception, uint32_t& invoked) {
    __try {
        ++invoked; // An exception can follow a partial native send; never retry.
        reinterpret_cast<void (*)(uint64_t)>(moduleBase + kPortalSummonRva)(c.state);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { exception = GetExceptionCode(); return false; }
}
bool InvokePlain(const Context& c, const void* bytes, uint32_t length, DWORD& exception, uint32_t& invoked, bool& busy) {
    __try {
        // BuildObject locks while serializing. Raw WriteBytes does not, so use
        // the same SRW lock for the complete write, then let Send lock itself.
        if (!*reinterpret_cast<uint8_t*>(c.stream + kPacketLockEnabledOff)) { busy = true; return false; }
        auto* lock = reinterpret_cast<PSRWLOCK>(c.stream + kPacketLockOff);
        if (!TryAcquireSRWLockExclusive(lock)) { busy = true; return false; }
        __try {
            reinterpret_cast<IoFn>(moduleBase + kPacketWriteRva)(c.stream, const_cast<void*>(bytes), length);
            ++invoked;
        } __finally { ReleaseSRWLockExclusive(lock); }
        reinterpret_cast<SendFn>(moduleBase + kPacketSendRva)(c.stream, 0, 0);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { exception = GetExceptionCode(); return false; }
}
void Detail(packet::Reply& reply, const char* detail) { std::snprintf(reply.detail, sizeof(reply.detail), "%s", detail); }
packet::Reply HandleRecovery(const packet::Request& request, const managed::Route& route) {
    packet::Reply reply{};
    const bool configure = request.operation == packet::Operation::ConfigureRecovery;
    recovery::Config config{};
    if (request.options || request.captureMask || request.itemKey || request.actionId || request.x || request.y ||
        request.expectedEntity || request.length != (configure ? sizeof(config) : 0)) {
        Detail(reply, "自动恢复请求参数无效"); return reply;
    }
    if (configure) {
        std::memcpy(&config, request.bytes, sizeof(config));
        if (!recovery::Valid(config)) { Detail(reply, "恢复阈值需为 0–100，药瓶或间隔参数无效"); return reply; }
        // Stopping remains available even while the character is loading or the
        // game's build/entry checks no longer pass.
        if (!config.enabled) {
            StopRecovery("自动恢复已停止"); recoveryConfig = config;
            reply.result = packet::Result::Ok;
            reply.objectLength = sizeof(recoveryState); std::memcpy(reply.object, &recoveryState, sizeof(recoveryState));
            Detail(reply, recoveryDetail.c_str()); return reply;
        }
    }
    try {
        std::string error;
        if (!Validate(error) || !ValidateFlask(error)) {
            StopRecovery(error.c_str()); reply.result = packet::Result::UnsupportedBuild;
        } else {
            recovery::Vitals vitals;
            const bool valid = SampleVitals(vitals);
            if (configure && !valid) {
                reply.result = packet::Result::ContextUnavailable;
                error = "角色生命数据暂不可读，请进入场景后再启用";
            } else {
                if (configure) {
                    recoveryConfig = config; recoveryOwner = route; nextRecoveryTick = 0;
                    recoveryState.enabled = 1;
                    recoveryState.lastResult = uint32_t(packet::Result::Ok);
                    recoveryDetail = "自动恢复已启用";
                    LOGI("[Recovery] enabled thresholds=%u/%u/%u flasks=%u/%u/%u intervals=%u/%u/%u",
                        config.threshold[0], config.threshold[1], config.threshold[2],
                        config.flask[0], config.flask[1], config.flask[2],
                        config.intervalMs[0], config.intervalMs[1], config.intervalMs[2]);
                }
                reply.result = packet::Result::Ok;
            }
        }
        Detail(reply, error.empty() ? recoveryDetail.c_str() : error.c_str());
    } catch (const std::exception& error) {
        recoveryState.valid = 0; reply.result = packet::Result::ContextUnavailable; Detail(reply, error.what());
    }
    reply.objectLength = sizeof(recoveryState); std::memcpy(reply.object, &recoveryState, sizeof(recoveryState));
    return reply;
}
} // namespace

namespace packetservice {
packet::Reply Handle(const packet::Request& request, const managed::Route& route) {
    std::lock_guard<std::mutex> lock(controlMutex);
    packet::Reply reply{};
    reply.captureMask = captureMask.load(); reply.captured = captured.load(); reply.dropped = dropped.load();
    if (request.version != packet::kVersion || request.operation < packet::Operation::Status ||
        request.operation > packet::Operation::RevealMinimap || request.length > packet::kMaxBytes ||
        (request.captureMask & ~(packet::Send | packet::Receive)) || (request.options & ~(packet::AppendEnd | packet::PreviewOnly))) {
        Detail(reply, "封包请求参数无效"); return reply;
    }
    if (request.operation == packet::Operation::ConfigureRecovery || request.operation == packet::Operation::RecoveryStatus)
        return HandleRecovery(request, route);
    if (request.operation == packet::Operation::ConfigureView || request.operation == packet::Operation::ViewStatus ||
        request.operation == packet::Operation::RevealMinimap) return viewservice::Handle(request, route);
    const bool portal = request.operation == packet::Operation::EnterPortal;
    const bool party = request.operation == packet::Operation::TeleportToPartyMember;
    const bool ritual = request.operation == packet::Operation::InteractRitual;
    const bool summon = request.operation == packet::Operation::SummonPortal;
    packet::PortalScene expectedScene{};
    const bool boundPortal = summon || (portal && request.length == sizeof(expectedScene));
    if ((summon && !portalreturn::ValidSummonRequest(request)) ||
        (portal && request.length && request.length != sizeof(expectedScene))) {
        Detail(reply, "传送门请求的地图身份或参数无效"); return reply;
    }
    if (boundPortal) {
        std::memcpy(&expectedScene, request.bytes, sizeof(expectedScene));
        if (!portalreturn::ValidScene(expectedScene)) { Detail(reply, "传送门地图身份无效"); return reply; }
    }
    packet::PartyTeleportTarget partyTarget{};
    if (party) {
        if (request.length != sizeof(partyTarget) || (request.options & packet::AppendEnd) ||
            request.itemKey || request.expectedEntity || request.captureMask) {
            Detail(reply, "队友传送参数无效；不支持实体 key 或追加结束动作"); return reply;
        }
        std::memcpy(&partyTarget, request.bytes, sizeof(partyTarget));
        if (!packet::ValidTarget(partyTarget)) { Detail(reply, "队友角色名或账号不是有效的 UTF-16 名称"); return reply; }
    }
    if (portal && (!request.itemKey || !request.expectedEntity || (request.options & packet::AppendEnd))) {
        Detail(reply, "请先刷新并选择传送门；进门不支持追加结束动作"); return reply;
    }
    if (ritual && (!request.itemKey || !request.expectedEntity || request.length || request.captureMask ||
                   request.actionId || request.x || request.y)) {
        Detail(reply, "祭祀交互参数无效；请重新扫描当前祭祀交互对象"); return reply;
    }
    if (request.operation == packet::Operation::Capture && !request.captureMask) {
        StopCapture(); reply.captureMask = 0; reply.result = packet::Result::Ok;
        Detail(reply, "已停止收发记录"); return reply;
    }
    try {
        std::string error;
        if (!Validate(error)) { reply.result = packet::Result::UnsupportedBuild; Detail(reply, error.c_str()); return reply; }
        if (party && !ValidateParty(error)) { reply.result = packet::Result::UnsupportedBuild; Detail(reply, error.c_str()); return reply; }
        if (summon && !Signature(kPortalSummonRva, kPortalSummonSignature)) {
            reply.result = packet::Result::UnsupportedBuild; Detail(reply, "原生召唤传送门完整函数指纹不匹配"); return reply;
        }
        const auto context = Current();
        const std::wstring partyName(partyTarget.name, partyTarget.name + partyTarget.nameLength);
        const std::wstring partyAccount(partyTarget.account, partyTarget.account + partyTarget.accountLength);
        const auto partyRecord = party ? FindPartyMember(context, partyName, partyAccount) : 0;
        if (party && !partyRecord) {
            reply.result = packet::Result::TargetMissing; Detail(reply, "该角色已不在当前队伍中，请重新获取队友列表"); return reply;
        }
        if (request.operation == packet::Operation::Status) {
            reply.result = packet::Result::Ok; Detail(reply, "封包入口与当前游戏上下文校验通过"); return reply;
        }
        if (request.operation == packet::Operation::Capture) {
            if (!InstallHooks(error)) { reply.result = packet::Result::HookFailed; Detail(reply, error.c_str()); return reply; }
            StopCapture(); captureOwner = route; watchedStream = context.stream; captureMask = request.captureMask;
            reply.captureMask = request.captureMask; reply.result = packet::Result::Ok;
            Detail(reply, "收发记录已开启；接收数据为解码后的明文流片段"); return reply;
        }
        if (request.operation == packet::Operation::Pick || portal || ritual) {
            const auto entity = request.itemKey ? FindTarget(context, request.itemKey,
                ritual ? L"Metadata/Terrain/Leagues/Ritual/RitualRuneInteractable" :
                portal ? L"MultiplexPortal" : L"WorldItem", ritual) : 0;
            if (!entity) {
                reply.result = packet::Result::TargetMissing;
                Detail(reply, ritual ? "周围树中未找到祭祀交互对象，请重新扫描" :
                    portal ? "周围树中未找到该传送门，请重新刷新" : "周围树中未找到该掉落物实体 key"); return reply;
            }
            if ((portal || ritual || request.expectedEntity) && entity != request.expectedEntity) {
                reply.result = packet::Result::ContextChanged;
                Detail(reply, ritual ? "祭祀交互对象已变化，请重新扫描" :
                    portal ? "传送门实体已变化，请重新刷新列表" : "掉落物实体已变化，请重新刷新列表"); return reply;
            }
        }
        if (request.operation == packet::Operation::SendPlain && !request.length) {
            Detail(reply, "请输入需要发送的十六进制明文"); return reply;
        }
        if (party && FindPartyMember(context, partyName, partyAccount) != partyRecord) {
            reply.result = packet::Result::ContextChanged; Detail(reply, "队伍已变化，未发送传送请求"); return reply;
        }
        const auto checked = Current();
        if (checked.state != context.state || checked.protocol != context.protocol || checked.stream != context.stream || checked.world != context.world) {
            reply.result = packet::Result::ContextChanged; Detail(reply, "游戏场景正在切换，未执行发送"); return reply;
        }
        if (party && Pointer(checked.state + kPacketStateStreamOff) != checked.stream) {
            reply.result = packet::Result::ContextChanged; Detail(reply, "队伍或发送上下文已变化，未发送传送请求"); return reply;
        }
        if (ritual) {
            ritualstate::Snapshot state;
            if (FindTarget(checked, request.itemKey, L"Metadata/Terrain/Leagues/Ritual/RitualRuneInteractable", true) != request.expectedEntity ||
                !ritualstate::ReadEntity(request.expectedEntity, ReadBytes, state) || !state.CanStart()) {
                reply.result = packet::Result::ContextChanged;
                Detail(reply, "祭祀不处于可开始状态，或状态读取失败；未发送交互"); return reply;
            }
        }
        if (boundPortal) {
            packet::PortalScene scene;
            recovery::Vitals vitals;
            if (!portalreturn::ReadScene(moduleBase, ReadBytes, scene) || !(scene == expectedScene) ||
                checked.world != scene.world || !recovery::ReadVitals(moduleBase, ReadBytes, vitals) ||
                vitals.entity != scene.player || vitals.world != scene.world || !vitals.current[0]) {
                reply.result = packet::Result::ContextChanged;
                Detail(reply, "传送门操作前地图、角色或生命状态变化，未调用"); return reply;
            }
        }
        if (portal && FindTarget(checked, request.itemKey, L"Metadata/MiscellaneousObjects/MultiplexPortal", true) != request.expectedEntity) {
            reply.result = packet::Result::ContextChanged; Detail(reply, "进门前目标实体已变化，未调用"); return reply;
        }
        if (summon) {
            const auto actor = Pointer(checked.state + kSkillStopObj188);
            if (Pointer(actor + skilllayout::kComponentOwnerEntity) != expectedScene.player ||
                Read<uint8_t>(checked.state + kPortalSummonGateOff) || Read<uint32_t>(actor + kIdSkillCurrentIdOff)) {
                reply.result = packet::Result::Busy; Detail(reply, "原生召唤状态未就绪或角色仍在执行动作，未调用"); return reply;
            }
        }
        if (boundPortal) {
            const auto final = Current(); packet::PortalScene scene;
            if (final.state != checked.state || final.protocol != checked.protocol || final.stream != checked.stream ||
                final.world != checked.world || !portalreturn::ReadScene(moduleBase, ReadBytes, scene) || !(scene == expectedScene)) {
                reply.result = packet::Result::ContextChanged;
                Detail(reply, "传送门调用前场景再次变化，未调用"); return reply;
            }
        }
        DWORD exception = 0; bool ok = false, busy = false;
        const bool preview = (request.options & packet::PreviewOnly) != 0;
        if (summon) {
            ok = preview || InvokeSummon(checked, exception, reply.invoked);
        } else if (party) {
            PartyObject object(partyName); object.vtable = moduleBase + kPacketPartyVtableRva; object.sequence = checked.sequence;
            // Reply.object is the existing 48-byte prefix; the full object is 56 bytes.
            reply.objectLength = sizeof(reply.object); std::memcpy(reply.object, &object, sizeof(reply.object));
            ok = preview || InvokeObject(checked, &object, exception, reply.invoked);
        } else if (request.operation == packet::Operation::SendPlain) {
            ok = preview || InvokePlain(context, request.bytes, request.length, exception, reply.invoked, busy);
        } else if (request.operation == packet::Operation::EndAction) {
            EndObject object{}; object.vtable = moduleBase + kPacketEndVtableRva; object.sequence = checked.sequence;
            reply.objectLength = sizeof(object); std::memcpy(reply.object, &object, sizeof(object));
            ok = preview || InvokeObject(context, &object, exception, reply.invoked);
        } else {
            ActionObject object{}; object.vtable = moduleBase + kPacketActionVtableRva; object.sequence = checked.sequence;
            const bool targeted = request.operation == packet::Operation::Pick || portal || ritual;
            object.action = targeted ? packet::kInteractAction : request.actionId;
            object.target = targeted ? request.itemKey : 0;
            object.x = portal ? 0 : request.x; object.y = portal ? 0 : request.y;
            object.flags = portal || ritual ? packet::kInteractFlags : request.actionFlags;
            reply.objectLength = sizeof(object); std::memcpy(reply.object, &object, sizeof(object));
            ok = preview || InvokeObject(context, &object, exception, reply.invoked);
        }
        if (ok && !preview && request.operation != packet::Operation::EndAction && (request.options & packet::AppendEnd)) {
            const auto current = Current();
            if (current.protocol != context.protocol || current.stream != context.stream || current.world != context.world) {
                reply.result = packet::Result::ContextChanged;
                Detail(reply, "首个动作已调用；场景发生变化，未追加结束动作"); return reply;
            }
            EndObject end{}; end.vtable = moduleBase + kPacketEndVtableRva;
            end.sequence = current.sequence;
            ok = InvokeObject(context, &end, exception, reply.invoked);
        }
        reply.exceptionCode = exception; reply.result = ok ? packet::Result::Ok : busy ? packet::Result::Busy : packet::Result::Exception;
        Detail(reply, preview ? (summon ? "原生召唤入口与现场校验通过，尚未调用" : "组包预览完成，尚未调用游戏组包或发送函数") :
               ok ? (summon ? "原生召唤函数已返回；是否成功以附近新门出现为准" :
                     party ? "队友传送请求已发送；是否成功以游戏切图为准" :
                     ritual ? "祭祀交互请求已发送；是否激活以游戏状态为准" :
                     portal ? "传送门交互请求已发送；是否进门以游戏切图为准" : "游戏组包/发送函数已返回；实际效果以游戏为准") :
               busy ? "游戏发送缓冲正在使用或未启用同步，未发送明文，请稍后再试" : "游戏函数发生异常，未自动重试");
        LOGI("[Packet] operation=%u key=%u invoked=%u exception=%08X", unsigned(request.operation), request.itemKey,
             reply.invoked, exception);
    } catch (const std::exception& error) {
        reply.result = packet::Result::ContextUnavailable; Detail(reply, error.what());
    }
    return reply;
}
void RecoveryTick(const std::function<bool(const managed::Route&)>& current, bool actionBusy) {
    std::lock_guard<std::mutex> lock(controlMutex);
    if (!recoveryConfig.enabled) return;
    if (!current(recoveryOwner)) { StopRecovery("通信会话已断开，自动恢复已停止"); return; }
    const auto now = GetTickCount64();
    if (now < nextRecoveryTick) return;
    nextRecoveryTick = now + 100;
    try {
        recovery::Vitals vitals;
        if (!SampleVitals(vitals)) { recoveryDetail = "角色数据暂不可读，等待场景就绪"; return; }
        if (!vitals.current[0]) { recoveryDetail = "角色已死亡，暂停使用药剂"; return; }
        if (actionBusy) { recoveryDetail = "等待当前技能调用返回"; return; }
        const auto due = recovery::Due(recoveryConfig, recoveryState, lastFlaskAttempt, now);
        recoveryDetail = "监测中；0% 规则关闭，同瓶按间隔使用";
        if (!due) return;
        std::string error;
        if (!Validate(error) || !ValidateFlask(error)) {
            recoveryState.lastResult = uint32_t(packet::Result::UnsupportedBuild);
            StopRecovery(error.c_str()); return;
        }
        for (uint32_t flask = 0; flask < recovery::kFlasks; ++flask) {
            if (!(due & (uint32_t(1) << flask))) continue;
            const auto context = Current();
            recovery::Vitals checked;
            if (!SampleVitals(checked) || checked.entity != vitals.entity || checked.world != vitals.world ||
                checked.world != context.world || Pointer(context.state + kPacketStateStreamOff) != context.stream ||
                !current(recoveryOwner) ||
                !(recovery::Due(recoveryConfig, recoveryState, lastFlaskAttempt, GetTickCount64()) & (uint32_t(1) << flask))) {
                recoveryDetail = "角色或场景变化，等待重新采样"; return;
            }
            const auto stable = Current();
            if (stable.state != context.state || stable.world != context.world || stable.protocol != context.protocol || stable.stream != context.stream) {
                recoveryDetail = "发送上下文变化，等待重新采样"; return;
            }
            FlaskObject object{}; object.vtable = moduleBase + kPacketFlaskVtableRva;
            object.sequence = stable.sequence; object.flask = flask;
            DWORD exception = 0; uint32_t invoked = 0;
            // Record before calling: ambiguous failures must never immediately retry.
            lastFlaskAttempt[flask] = GetTickCount64(); recoveryState.lastFlask = flask;
            const bool ok = InvokeObject(stable, &object, exception, invoked);
            recoveryState.lastResult = uint32_t(ok ? packet::Result::Ok : packet::Result::Exception);
            LOGI("[Recovery] flask=%u life=%u/%u mana=%u/%u shield=%u/%u invoked=%u exception=%08X",
                flask, checked.current[0], checked.maximum[0], checked.current[1], checked.maximum[1],
                checked.current[2], checked.maximum[2], invoked, exception);
            if (!ok) { StopRecovery("药剂调用发生异常，自动恢复已停止，请检查日志"); return; }
            ++recoveryState.sent[flask];
            recoveryDetail = flask ? "已发送蓝瓶请求，等待游戏恢复" : "已发送血瓶请求，等待游戏恢复";
        }
    } catch (const std::exception& error) {
        recoveryState.valid = 0; recoveryDetail = std::string("等待角色上下文：") + error.what();
    }
}
void Tick(const std::function<bool(const managed::Route&)>& current,
          const std::function<void(const packet::CaptureEvent&)>& publish) {
    std::deque<packet::CaptureEvent> ready;
    {
        std::lock_guard<std::mutex> lock(controlMutex);
        if (!captureMask.load()) return;
        if (!current(captureOwner)) { StopCapture(); return; }
        uint64_t stream = 0;
        try { stream = Current().stream; } catch (...) {}
        if (stream != watchedStream.load()) {
            watchedStream = 0; ++captureGeneration;
            std::lock_guard<std::mutex> queue(queueMutex); events.clear(); watchedStream = stream;
        }
        std::lock_guard<std::mutex> queue(queueMutex);
        // Bound the work in the Monitor navigation tick even with busy traffic.
        for (size_t i = 0; i < 32 && !events.empty(); ++i) { ready.push_back(events.front()); events.pop_front(); }
    }
    for (const auto& event : ready) publish(event);
}
bool Shutdown() {
    if (!viewservice::Shutdown()) return false;
    std::lock_guard<std::mutex> lock(controlMutex); StopCapture(); StopRecovery("Monitor 已停止");
    if (sendHookCreated || readHookCreated) {
        const auto disable = [](uint64_t rva, bool created) {
            if (!created) return true;
            const auto status = MH_DisableHook(reinterpret_cast<void*>(moduleBase + rva));
            return status == MH_OK || status == MH_ERROR_DISABLED || status == MH_ERROR_NOT_CREATED;
        };
        if (!disable(kPacketSendRva, sendHookCreated) || !disable(kPacketReadRva, readHookCreated)) return false;
        const auto deadline = GetTickCount64() + 10000;
        while (activeHooks.load() && GetTickCount64() < deadline) Sleep(5);
        if (activeHooks.load()) return false;
        const auto remove = [](uint64_t rva, bool& created) {
            if (!created) return true;
            const auto status = MH_RemoveHook(reinterpret_cast<void*>(moduleBase + rva));
            if (status != MH_OK && status != MH_ERROR_NOT_CREATED) return false;
            created = false; return true;
        };
        if (!remove(kPacketSendRva, sendHookCreated) || !remove(kPacketReadRva, readHookCreated)) return false;
        hooksInstalled = false;
    }
    if (ownHookRuntime) { if (MH_Uninitialize() != MH_OK) return false; ownHookRuntime = false; }
    return true;
}
} // namespace packetservice
