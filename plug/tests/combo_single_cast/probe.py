"""One production IdSkillCaster Begin/Release, bounded observation, then cleanup.

Imports only the existing diagnostic host's read/transport helpers. No production
Monitor is replaced. --cast explicitly enables the single call; no retry.
"""
import argparse
import ctypes as C
import datetime as dt
import hashlib
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests' / 'skill_id_call'))
import probe as host


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pid', required=True, type=int)
    ap.add_argument('--cast', action='store_true')
    ap.add_argument('--label', default='inspect')
    ap.add_argument('--hold-ms', type=int, choices=(0, 3000), default=0)
    ap.add_argument('--dodge', action='store_true', help='Use the confirmed DodgeRoll operation, never Begin')
    ap.add_argument('--dx', type=int, default=40)
    ap.add_argument('--dy', type=int, default=0)
    args = ap.parse_args()
    if args.dodge and args.hold_ms:
        ap.error('DodgeRoll requires zero hold time')
    out = ROOT / '.build' / 'tests' / 'combo_single_cast'
    out.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime('%Y%m%d%H%M%S%f')
    path = out / f'{stamp}.json'
    report = {'label': args.label, 'pid': args.pid, 'castRequested': args.cast, 'dodge': args.dodge,
              'productionCasterSha256': hashlib.sha256((ROOT / 'Monitor' / 'IdSkillCaster.cpp').read_bytes()).hexdigest(),
              'productionArgumentsSha256': hashlib.sha256((ROOT / 'Monitor' / 'IdSkillCaster.h').read_bytes()).hexdigest()}
    p = None
    code = 1
    try:
        p = host.Process(args.pid)
        constants = host.load_constants()
        def inspect_current():
            return host.inspect(p, constants['kIdSkillCallRva'], constants['kSkillManagerGetterRva'])
        basis = inspect_current()
        report['basis'] = basis
        pl = basis['components']['Player']
        ctx = basis['context']
        report['gate669'] = host.strict(p, host.strict(p, ctx + 0x1A0) + 0x669, '<B')
        report['pendingBefore'] = host.strict(p, pl + 0x214, '<I')
        report['code'] = host.code_evidence(p, [(constants['kIdSkillCallRva'], 0x209),
            (constants['kSkillManagerGetterRva'], 8), (constants['kSkillStopRva'], 0x7E)])
        skill_name = 'DodgeRollPlayer' if args.dodge else 'OilGrenadePlayer'
        skills = [s for s in basis['skills'] if s['internalId'] == skill_name and
                  (not args.dodge or s['id'] == constants['kDodgeRollSkillId'])]
        if len(skills) != 1:
            raise RuntimeError(f'Exactly one confirmed currently owned {skill_name} ID is required')
        report['skillIdHex'] = skills[0]['idHex']
        if not args.cast:
            report['readOnly'] = True
        else:
            block = host.Block()
            block.magic, block.version, block.bytes = 0x44494B53, 1, C.sizeof(block)
            block.cast, block.skillId = 1, skills[0]['id']
            block.expectedEntity, block.expectedContext = basis['entity'], ctx
            for name in ('actor', 'player', 'life', 'positioned'):
                setattr(block, name, basis['components'][name.capitalize()])
            block.x, block.y = basis['position'][0] + args.dx, basis['position'][1] + args.dy
            block.holdMs, block.observeMs = args.hold_ms, 3400
            remote = host.Remote(p)
            report['temporaryDllUnloaded'] = False
            try:
                result = remote.run(block, out / 'ComboSingleCastProbe.dll')
                report['result'] = {name: getattr(result, name) for name in
                    ('status', 'exceptionCode', 'castReturned', 'stopReturned', 'sampleCount')}
                report['result']['detail'] = result.error.decode('utf-8', errors='replace')
                if result.sampleCount > 192:
                    raise RuntimeError('Invalid sample count')
                samples = [{n: getattr(s, n) for n, _ in host.Sample._fields_}
                           for s in result.samples[:result.sampleCount]]
                report['samples'] = samples
                drops = []
                for before, after in zip(samples, samples[1:]):
                    if before['mana'] - after['mana'] >= 20:
                        drops.append({'ms': after['tick'] - samples[1]['tick'],
                                      'before': before['mana'], 'after': after['mana']})
                report['manaDropsOver20'] = drops
                if result.status != 1:
                    raise RuntimeError(report['result']['detail'])
            finally:
                report['temporaryDllUnloaded'] = remote.close()
            if not report['temporaryDllUnloaded']:
                raise RuntimeError('Temporary probe DLL not confirmed unloaded')
        report['after'] = inspect_current()
        report['positionBefore'] = basis['position']
        report['positionAfter'] = report['after']['position']
        report['pendingAfter'] = host.strict(p, pl + 0x214, '<I')
        code = 0
    except Exception as ex:
        report['error'] = f'{type(ex).__name__}: {ex}'
    finally:
        if p:
            p.close()
        report['exitCode'] = code
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k not in ('basis', 'after', 'samples', 'code')},
                     ensure_ascii=False, indent=2))
    print('RESULT', path)
    return code


if __name__ == '__main__':
    raise SystemExit(main())
