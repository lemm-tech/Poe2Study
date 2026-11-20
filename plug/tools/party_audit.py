"""Read party names and their map identities using ReadProcessMemory only.

No injection, game calls, packet sending, input simulation, or process writes.
Offsets come from Common/GameOffsets.h. Requires 64-bit Windows Python and the
existing offset_audit.py dependency (pefile). See docs/队伍成员与地图指针链_20261118.md.
"""
import argparse
import ctypes
import datetime
import json
from pathlib import Path
import struct
import sys

from offset_audit import Process, load_constants


class ReadError(RuntimeError):
    pass


class PartyReader:
    def __init__(self, process):
        self.p = process
        self.c = load_constants()

    def raw(self, address, size):
        if size == 0:
            return b""
        data = self.p.read(address, size)
        if data is None:
            raise ReadError(f"Unreadable memory at {address:#x}, size {size:#x}")
        return data

    def number(self, address, fmt="<Q"):
        return struct.unpack(fmt, self.raw(address, struct.calcsize(fmt)))[0]

    def follow(self, address, offsets):
        trace = []
        for offset in offsets:
            slot = address + offset
            address = self.number(slot)
            if not 0x10000 <= address < 0x800000000000:
                raise ReadError(f"Invalid pointer at {slot:#x}: {address:#x}")
            trace.append({"slot": hex(slot), "offset": hex(offset), "value": hex(address)})
        return address, trace

    def vector(self, address, stride, limit):
        begin, end, capacity = struct.unpack("<QQQ", self.raw(address, 24))
        if begin == end == capacity == 0:
            return (begin, end, capacity), b""
        if not (0x10000 <= begin <= end <= capacity < 0x800000000000):
            raise ReadError(f"Invalid vector bounds at {address:#x}")
        if (end - begin) % stride or (capacity - begin) % stride:
            raise ReadError(f"Invalid vector stride at {address:#x}")
        if (capacity - begin) // stride > limit:
            raise ReadError(f"Vector exceeds bounded read limit at {address:#x}")
        return (begin, end, capacity), self.raw(begin, end - begin)

    def wstring(self, record, offset):
        c = self.c
        length = struct.unpack_from("<Q", record, offset + c["kStdStringLengthOff"])[0]
        capacity = struct.unpack_from("<Q", record, offset + c["kStdStringMaxLenOff"])[0]
        if length > 256 or capacity < length or capacity > 0x100000:
            raise ReadError("Invalid std::wstring length/capacity")
        if capacity <= c["kStdStringSsoLimit"]:
            data = record[offset:offset + length * 2]
        else:
            pointer = struct.unpack_from("<Q", record, offset + c["kStdStringDataOff"])[0]
            data = self.raw(pointer, length * 2)
        result = data.decode("utf-16-le")
        if "\0" in result:
            raise ReadError("Embedded NUL in identity string")
        return result

    def utf16z(self, address):
        if not address:
            raise ReadError("Null UTF-16Z pointer")
        data = bytearray()
        for offset in range(0, 1024, 2):
            unit = self.raw(address + offset, 2)
            if unit == b"\0\0":
                return data.decode("utf-16-le")
            data.extend(unit)
        raise ReadError(f"Unterminated UTF-16Z at {address:#x}")

    def area(self, row):
        c = self.c
        return {
            "row": hex(row),
            "numericId": f'0x{self.number(row + c["kWorldAreaNumericIdOff"], "<H"):04X}',
            "id": self.utf16z(self.number(row + c["kWorldAreaIdOff"])),
            "name": self.utf16z(self.number(row + c["kWorldAreaNameOff"])),
        }

    def snapshot(self):
        p, c = self.p, self.c
        owner, root_trace = self.follow(p.base, [c["BaseOffset"], c["kSkillStopChainOff1"]])
        state, _ = self.follow(owner, [c["kSkillStopChainOff2"], 0])
        if self.number(state + c["kPacketProtocolOff"]) != owner:
            raise ReadError("State/owner relationship changed")
        world = self.number(owner + c["kPacketWorldOff"])
        table = self.number(world + c["kWorldAreaTableOff"])
        table_name = self.utf16z(self.number(table + c["kWorldAreaTableNameOff"]))
        if table_name != "Data/Balance/WorldAreas.dat":
            raise ReadError(f"Unexpected area table: {table_name!r}")
        storage = self.number(table + c["kWorldAreaStorageOff"])
        rows_header, rows = self.vector(storage + c["kWorldAreaRowsBeginOff"],
                                        c["kWorldAreaRowStride"], 16384)
        lookup_header, lookup = self.vector(storage + c["kWorldAreaLookupBeginOff"],
                                            c["kWorldAreaLookupStride"], 16384)
        area_rows = {}
        for offset in range(0, len(lookup), c["kWorldAreaLookupStride"]):
            key = struct.unpack_from("<H", lookup, offset + c["kWorldAreaLookupKeyOff"])[0]
            row = struct.unpack_from("<Q", lookup, offset + c["kWorldAreaLookupRowOff"])[0]
            delta = row - rows_header[0]
            if not 0 <= delta < len(rows) or delta % c["kWorldAreaRowStride"]:
                raise ReadError("Area lookup points outside the row vector")
            row_key = struct.unpack_from("<H", rows, delta + c["kWorldAreaNumericIdOff"])[0]
            if row_key != key or key in area_rows:
                raise ReadError("Area lookup key mismatch/duplicate")
            area_rows[key] = row

        roster_header, roster = self.vector(owner + c["kPartyRosterBeginOff"],
                                            c["kPartyRosterStride"], 64)
        profiles_header, profiles = self.vector(owner + c["kPartyProfilesBeginOff"],
                                                c["kPartyProfileStride"], 4096)
        by_identity = {}
        for offset in range(0, len(profiles), c["kPartyProfileStride"]):
            record = profiles[offset:offset + c["kPartyProfileStride"]]
            account = self.wstring(record, c["kPartyProfileAccountOff"])
            name = self.wstring(record, c["kPartyProfileNameOff"])
            key = struct.unpack_from("<H", record, c["kPartyProfileAreaKeyOff"])[0]
            identity = (account.casefold(), name.casefold())
            by_identity.setdefault(identity, []).append((profiles_header[0] + offset, key))

        members = []
        for offset in range(0, len(roster), c["kPartyRosterStride"]):
            record = roster[offset:offset + c["kPartyRosterStride"]]
            account = self.wstring(record, c["kPartyRosterAccountOff"])
            name = self.wstring(record, c["kPartyRosterNameOff"])
            matches = by_identity.get((account.casefold(), name.casefold()), [])
            member = {"name": name, "account": account,
                      "rosterRecord": hex(roster_header[0] + offset),
                      "profileRecord": None, "areaKey": None, "map": None}
            if len(matches) == 1:
                address, key = matches[0]
                member.update(profileRecord=hex(address), areaKey=f"0x{key:04X}")
                if key and key in area_rows:
                    member["map"] = self.area(area_rows[key])
                    member["mapStatus"] = "resolved"
                else:
                    member["mapStatus"] = "area_unavailable"
            else:
                member["mapStatus"] = "profile_missing" if not matches else "profile_ambiguous"
            members.append(member)

        local_row = self.number(world + c["kWorldAreaRecordOff"])
        return {
            "pointers": {"rootTrace": root_trace, "owner": hex(owner), "state": hex(state),
                         "world": hex(world), "areaTable": hex(table), "areaStorage": hex(storage),
                         "rosterVector": [hex(x) for x in roster_header],
                         "profileVector": [hex(x) for x in profiles_header],
                         "areaRows": [hex(x) for x in rows_header],
                         "areaLookup": [hex(x) for x in lookup_header]},
            "counts": {"party": len(members), "profiles": len(profiles) // c["kPartyProfileStride"],
                       "areas": len(area_rows)},
            "localMap": self.area(local_row),
            "members": members,
        }

    def read(self):
        c, p = self.c, self.p
        nt = p.base + self.number(p.base + 0x3C, "<I")
        timestamp = self.number(nt + 8, "<I")
        image_size = self.number(nt + 0x50, "<I")
        machine = self.number(nt + 4, "<H")
        if (machine, timestamp, image_size) != (0x8664, c["kGameTimeDateStamp"], c["kGameImageSize"]):
            raise ReadError("Unsupported game build; audit offsets again before reading party data")
        # Two consecutive independently resolved samples must agree on all emitted data.
        previous = self.snapshot()
        for _ in range(3):
            current = self.snapshot()
            if previous == current:
                return {
                    "capturedAt": datetime.datetime.now().astimezone().isoformat(),
                    "readOnly": True, "pid": p.pid,
                    "module": {"path": str(p.path), "base": hex(p.base),
                               "timeDateStamp": hex(timestamp), "sizeOfImage": hex(image_size)},
                    "consistentSamples": 2, **current,
                }
            previous = current
        raise ReadError("Party/map identity changed during reads; run again after it settles")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True, help="PathOfExile.exe process ID")
    parser.add_argument("--name", help="Only emit this exact character name (case insensitive)")
    parser.add_argument("--output", type=Path, help="Optional local JSON evidence path")
    args = parser.parse_args()
    if ctypes.sizeof(ctypes.c_void_p) != 8:
        parser.error("64-bit Python is required")
    process = None
    try:
        process = Process(args.pid)
        result = PartyReader(process).read()
        if args.name:
            result["members"] = [m for m in result["members"] if m["name"].casefold() == args.name.casefold()]
            result["nameFilter"] = args.name
        text = json.dumps(result, ensure_ascii=False, indent=2)
        if args.output:
            args.output.write_text(text + "\n", encoding="utf-8")
        print(text)
        return 0
    except (ReadError, OSError, UnicodeError, struct.error) as error:
        print(json.dumps({"error": str(error), "readOnly": True}, ensure_ascii=False), file=sys.stderr)
        return 1
    finally:
        if process:
            process.close()


if __name__ == "__main__":
    raise SystemExit(main())
