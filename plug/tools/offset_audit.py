"""Read-only live offset audit. Requires 64-bit Python, pefile, capstone.

Never injects, calls game functions, or writes process memory.
Use --discover for a bounded search around the checked-in global RVAs.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import datetime
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

import pefile


def hx(value):
    return hex(value) if value is not None else None


class Process:
    def __init__(self, pid):
        self.pid = pid
        self.k = C.WinDLL("kernel32", use_last_error=True)
        self.k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
        self.k.OpenProcess.restype = W.HANDLE
        self.k.CloseHandle.argtypes = [W.HANDLE]
        self.k.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p,
                                            C.c_size_t, C.POINTER(C.c_size_t)]
        self.k.ReadProcessMemory.restype = W.BOOL
        self.handle = self.k.OpenProcess(0x410, False, pid)
        if not self.handle:
            raise C.WinError(C.get_last_error())
        self.k.K32EnumProcessModules.argtypes = [W.HANDLE, C.c_void_p, W.DWORD,
                                                C.POINTER(W.DWORD)]
        modules = (C.c_void_p * 2048)()
        needed = W.DWORD()
        if not self.k.K32EnumProcessModules(self.handle, modules, C.sizeof(modules),
                                            C.byref(needed)):
            self.close()
            raise C.WinError(C.get_last_error())
        self.base = modules[0]
        self.k.K32GetModuleFileNameExW.argtypes = [W.HANDLE, C.c_void_p, W.LPWSTR, W.DWORD]
        path = C.create_unicode_buffer(32768)
        if not self.k.K32GetModuleFileNameExW(self.handle, self.base, path, len(path)):
            self.close()
            raise C.WinError(C.get_last_error())
        self.path = Path(path.value)

    def close(self):
        if self.handle:
            self.k.CloseHandle(self.handle)
            self.handle = None

    def read(self, address, size):
        if not address or not 0x10000 <= address < 0x800000000000 or size > 0x1000000:
            return None
        buf, done = C.create_string_buffer(size), C.c_size_t()
        ok = self.k.ReadProcessMemory(self.handle, address, buf, size, C.byref(done))
        return buf.raw if ok and done.value == size else None

    def unpack(self, address, fmt):
        raw = self.read(address, struct.calcsize(fmt))
        return struct.unpack(fmt, raw)[0] if raw else None

    def q(self, address):
        return self.unpack(address, "<Q")

    def chain(self, offsets, start=None):
        address = self.base if start is None else start
        trace = []
        for offset in offsets:
            value = self.q(address + offset) if address else None
            trace.append({"slot": hx(address + offset) if address else None,
                          "offset": hx(offset), "value": hx(value)})
            if not value or not 0x10000 <= value < 0x800000000000:
                return None, trace
            address = value
        return address, trace

    def string(self, address, wide=True, std=True):
        if std:
            length = self.q(address + 0x10)
            capacity = self.q(address + 0x18)
            if length is None or capacity is None or not 0 < length <= 512 or capacity < length:
                return None
            if capacity > (7 if wide else 15):
                address = self.q(address)
            raw = self.read(address, length * (2 if wide else 1))
        else:
            raw = self.read(address, 96)
            if raw:
                raw = raw.split(b"\0", 1)[0]
        return raw.decode("utf-16-le" if wide else "utf-8", errors="replace") if raw else None


def load_constants():
    text = (Path(__file__).parents[1] / "Common" / "GameOffsets.h").read_text(encoding="utf-8-sig")
    constants = {name: int(value, 0) for name, value in re.findall(
        r"(?:#define\s+|constexpr\s+(?:uint\d+_t|int)\s+)(\w+)\s*(?:=\s*)?(0x[\da-fA-F]+|\d+)", text)}
    # Shared headers also use comma-separated declarations for related RVAs.
    for declaration in re.findall(r"constexpr\s+(?:uint\d+_t|int)\s+([^;]+);", text):
        for name, value in re.findall(r"\b(\w+)\s*=\s*(0x[\da-fA-F]+|\d+)[uUlL]*\s*(?=,|$)", declaration):
            constants[name] = int(value, 0)
    return constants


def signatures(pe):
    patterns = {"cost": "44 8B 1A 4C 8B C9 45 85 DB 78 71",
                "skill": "48 89 5C 24 10 4C 89 4C 24 20 66 44 89 44 24 18 55 56 57 41 54",
                "stop": "48 89 5C 24 08 57 48 83 EC 40 48 8B 99 A8 01 00 00 48 8D 54 24 20 B8 AC 01 00 00 48 8B F9"}
    result = {}
    for name, pattern in patterns.items():
        hits = []
        needle = bytes.fromhex(pattern)
        for section in pe.sections:
            if not section.Characteristics & 0x20000000:
                continue
            data, position = section.get_data(), 0
            while True:
                position = data.find(needle, position)
                if position < 0:
                    break
                hits.append(hex(section.VirtualAddress + position))
                position += 1
        result[name] = {"pattern": pattern, "hits": hits}
    return result


def components(p, entity, c):
    header, _ = p.chain([c["kEntityPairsOff0"], c["kEntityPairsOff1"]], entity)
    if not header:
        return {}
    begin = p.q(header + c["kEntityPairsOff2"])
    end = p.q(header + c["kEntityPairsEndOff"])
    array = p.q(entity + c["kEntityComponentsOff"])
    array_end = p.q(entity + c["kEntityComponentsEndOff"])
    if not begin or not end or not array or not array_end:
        return {}
    if not 0 < end - begin <= 0x4000 or (end - begin) % 16:
        return {}
    count = (end - begin) // c["kComponentPairSize"]
    if array_end - array != count * c["kComponentPtrSize"]:
        return {}
    result = {}
    for i in range(count):
        pair = begin + i * c["kComponentPairSize"]
        name = p.string(p.q(pair), wide=False, std=False)
        index = p.unpack(pair + c["kComponentPairIndexOff"], "<I")
        if not name or index is None or index >= count:
            return {}
        pointer = p.q(array + index * c["kComponentPtrSize"])
        if not pointer:
            return {}
        result[name] = pointer
    return result


def tree(p, head, double_entity, c):
    if not head or p.unpack(head + c["kNodeFlagOff"], "<B") != 1:
        return {"valid": False, "sentinel": hx(head)}, []
    pending, seen, entities = [p.q(head + c["kNodeParentOff"])], set(), []
    while pending and len(seen) < 8192:
        node = pending.pop()
        if node == head:
            continue
        if not node or node in seen:
            return {"valid": False, "error": "invalid/repeated tree node"}, entities
        flag = p.unpack(node + c["kNodeFlagOff"], "<B")
        if flag != 0:
            return {"valid": False, "error": "unreadable/non-node flag"}, entities
        seen.add(node)
        entity = p.q(node + c["kNodeEntityOff"])
        if double_entity:
            entity = p.q(entity) if entity else None
        entities.append(entity)
        pending.extend([p.q(node + c["kNodeLeftOff"]), p.q(node + c["kNodeRightOff"])])
    return {"valid": not pending, "sentinel": hx(head), "count": len(entities)}, entities


def verify(p, c, pe):
    source = (Path(__file__).parents[1] / "Common" / "GameOffsets.h").read_text(encoding="utf-8-sig")
    checks = {}
    nt_offset = p.unpack(p.base + 0x3C, "<I")
    live_timestamp = p.unpack(p.base + nt_offset + 8, "<I") if nt_offset else None
    live_size = p.unpack(p.base + nt_offset + 0x50, "<I") if nt_offset else None
    checks["build"] = {"pass": live_timestamp == c["kGameTimeDateStamp"] and live_size == c["kGameImageSize"],
                       "liveTimestamp": hx(live_timestamp), "liveSize": hx(live_size)}
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    disassembler = Cs(CS_ARCH_X86, CS_MODE_64)
    for name, rva_name, signature_name in [
        ("cost", "CostCall_Offset", "kCostCallSignature"),
        ("walk", "kActionWalkEntryRva", "kActionWalkSignature"),
        ("skill", "kSkillCallRva", "kSkillCallSignature"),
        ("stop", "kSkillStopRva", "kSkillStopSignature")]:
        match = re.search(r"\b" + signature_name + r"\[\]\s*=\s*\{([^}]+)\}", source)
        needle = bytes(int(x, 16) for x in re.findall(r"0x[\da-fA-F]+", match.group(1)))
        rva = c[rva_name]
        live = p.read(p.base + rva, 0x90)
        previous = p.read(p.base + rva - 1, 1)
        checks[name] = {"pass": bool(live and live.startswith(needle) and live == pe.get_data(rva, len(live)) and previous == b"\xCC"),
                        "rva": hx(rva), "expectedPrefix": needle.hex(" "),
                        "livePrefix": live[:len(needle)].hex(" ") if live else None,
                        "precedingByte": previous.hex() if previous else None,
                        "instructions": [f"{i.address:#x}: {i.mnemonic} {i.op_str}"
                                         for i in disassembler.disasm(live[:0x80], rva)] if live else []}
    root = c["BaseOffset"]
    state, state_trace = p.chain([root, c["kSkillStopChainOff1"], c["kSkillStopChainOff2"], 0])
    world, world_trace = p.chain([0x1A8, 0x20F0], state) if state else (None, [])
    player, player_trace = p.chain([0x5C0, 0], world) if world else (None, [])
    parent = p.q(player + 0x78) if player else None
    map_address = parent + 0x8F8 if parent else None
    width = p.unpack(map_address + c["MAP_OFF_GRID_W"], "<I") if map_address else None
    height = p.unpack(map_address + c["MAP_OFF_GRID_H"], "<I") if map_address else None
    map_ok = bool(width and height and width <= 0x1000 and height <= 0x1000 and parent == world)
    layers = []
    if map_ok:
        expected = ((width * c["CELL_SIZE"] + 1) // 2) * height * c["CELL_SIZE"]
        for i in range(c["MAP_LAYER_COUNT"]):
            raw = p.read(map_address + c["MAP_OFF_ARY_BEGIN"] + i * c["MAP_LAYER_STRIDE"], 24)
            begin, end, capacity = struct.unpack("<QQQ", raw) if raw else (0, 0, 0)
            valid = end - begin == expected and capacity >= end
            sample = p.read(begin, expected) if valid else None
            empty = begin == end == capacity == 0
            layers.append({"index": i, "begin": hx(begin), "end": hx(end), "bytes": end - begin,
                           "expectedBytes": expected, "empty": empty, "valid": bool(sample) if not empty else True,
                           "nonzeroBytes": sum(b != 0 for b in sample) if sample else 0,
                           "sha256": hashlib.sha256(sample).hexdigest() if sample else None})
        row_stride = p.unpack(map_address + c["MAP_OFF_ROW_STRIDE"], "<I")
        map_ok = (all(layer["valid"] for layer in layers) and any(not layer["empty"] for layer in layers)
                  and row_stride == (width * c["CELL_SIZE"] + 1) // 2)
    checks["map"] = {"pass": map_ok, "address": hx(map_address), "grid": [width, height],
                     "world": [width * c["CELL_SIZE"], height * c["CELL_SIZE"]] if width and height else None,
                     "trace": state_trace + world_trace + player_trace + [
                         {"offset": "0x78", "value": hx(parent)}, {"tailAdd": "0x8f8", "value": hx(map_address)}],
                     "layers": layers}
    checks["map"]["rowStride"] = p.unpack(map_address + c["MAP_OFF_ROW_STRIDE"], "<I") if map_address else None
    comps = components(p, player, c) if player else {}
    positioned, life, player_comp = (comps.get(name) for name in ("Positioned", "Life", "Player"))
    x = p.unpack(positioned + c["kPositioned_EntXOff"], "<i") if positioned else None
    y = p.unpack(positioned + c["kPositioned_EntYOff"], "<i") if positioned else None
    holder = p.q(player + c["kMetadataPathOff0"]) if player else None
    metadata = p.string(holder + c["kMetadataPathOff1"]) if holder else None
    player_name = p.string(player_comp + c["kPlayer_NameOff"]) if player_comp else None
    checks["player"] = {"pass": bool(comps and player_name and metadata and metadata.startswith("Metadata/Characters/") and
                                      map_ok and x is not None and y is not None and 0 <= x < width * c["CELL_SIZE"] and 0 <= y < height * c["CELL_SIZE"]),
                        "address": hx(player), "metadata": metadata, "name": player_name, "position": [x, y],
                        "hp": p.unpack(life + c["kLife_HpOff"], "<I") if life else None,
                        "components": {key: hx(value) for key, value in comps.items()}}
    inv_head, inv_trace = p.chain([root, 0x348, 0x48, 0, c["kInvTreeArrayOff"], c["kInvTreeArrayTreeOff"], c["kInvTreeSentinelOff"]])
    inventory, items = tree(p, inv_head, True, c)
    samples = []
    for entity in items[:8]:
        ic = components(p, entity, c) if entity else {}
        base = ic.get("Base")
        item_info = p.q(base + c["kBase_ItemInfoOff"]) if base else None
        samples.append({"entity": hx(entity), "name": p.string(item_info + c["kBase_NameOff"]) if item_info else None})
    checks["inventory"] = {**inventory, "pass": inventory["valid"] and all(item["name"] for item in samples),
                           "trace": inv_trace, "items": samples}
    sur_head, sur_trace = p.chain([root, 0x348, 0x48, 0, 0x1A8, 0x20F0, 0x718])
    surroundings, entities = tree(p, sur_head, False, c)
    checks["surroundings"] = {**surroundings, "pass": surroundings["valid"] and player in entities,
                              "trace": sur_trace, "containsPlayer": player in entities}
    action, action_trace = p.chain([c["kActionSystemBaseOffset"], c["kActionSystemOuterOff"] +
                                   c["kActionSystemSlotIndex"] * 8 + c["kActionSystemInnerOff"], c["kActionSystemObjOff"]])
    vt = p.q(action) if action else None
    walk = p.q(vt + c["kActionWalkVtblOff"]) if vt else None
    mid = p.q(action + c["kSkillThisOff1D0"]) if action else None
    off3e0 = p.q(mid + c["kSkillThisOff3E0"]) if mid else None
    off3f0 = p.q(mid + c["kSkillThisOff3F0"]) if mid else None
    checks["action"] = {"pass": bool(walk == p.base + c["kActionWalkEntryRva"] and off3e0 and off3f0 and p.read(off3e0, 8) and p.read(off3f0, 8)),
                        "trace": action_trace, "vtableRva": hx(vt - p.base) if vt else None,
                        "walkRva": hx(walk - p.base) if walk else None,
                        "this1d0": hx(mid), "inner3e0": hx(off3e0), "inner3f0": hx(off3f0)}
    skill_array, skill_trace = p.chain([c["kSkillContainerBase"], c["kSkillContainerOff0"], c["kSkillContainerOff1"]])
    slots = []
    if skill_array:
        for i in range(10):
            obj = p.q(skill_array + i * c["kSkillSlotStride"])
            ptr = p.q(obj + c["kSkillSharedPtrOff"]) if obj else None
            rep = p.q(obj + c["kSkillSharedPtrRepOff"]) if obj else None
            uses = p.unpack(rep + c["kRefUsesOff"], "<I") if rep else None
            vt = p.q(ptr) if ptr else None
            entry = p.q(vt + c["kSkillAbortVtblOff"]) if vt else None
            slots.append({"index": i, "object": hx(obj), "ptr": hx(ptr), "rep": hx(rep), "uses": uses,
                          "innerVtbl520": hx(entry),
                          "innerVtbl520InText": bool(entry and p.base + 0x1000 <= entry < p.base + pe.sections[0].VirtualAddress + pe.sections[0].Misc_VirtualSize)})
    checks["skillContainer"] = {"pass": any(slot["ptr"] and slot["rep"] and slot["uses"] and 1 < slot["uses"] < 10000 for slot in slots),
                                "trace": skill_trace, "slots": slots, "runtimeCastTested": False}
    gate_obj = p.q(state + c["kSkillStopObj1A0"]) if state else None
    checks["stopThis"] = {"pass": bool(state and gate_obj and p.read(gate_obj + c["kSkillStopGate669"], 1)),
                          "address": hx(state), "gate669": p.unpack(gate_obj + c["kSkillStopGate669"], "<B") if gate_obj else None,
                          "flag181": p.unpack(state + c["kSkillStopFlagOff"], "<B") if state else None,
                          "runtimeStopTested": False}
    return {"allPass": all(check["pass"] for check in checks.values()), "checks": checks,
            "scope": "Read-only pointer/data/entry validation; no game calls, hooks, injection, or process writes."}


def discover(p, constants):
    result = {}
    for name, center in [("root", constants["BaseOffset"]),
                         ("action", constants["kActionSystemBaseOffset"]),
                         ("skill", constants["kSkillContainerBase"])]:
        start, stop = center - 0x40000, center + 0x40000
        start &= ~0xFFF
        hits, seen = [], set()
        for page in range(start, stop, 0x1000):
            raw = p.read(p.base + page, 0x1000)
            if raw is None:
                continue
            for i in range(0, 0x1000, 8):
                pointer = struct.unpack_from("<Q", raw, i)[0]
                if not 0x10000 <= pointer < p.base or pointer & 7 or pointer in seen:
                    continue
                seen.add(pointer)
                rva = page + i
                if name == "root":
                    end, trace = p.chain([rva, 0x348, 0x48, 0, 0x1A8, 0x20F0, 0x5C0, 0, 0x78])
                    if not end:
                        continue
                    dims = p.read(end + 0x8F8 + 0x18, 16)
                    if dims:
                        width, _, height, _ = struct.unpack("<IIII", dims)
                        if 0 < width <= 0x1000 and 0 < height <= 0x1000:
                            hits.append({"rva": hx(rva), "trace": trace,
                                         "map": hx(end + 0x8F8), "grid": [width, height]})
                elif name == "action":
                    end, trace = p.chain([rva, 0x300, 0x3E8])
                    vt = p.q(end) if end else None
                    entry = p.q(vt + 0x198) if vt and p.base < vt < p.base + 0x4CBB000 else None
                    if entry and p.base < entry < p.base + 0x2E9D000:
                        hits.append({"rva": hx(rva), "trace": trace,
                                     "vtableRva": hx(vt - p.base), "walkRva": hx(entry - p.base)})
                else:
                    end, trace = p.chain([rva, 0x578, 0x330, 0x10])
                    ptr = p.q(end + 0x2D8) if end else None
                    rep = p.q(end + 0x2E0) if end else None
                    if ptr and rep and p.read(ptr, 16) and p.read(rep, 16):
                        hits.append({"rva": hx(rva), "trace": trace,
                                     "ptr": hx(ptr), "rep": hx(rep)})
        result[name] = {"range": [hx(start), hx(stop)], "candidates": hits}
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--discover", action="store_true")
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()
    p = Process(args.pid)
    try:
        data = p.path.read_bytes()
        pe = pefile.PE(data=data, fast_load=True)
        result = {"capturedAt": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  "pid": p.pid, "module": str(p.path), "base": hx(p.base),
                  "sha256": hashlib.sha256(data).hexdigest(),
                  "timestamp": hx(pe.FILE_HEADER.TimeDateStamp),
                  "imageSize": hx(pe.OPTIONAL_HEADER.SizeOfImage),
                  "signatures": signatures(pe)}
        result["verification"] = verify(p, load_constants(), pe)
        if args.discover:
            result["discovery"] = discover(p, load_constants())
        output = json.dumps(result, ensure_ascii=False, indent=2)
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(output + "\n", encoding="utf-8")
        print(output)
        return 0 if result["verification"]["allPass"] else 1
    finally:
        p.close()


if __name__ == "__main__":
    sys.exit(main())
