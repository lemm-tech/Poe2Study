"""Standalone uint32 skill-ID call experiment; never imports production call code.

Default: external read-only inspection. --dll-preflight: temporary DLL, no game call.
--cast: exactly one ID call followed by one stop; no retry and no slot-based cast.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import datetime as dt
import hashlib
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from offset_audit import Process, load_constants, components
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

OUT = ROOT / ".build" / "tests" / "skill_id_call"
ID_CALL = 0x211ECD0
SIG = bytes.fromhex("48 89 5c 24 10 48 89 74 24 18 55 57 41 56 48 8b ec 48 81 ec 80 00 00 00")
MANAGER_GETTER = 0x16DF650
MANAGER_SIG = bytes.fromhex("48 8d 81 c0 09 00 00 c3")


class Sample(C.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("tick", C.c_uint64)] + [(n, C.c_uint32) for n in ("hp", "mana", "es")] + [
        ("x", C.c_int32), ("y", C.c_int32)] + [
        (n, C.c_uint32) for n in ("pendingId", "activeId", "phase")]


class Block(C.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [(n, C.c_uint32) for n in ("magic", "version", "bytes", "cast", "skillId")] + [
        ("x", C.c_int32), ("y", C.c_int32)] + [(n, C.c_uint32) for n in (
        "flags", "flagA", "flagB", "holdMs", "observeMs")] + [(n, C.c_uint64) for n in (
        "expectedEntity", "expectedContext")] + [(n, C.c_uint32) for n in (
        "status", "exceptionCode", "castReturned", "stopReturned")] + [(n, C.c_uint64) for n in (
        "context", "entity", "actor", "player", "life", "positioned")] + [
        ("sampleCount", C.c_uint32), ("error", C.c_char * 256), ("samples", Sample * 192)]


assert C.sizeof(Sample) == 40 and C.sizeof(Block) == 8068


def strict(p, address, fmt="<Q"):
    value = p.unpack(address, fmt)
    if value is None:
        raise RuntimeError(f"Unreadable memory at {address:#x}")
    return value


def wide(p, address):
    chars = []
    for i in range(256):
        ch = strict(p, address + i * 2, "<H")
        if not ch:
            return "".join(chars)
        chars.append(chr(ch))
    raise RuntimeError("Unterminated name")


def managed_skills(p, actor, entity, manager_getter=MANAGER_GETTER):
    getter = strict(p, strict(p, actor) + 0x168)
    if getter != p.base + manager_getter or p.read(getter, len(MANAGER_SIG)) != MANAGER_SIG:
        raise RuntimeError("Actor skill-manager getter fingerprint mismatch")
    manager = actor + 0x9C0
    if strict(p, manager + 0x140) != entity:
        raise RuntimeError("Skill manager belongs to another entity")
    begin, end, capacity = (strict(p, manager + o) for o in (0x148, 0x150, 0x158))
    if not (0 < end - begin <= 512 * 16 and (end - begin) % 16 == 0 and
            end <= capacity <= begin + 512 * 16 and (capacity - begin) % 16 == 0):
        raise RuntimeError("Invalid actor skill vector")
    result = []
    previous = 0
    for entry in range(begin, end, 16):
        ptr, rep = strict(p, entry), strict(p, entry + 8)
        skill_id = strict(p, ptr + 0x40, "<I")
        if skill_id <= previous or strict(p, ptr + 0xB8) != manager:
            raise RuntimeError("Skill vector changed or has invalid ownership/order")
        previous = skill_id
        item = {"id": skill_id, "idHex": hex(skill_id), "ptr": ptr, "rep": rep,
                "uses": strict(p, rep + 8, "<I"), "internalId": "", "localizedName": ""}
        # Built-in entries can have no effect metadata. They are listed, never auto-cast.
        try:
            effect = strict(p, strict(p, ptr + 0x58))
            item["internalId"] = wide(p, strict(p, effect))
            active = strict(p, effect + 0x4F)
            item["localizedName"] = wide(p, strict(p, active + 8))
        except RuntimeError as ex:
            item["metadataNote"] = str(ex)
        result.append(item)
    if (begin, end, capacity) != tuple(strict(p, manager + o) for o in (0x148, 0x150, 0x158)):
        raise RuntimeError("Skill vector changed during inspection")
    return {"address": manager, "begin": begin, "end": end, "capacity": capacity,
            "count": len(result), "skills": result}


def inspect(p, id_call=ID_CALL, manager_getter=MANAGER_GETTER):
    c = load_constants()
    nt = strict(p, p.base + 0x3C, "<I")
    if strict(p, p.base + nt + 8, "<I") != c["kGameTimeDateStamp"] or strict(
            p, p.base + nt + 0x50, "<I") != c["kGameImageSize"]:
        raise RuntimeError("Unsupported game PE build")
    if p.read(p.base + id_call, len(SIG)) != SIG:
        raise RuntimeError("ID-call runtime signature mismatch")
    ctx, _ = p.chain([c["BaseOffset"], 0x348, 0x48, 0])
    entity, _ = p.chain([c["BaseOffset"], 0x348, 0x48, 0, 0x1A8, 0x20F0, 0x5C0, 0])
    if not ctx or not entity:
        raise RuntimeError("Enter the game world first")
    comps = components(p, entity, c)
    if not all(n in comps for n in ("Actor", "Player", "Life", "Positioned")):
        raise RuntimeError("Character components unavailable")
    if strict(p, ctx + 0x198) != comps["Actor"] or strict(p, ctx + 0x188) != comps["Player"]:
        raise RuntimeError("Direct-call context does not own the current player")
    positioned = comps["Positioned"]
    position = [strict(p, positioned + c["kPositioned_EntXOff"], "<i"),
                strict(p, positioned + c["kPositioned_EntYOff"], "<i")]
    map_address = strict(p, entity + 0x78) + 0x8F8
    grid = [strict(p, map_address + c[n], "<I") for n in ("MAP_OFF_GRID_W", "MAP_OFF_GRID_H")]
    if not all(0 < v <= c["kMaxGridDimension"] for v in grid):
        raise RuntimeError("Invalid current map dimensions")
    world = [v * c["CELL_SIZE"] for v in grid]
    if not all(0 <= v < limit for v, limit in zip(position, world)):
        raise RuntimeError("Character position outside current map")
    manager = managed_skills(p, comps["Actor"], entity, manager_getter)
    return {"context": ctx, "entity": entity, "components": comps,
            "position": position, "map": {"address": map_address, "grid": grid, "world": world},
            "skillManager": {k: v for k, v in manager.items() if k != "skills"}, "skills": manager["skills"],
            "gates": {hex(o): strict(p, ctx + o, "<B") for o in (0x7B10, 0x8C08)}}


def code_evidence(p, entries=None):
    pe = pefile.PE(str(p.path), fast_load=True)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    result = []
    if entries is None:
        entries = ((0x18C940, 0x1E7), (0x1856E0, 0x10F), (ID_CALL, 0x209),
                   (0x1DE1920, 0x50), (0x211F110, 0x7E), (MANAGER_GETTER, 8), (0x1EDBEA7, 0x7B))
    for rva, size in entries:
        data = p.read(p.base + rva, size)
        if not data or data != pe.get_data(rva, size):
            raise RuntimeError(f"Live/disk code mismatch at {rva:#x}")
        result.append({"rva": hex(rva), "liveEqualsDisk": True,
            "sha256": hashlib.sha256(data).hexdigest(), "bytes": data.hex(" "),
            "instructions": [f"{i.address:#x}: {i.mnemonic} {i.op_str}" for i in md.disasm(data, rva)]})
    return result


class Remote:
    def __init__(self, p):
        self.p, self.k = p, p.k
        signatures = {
            "VirtualAllocEx": ([W.HANDLE, C.c_void_p, C.c_size_t, W.DWORD, W.DWORD], C.c_void_p),
            "VirtualFreeEx": ([W.HANDLE, C.c_void_p, C.c_size_t, W.DWORD], W.BOOL),
            "WriteProcessMemory": ([W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)], W.BOOL),
            "CreateRemoteThread": ([W.HANDLE, C.c_void_p, C.c_size_t, C.c_void_p, C.c_void_p, W.DWORD, C.c_void_p], W.HANDLE),
            "WaitForSingleObject": ([W.HANDLE, W.DWORD], W.DWORD),
            "GetExitCodeThread": ([W.HANDLE, C.POINTER(W.DWORD)], W.BOOL),
            "GetModuleHandleW": ([W.LPCWSTR], W.HMODULE),
            "GetProcAddress": ([W.HMODULE, C.c_char_p], C.c_void_p),
            "GetModuleHandleExW": ([W.DWORD, C.c_void_p, C.POINTER(W.HMODULE)], W.BOOL),
            "GetModuleFileNameW": ([W.HMODULE, W.LPWSTR, W.DWORD], W.DWORD),
        }
        for name, (args, ret) in signatures.items():
            getattr(self.k, name).argtypes = args
            getattr(self.k, name).restype = ret
        self.handle = self.k.OpenProcess(0x43A, False, p.pid)
        if not self.handle:
            raise C.WinError(C.get_last_error())
        self.allocations = []
        self.pending = False
        self.module = 0

    def modules(self):
        modules = (C.c_void_p * 2048)()
        needed = W.DWORD()
        if not self.k.K32EnumProcessModules(self.handle, modules, C.sizeof(modules), C.byref(needed)):
            raise C.WinError(C.get_last_error())
        result = {}
        for base in modules[:needed.value // C.sizeof(C.c_void_p)]:
            path = C.create_unicode_buffer(32768)
            if self.k.K32GetModuleFileNameExW(self.handle, base, path, len(path)):
                result[path.value.lower()] = base
        return result

    def system_export(self, name):
        address = self.k.GetProcAddress(self.k.GetModuleHandleW("kernel32.dll"), name.encode())
        owner = W.HMODULE()
        if not address or not self.k.GetModuleHandleExW(6, address, C.byref(owner)):
            raise RuntimeError(f"Cannot resolve {name}")
        path = C.create_unicode_buffer(32768)
        self.k.GetModuleFileNameW(owner, path, len(path))
        remote = self.modules().get(path.value.lower())
        if not remote:
            raise RuntimeError(f"Remote system module missing for {name}")
        return remote + address - owner.value

    def allocate(self, data):
        address = self.k.VirtualAllocEx(self.handle, None, len(data), 0x3000, 4)
        if not address:
            raise C.WinError(C.get_last_error())
        self.allocations.append(address)
        done = C.c_size_t()
        buf = C.create_string_buffer(data)
        if not self.k.WriteProcessMemory(self.handle, address, buf, len(data), C.byref(done)) or done.value != len(data):
            raise C.WinError(C.get_last_error())
        return address

    def call(self, entry, arg, timeout=8000):
        thread = self.k.CreateRemoteThread(self.handle, None, 0, entry, arg, 0, None)
        if not thread:
            raise C.WinError(C.get_last_error())
        try:
            waited = self.k.WaitForSingleObject(thread, timeout)
            if waited != 0:
                self.pending = True
                raise RuntimeError("Remote thread did not complete; DLL and argument buffer retained, no replay")
            code = W.DWORD()
            if not self.k.GetExitCodeThread(thread, C.byref(code)):
                raise C.WinError(C.get_last_error())
            return code.value
        finally:
            self.k.CloseHandle(thread)

    def run(self, block, dll):
        path = str(dll.resolve())
        if path.lower() in self.modules():
            raise RuntimeError("Experimental DLL already loaded; previous test must finish first")
        address = self.allocate((path + "\0").encode("utf-16-le"))
        self.call(self.system_export("LoadLibraryW"), address)
        self.module = self.modules().get(path.lower(), 0)
        if not self.module:
            raise RuntimeError("Temporary probe DLL failed to load")
        pe = pefile.PE(path)
        export = next((e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name == b"ProbeRun"), None)
        if export is None:
            raise RuntimeError("ProbeRun export missing")
        request = self.allocate(bytes(block))
        self.call(self.module + export, request)
        raw = self.p.read(request, C.sizeof(Block))
        if not raw:
            raise RuntimeError("Probe result unreadable")
        return Block.from_buffer_copy(raw)

    def close(self):
        unloaded = False
        errors = []
        try:
            if self.module and not self.pending:
                try:
                    released = self.call(self.system_export("FreeLibrary"), self.module)
                    unloaded = bool(released and self.module not in self.modules().values())
                    if not unloaded:
                        errors.append("Temporary DLL unload could not be confirmed")
                except Exception as ex:
                    errors.append(str(ex))
            if not self.pending:
                for address in self.allocations:
                    if not self.k.VirtualFreeEx(self.handle, address, 0, 0x8000):
                        errors.append(str(C.WinError(C.get_last_error())))
            else:
                errors.append("Remote thread pending; argument buffers and DLL retained")
        finally:
            self.k.CloseHandle(self.handle)
        if errors:
            raise RuntimeError("Cleanup incomplete: " + "; ".join(errors))
        return unloaded


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pid", type=int, default=0)
    ap.add_argument("--id", type=lambda v: int(v, 0))
    action = ap.add_mutually_exclusive_group()
    action.add_argument("--cast", action="store_true")
    action.add_argument("--dll-preflight", action="store_true")
    ap.add_argument("--x", type=int)
    ap.add_argument("--y", type=int)
    ap.add_argument("--hold-ms", type=int, default=800)
    ap.add_argument("--flags", type=lambda v: int(v, 0), default=0x400)
    args = ap.parse_args()
    if not args.pid:
        import subprocess
        raw = subprocess.check_output(["powershell", "-NoProfile", "-Command",
            "@(Get-Process PathOfExile -ErrorAction SilentlyContinue).Id | ConvertTo-Json -Compress"], text=True)
        ids = json.loads(raw) if raw.strip() else []
        ids = [ids] if isinstance(ids, int) else ids
        if len(ids) != 1:
            ap.error("Select --pid when there is not exactly one game")
        args.pid = ids[0]
    if (args.x is None) != (args.y is None) or not 0 <= args.hold_ms <= 1800 or args.flags not in (0x400, 0x401):
        ap.error("Use both coordinates, hold 0..1800ms, flags 0x400/0x401")
    if args.x is not None and not all(0 <= v <= 0x7FFFFFFF for v in (args.x, args.y)):
        ap.error("Coordinates must be non-negative int32 values")
    if (args.cast or args.dll_preflight) and (args.id is None or not 0 < args.id <= 0xFFFFFFFF):
        ap.error("--cast/--dll-preflight requires a complete uint32 --id")
    OUT.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).strftime("%Y%m%d%H%M%S%f")
    output = OUT / f"{'cast' if args.cast else 'preflight' if args.dll_preflight else 'inspect'}-{stamp}.json"
    report = {"schemaVersion": 2, "capturedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
              "pid": args.pid, "mode": "cast" if args.cast else "dll-preflight" if args.dll_preflight else "inspect",
              "gameplaySuccess": False, "slotUsedAsCallParameter": False, "slotUsedForDiscovery": False,
              "idCallRva": hex(ID_CALL), "remoteAttempted": False}
    p = None
    code = 1
    try:
        p = Process(args.pid)
        basis = inspect(p)
        report.update({"moduleBase": hex(p.base), "basis": basis, "code": code_evidence(p)})
        print(f"Actor skill manager: {len(basis['skills'])} IDs (including built-in/triggered skills)")
        for skill in basis["skills"]:
            if skill["internalId"]:
                print(f"{skill['idHex']:12} {skill['internalId']:38} {skill['localizedName']}")
        if not (args.cast or args.dll_preflight):
            report["readOnly"] = True
            code = 0
        else:
            matches = [s for s in basis["skills"] if s["id"] == args.id]
            if len(matches) != 1:
                raise RuntimeError("ID is not among currently verified character skills; no call attempted")
            block = Block()
            block.magic, block.version, block.bytes = 0x44494B53, 1, C.sizeof(Block)
            block.cast, block.skillId = int(args.cast), args.id
            block.flags, block.flagA, block.flagB = args.flags, 0, 0
            block.holdMs, block.observeMs = args.hold_ms, 1000
            block.expectedEntity, block.expectedContext = basis["entity"], basis["context"]
            block.x = basis["position"][0] + 40 if args.x is None else args.x
            block.y = basis["position"][1] if args.y is None else args.y
            if not all(0 <= v < limit for v, limit in zip((block.x, block.y), basis["map"]["world"])):
                raise RuntimeError("Target coordinates outside current map; no call attempted")
            report["request"] = {n: getattr(block, n) for n in (
                "skillId", "x", "y", "flags", "flagA", "flagB", "holdMs", "observeMs")}
            remote = Remote(p)
            report["remoteAttempted"] = True
            report["temporaryDllUnloaded"] = False
            try:
                result = remote.run(block, OUT / "SkillIdCallProbe.dll")
                if result.sampleCount > 192:
                    raise RuntimeError("Invalid sample count")
                report["result"] = {n: getattr(result, n) for n in (
                    "status", "exceptionCode", "castReturned", "stopReturned", "context", "entity", "sampleCount")}
                report["result"]["error"] = result.error.decode("utf-8", errors="replace")
                samples = [{n: getattr(s, n) for n, _ in Sample._fields_} for s in result.samples[:result.sampleCount]]
                report["samples"] = samples
                if result.status != 1:
                    raise RuntimeError(report["result"]["error"])
                before = [s for s in samples if s["phase"] == 0]
                during = [s for s in samples if s["phase"] == 1]
                active = any(s["activeId"] == args.id for s in during)
                pending = any(s["pendingId"] == args.id for s in during)
                mana_drop = bool(before and during and min(s["mana"] for s in during) < before[-1]["mana"])
                cleared = bool(samples and samples[-1]["phase"] == 2 and samples[-1]["pendingId"] == 0)
                report["evidence"] = {"activeSkillMatchesIdExperimental": active, "pendingActionMatchesId": pending,
                                      "manaDropped": mana_drop,
                                      "pendingActionClearedAfterStop": cleared,
                                      "note": "Local action evidence; does not prove a server hit or an unbound skill"}
                report["gameplaySuccess"] = bool(args.cast and pending and mana_drop and cleared)
                code = 0 if not args.cast or report["gameplaySuccess"] else 3
            finally:
                report["temporaryDllUnloaded"] = remote.close()
            if not report["temporaryDllUnloaded"]:
                raise RuntimeError("Temporary DLL not confirmed unloaded")
            report["after"] = inspect(p)
    except Exception as ex:
        code = 1
        report["error"] = f"{type(ex).__name__}: {ex}"
        print("FAIL", report["error"])
    finally:
        if p:
            p.close()
        report["exitCode"] = code
        with output.open("x", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)
        print("RESULT", output)
    if code == 0:
        print("PASS", "actual action and mana evidence" if args.cast else "preflight/inspection only")
    elif code == 3:
        print("INCONCLUSIVE: call returned, but actual-action evidence is insufficient; no retry")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
