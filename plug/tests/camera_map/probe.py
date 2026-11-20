"""Independent live camera experiment. Default: read-only; --zoom writes temporarily."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from offset_audit import Process

TIMESTAMP, IMAGE_SIZE = 0x6AFB7637, 0x4CBB000
MANAGER_RVA, GLOBAL_RVA = 0x45EF380, 0x45EF370
ZOOM_INPUT_RVA = 0x1718DC
ZOOM_INPUT_SIGNATURE = bytes.fromhex('66 41 0F 6E 4E 18 0F 57 C0 0F 5B C9')


def now():
    return dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).isoformat(timespec='milliseconds')


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')


class Camera:
    def __init__(self, process):
        self.p = process
        nt = process.unpack(process.base+0x3c, '<I')
        if not nt or (process.unpack(process.base+nt+8, '<I'),
                      process.unpack(process.base+nt+0x50, '<I')) != (TIMESTAMP, IMAGE_SIZE):
            raise RuntimeError('Unsupported game build')
        if process.read(process.base+ZOOM_INPUT_RVA, len(ZOOM_INPUT_SIGNATURE)) != ZOOM_INPUT_SIGNATURE:
            raise RuntimeError('Native zoom input signature mismatch')
        self.identity = self.resolve()

    def resolve(self):
        p = self.p
        manager, _ = p.chain([MANAGER_RVA])
        client, _ = p.chain([0x90], manager)
        camera, _ = p.chain([0x368], client)
        glob, _ = p.chain([GLOBAL_RVA])
        owner, _ = p.chain([0x348], glob)
        state, _ = p.chain([0x48, 0], owner)
        world, _ = p.chain([0x1a0], state)
        if not all((manager, client, camera, glob, owner, state, world)):
            raise RuntimeError('Camera chain unavailable')
        if p.q(camera+0x98) != world or p.q(glob+0x1f8) != camera:
            raise RuntimeError('Camera/world/secondary reference mismatch')
        return {'manager': manager, 'client': client, 'camera': camera,
                'global': glob, 'owner': owner, 'state': state, 'world': world}

    def check(self):
        if self.resolve() != self.identity:
            raise RuntimeError('Camera or area identity changed')

    def snapshot(self):
        self.check()
        p, a, w = self.p, self.identity['camera'], self.identity['world']
        row = {'time': now(), 'target528': p.unpack(a+0x528, '<f'),
               'current520': p.unpack(a+0x520, '<f'), 'override524': p.unpack(a+0x524, '<f'),
               'worldZoom1580': p.unpack(w+0x1580, '<f'), 'worldCurrent15d4': p.unpack(w+0x15d4, '<f'),
               'cameraPosition454': list(struct.unpack('<3f', p.read(a+0x454, 12))),
               'cameraFocus484': list(struct.unpack('<3f', p.read(a+0x484, 12)))}
        if any(not isinstance(row[k], (int, float)) or not math.isfinite(row[k])
               for k in ['target528', 'current520', 'override524']):
            raise RuntimeError('Invalid camera values')
        return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int, required=True)
    ap.add_argument('--zoom', type=float)
    ap.add_argument('--duration', type=float, default=5)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    if args.zoom is not None and (not math.isfinite(args.zoom) or not 0.25 <= args.zoom <= 3.0):
        ap.error('--zoom must be between 0.25 and 3.0 for this experiment')
    if not 0 < args.duration <= 120:
        ap.error('--duration must be between 0 and 120 seconds')
    p = Process(args.pid)
    write_handle = None
    wrote = False
    record = {'started': now(), 'pid': args.pid, 'base': hex(p.base), 'image': str(p.path),
              'sha256': hashlib.sha256(p.path.read_bytes()).hexdigest(),
              'mode': 'temporary_zoom' if args.zoom is not None else 'read_only', 'samples': []}
    try:
        camera = Camera(p)
        record['identity'] = {k: hex(v) for k,v in camera.identity.items()}
        record['baseline'] = camera.snapshot()
        original = p.read(camera.identity['camera']+0x528, 4)
        address = camera.identity['camera']+0x528
        if args.zoom is not None:
            write_handle = p.k.OpenProcess(0x28, False, args.pid)
            if not write_handle:
                raise C.WinError(C.get_last_error())
            p.k.WriteProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p,
                                              C.c_size_t, C.POINTER(C.c_size_t)]
            p.k.WriteProcessMemory.restype = W.BOOL

            def write(raw):
                camera.check()
                done = C.c_size_t()
                buf = C.create_string_buffer(raw)
                if not p.k.WriteProcessMemory(write_handle, address, buf, len(raw), C.byref(done)) or done.value != len(raw):
                    raise C.WinError(C.get_last_error())

            record['originalBytes'] = original.hex(' ')
            record['requestedZoom'] = args.zoom
            save(args.out, record)
            camera.check()
            write(struct.pack('<f', args.zoom))
            wrote = True
            print(json.dumps({'phase': 'applied', 'time': now(), 'zoom': args.zoom,
                              'autoRestoreSeconds': args.duration}), flush=True)
        deadline = time.monotonic()+args.duration
        while time.monotonic() < deadline:
            record['samples'].append(camera.snapshot())
            save(args.out, record)
            time.sleep(0.1)
    except BaseException as exc:
        record['error'] = repr(exc)
        raise
    finally:
        if wrote:
            try:
                camera.check()
                current = p.read(address, 4)
                if current != struct.pack('<f', args.zoom):
                    raise RuntimeError('Target changed externally; original not written over it')
                write(original)
                time.sleep(0.5)
                record['restored'] = camera.snapshot()
                record['restoreVerified'] = p.read(address,4) == original
            except Exception as exc:
                record['restoreError'] = repr(exc)
        if write_handle:
            p.k.CloseHandle(write_handle)
        record['finished'] = now()
        save(args.out, record)
        p.close()
    print(json.dumps({'mode': record['mode'], 'samples': len(record['samples']),
                      'baseline': record.get('baseline'), 'last': record['samples'][-1] if record['samples'] else None,
                      'restored': record.get('restored'), 'restoreVerified': record.get('restoreVerified')}, ensure_ascii=False), flush=True)
    if wrote and not record.get('restoreVerified'):
        raise RuntimeError('Restoration needs review: '+str(record.get('restoreError')))


if __name__ == '__main__':
    main()
