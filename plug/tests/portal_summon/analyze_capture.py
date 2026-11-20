"""Extract complete D0 serializer writes and correlate them with new portals."""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from offset_audit import Process
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--pid', type=int)
    args = ap.parse_args()
    folder = args.input.resolve()
    rows = [json.loads(line) for line in (folder / 'packets.jsonl').read_text(encoding='utf-8').splitlines() if line.strip()]
    session = json.loads((folder / 'session.json').read_text(encoding='utf-8'))
    ended = json.loads((folder / 'ended.json').read_text(encoding='utf-8'))
    baseline = json.loads((folder / 'entity_baseline.json').read_text(encoding='utf-8'))
    seen = {(p['key'], p['entity']) for p in baseline['portals']}
    new_portals = []
    for path in sorted(folder.glob('entity_change_*.json')):
        snapshot = json.loads(path.read_text(encoding='utf-8'))
        for portal in snapshot['portals']:
            ident = portal['key'], portal['entity']
            if ident not in seen:
                seen.add(ident)
                if portal['hideoutName'] and portal['portalMetadata'] and portal['nearPlayer']:
                    new_portals.append({'capturedAt': snapshot['capturedAt'],
                                        'player': snapshot['player'], 'sourceFile': path.name, **portal})
    matched = []
    for build in (r for r in rows if r['kind'] == 1 and r['opcode'] == 0xD0):
        writes = sorted((r for r in rows if r['kind'] == 2 and r['buildSequence'] == build['sequence']), key=lambda r: r['sequence'])
        wire = b''.join(bytes.fromhex(r['dataHex']) for r in writes)
        obj = bytes.fromhex(build['objectHex'])
        later = [p for p in new_portals if p['capturedAt'] >= build['observedAt']]
        portal = min(later, key=lambda p: p['capturedAt']) if later else None
        matched.append({'build': build, 'writes': writes, 'serializedHex': wire.hex(' '),
                        'completeFragments': bool(writes) and all(not r['flags'] and r['dataLength'] == r['originalLength'] for r in writes),
                        'objectFields': {'opcode': hex(struct.unpack_from('<H', obj, 8)[0]), 'sequence': obj[0x10]},
                        'newPortal': portal,
                        'observationDelaySeconds': round((dt.datetime.fromisoformat(portal['capturedAt']) -
                            dt.datetime.fromisoformat(build['observedAt'])).total_seconds(), 3) if portal else None})
    report = {'createdAt': dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).isoformat(),
              'captureSession': session, 'captureEnd': ended, 'sourceDirectory': str(folder),
              'captureScope': 'Original user-generated game requests only. No replay or native summon invocation.',
              'summonRequests': matched, 'count': len(matched), 'newPortalCount': len(new_portals),
              'allWiresEqual': len({r['serializedHex'] for r in matched}) == 1,
              'allFragmentsComplete': bool(matched) and all(r['completeFragments'] for r in matched),
              'nativeSummonRva': '0x21236f0', 'packetBuildRva': '0x212f0f0', 'packetSendRva': '0x21a8560',
              'sequenceSource': 'uint8 at current state + 0x181; value 1 in this capture, not a constant',
              'objectLayout': {'size': '0x18', 'vtable': 'uint64 at +0', 'opcode': 'uint16 at +8', 'sequence': 'uint8 at +0x10'},
              'disconnect': {'loggedAt': '2026-11-19T02:40:11+08:00', 'message': 'Abnormal disconnect: 發生無法預期的斷線。',
                             'processTerminated': False, 'causality': 'Undetermined; hook overhead cannot be excluded.'}}
    if args.pid:
        p = Process(args.pid)
        try:
            pe = pefile.PE(str(p.path), fast_load=True)
            md = Cs(CS_ARCH_X86, CS_MODE_64)
            code = []
            for rva, size in [(0x21236F0, 0x8D), (0x1F4C130, 0x58), (0x212F0F0, 0xB3)]:
                disk, live = pe.get_data(rva, size), p.read(p.base + rva, size)
                if live != disk:
                    raise RuntimeError(f'Live/disk code mismatch at {rva:#x}')
                code.append({'rva': hex(rva), 'size': size, 'liveEqualsDisk': True,
                             'sha256': hashlib.sha256(live).hexdigest(), 'bytes': live.hex(' '),
                             'instructions': [f'{i.address:#x}: {i.mnemonic} {i.op_str}' for i in md.disasm(live, rva)]})
            report['codeEvidence'] = code
        finally:
            p.close()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: report[key] for key in ['count', 'newPortalCount', 'allWiresEqual', 'allFragmentsComplete', 'nativeSummonRva']}, indent=2))
    for row in matched:
        print(row['build']['observedAt'], row['serializedHex'], row['newPortal']['key'] if row['newPortal'] else None,
              row['observationDelaySeconds'])


if __name__ == '__main__':
    main()
