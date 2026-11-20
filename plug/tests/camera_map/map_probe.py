"""Independent minimap experiment: read-only unless --reveal is supplied.

The native reveal request changes exploration data. Clearing the request byte
does not undo revealed fog. Save the exact pre-test buffer before any write.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import math
from pathlib import Path
import struct
import time

from probe import Camera, Process, now, save

SIGNATURES = {
    0x121775C: bytes.fromhex('41 C7 04 82 00 00 80 3F'),
    0x121828B: bytes.fromhex('41 80 7F 61 00'),
    0x1218883: bytes.fromhex('66 41 C7 47 60 00 00'),
}


class Minimap:
    def __init__(self, process):
        self.p = process
        self.camera = Camera(process)
        for rva, raw in SIGNATURES.items():
            if process.read(process.base+rva, len(raw)) != raw:
                raise RuntimeError(f'Native minimap signature mismatch: {rva:#x}')
        self.identity = self.resolve()

    def resolve(self):
        self.camera.check()
        p = self.p
        world = self.camera.identity['world']
        m = p.q(self.camera.identity['client']+0x4C0)
        if not m or p.q(m) != world+0xE78 or not p.q(m+0x40):
            raise RuntimeError('Minimap/world/render resource mismatch')
        descriptor = p.q(m+0x2A8)
        header = p.read(descriptor, 40)
        if not header:
            raise RuntimeError('Unreadable exploration descriptor')
        width, height, world_width, world_height, pixels, end, cap = struct.unpack('<IIffQQQ', header)
        count = width*height
        if not (0 < width <= 4096 and 0 < height <= 4096 and count <= 1048576
                and math.isfinite(world_width) and world_width > 0
                and math.isfinite(world_height) and world_height > 0
                and 0x10000 <= pixels < 0x800000000000
                and end-pixels == count*4 and end <= cap <= pixels+0x1000000):
            raise RuntimeError('Invalid exploration dimensions/vector bounds')
        return {'minimap': m, 'descriptor': descriptor, 'pixels': pixels,
                'end': end, 'capacity': cap, 'width': width, 'height': height,
                'worldWidth': world_width, 'worldHeight': world_height}

    def check(self):
        if self.resolve() != self.identity:
            raise RuntimeError('Minimap or area identity changed; refusing writes')

    def snapshot(self):
        self.check()
        m = self.identity['minimap']
        raw = self.p.read(self.identity['pixels'], self.identity['width']*self.identity['height']*4)
        if not raw:
            raise RuntimeError('Exploration buffer unreadable')
        values = struct.unpack(f'<{len(raw)//4}f', raw)
        if any(not math.isfinite(v) or not 0 <= v <= 1.00001 for v in values):
            raise RuntimeError('Invalid exploration values')
        flags = self.p.read(m+0x60, 3)
        if not flags or any(v not in (0, 1) for v in flags):
            raise RuntimeError('Invalid minimap request bytes')
        begin, end, cap = struct.unpack('<QQQ', self.p.read(m+0x68, 24))
        row = {'time': now(), 'flags60_62': list(flags),
               'cells': len(values), 'zero': values.count(0.0), 'one': values.count(1.0),
               'min': min(values), 'max': max(values), 'bufferSha256': hashlib.sha256(raw).hexdigest(),
               'queueCount': (end-begin)//12 if begin <= end <= cap and (end-begin)%12 == 0 else None}
        self.check()
        return row, raw


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pid', required=True, type=int)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--duration', type=float, default=8)
    ap.add_argument('--reveal', action='store_true')
    args = ap.parse_args()
    if not 0 < args.duration <= 30:
        ap.error('--duration must be between 0 and 30 seconds')
    if args.out.exists():
        ap.error('--out must be new to preserve earlier evidence')
    p = Process(args.pid)
    write_handle = None
    requested = False
    record = {'started': now(), 'pid': args.pid, 'base': hex(p.base), 'image': str(p.path),
              'sha256': hashlib.sha256(p.path.read_bytes()).hexdigest(),
              'mode': 'native_full_reveal_request' if args.reveal else 'read_only',
              'samples': [], 'visualConfirmed': None,
              'explorationRestored': False if args.reveal else None}
    try:
        minimap = Minimap(p)
        record['cameraIdentity'] = {k: hex(v) for k, v in minimap.camera.identity.items()}
        record['minimapIdentity'] = {k: hex(v) if isinstance(v, int) and k not in ('width','height') else v
                                     for k, v in minimap.identity.items()}
        record['baseline'], before = minimap.snapshot()
        before_path = args.out.with_suffix('.before.bin')
        after_path = args.out.with_suffix('.after.bin')
        if before_path.exists() or after_path.exists():
            raise RuntimeError('Evidence buffer already exists')
        before_path.parent.mkdir(parents=True, exist_ok=True)
        with before_path.open('xb') as f:
            f.write(before)
        record['beforeBuffer'] = str(before_path.resolve())
        save(args.out, record)
        if args.reveal:
            if record['baseline']['flags60_62'] != [0, 0, 0]:
                raise RuntimeError('Native request already pending; wait before testing')
            write_handle = p.k.OpenProcess(0x28, False, args.pid)
            if not write_handle:
                raise C.WinError(C.get_last_error())
            p.k.WriteProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p,
                                              C.c_size_t, C.POINTER(C.c_size_t)]
            p.k.WriteProcessMemory.restype = W.BOOL
            address = minimap.identity['minimap']+0x61

            def write_flag(raw):
                minimap.check()
                done = C.c_size_t()
                buffer = C.create_string_buffer(raw)
                if not p.k.WriteProcessMemory(write_handle, address, buffer, len(raw), C.byref(done)) or done.value != len(raw):
                    raise C.WinError(C.get_last_error())

            current, _ = minimap.snapshot()
            if current['flags60_62'] != [0, 0, 0] or current['bufferSha256'] != record['baseline']['bufferSha256']:
                raise RuntimeError('Baseline changed before write; take a fresh baseline')
            record['requestAddress'] = hex(address)
            record['requestTime'] = now()
            save(args.out, record)
            write_flag(b'\x01')
            requested = True
            print('Applied native full-reveal byte; observing consumption.', flush=True)
        deadline = time.monotonic()+args.duration
        while time.monotonic() < deadline:
            sample, _ = minimap.snapshot()
            record['samples'].append(sample)
            if requested and sample['flags60_62'][1] == 0 and 'consumedAt' not in record:
                record['consumedAt'] = now()
            time.sleep(0.1)
        record['after'], after = minimap.snapshot()
        with after_path.open('xb') as f:
            f.write(after)
        record['afterBuffer'] = str(after_path.resolve())
        record['changedCells'] = sum(a != b for a, b in zip(struct.iter_unpack('<f', before), struct.iter_unpack('<f', after)))
        record['cpuFullyRevealed'] = record['after']['one'] == record['after']['cells']
    except BaseException as exc:
        record['error'] = repr(exc)
        raise
    finally:
        if requested:
            try:
                minimap.check()
                pending = p.read(address, 1)
                if pending == b'\x01':
                    write_flag(b'\x00')
                    record['pendingRequestCleared'] = True
                elif pending != b'\x00':
                    raise RuntimeError('Unexpected request byte; no cleanup write')
                record['requestByteAtEnd'] = p.unpack(address, '<B')
            except Exception as exc:
                record['cleanupError'] = repr(exc)
        if write_handle:
            p.k.CloseHandle(write_handle)
        record['finished'] = now()
        save(args.out, record)
        p.close()
    print({k: record.get(k) for k in ('mode','baseline','after','changedCells','consumedAt','cpuFullyRevealed','requestByteAtEnd','cleanupError')}, flush=True)
    if record.get('cleanupError') or (requested and record.get('requestByteAtEnd') != 0):
        raise RuntimeError('Pending reveal request cleanup needs review')


if __name__ == '__main__':
    main()
