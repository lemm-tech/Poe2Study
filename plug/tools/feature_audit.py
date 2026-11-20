"""Read-only audit of all current feature chains, including packet and party data.

Requires Windows x64 Python, pefile and capstone. Never writes target memory,
injects, installs hooks, sends packets, or calls any game function.
"""
import argparse
import ctypes
import datetime
import hashlib
import json
from pathlib import Path
import re
import struct

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP
from offset_audit import Process, load_constants, components, tree, verify
from party_audit import PartyReader, ReadError


def name_parts(reader, mods, c):
    bounds, data = reader.vector(mods + c['kMods_VectorOff'], c['kMods_PrefixEntryStride'], 64)
    if len(data) > c['kMods_MaxNameParts'] * c['kMods_PrefixEntryStride']:
        raise ReadError('Too many item name fragments')
    parts = []
    for offset in range(0, len(data), c['kMods_PrefixEntryStride']):
        record = struct.unpack_from('<Q', data, offset)[0]
        parts.append(reader.utf16z(reader.number(record + c['kMods_PrefixNameOff'])))
    return parts


def audit(p):
    data = p.path.read_bytes()
    pe = pefile.PE(data=data, fast_load=True)
    c = load_constants()
    source = (Path(__file__).parents[1] / 'Common/GameOffsets.h').read_text(encoding='utf-8-sig')
    expected_hash = re.search(r'kGameFileSha256\s*=\s*"([a-fA-F0-9]+)"', source).group(1)
    sha = hashlib.sha256(data).hexdigest()
    reader = PartyReader(p)
    nt = p.base + reader.number(p.base + 0x3C, '<I')
    stamp, size = reader.number(nt + 8, '<I'), reader.number(nt + 0x50, '<I')
    if (sha != expected_hash or stamp != c['kGameTimeDateStamp'] or size != c['kGameImageSize'] or
            pe.FILE_HEADER.TimeDateStamp != stamp or pe.OPTIONAL_HEADER.SizeOfImage != size):
        raise ReadError('Disk/live game identity does not match the calibrated build')
    result = {
        'capturedAt': datetime.datetime.now().astimezone().isoformat(),
        'readOnly': True, 'pid': p.pid, 'module': str(p.path), 'base': hex(p.base),
        'sha256': sha, 'timestamp': hex(stamp), 'imageSize': hex(size),
        'core': verify(p, c, pe), 'checks': {},
        'scope': 'Pointer/data and disk/live code inspection only; no UI launch, game actions, hooks, injection or runtime tests.'
    }
    checks = result['checks']
    entries = {}
    for name, rva_name, sig_name in [
        ('idSkill', 'kIdSkillCallRva', 'kIdSkillCallSignature'),
        ('skillManager', 'kSkillManagerGetterRva', 'kSkillManagerGetterSignature'),
        ('packetBuild', 'kPacketBuildRva', 'kPacketBuildSignature'),
        ('packetSend', 'kPacketSendRva', 'kPacketSendSignature'),
        ('packetWrite', 'kPacketWriteRva', 'kPacketIoSignature'),
        ('packetRead', 'kPacketReadRva', 'kPacketIoSignature'),
        ('packetSerialize', 'kPacketSerializeRva', 'kPacketSerializeSignature'),
        ('packetEndSerialize', 'kPacketEndSerializeRva', 'kPacketEndSerializeSignature'),
        ('skillIdRead', 'kIdInstructionRva', 'kIdInstruction')
    ]:
        match = re.search(r'\b' + sig_name + r'\[\]\s*=\s*\{([^}]+)\}', source)
        expected = bytes(int(v, 0) for v in re.findall(r'0x[\da-fA-F]+|\b\d+\b', match.group(1)))
        rva = c[rva_name]
        live = reader.raw(p.base + rva, len(expected))
        disk = pe.get_data(rva, len(expected))
        entries[name] = {'pass': live == disk == expected, 'rva': hex(rva),
                         'bytes': live.hex(' '), 'precedingByte': pe.get_data(rva - 1, 1).hex()}
    checks['entries'] = {'pass': all(v['pass'] for v in entries.values()), 'functions': entries}

    initial_global = reader.number(p.base + c['BaseOffset'])
    state, _ = reader.follow(p.base, [c['BaseOffset'], c['kSkillStopChainOff1'], c['kSkillStopChainOff2'], 0])
    owner = reader.number(state + c['kPacketProtocolOff'])
    world = reader.number(owner + c['kPacketWorldOff'])
    stream = reader.number(owner + c['kPacketStreamOff'])
    stream_fields = {name: reader.number(stream + c[key], fmt) for name, key, fmt in [
        ('lockEnabled', 'kPacketLockEnabledOff', '<B'), ('sendSize', 'kPacketSendSizeOff', '<Q'),
        ('sendConsumed', 'kPacketSendConsumedOff', '<Q'), ('sendBuffer', 'kPacketSendBufferOff', '<Q'),
        ('receiveError', 'kPacketRecvErrorOff', '<B'), ('receivePosition', 'kPacketRecvPositionOff', '<Q')]}
    checks['packetContext'] = {
        'pass': bool(p.read(stream + c['kPacketLockOff'], 8) and
                     stream_fields['sendSize'] >= stream_fields['sendConsumed'] and
                     stream_fields['lockEnabled'] in (0, 1) and stream_fields['receiveError'] in (0, 1)),
        'state': hex(state), 'owner': hex(owner), 'world': hex(world), 'stream': hex(stream),
        'sequence': reader.number(state + c['kPacketSequenceOff'], '<B'), 'fields': stream_fields}
    vtables = {}
    for name, vt_name, fn_name in [('action', 'kPacketActionVtableRva', 'kPacketSerializeRva'),
                                   ('end', 'kPacketEndVtableRva', 'kPacketEndSerializeRva')]:
        rva, target = c[vt_name], c[fn_name]
        live = reader.number(p.base + rva + 8)
        disk = struct.unpack('<Q', pe.get_data(rva + 8, 8))[0] - pe.OPTIONAL_HEADER.ImageBase
        vtables[name] = {'pass': live == p.base + target and disk == target,
                        'rva': hex(rva), 'serializeRva': hex(live - p.base)}
    checks['packetVtables'] = {'pass': all(v['pass'] for v in vtables.values()), 'tables': vtables}

    entity, _ = reader.follow(world, [0x5C0, 0])
    comps = components(p, entity, c)
    actor, player = reader.number(state + c['kIdSkillContextActorOff']), reader.number(state + c['kSkillStopObj188'])
    manager = actor + c['kSkillManagerInActorOff']
    getter = reader.number(reader.number(actor) + c['kSkillManagerGetterVtblOff'])
    manager_bounds, values = reader.vector(manager + c['kSkillManagerVectorOff'], 16, 512)
    ids = []
    skills_valid = bool(values)
    for offset in range(0, len(values), 16):
        ptr = struct.unpack_from('<Q', values, offset)[0]
        skill_id = reader.number(ptr + c['kRuntimeSkillId'], '<I')
        skills_valid &= bool(skill_id and (not ids or skill_id > ids[-1]) and
                             reader.number(ptr + c['kSkillOwner']) == manager)
        ids.append(skill_id)
    checks['idSkillContext'] = {
        'pass': bool(actor == comps.get('Actor') and player == comps.get('Player') and
                     reader.number(actor + c['kComponentOwnerEntity']) == entity and
                     reader.number(player + c['kComponentOwnerEntity']) == entity and
                     reader.number(manager + c['kOwnerEntity']) == entity and
                     getter == p.base + c['kSkillManagerGetterRva'] and skills_valid),
        'actor': hex(actor), 'player': hex(player), 'manager': hex(manager), 'getterRva': hex(getter - p.base),
        'bounds': [hex(v) for v in manager_bounds], 'ownedIds': [hex(v) for v in ids],
        'currentSkillId': hex(reader.number(player + c['kIdSkillCurrentIdOff'], '<I'))}

    global_ = reader.number(p.base + c['kSkillContainerBase'])
    container = reader.number(global_ + c['kSkillContainerOff0'])
    bounds, values = reader.vector(container + c['kSkillContainerOff1'], c['kSkillSlotStride'], 64)
    slots, slot_ok = [], True
    for index in range(len(values) // 8):
        obj = struct.unpack_from('<Q', values, index * 8)[0]
        item = {'index': index, 'object': hex(obj)}
        if obj:
            ptr, rep = struct.unpack('<QQ', reader.raw(obj + c['kSkillSharedPtrOff'], 16))
            slot_ok &= bool(ptr) == bool(rep)
            if ptr and rep:
                skill_id = reader.number(ptr + c['kRuntimeSkillId'], '<I')
                own = reader.number(ptr + c['kSkillOwner'])
                level = reader.number(ptr + c['kLevelRecord'])
                effect = reader.number(level + c['kGrantedEffectRecord'])
                active = reader.number(effect + c['kActiveSkillRecord'])
                item.update(id=hex(skill_id), uses=reader.number(rep + c['kRefUsesOff'], '<I'),
                            weak=reader.number(rep + c['kRefWeakOff'], '<I'),
                            internalId=reader.utf16z(reader.number(effect + c['kGrantedEffectIdText'])),
                            code=reader.utf16z(reader.number(active + c['kActiveSkillCodeText'])),
                            name=reader.utf16z(reader.number(active + c['kLocalizedNameText'])))
                slot_ok &= bool(skill_id in ids and reader.number(own + c['kOwnerEntity']) == entity and item['uses'] and item['weak'])
        slots.append(item)
    checks['skillNamesAndIds'] = {'pass': bool(slot_ok and any('id' in s for s in slots)),
                                  'bounds': [hex(v) for v in bounds], 'slots': slots}

    inv_head, _ = reader.follow(p.base, [c['BaseOffset'], c['kSkillStopChainOff1'], c['kSkillStopChainOff2'], 0,
                                        c['kInvTreeArrayOff'], c['kInvTreeArrayTreeOff'], c['kInvTreeSentinelOff']])
    inventory, items = tree(p, inv_head, True, c)
    item_fields, items_ok = [], inventory['valid']
    for item_entity in items:
        ic = components(p, item_entity, c)
        base = ic.get('Base')
        info = reader.number(base + c['kBase_ItemInfoOff']) if base else 0
        name = p.string(info + c['kBase_NameOff']) if info else None
        grade = reader.number(ic['Mods'] + c['kMods_GradeOff'], '<B') if 'Mods' in ic else None
        stack = reader.number(ic['Stack'] + c['kStack_CountOff'], '<B') if 'Stack' in ic else None
        prefix = []
        if 'Mods' in ic:
            prefix = name_parts(reader, ic['Mods'], c)
        items_ok &= bool(name and (grade is None or grade <= 3))
        item_fields.append({'entity': hex(item_entity), 'name': name, 'grade': grade, 'stack': stack, 'prefix': prefix})
    checks['inventoryFields'] = {'pass': bool(items_ok), 'items': item_fields}
    inventory_bounds, inventories = reader.vector(state + c['kInvTreeArrayOff'], c['kInvTreeArrayStride'], 256)
    equipment, equipment_ok = [], True
    for offset in range(0, len(inventories), c['kInvTreeArrayStride']):
        holder = struct.unpack_from('<Q', inventories, offset + c['kInvTreeArrayTreeOff'])[0]
        head = p.q(holder + c['kInvTreeSentinelOff']) if holder else None
        _, candidates = tree(p, head, True, c)
        for candidate in candidates:
            ec = components(p, candidate, c)
            if 'Mods' not in ec or 'Base' not in ec:
                continue
            info = reader.number(ec['Base'] + c['kBase_ItemInfoOff'])
            name = p.string(info + c['kBase_NameOff'])
            grade = reader.number(ec['Mods'] + c['kMods_GradeOff'], '<B')
            prefix = name_parts(reader, ec['Mods'], c)
            equipment_ok &= bool(name and grade <= 3)
            equipment.append({'inventoryIndex': offset // c['kInvTreeArrayStride'], 'entity': hex(candidate),
                              'baseName': name, 'grade': grade, 'parts': prefix, 'prefix': ''.join(prefix)})
    checks['equipmentNameFields'] = {'pass': bool(equipment_ok and equipment),
                                      'inventoryBounds': [hex(v) for v in inventory_bounds], 'items': equipment}
    nearby_head = reader.number(world + 0x718)
    nearby, entities = tree(p, nearby_head, False, c)
    observed, fields_ok = [], nearby['valid']
    for nearby_entity in entities:
        holder = p.q(nearby_entity + c['kMetadataPathOff0']) if nearby_entity else None
        metadata = p.string(holder + c['kMetadataPathOff1']) if holder else None
        if not metadata or not any(s in metadata for s in ('Metadata/Characters/', 'WorldItem', 'MultiplexPortal')):
            continue
        nc = components(p, nearby_entity, c)
        row = {'entity': hex(nearby_entity), 'metadata': metadata}
        if 'Player' in nc:
            row['name'] = p.string(nc['Player'] + c['kPlayer_NameOff'])
            fields_ok &= bool(row['name'])
        if 'Positioned' in nc:
            row['position'] = [reader.number(nc['Positioned'] + c[key], '<i') for key in ('kPositioned_EntXOff', 'kPositioned_EntYOff')]
        if 'Render' in nc:
            row['renderName'] = p.string(nc['Render'] + c['kRender_NameOff'])
        if 'WorldItem' in nc:
            nested = reader.number(nc['WorldItem'] + c['kWorldItem_EntityOff'])
            nested_comps = components(p, nested, c)
            item_info = reader.number(nested_comps['Base'] + c['kBase_ItemInfoOff']) if 'Base' in nested_comps else 0
            row['itemName'] = p.string(item_info + c['kBase_NameOff']) if item_info else None
            fields_ok &= bool(row['itemName'])
        observed.append(row)
    checks['nearbyFields'] = {'pass': bool(fields_ok), 'treeCount': len(entities), 'observed': observed,
                              'note': 'WorldItem/portal data can only be sampled when present; this does not exercise pickup or portal actions.'}
    checks['partyAndWorldArea'] = {'pass': True, **reader.read()}

    cs = Cs(CS_ARCH_X86, CS_MODE_64)
    cs.detail = True
    rip_proofs = {}
    for name, rva, expected in [('globalWrite', 0x10F987, c['BaseOffset']),
                                 ('globalRead', 0x10F9EC, c['BaseOffset']),
                                 ('actionVtable', c['kIdSkillCallRva'] + 0xCA, c['kPacketActionVtableRva']),
                                 ('endVtable', c['kSkillStopRva'] + 0x23, c['kPacketEndVtableRva'])]:
        instruction = next(cs.disasm(pe.get_data(rva, 15), rva))
        targets = [instruction.address + instruction.size + o.mem.disp for o in instruction.operands
                   if o.type == X86_OP_MEM and o.mem.base == X86_REG_RIP]
        rip_proofs[name] = {'pass': expected in targets, 'instructionRva': hex(rva),
                            'expectedRva': hex(expected), 'targets': [hex(t) for t in targets]}
    checks['staticReferences'] = {'pass': all(v['pass'] for v in rip_proofs.values()), 'proofs': rip_proofs}
    result['staticCode'] = {}
    for name, rva, length in [('globalWrite', 0x10F987, 7), ('globalRead', 0x10F9EC, 7),
                              ('idSkill', c['kIdSkillCallRva'], 0x210), ('stop', c['kSkillStopRva'], 0x7E),
                              ('packetBuild', c['kPacketBuildRva'], 0xB3), ('packetSend', c['kPacketSendRva'], 0x4A0),
                              ('packetWrite', c['kPacketWriteRva'], 0x160), ('packetRead', c['kPacketReadRva'], 0x130),
                              ('serialize', c['kPacketSerializeRva'], 0x63), ('endSerialize', c['kPacketEndSerializeRva'], 0x58),
                              ('actionBody', 0x1EDC480, 0x100), ('partyRoster', 0x23FCC0, 0x1F0),
                              ('partyProfileLookup', 0x2138E70, 0xD0)]:
        raw = pe.get_data(rva, length)
        result['staticCode'][name] = {'rva': hex(rva), 'diskLiveEqual': p.read(p.base + rva, length) == raw,
                                     'bytes': raw.hex(' '), 'instructions': [
                                         f'{i.address:#x}: {i.mnemonic} {i.op_str}' for i in cs.disasm(raw, rva)]}
    checks['codeMatchesDisk'] = {'pass': all(v['diskLiveEqual'] for v in result['staticCode'].values())}
    checks['stableRoots'] = {'pass': reader.number(p.base + c['BaseOffset']) == initial_global and
                            reader.number(owner + c['kPacketWorldOff']) == world and
                            reader.number(state + c['kIdSkillContextActorOff']) == actor and
                            tuple(reader.number(manager + c['kSkillManagerVectorOff'] + i * 8) for i in range(3)) == manager_bounds and
                            tuple(reader.number(container + c['kSkillContainerOff1'] + i * 8) for i in range(3)) == bounds}
    result['allPass'] = result['core']['allPass'] and all(v['pass'] for v in checks.values())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', required=True, type=int)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if ctypes.sizeof(ctypes.c_void_p) != 8:
        parser.error('Requires 64-bit Python')
    p = None
    try:
        p = Process(args.pid)
        result = audit(p)
    except (OSError, ReadError, ValueError, struct.error) as error:
        result = {'allPass': False, 'readOnly': True, 'error': str(error)}
    finally:
        if p:
            p.close()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'allPass': result['allPass'], 'out': str(args.out),
                      'checks': {k: v['pass'] for k, v in result.get('checks', {}).items()},
                      'error': result.get('error')}, ensure_ascii=False, indent=2))
    return 0 if result['allPass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
