"""Independent native summon call. Default reads only; --load-check issues no request.

--cast performs exactly one native call, then observes a new near-player hideout
portal. It never sends captured bytes, retries, clicks, or falls back to raw packets.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import time

import capture_host as host


def summon_signature():
    source = (host.ROOT / 'Common/GameOffsets.h').read_text(encoding='utf-8-sig')
    match = re.search(r'kPortalSummonSignature\[\]\s*=\s*\{([^}]+)\}', source)
    if not match:
        raise RuntimeError('Shared native summon fingerprint is missing')
    expected = bytes(int(value, 16) for value in re.findall(r'0x[0-9A-Fa-f]+', match.group(1)))
    if len(expected) != 0x8D:
        raise RuntimeError('Shared native summon fingerprint is incomplete')
    return expected


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pid', type=int, required=True)
    ap.add_argument('--out', type=Path, required=True)
    action = ap.add_mutually_exclusive_group()
    action.add_argument('--load-check', action='store_true')
    action.add_argument('--cast', action='store_true')
    args = ap.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    report = {'startedAt': host.stamp(), 'pid': args.pid,
              'mode': 'cast' if args.cast else 'load-check' if args.load_check else 'read-only',
              'nativeCallsAttempted': 0, 'portalObserved': False,
              'temporaryDllUnloaded': None, 'hooksInstalled': False, 'rawPacketReplay': False}
    process, capture = None, None
    code = 1
    try:
        process = host.Process(args.pid)
        constants = host.load_constants()
        probe = host.entities.Probe(process, constants, 120)
        expected = summon_signature()
        report['nativeRva'] = hex(constants['kPortalSummonRva'])
        native = process.read(process.base + constants['kPortalSummonRva'], len(expected))
        if native != expected:
            raise RuntimeError('Whole native summon function fingerprint mismatch')
        report['nativeFingerprintSha256'] = hashlib.sha256(native).hexdigest()
        before = probe.scan()
        report['before'] = before
        previous = before
        for _ in range(3):
            time.sleep(0.2)
            sample = probe.scan()
            if (sample['world'] != before['world'] or sample['player'] != previous['player'] or
                    sample['currentSkillId'] != 0):
                raise RuntimeError('Player must be idle and stationary in the same scene')
            previous = sample
        comps = host.components(process, int(before['player']['entity'], 16), constants)
        life = comps.get('Life')
        hp = process.unpack(life + constants['kLife_HpOff'], '<I') if life else None
        if hp is None or hp <= 0:
            raise RuntimeError('Player must be alive')
        state = int(before['state'], 16)
        report['sequenceAtInspection'] = process.unpack(state + constants['kPacketSequenceOff'], '<B')
        report['summonGate8c08'] = process.unpack(state + constants['kPortalSummonGateOff'], '<B')
        if report['summonGate8c08'] != 0:
            raise RuntimeError('Native summon state gate is active')
        if args.load_check or args.cast:
            dll = host.ROOT / '.build/tests/portal_summon/PortalSummonCall.dll'
            report['dllSha256'] = hashlib.sha256(dll.read_bytes()).hexdigest()
            capture = host.Capture(process, dll)
            capture.load()
            if args.cast:
                report['nativeCallsAttempted'] = 1  # Conservative if remote invocation times out.
            block = capture.command(4 if args.cast else 0)
            report['returned'] = block.status == 1
            report['callTick'] = block.events[0].tick if block.eventCount else None
            report['exceptionCode'] = block.exceptionCode
            if args.cast:
                baseline_ids = {(p['key'], p['entity']) for p in before['portals']}
                report['observations'] = []
                deadline = time.monotonic() + 12
                while time.monotonic() < deadline:
                    sample = probe.scan()
                    report['observations'].append({key: value for key, value in sample.items() if key != 'surroundings'})
                    if sample['world'] != before['world'] or sample['player']['entity'] != before['player']['entity']:
                        raise RuntimeError('Scene changed after call; no retry')
                    found = [p for p in sample['matches'] if (p['key'], p['entity']) not in baseline_ids]
                    if found:
                        report['portalObserved'] = True
                        report['newPortals'] = found
                        report['after'] = sample
                        break
                    time.sleep(0.15)
                if not report['portalObserved']:
                    raise RuntimeError('Call returned but no new near-player hideout portal observed; no retry')
        code = 0
    except BaseException as exc:
        report['error'] = f'{type(exc).__name__}: {exc}'
    finally:
        if capture:
            try:
                report['temporaryDllUnloaded'] = capture.close()
                if not report['temporaryDllUnloaded']:
                    code = 1
            except Exception as exc:
                report['cleanupError'] = str(exc)
                code = 1
        if process:
            process.close()
        report['endedAt'] = host.stamp()
        report['exitCode'] = code
        host.output(args.out, report)
    print(json.dumps({key: value for key, value in report.items() if key not in ('before', 'after', 'observations')}, ensure_ascii=False, indent=2))
    return code


if __name__ == '__main__':
    raise SystemExit(main())
