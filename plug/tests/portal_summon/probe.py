"""Independent read-only portal summon observation; not part of Poe2Study.sln."""
import argparse
import datetime as dt
import json
import math
from pathlib import Path
import struct
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from offset_audit import Process, components, load_constants


def now():
    return dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).isoformat(timespec='milliseconds')


def save(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


class Probe:
    def __init__(self, process, constants, radius):
        self.p, self.c, self.radius = process, constants, radius
        self.cache = {}
        self.world = None
        nt = self.p.unpack(self.p.base + 0x3c, '<I')
        if not nt or (self.p.unpack(self.p.base + nt + 8, '<I'),
                      self.p.unpack(self.p.base + nt + 0x50, '<I')) != (
                          constants['kGameTimeDateStamp'], constants['kGameImageSize']):
            raise RuntimeError('Live game build does not match GameOffsets.h')

    def position(self, comps):
        address = comps.get('Positioned')
        raw = self.p.read(address + self.c['kPositioned_EntXOff'], 8) if address else None
        return list(struct.unpack('<ii', raw)) if raw else None

    def describe(self, key, entity):
        identity = (key, entity)
        if identity not in self.cache:
            holder = self.p.q(entity + self.c['kMetadataPathOff0'])
            metadata = self.p.string(holder + self.c['kMetadataPathOff1']) if holder else None
            comps = components(self.p, entity, self.c)
            render = comps.get('Render')
            name = self.p.string(render + self.c['kRender_NameOff']) if render else None
            result = {'key': hex(key), 'entity': hex(entity), 'metadata': metadata,
                      'name': name, 'components': comps}
            if metadata and comps:
                self.cache[identity] = result
            return result
        return self.cache[identity]

    def scan(self):
        p, c = self.p, self.c
        state, _ = p.chain([c['BaseOffset'], 0x348, 0x48, 0])
        world, _ = p.chain([0x1a8, 0x20f0], state) if state else (None, [])
        if not world:
            raise RuntimeError('World unavailable')
        if world != self.world:
            self.cache.clear()
            self.world = world
        player, _ = p.chain([0x5c0, 0], world)
        player_comps = components(p, player, c) if player else {}
        player_pos = self.position(player_comps)
        if player_pos is None:
            raise RuntimeError('Player position unavailable')
        head = p.q(world + 0x718)
        if not head or p.unpack(head + c['kNodeFlagOff'], '<B') != 1:
            raise RuntimeError('Surroundings sentinel unavailable')
        root = p.q(head + c['kNodeParentOff'])
        pending, visited, rows = [root], set(), []
        while pending:
            node = pending.pop()
            if node == head:
                continue
            if not node or node in visited or len(visited) >= 8192:
                raise RuntimeError('Invalid/cyclic surroundings tree')
            visited.add(node)
            raw = p.read(node, 0x30)
            if not raw or raw[c['kNodeFlagOff']] != 0:
                raise RuntimeError('Tree node unreadable or changed')
            key = struct.unpack_from('<I', raw, c['kNodeKeyOff'])[0]
            entity = struct.unpack_from('<Q', raw, c['kNodeEntityOff'])[0]
            if not entity:
                raise RuntimeError('Entity unavailable')
            row = dict(self.describe(key, entity))
            comps = row.pop('components')
            metadata = (row['metadata'] or '').lower()
            portal = 'portal' in metadata
            # Refresh render names: newly created portals may initially have no destination.
            if portal or '藏身處' in (row['name'] or ''):
                render = comps.get('Render')
                row['name'] = p.string(render + c['kRender_NameOff']) if render else None
            row['position'] = self.position(comps)
            row['distance'] = round(math.dist(player_pos, row['position']), 3) if row['position'] else None
            row['portalMetadata'] = portal
            row['hideoutName'] = '藏身處' in (row['name'] or '')
            row['nearPlayer'] = row['distance'] is not None and row['distance'] <= self.radius
            if portal or row['hideoutName']:
                row['componentAddresses'] = {name: hex(address) for name, address in comps.items()}
            check = p.read(node + c['kNodeKeyOff'], 16)
            if not check or struct.unpack_from('<I', check)[0] != key or struct.unpack_from('<Q', check, 8)[0] != entity:
                raise RuntimeError('Entity identity changed during scan')
            rows.append(row)
            pending.extend(struct.unpack_from('<Q', raw, offset)[0] for offset in (
                c['kNodeLeftOff'], c['kNodeRightOff']))
        checked_state, _ = p.chain([c['BaseOffset'], 0x348, 0x48, 0])
        checked_world, _ = p.chain([0x1a8, 0x20f0], checked_state) if checked_state else (None, [])
        checked_player, _ = p.chain([0x5c0, 0], checked_world) if checked_world else (None, [])
        if (checked_world != world or checked_player != player or p.q(world + 0x718) != head or
                p.q(head + c['kNodeParentOff']) != root):
            raise RuntimeError('Scene changed during scan')
        live_identities = {(int(r['key'], 16), int(r['entity'], 16)) for r in rows}
        self.cache = {identity: row for identity, row in self.cache.items() if identity in live_identities}
        portals = sorted((r for r in rows if r['portalMetadata'] or r['hideoutName']),
                         key=lambda r: r['distance'] if r['distance'] is not None else float('inf'))
        matches = [r for r in portals if r['portalMetadata'] and r['hideoutName'] and r['nearPlayer']]
        player_component = player_comps.get('Player')
        result = {'capturedAt': now(), 'world': hex(world), 'state': hex(state),
                  'player': {'entity': hex(player), 'position': player_pos,
                             'name': p.string(player_component + c['kPlayer_NameOff']) if player_component else None},
                  'currentSkillId': p.unpack(player_component + c['kIdSkillCurrentIdOff'], '<I') if player_component else None,
                  'tree': {'sentinel': hex(head), 'root': hex(root), 'count': len(rows)},
                  'portals': portals, 'matches': matches, 'surroundings': rows}
        return result

    def samples(self, portals):
        result = []
        for portal in portals:
            row = {'key': portal['key'], 'entity': portal['entity'], 'samples': {}}
            for name, size in [('Transitionable', 0x180), ('Targetable', 0x100),
                               ('InteractionAction', 0x180), ('StateMachine', 0x240),
                               ('Positioned', 0x460), ('Render', 0x200)]:
                address = portal.get('componentAddresses', {}).get(name)
                raw = self.p.read(int(address, 16), size) if address else None
                if raw:
                    row['samples'][name] = {'address': address, 'hex': raw.hex(' ')}
            result.append(row)
        return result


def signature(snapshot):
    return (snapshot['world'], tuple((r['key'], r['entity'], r['name'], tuple(r['position'] or []))
                                    for r in sorted(snapshot['portals'], key=lambda r: r['key'])))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pid', type=int, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--interval', type=float, default=0.1)
    ap.add_argument('--duration', type=float, default=1800)
    ap.add_argument('--radius', type=float, default=120)
    ap.add_argument('--once', action='store_true')
    args = ap.parse_args()
    if args.interval < 0.05 or args.duration <= 0 or args.radius <= 0:
        ap.error('interval >= 0.05, duration > 0, radius > 0 required')
    args.out.mkdir(parents=True, exist_ok=True)
    p = Process(args.pid)
    try:
        probe = Probe(p, load_constants(), args.radius)
        session = {'startedAt': now(), 'pid': p.pid, 'module': str(p.path), 'moduleBase': hex(p.base),
                   'readOnly': True, 'intervalSeconds': args.interval, 'durationSeconds': args.duration,
                   'nearRadiusWorldPixels': args.radius,
                   'matchRule': 'portal metadata AND Render name contains 藏身處 AND distance <= radius',
                   'note': 'Polling observations only; portal changes do not prove an exact button-click count.'}
        save(args.out / 'session.json', session)
        begin, last_heartbeat, previous, previous_skill = time.monotonic(), 0, None, None
        count, changes, errors = 0, 0, 0
        status = 'starting'
        with (args.out / 'observations.jsonl').open('a', encoding='utf-8', buffering=1) as log:
            while time.monotonic() - begin < args.duration and not (args.out / 'STOP').exists():
                cycle = time.monotonic()
                try:
                    snapshot = probe.scan()
                    count += 1
                    current = signature(snapshot)
                    changed = previous is not None and current != previous
                    initial = previous is None
                    skill_changed = snapshot['currentSkillId'] != previous_skill
                    if initial or changed:
                        if changed:
                            changes += 1
                        event = {'type': 'baseline' if initial else 'portal_change', 'changeIndex': changes,
                                 **snapshot, 'componentSamples': probe.samples(snapshot['portals'])}
                        save(args.out / ('baseline.json' if initial else f'change_{changes:03d}.json'), event)
                        log.write(json.dumps(event, ensure_ascii=False) + '\n')
                        print(json.dumps({k: event[k] for k in ['type', 'capturedAt', 'changeIndex', 'player', 'portals']}, ensure_ascii=False), flush=True)
                    if initial or changed or skill_changed or cycle - last_heartbeat >= 1:
                        log.write(json.dumps({'type': 'observation', **{k: v for k, v in snapshot.items() if k != 'surroundings'}}, ensure_ascii=False) + '\n')
                        last_heartbeat = cycle
                    previous, previous_skill = current, snapshot['currentSkillId']
                    status = 'armed'
                    save(args.out / 'status.json', {'status': status, 'updatedAt': now(), 'samples': count,
                         'portalChanges': changes, 'errors': errors,
                         **{k: snapshot[k] for k in ['player', 'tree', 'portals', 'matches', 'currentSkillId']}})
                    if args.once:
                        break
                except (OSError, RuntimeError, ValueError) as exc:
                    errors += 1
                    status = 'waiting_for_valid_scene'
                    log.write(json.dumps({'type': 'read_error', 'capturedAt': now(), 'error': str(exc)}, ensure_ascii=False) + '\n')
                    save(args.out / 'status.json', {'status': status, 'updatedAt': now(), 'samples': count,
                         'portalChanges': changes, 'errors': errors, 'lastError': str(exc)})
                    if args.once:
                        raise
                time.sleep(max(0, args.interval - (time.monotonic() - cycle)))
        save(args.out / 'ended.json', {'endedAt': now(), 'samples': count, 'portalChanges': changes,
                                     'errors': errors, 'reason': 'once' if args.once else 'stop_or_duration'})
    finally:
        p.close()


if __name__ == '__main__':
    main()
