"""Read-only ritual text/entity investigation. No injection, writes, or game calls.

Stores only keyword hits and bounded surrounding object bytes, never full memory.
Offsets discovered here are research candidates, not production state predicates.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import datetime
import json
from pathlib import Path
import re
import struct
import time

from offset_audit import Process, components, load_constants, tree

class MemoryInfo(C.Structure):
    _fields_ = [('BaseAddress', C.c_uint64), ('AllocationBase', C.c_uint64),
                ('AllocationProtect', W.DWORD), ('PartitionId', W.WORD),
                ('RegionSize', C.c_uint64), ('State', W.DWORD),
                ('Protect', W.DWORD), ('Type', W.DWORD)]

def regions(p):
    p.k.VirtualQueryEx.argtypes = [W.HANDLE, C.c_void_p, C.POINTER(MemoryInfo), C.c_size_t]
    p.k.VirtualQueryEx.restype = C.c_size_t
    address = 0x10000
    while address < 0x800000000000:
        info = MemoryInfo()
        if not p.k.VirtualQueryEx(p.handle, address, C.byref(info), C.sizeof(info)): break
        end = info.BaseAddress + info.RegionSize
        if end <= address: break
        if info.State == 0x1000 and not info.Protect & 0x100 and info.Protect & 0xEE:
            yield {'base':info.BaseAddress, 'size':info.RegionSize,
                   'allocation':info.AllocationBase, 'protect':info.Protect, 'type':info.Type}
        address = end

def blocks(p, items, chunk=0x400000):
    for r in items:
        for address in range(r['base'],r['base']+r['size'],chunk):
            n = min(chunk,r['base']+r['size']-address)
            data = p.read(address,n)
            if data is not None: yield address,data,r
            else:
                for page in range(address,address+n,0x1000):
                    data=p.read(page,min(0x1000,address+n-page))
                    if data is not None: yield page,data,r

def ztext(p,address,wide=True,limit=256):
    raw=p.read(address,limit*(2 if wide else 1))
    if raw is None: return None
    if wide:
        end=next((i for i in range(0,len(raw)-1,2) if raw[i:i+2]==b'\0\0'),len(raw))
        return raw[:end].decode('utf-16-le',errors='replace')
    return raw.split(b'\0',1)[0].decode('utf-8',errors='replace')

def rtti(p,vtable):
    if not p.base <= vtable < p.base+0x4CBB000: return None
    locator=p.q(vtable-8)
    raw=p.read(locator,24) if locator else None
    if not raw: return None
    signature,offset,ctor,type_rva,hierarchy,self_rva=struct.unpack('<6I',raw)
    if signature!=1 or locator-self_rva!=p.base or type_rva>=0x4CBB000: return None
    return {'name':ztext(p,p.base+type_rva+16,False),'offset':offset,'vtableRva':hex(vtable-p.base)}

def object_starts(p,address,before=0x600):
    start=max(0x10000,address-before)&~7
    raw=p.read(start,address-start+8)
    if raw is None: return []
    found=[]
    for i in range(0,len(raw)-7,8):
        value=struct.unpack_from('<Q',raw,i)[0]
        if p.base <= value < p.base+0x4CBB000:
            name=rtti(p,value)
            if name is None:
                entry=p.q(value)
                if entry and p.base+0x1000<=entry<p.base+0x2E00000:
                    name={'name':None,'vtableRva':hex(value-p.base),'heuristic':True}
            if name: found.append({'address':hex(start+i),'fieldOffset':hex(address-start-i),**name})
    return found[-16:]

def snapshot(p,c):
    state,_=p.chain([c['BaseOffset'],0x348,0x48,0])
    world,_=p.chain([0x1A8,0x20F0],state)
    head=p.q(world+0x718) if world else None
    status,entities=tree(p,head,False,c)
    rows=[]
    for entity in entities:
        if not entity: continue
        holder=p.q(entity+c['kMetadataPathOff0'])
        metadata=p.string(holder+c['kMetadataPathOff1']) if holder else None
        if not metadata or 'ritual' not in metadata.lower(): continue
        comps=components(p,entity,c)
        row={'entity':hex(entity),'metadata':metadata,'components':{k:hex(v) for k,v in comps.items()},'samples':{}}
        position=comps.get('Positioned')
        raw=p.read(position+c['kPositioned_EntXOff'],8) if position else None
        row['position']=list(struct.unpack('<ii',raw)) if raw else None
        for name,n in [('StateMachine',0x400),('InteractionAction',0x180),('Targetable',0x100),('Functions',0x180),('Render',0x220)]:
            address=comps.get(name)
            if address:
                data=p.read(address,n)
                row['samples'][name]={'address':hex(address),'rtti':rtti(p,p.q(address) or 0),'hex':data.hex(' ') if data else None}
        rows.append(row)
    return {'state':hex(state or 0),'world':hex(world or 0),'tree':status,'ritualEntities':rows}

def save(path,data):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pid',type=int,required=True)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--refs',type=Path,help='Prior string scan; scan references to its keyword addresses')
    ap.add_argument('--targets',nargs='*',help='Explicit hex addresses for a reference scan')
    ap.add_argument('--snapshot-only',action='store_true')
    ap.add_argument('--private-only',action='store_true',help='Scan ordinary private heaps, excluding mapped GPU/file buffers')
    args=ap.parse_args()
    p=Process(args.pid); c=load_constants()
    try:
        nt=p.unpack(p.base+0x3c,'<I')
        if p.unpack(p.base+nt+8,'<I')!=c['kGameTimeDateStamp']: raise RuntimeError('Game build mismatch')
        data={'createdAt':datetime.datetime.now().astimezone().isoformat(),'pid':p.pid,'moduleBase':hex(p.base),'readOnly':True,'before':snapshot(p,c)}
        save(args.out,data)
        if args.snapshot_only: print(json.dumps(data,ensure_ascii=False));return
        memory=list(regions(p)); total=sum(r['size'] for r in memory)
        if args.private_only:memory=[r for r in memory if r['type']==0x20000 and not r['protect']&0x400]
        print(f'regions={len(memory)} readableGiB={total/2**30:.2f}',flush=True)
        if args.refs or args.targets:
            targets={}
            if args.refs:
                old=json.loads(args.refs.read_text(encoding='utf-8'))
                for h in old['hits']:targets[int(h['address'],16)]=h.get('label','text')
            for target in args.targets or []:targets[int(target,0)]=target
            patterns={struct.pack('<Q',a):label for a,label in targets.items()}
            memory=[r for r in memory if r['protect']&0xCC]
        else:
            patterns={s.encode(enc):s+' / '+enc for s in ['點擊以開始祭祀','祭祀神壇'] for enc in ['utf-16-le','utf-8']}
        regex=re.compile(b'|'.join(re.escape(x) for x in patterns))
        hits=[]; seen=set(); scanned=0; start=time.monotonic(); next_report=start+15
        previous_end=0; tail=b''
        for address,block,r in blocks(p,memory):
            prefix=tail if previous_end==address else b''
            raw=prefix+block; origin=address-len(prefix)
            for match in regex.finditer(raw):
                slot=origin+match.start()
                if slot in seen:continue
                seen.add(slot)
                if len(hits)>=1000:continue
                context=p.read(max(0x10000,slot-64),256)
                hit={'address':hex(slot),'label':patterns[match.group()],
                     'region':{k:hex(v) for k,v in r.items()},'contextStart':hex(slot-64),
                     'contextHex':context.hex(' ') if context else None}
                if args.refs or args.targets:
                    hit['target']=hex(struct.unpack('<Q',match.group())[0]);hit['objects']=object_starts(p,slot)
                else: hit['text']=ztext(p,slot,'utf-16' in hit['label'])
                hits.append(hit)
                print('hit',hex(slot),hit['label'],flush=True)
            scanned+=len(block);tail=raw[-64:];previous_end=address+len(block)
            if time.monotonic()>=next_report:
                print(f'scannedGiB={scanned/2**30:.2f} hits={len(hits)} elapsed={time.monotonic()-start:.1f}s',flush=True)
                next_report=time.monotonic()+15
        data.update(hits=hits,scannedBytes=scanned,elapsedSeconds=time.monotonic()-start,after=snapshot(p,c))
        save(args.out,data)
        print(f'COMPLETE hits={len(hits)} seconds={data["elapsedSeconds"]:.1f} out={args.out}',flush=True)
    finally:p.close()

if __name__=='__main__':main()
