"""Capture outgoing packet objects, serializer writes and stacks in a temporary DLL.

Observes user actions; does not issue game requests. STOP file ends recording and
removes both hooks before releasing the DLL. No production binary is replaced.
"""
import argparse
import ctypes as C
import datetime as dt
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from offset_audit import Process, components, load_constants
import pefile


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


transport = module('portal_remote_transport', ROOT / 'tests/skill_id_call/probe.py')
entities = module('portal_entity_probe', Path(__file__).with_name('probe.py'))


class Event(C.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [(name, C.c_uint64) for name in (
        'sequence', 'tick', 'buildSequence', 'caller', 'rcx', 'rdx', 'r8', 'r9',
        'vtable', 'serializer', 'world', 'entity')] + [(name, C.c_uint32) for name in (
            'kind', 'threadId', 'opcode', 'objectLength', 'dataLength', 'originalLength', 'stackCount', 'flags')] + [
                ('x', C.c_int32), ('y', C.c_int32), ('stack', C.c_uint64 * 24),
                ('object', C.c_uint8 * 256), ('data', C.c_uint8 * 1024)]


class Block(C.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [(name, C.c_uint32) for name in (
        'magic', 'version', 'bytes', 'operation', 'status', 'exceptionCode', 'eventCount', 'hooksActive')] + [
            (name, C.c_uint64) for name in (
                'state', 'protocol', 'stream', 'world', 'entity', 'positioned', 'totalEvents', 'dropped')] + [
                    ('events', Event * 32)]


assert C.sizeof(Event) == 1608 and C.sizeof(Block) == 51552


def stamp():
    return dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).isoformat(timespec='milliseconds')


def output(path, data):
    temp = path.with_suffix('.tmp')
    temp.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    temp.replace(path)


def required(value, label):
    if value is None or value == 0:
        raise RuntimeError(f'{label} unavailable')
    return value


class Capture:
    def __init__(self, process, dll):
        self.p = process
        self.remote = transport.Remote(process)
        self.request = 0
        self.entry = 0
        self.started = False
        self.cleaned = False
        self.c = load_constants()
        self.original = {name: required(process.read(process.base + self.c[name], 32), name)
                         for name in ('kPacketBuildRva', 'kPacketWriteRva')}
        state, _ = process.chain([self.c['BaseOffset'], 0x348, 0x48, 0])
        protocol = required(process.q(required(state, 'state') + self.c['kPacketProtocolOff']), 'protocol')
        world = required(process.q(protocol + self.c['kPacketWorldOff']), 'world')
        player, _ = process.chain([0x5C0, 0], world)
        comps = components(process, required(player, 'player'), self.c)
        self.block = Block()
        self.block.magic, self.block.version, self.block.bytes = 0x50525443, 1, C.sizeof(Block)
        self.block.state, self.block.protocol, self.block.world = state, protocol, world
        self.block.entity = player
        self.block.positioned = required(comps.get('Positioned'), 'Positioned')
        self.block.stream = required(process.q(protocol + self.c['kPacketStreamOff']), 'stream')
        self.dll = dll.resolve()

    def load(self):
        path = str(self.dll)
        if path.lower() in self.remote.modules():
            raise RuntimeError('Capture DLL already loaded; finish previous capture first')
        arg = self.remote.allocate((path + '\0').encode('utf-16-le'))
        self.remote.call(self.remote.system_export('LoadLibraryW'), arg)
        self.remote.module = self.remote.modules().get(path.lower(), 0)
        if not self.remote.module:
            raise RuntimeError('Capture DLL did not load')
        pe = pefile.PE(path)
        export = next(e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name == b'ProbeRun')
        self.entry = self.remote.module + export
        self.request = self.remote.allocate(bytes(self.block))

    def command(self, operation):
        self.block.operation = operation
        self.block.status = self.block.exceptionCode = self.block.eventCount = 0
        self.block.hooksActive = 1  # Conservative if remote result cannot be read.
        raw = bytes(self.block)
        done = C.c_size_t()
        source = C.create_string_buffer(raw)
        if not self.p.k.WriteProcessMemory(self.remote.handle, self.request, source, len(raw), C.byref(done)) or done.value != len(raw):
            raise C.WinError(C.get_last_error())
        result = self.remote.call(self.entry, self.request)
        raw = self.p.read(self.request, C.sizeof(Block))
        if raw is None:
            raise RuntimeError('Capture response unreadable; retain module')
        self.block = Block.from_buffer_copy(raw)
        if result or self.block.status != 1:
            raise RuntimeError(f'Capture operation {operation} failed: {result}/{self.block.exceptionCode}; hooks={self.block.hooksActive}')
        return self.block

    def close(self):
        if self.remote.pending:
            self.remote.close()
            return False
        if self.started:
            self.command(3)
            self.started = False
        if self.block.hooksActive:
            # Never unload executable callback code still referenced by an entry hook.
            self.remote.pending = True
            self.remote.close()
            return False
        unloaded = self.remote.close()
        self.cleaned = unloaded and all(self.p.read(self.p.base + self.c[name], 32) == raw
                                        for name, raw in self.original.items())
        return self.cleaned


def event_json(event, base, image_size):
    if event.stackCount > 24 or event.objectLength > 256 or event.dataLength > 1024:
        raise RuntimeError('Invalid capture event bounds')
    result = {name: getattr(event, name) for name in (
        'sequence', 'tick', 'buildSequence', 'kind', 'threadId', 'opcode', 'objectLength',
        'dataLength', 'originalLength', 'flags')}
    result['observedAt'] = stamp()
    for name in ('caller', 'rcx', 'rdx', 'r8', 'r9', 'vtable', 'serializer', 'world', 'entity'):
        result[name] = hex(getattr(event, name))
    for name in ('caller', 'vtable', 'serializer'):
        value = getattr(event, name)
        result[name + 'Rva'] = hex(value - base) if base <= value < base + image_size else None
    result['opcodeHex'] = hex(event.opcode)
    result['playerPosition'] = [event.x, event.y]
    result['stack'] = [hex(value) for value in event.stack[:event.stackCount]]
    result['gameStackRvas'] = [hex(value - base) for value in event.stack[:event.stackCount]
                               if base <= value < base + image_size]
    result['objectHex'] = bytes(event.object[:event.objectLength]).hex(' ')
    result['dataHex'] = bytes(event.data[:event.dataLength]).hex(' ')
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pid', type=int, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--duration', type=float, default=600)
    ap.add_argument('--dll', type=Path, default=ROOT / '.build/tests/portal_summon/PortalCapture.dll')
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    p, capture = None, None
    events_count, changes, last_signature, last_portal = 0, 0, None, None
    report = {'startedAt': stamp(), 'pid': args.pid, 'captureOnly': True, 'gameRequestsIssued': 0,
              'dll': str(args.dll.resolve()), 'dllSha256': hashlib.sha256(args.dll.read_bytes()).hexdigest(),
              'durationSeconds': args.duration, 'hooksRemovedAndDllUnloaded': False}
    code = 1
    try:
        p = Process(args.pid)
        report['moduleBase'] = hex(p.base)
        c = load_constants()
        probe = entities.Probe(p, c, 120)
        before = probe.scan()
        output(args.out / 'entity_baseline.json', before)
        capture = Capture(p, args.dll)
        capture.load()
        capture.started = True  # Always attempt teardown if installation partially succeeds.
        capture.command(1)
        report['armedAt'] = stamp()
        report['context'] = {name: hex(getattr(capture.block, name)) for name in (
            'state', 'protocol', 'stream', 'world', 'entity', 'positioned')}
        report['originalEntryBytes'] = {name: raw.hex(' ') for name, raw in capture.original.items()}
        output(args.out / 'session.json', report)
        print('ARMED ' + report['armedAt'], flush=True)
        start, last_poll = time.monotonic(), 0
        with (args.out / 'packets.jsonl').open('a', encoding='utf-8', buffering=1) as packets, (
                args.out / 'entities.jsonl').open('a', encoding='utf-8', buffering=1) as entity_log:
            while time.monotonic() - start < args.duration and not (args.out / 'STOP').exists():
                block = capture.command(2)
                for event in block.events[:block.eventCount]:
                    packets.write(json.dumps(event_json(event, p.base, c['kGameImageSize']), ensure_ascii=False) + '\n')
                    events_count += 1
                if time.monotonic() - last_poll > 0.2:
                    try:
                        sample = probe.scan()
                        compact = {key: value for key, value in sample.items() if key != 'surroundings'}
                        entity_log.write(json.dumps(compact, ensure_ascii=False) + '\n')
                        current = entities.signature(sample)
                        if last_signature is not None and current != last_signature:
                            changes += 1
                            output(args.out / f'entity_change_{changes:03d}.json', sample)
                        last_signature, last_portal = current, compact
                    except (OSError, RuntimeError, ValueError) as exc:
                        entity_log.write(json.dumps({'capturedAt': stamp(), 'readError': str(exc)}) + '\n')
                    last_poll = time.monotonic()
                output(args.out / 'status.json', {'status': 'armed', 'updatedAt': stamp(),
                    'capturedEvents': events_count, 'totalEvents': block.totalEvents, 'dropped': block.dropped,
                    'portalChanges': changes, 'lastEntitySample': last_portal})
                if block.eventCount < 32:
                    time.sleep(0.15)
        final = capture.command(3)
        capture.started = False
        with (args.out / 'packets.jsonl').open('a', encoding='utf-8') as packets:
            while True:
                for event in final.events[:final.eventCount]:
                    packets.write(json.dumps(event_json(event, p.base, c['kGameImageSize']), ensure_ascii=False) + '\n')
                    events_count += 1
                if final.eventCount < 32:
                    break
                final = capture.command(2)
        report['totalEvents'] = final.totalEvents
        report['dropped'] = final.dropped
        code = 0
    except BaseException as exc:
        report['error'] = f'{type(exc).__name__}: {exc}'
        print(report['error'], flush=True)
    finally:
        if capture:
            try:
                report['hooksRemovedAndDllUnloaded'] = capture.close()
                if not report['hooksRemovedAndDllUnloaded']:
                    code = 1
            except Exception as exc:
                report['cleanupError'] = str(exc)
                code = 1
        if p:
            p.close()
        report.update(endedAt=stamp(), capturedEvents=events_count, portalChanges=changes, exitCode=code)
        output(args.out / 'ended.json', report)
        output(args.out / 'status.json', {'status': 'stopped' if code == 0 else 'error', **report})
        print(json.dumps(report, ensure_ascii=False), flush=True)
    return code


if __name__ == '__main__':
    raise SystemExit(main())
