"""Read-only ritual StateMachine and live world-label snapshots.

Research for build 0x6AFB7637. UI lookup searches the bounded native state
allocation and validates type, owner, parent and entity links; it does not use
the incidental state+0x718/0x2210 callback arguments as a production UI chain.
No injection, game calls, input events or process-memory writes.
"""
import argparse
import datetime
import json
from pathlib import Path
import re
import struct
import time

from ritual_state_audit import Process, components, load_constants, save, tree

# Investigation layout, checked against this build's native constructors and
# text update routine. These are research offsets, not production predicates.
STATE_ALLOCATION_SIZE = 0x8C70
RESEARCH_TIMESTAMP = 0x6AFB7637
RESEARCH_IMAGE_SIZE = 0x4CBB000
INGAME_UI_VTABLE = 0x317D200
WORLD_LAYER_VTABLE = 0x35ABB28
WORLD_LABELS_VTABLE = 0x315E010
ALTAR_UI_VTABLE = 0x315EDA8
UI_PARENT = 0xB8
UI_CHILDREN_BEGIN, UI_CHILDREN_END = 0x10, 0x18
SM_DEFINITION, SM_VALUES_BEGIN, SM_VALUES_END = 0x158, 0x160, 0x168
SM_KEYS_BEGIN, SM_KEYS_END, SM_KEY_STRIDE = 0x10, 0x18, 0xC0
ALTAR_ENTITY, ALTAR_TITLE, ALTAR_BODY = 0x2E0, 0x310, 0x318
TEXT_STRING = 0x360


def state_values(p, address):
    if not address:
        return {'valid': False, 'error': 'StateMachine component absent'}
    for _ in range(2):
        definition = p.q(address + SM_DEFINITION)
        begin = p.q(definition + SM_KEYS_BEGIN) if definition else None
        end = p.q(definition + SM_KEYS_END) if definition else None
        values = p.q(address + SM_VALUES_BEGIN)
        values_end = p.q(address + SM_VALUES_END)
        if begin is None or end is None or not values or values_end is None:
            return {'valid': False, 'error': 'unreadable state table'}
        if not 0 <= end - begin <= 256 * SM_KEY_STRIDE or (end - begin) % SM_KEY_STRIDE:
            return {'valid': False, 'error': 'invalid state definition vector'}
        count = (end - begin) // SM_KEY_STRIDE
        if values_end - values != count * 8:
            return {'valid': False, 'error': 'state key/value count mismatch'}
        raw = p.read(values, count * 8)
        if raw is None:
            return {'valid': False, 'error': 'unreadable state values'}
        result = {}
        for i in range(count):
            name = p.string(begin + i * SM_KEY_STRIDE, wide=False)
            if not name or not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]{0,127}', name) or name in result:
                return {'valid': False, 'error': 'invalid/duplicate state key'}
            result[name] = struct.unpack_from('<i', raw, i * 8)[0]
        if (p.q(address + SM_DEFINITION) == definition and
                p.q(definition + SM_KEYS_BEGIN) == begin and
                p.q(definition + SM_KEYS_END) == end and
                p.q(address + SM_VALUES_BEGIN) == values and
                p.q(address + SM_VALUES_END) == values_end and
                p.read(values, len(raw)) == raw):
            return {'valid': True, 'component': hex(address), 'values': result}
    return {'valid': False, 'error': 'state table changed during snapshot'}


def children(p, address):
    begin, end = p.q(address + UI_CHILDREN_BEGIN), p.q(address + UI_CHILDREN_END)
    if begin is None or end is None or not 0 <= end - begin <= 8192 or (end - begin) % 8:
        raise ValueError('invalid UI children vector at ' + hex(address))
    if begin == end:
        return []
    raw = p.read(begin, end - begin)
    if raw is None:
        raise ValueError('unreadable UI children at ' + hex(address))
    return [value[0] for value in struct.iter_unpack('<Q', raw)]


def ancestor_flags(p, address, stop=0, max_depth=24):
    result, seen = [], set()
    while address and address not in seen and len(result) < max_depth:
        seen.add(address)
        flags, parent = p.unpack(address + 0x168, '<I'), p.q(address + UI_PARENT)
        if flags is None or parent is None:
            break
        result.append({'address': hex(address), 'flags168': hex(flags),
                       'bit11': bool(flags & (1 << 11))})
        if address == stop:
            return result, True
        address = parent
    return result, False


def live_ui(p, state, protocol, entities):
    raw = p.read(state, STATE_ALLOCATION_SIZE)
    if raw is None:
        return {'complete': False, 'error': 'unreadable native state allocation'}
    candidates = set()
    for (value,) in struct.iter_unpack('<Q', raw):
        if (0x10000 <= value < 0x700000000000 and value % 8 == 0 and
                p.q(value) == p.base + INGAME_UI_VTABLE and
                p.q(value + 0x310) == state and p.q(value + 0x308) == protocol):
            candidates.add(value)
    if len(candidates) != 1:
        return {'complete': False, 'error': 'typed InGameUI lookup is missing/ambiguous',
                'candidates': [hex(x) for x in sorted(candidates)]}
    root = next(iter(candidates))
    parent = p.q(root + UI_PARENT)
    result = {'complete': False, 'inGameUI': hex(root),
              'lookup': 'bounded state references with vtable and owner checks', 'labels': []}
    try:
        if not parent or root not in children(p, parent):
            raise ValueError('InGameUI parent/children mismatch')
        def unique_child(parent_address, vtable_rva):
            matches = [child for child in children(p, parent_address)
                       if p.q(child) == p.base + vtable_rva and
                       p.q(child + UI_PARENT) == parent_address]
            if len(matches) != 1:
                raise ValueError('missing/ambiguous world-label UI branch')
            return matches[0]
        # The inventory and all cached panels can exceed 8192 elements. Inspect
        # only the typed world-label branch, not every hidden in-game panel.
        world_layer = unique_child(root, WORLD_LAYER_VTABLE)
        labels_root = unique_child(world_layer, WORLD_LABELS_VTABLE)
        result['worldLabels'] = hex(labels_root)
        pending, seen = [labels_root], set()
        while pending:
            address = pending.pop()
            if not address or address in seen or len(seen) >= 8192:
                raise ValueError('invalid/cyclic/oversized UI tree')
            seen.add(address)
            vtable = p.q(address)
            if vtable is None or not p.base <= vtable < p.base + 0x4CBB000:
                raise ValueError('invalid UI vtable at ' + hex(address))
            if vtable == p.base + ALTAR_UI_VTABLE:
                entity = p.q(address + ALTAR_ENTITY)
                row = entities.get(entity)
                if not row or not row['metadata'].endswith('/RitualRuneInteractable'):
                    raise ValueError('altar UI is not bound to a current ritual entity')
                label = {'address': hex(address), 'entity': hex(entity), 'position': row['position']}
                for name, offset in [('title', ALTAR_TITLE), ('body', ALTAR_BODY)]:
                    text_ui = p.q(address + offset)
                    if not text_ui:
                        raise ValueError('missing ' + name + ' UI')
                    flags, linked = ancestor_flags(p, text_ui, address)
                    text = p.string(text_ui + TEXT_STRING)
                    if not linked or not text:
                        raise ValueError('unreadable/unlinked ' + name + ' UI')
                    label[name] = {'text': text, 'address': hex(text_ui), 'ancestors': flags}
                label['altarAncestors'], linked = ancestor_flags(p, address, root)
                if not linked:
                    raise ValueError('altar parent chain does not reach InGameUI')
                label['ui344to346Raw'] = list(p.read(address + 0x344, 3) or b'')
                result['labels'].append(label)
            for child in children(p, address):
                if p.q(child + UI_PARENT) != address:
                    raise ValueError('UI parent/children mismatch at ' + hex(child))
                pending.append(child)
        result.update(complete=True, visited=len(seen))
    except ValueError as error:
        result['error'] = str(error)
    return result


def capture(p, c):
    state, _ = p.chain([c['BaseOffset'], 0x348, 0x48, 0])
    if not state:
        raise ValueError('native state unavailable')
    protocol = p.q(state + 0x1A8)
    world = p.q(protocol + 0x20F0) if protocol else None
    scan, entity_list = tree(p, p.q(world + 0x718) if world else None, False, c)
    rows = {}
    for address in entity_list:
        holder = p.q(address + c['kMetadataPathOff0']) if address else None
        metadata = p.string(holder + c['kMetadataPathOff1']) if holder else None
        if not metadata or '/Leagues/Ritual/' not in metadata:
            continue
        comps = components(p, address, c)
        position = comps.get('Positioned')
        raw = p.read(position + c['kPositioned_EntXOff'], 8) if position else None
        rows[address] = {'entity': hex(address), 'metadata': metadata,
                         'position': list(struct.unpack('<ii', raw)) if raw else None,
                         'stateMachine': state_values(p, comps.get('StateMachine'))}
    result = {'createdAt': datetime.datetime.now().astimezone().isoformat(),
              'state': hex(state), 'world': hex(world or 0), 'tree': scan,
              'ritualEntities': list(rows.values()), 'ui': live_ui(p, state, protocol, rows)}
    state_after, _ = p.chain([c['BaseOffset'], 0x348, 0x48, 0])
    world_after = p.chain([0x1A8, 0x20F0], state_after)[0] if state_after else None
    result['sameStateAndWorld'] = state_after == state and world_after == world
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pid', type=int, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--watch-seconds', type=int, default=0,
                    help='Collect changes for up to 300 seconds; no game actions')
    args = ap.parse_args()
    if not 0 <= args.watch_seconds <= 300:
        ap.error('--watch-seconds must be 0..300')
    p, c = Process(args.pid), load_constants()
    try:
        nt = p.unpack(p.base + 0x3C, '<I')
        if (nt is None or c['kGameTimeDateStamp'] != RESEARCH_TIMESTAMP or
                p.unpack(p.base + nt + 8, '<I') != RESEARCH_TIMESTAMP or
                p.unpack(p.base + nt + 0x50, '<I') != RESEARCH_IMAGE_SIZE):
            raise ValueError('game build mismatch')
        data = {'readOnly': True, 'pid': p.pid, 'moduleBase': hex(p.base),
                'buildTimestamp': hex(c['kGameTimeDateStamp']), 'samples': []}
        deadline, signature, count = time.monotonic() + args.watch_seconds, None, 0
        while True:
            current = capture(p, c)
            # Nearby unrelated effects change the tree count every few frames.
            # Record semantic ritual changes and validity, not that noise.
            comparable = {
                'state': current['state'], 'world': current['world'],
                'treeValid': current['tree']['valid'],
                'sameStateAndWorld': current['sameStateAndWorld'],
                'ritualEntities': current['ritualEntities'],
                'ui': {k: v for k, v in current['ui'].items() if k != 'visited'},
            }
            new_signature = json.dumps(comparable, ensure_ascii=False, sort_keys=True)
            count += 1
            if new_signature != signature:
                data['samples'].append(current)
                signature = new_signature
                data['captureCount'] = count
                save(args.out, data)
                summary = {
                    'time': current['createdAt'], 'treeValid': current['tree']['valid'],
                    'uiComplete': current['ui']['complete'],
                    'states': [{'entity': row['entity'], 'position': row['position'],
                                **row['stateMachine']} for row in current['ritualEntities']],
                    'labels': [{'entity': row['entity'], 'title': row['title']['text'],
                                'body': row['body']['text']} for row in current['ui']['labels']],
                }
                print(json.dumps(summary, ensure_ascii=False), flush=True)
            if time.monotonic() >= deadline:
                break
            time.sleep(1)
        data['captureCount'] = count
        save(args.out, data)
        print(f'COMPLETE captures={count} changes={len(data["samples"])} out={args.out}', flush=True)
    finally:
        p.close()


if __name__ == '__main__':
    main()
