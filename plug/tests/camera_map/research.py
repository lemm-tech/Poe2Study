"""Offline PE evidence helper for the independent camera/minimap experiment."""
import argparse
import bisect
import json
from pathlib import Path
import re
import struct

import numpy as np
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64


class Image:
    def __init__(self, path):
        self.path = Path(path)
        self.pe = pefile.PE(str(path), fast_load=True)
        self.pe.parse_data_directories(directories=[3])
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.md = Cs(CS_ARCH_X86, CS_MODE_64)
        self.functions = sorted((e.struct.BeginAddress, e.struct.EndAddress)
                                for e in self.pe.DIRECTORY_ENTRY_EXCEPTION)
        self.starts = [f[0] for f in self.functions]
        self.decoded = {}

    def bounds(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0 and self.functions[i][0] <= rva < self.functions[i][1]:
            return self.functions[i]
        return rva, rva + 256

    def disasm(self, rva, size=None):
        start, end = self.bounds(rva)
        if size is not None:
            start, end = rva, rva + size
        return [f'{i.address:#x}: {i.mnemonic} {i.op_str}'
                for i in self.md.disasm(self.pe.get_data(start, end-start), start)]

    def xrefs(self, targets):
        """Find disp32 operands, then validate each candidate by disassembly."""
        result = {hex(t): [] for t in targets}
        for sec in self.pe.sections:
            if not sec.Characteristics & 0x20000000:
                continue
            raw = sec.get_data()
            for alignment in range(4):
                count = (len(raw) - alignment) // 4
                values = np.frombuffer(raw, dtype='<u4', count=count, offset=alignment)
                slots = np.arange(count, dtype=np.uint32) * 4 + sec.VirtualAddress + alignment
                destinations = values + slots + 4
                for target in targets:
                    for index in np.flatnonzero(destinations == target):
                        slot = int(slots[index])
                        begin, end = self.bounds(slot)
                        if begin not in self.decoded:
                            self.decoded[begin] = list(self.md.disasm(self.pe.get_data(begin, end-begin), begin))
                        for instruction in self.decoded[begin]:
                            if instruction.address + instruction.size != slot + 4:
                                continue
                            if ('rip' in instruction.op_str or instruction.mnemonic in ('call', 'jmp')):
                                row = {'rva': hex(instruction.address), 'instruction': instruction.mnemonic+' '+instruction.op_str,
                                       'function': [hex(begin), hex(end)]}
                                if row not in result[hex(target)]:
                                    result[hex(target)].append(row)
        return result

    def pointers(self, target):
        result = []
        for sec in self.pe.sections:
            data, at = sec.get_data(), 0
            needle = struct.pack('<Q', self.base + target)
            while (at := data.find(needle, at)) >= 0:
                result.append(hex(sec.VirtualAddress + at))
                at += 1
        return result

    def rtti(self, token):
        """Follow x64 MSVC type descriptor -> complete object locator -> vtable."""
        raw = self.pe.__data__
        types = []
        for m in re.finditer(rb'\.\?AV[^\x00]{1,1400}\x00', raw):
            name = m.group()[:-1].decode(errors='replace')
            if token.lower() not in name.lower():
                continue
            name_rva = self.pe.get_rva_from_offset(m.start())
            descriptor = name_rva - 16
            locators = []
            for sec in self.pe.sections:
                data = sec.get_data()
                needle = struct.pack('<I', descriptor)
                at = 0
                while (at := data.find(needle, at)) >= 0:
                    col = sec.VirtualAddress + at - 12
                    body = self.pe.get_data(col, 24)
                    if len(body) == 24:
                        signature, offset, ctor, typ, hierarchy, self_rva = struct.unpack('<6I', body)
                        if signature == 1 and self_rva == col:
                            locator = {'rva': hex(col), 'offset': offset, 'vtables': []}
                            for other in self.pe.sections:
                                b = other.get_data()
                                pos = 0
                                ptr = struct.pack('<Q', self.base + col)
                                while (pos := b.find(ptr, pos)) >= 0:
                                    vt = other.VirtualAddress + pos + 8
                                    entries = struct.unpack('<8Q', self.pe.get_data(vt, 64))
                                    locator['vtables'].append({'rva': hex(vt), 'entries': [hex(x-self.base) for x in entries]})
                                    pos += 8
                            locators.append(locator)
                    at += 4
            types.append({'name': name, 'descriptor': hex(descriptor), 'locators': locators})
        return types


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', required=True, help='Path to the local game executable')
    ap.add_argument('--xref', nargs='*', type=lambda x: int(x, 0))
    ap.add_argument('--disasm', nargs='*', type=lambda x: int(x, 0))
    ap.add_argument('--size', type=lambda x: int(x, 0))
    ap.add_argument('--rtti')
    ap.add_argument('--pointers', nargs='*', type=lambda x: int(x, 0))
    ap.add_argument('--out', type=Path)
    args = ap.parse_args()
    image = Image(args.image)
    result = {'image': str(image.path), 'timestamp': hex(image.pe.FILE_HEADER.TimeDateStamp),
              'sizeOfImage': hex(image.pe.OPTIONAL_HEADER.SizeOfImage)}
    if args.xref:
        result['xrefs'] = image.xrefs(args.xref)
    if args.disasm:
        result['disasm'] = {hex(rva): image.disasm(rva, args.size) for rva in args.disasm}
    if args.rtti:
        result['rtti'] = image.rtti(args.rtti)
    if args.pointers:
        result['pointers'] = {hex(t): image.pointers(t) for t in args.pointers}
    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(rendered+'\n', encoding='utf-8')
    print(rendered)


if __name__ == '__main__':
    main()
