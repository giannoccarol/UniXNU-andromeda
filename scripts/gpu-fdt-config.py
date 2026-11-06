#!/usr/bin/env python3
"""Read a saved live FDT and resolve GPU phandles/OPPs without hardware access.

Prints a bounded source-bound configuration report, not a ready-to-vote table.
Speed bin is NEVER inferred from a filename/chipid or guessed from the fastest
entry. --speed-bin may be supplied only after independent eFuse/kernel evidence.
"""
import argparse
from collections import deque
import hashlib
import json
from pathlib import Path
import struct


def cells(value):
    if not value or len(value) % 4:
        raise ValueError('property is not nonempty u32 cells')
    return list(struct.unpack(f'>{len(value)//4}I', value))


def strings(value):
    if not value or value[-1] != 0:
        raise ValueError('property is not a terminated string list')
    return value[:-1].decode('ascii').split('\0')


def parse(blob):
    if len(blob) < 40 or len(blob) > 4*1024*1024:
        raise ValueError('FDT length outside policy')
    magic,total,off_struct,off_strings,off_reserved,version,last,_,string_size,struct_size = struct.unpack_from('>10I',blob)
    if magic != 0xd00dfeed or total != len(blob) or version != 17 or last > 17:
        raise ValueError('FDT header/version/length mismatch')
    for offset,size in ((off_struct,struct_size),(off_strings,string_size)):
        if offset < 40 or offset > total or size > total-offset:
            raise ValueError('FDT section outside blob')
    if off_struct&3 or struct_size&3 or off_reserved&7 or off_reserved<40 or off_reserved>total-16:
        raise ValueError('invalid FDT alignment/reservation offset')
    if off_struct < off_strings+string_size and off_strings < off_struct+struct_size:
        raise ValueError('overlapping FDT sections')
    pos=off_reserved
    for _ in range(256):
        if pos>total-16:
            raise ValueError('unterminated reserve map')
        address,size=struct.unpack_from('>QQ',blob,pos)
        pos+=16
        if not address and not size:
            break
    else:
        raise ValueError('reserve map exceeds limit')
    if any(off_reserved<o+n and o<pos for o,n in ((off_struct,struct_size),(off_strings,string_size))):
        raise ValueError('reserve map overlaps sections')
    table=blob[off_strings:off_strings+string_size]
    end=off_struct+struct_size;pos=off_struct;stack=[];nodes={};finished=False
    def align(n): return (n+3)&~3
    for _ in range(200000):
        if pos+4>end:
            raise ValueError('missing FDT end')
        token=struct.unpack_from('>I',blob,pos)[0];pos+=4
        if token==1:
            nul=blob.find(b'\0',pos,end)
            if nul<0 or nul-pos>255 or len(stack)>=64:
                raise ValueError('invalid FDT node name/depth')
            name=blob[pos:nul].decode('ascii');pos=align(nul+1)
            if '/' in name or (not stack and name) or (stack and not name):
                raise ValueError('invalid root/node name')
            path='/' if not stack else stack[-1].rstrip('/')+'/'+name
            if path in nodes or len(nodes)>=16384:
                raise ValueError('duplicate/excess node')
            nodes[path]={};stack.append(path)
        elif token==2:
            if not stack: raise ValueError('unbalanced end-node')
            stack.pop()
        elif token==3:
            if not stack or pos+8>end: raise ValueError('invalid property header')
            length,nameoff=struct.unpack_from('>II',blob,pos);pos+=8
            if length>end-pos or nameoff>=len(table): raise ValueError('invalid property range')
            nul=table.find(b'\0',nameoff)
            if nul<0 or nul==nameoff or nul-nameoff>255: raise ValueError('invalid property name')
            name=table[nameoff:nul].decode('ascii')
            if name in nodes[stack[-1]]: raise ValueError('duplicate property')
            nodes[stack[-1]][name]=blob[pos:pos+length];pos=align(pos+length)
        elif token==4:
            pass
        elif token==9:
            if stack or '/' not in nodes: raise ValueError('unclosed FDT tree')
            if any(blob[pos:end]): raise ValueError('nonzero data after FDT end')
            finished=True;break
        else:
            raise ValueError(f'unknown FDT token {token}')
    if not finished: raise ValueError('FDT token limit')
    return nodes


def report(blob,speed_bin=None):
    nodes=parse(blob)
    handles={}
    for path,props in nodes.items():
        values=[]
        for key in ('phandle','linux,phandle'):
            if key in props:
                value=cells(props[key])
                if len(value)!=1 or value[0] in (0,0xffffffff): raise ValueError('invalid phandle')
                values.append(value[0])
        if values:
            if len(set(values))!=1 or values[0] in handles: raise ValueError('duplicate/conflicting phandle')
            handles[values[0]]=path
    def unique(compatible):
        needle=compatible.encode('ascii')
        result=[p for p,v in nodes.items() if needle in v.get('compatible',b'').split(b'\0')]
        if len(result)!=1: raise ValueError(f'expected one {compatible}')
        if compatible not in strings(nodes[result[0]]['compatible']): raise ValueError('malformed target compatible')
        return result[0]
    def scalar(path,name):
        v=cells(nodes[path][name])
        if len(v)!=1: raise ValueError(f'{name} must be scalar')
        return v[0]
    def ref(path,name):
        v=scalar(path,name)
        if v not in handles: raise ValueError('unresolved phandle')
        return handles[v]
    def children(path):
        return [p for p in nodes if p.rsplit('/',1)[0]==path]
    def regs(path):
        parent=path.rsplit('/',1)[0] or '/'
        ac=scalar(parent,'#address-cells');sc=scalar(parent,'#size-cells')
        if ac not in (1,2) or sc not in (1,2): raise ValueError('unsupported register cell geometry')
        raw=cells(nodes[path]['reg']);width=ac+sc
        if len(raw)%width: raise ValueError('incomplete register range')
        names=strings(nodes[path]['reg-names'])
        if len(names)!=len(raw)//width or len(names)!=len(set(names)): raise ValueError('reg-names mismatch')
        def combine(a):
            x=0
            for n in a: x=(x<<32)|n
            return x
        result={}
        for i,name in enumerate(names):
            r=raw[i*width:(i+1)*width];address=combine(r[:ac]);size=combine(r[ac:])
            if not size or address+size>1<<64: raise ValueError('invalid physical range')
            result[name]={'address':address,'bytes':size}
        return result
    gpu=unique('qcom,kgsl-3d0');gmu=unique('qcom,gpu-gmu')
    if scalar(gpu,'qcom,chipid')!=0x06040001: raise ValueError('not expected Cepheus A640 chipid')
    opppath=ref(gpu,'operating-points-v2');opps=[]
    for child in children(opppath):
        hz=cells(nodes[child]['opp-hz']);level=scalar(child,'opp-microvolt')
        if len(hz)!=2 or hz[0] or not hz[1] or hz[1]%1000 or not 1<=level<=65536:
            raise ValueError('invalid OPP Hz/abstract level')
        opps.append({'hz':hz[1],'raw_vlvl':level-1})
    opps.sort(key=lambda x:x['hz'])
    if not opps or len(opps)>15 or len({x['hz'] for x in opps})!=len(opps): raise ValueError('invalid OPP table')
    oppmap={x['hz']:x['raw_vlvl'] for x in opps}
    binpath=gpu+'/qcom,gpu-pwrlevel-bins';bins=[]
    for path in children(binpath):
        index=scalar(path,'qcom,speed-bin');levels=[]
        for child in children(path):
            if 'qcom,gpu-freq' not in nodes[child]: continue
            hz=scalar(child,'qcom,gpu-freq')
            if hz and hz not in oppmap: raise ValueError('powerlevel frequency not in selected OPP phandle')
            levels.append({'index':scalar(child,'reg'),'hz':hz,'raw_vlvl':oppmap.get(hz,0),
                'bus_index':scalar(child,'qcom,bus-freq'),'bus_min':scalar(child,'qcom,bus-min'),
                'bus_max':scalar(child,'qcom,bus-max')})
        levels.sort(key=lambda x:x['index'])
        if len(levels)<2 or len(levels)>16 or [x['index'] for x in levels]!=list(range(len(levels))) or levels[-1]['hz']:
            raise ValueError('powerlevel indices/off entry invalid')
        if any(levels[i]['hz']<=levels[i+1]['hz'] for i in range(len(levels)-1)): raise ValueError('powerlevels not descending')
        initial=scalar(path,'qcom,initial-pwrlevel')
        throttle=scalar(gpu,'qcom,throttle-pwrlevel')
        if initial>=len(levels)-1 or throttle>=len(levels): raise ValueError('bad initial/throttle index')
        mask=((1<<(throttle+1))-1)<<(len(levels)-1-throttle)
        bins.append({'bin':index,'initial_index':initial,'throttle_mask':mask,'levels_descending':levels,
                     'lm_enabled_by_source':index!=0,'acd_mailbox_by_source':index!=0})
    if not bins or len({b['bin'] for b in bins})!=len(bins): raise ValueError('invalid speed bins')
    bins.sort(key=lambda b:b['bin'])
    def bus(path):
        cases=scalar(path,'qcom,msm-bus,num-cases');paths=scalar(path,'qcom,msm-bus,num-paths')
        raw=cells(nodes[path]['qcom,msm-bus,vectors-KBps'])
        if not 1<=cases<=16 or paths!=1 or len(raw)!=4*cases: raise ValueError('unsupported bus table')
        rows=[{'src':raw[i],'dst':raw[i+1],'ab_kbps':raw[i+2],'ib_kbps':raw[i+3]} for i in range(0,len(raw),4)]
        if len({(r['src'],r['dst']) for r in rows})!=1: raise ValueError('changing bus path')
        return rows
    ddr=bus(gpu);cnoc=bus(gmu)
    # The actual directed interconnect graph, not a hardcoded guessed path.
    ids={}
    for path,props in nodes.items():
        if path.startswith('/soc/ad-hoc-bus/') and 'cell-id' in props:
            ident=scalar(path,'cell-id')
            if ident in ids: raise ValueError('duplicate interconnect cell-id')
            ids[ident]=path
    def route(rows):
        src,dst=rows[0]['src'],rows[0]['dst']
        if src not in ids or dst not in ids: raise ValueError('interconnect endpoint missing')
        todo=deque([(ids[src],[ids[src]])]);found=[];steps=0
        while todo:
            steps+=1
            if steps>16384: raise ValueError('interconnect traversal budget exceeded')
            node,path=todo.popleft()
            if node==ids[dst]:
                found.append(path)
                if len(found)>1: raise ValueError('ambiguous GPU bus path')
                continue
            if len(path)>=32: raise ValueError('interconnect depth exceeded')
            for handle in cells(nodes[node]['qcom,connections']) if 'qcom,connections' in nodes[node] else []:
                if handle not in handles: raise ValueError('unresolved interconnect edge')
                child=handles[handle]
                if child not in path: todo.append((child,path+[child]))
        if len(found)!=1: raise ValueError('missing GPU bus path')
        result=[]
        for node in found[0]:
            resources=[]
            for handle in cells(nodes[node]['qcom,bcms']) if 'qcom,bcms' in nodes[node] else []:
                if handle not in handles: raise ValueError('unresolved BCM reference')
                target=handles[handle];name=strings(nodes[target]['qcom,bcm-name'])
                if len(name)!=1: raise ValueError('invalid BCM resource name')
                rscs=cells(nodes[target]['qcom,rscs'])
                if len(rscs)!=1 or rscs[0] not in handles: raise ValueError('ambiguous BCM RSC')
                rscpath=handles[rscs[0]]
                resources.append({'path':target,'name':name[0],'rsc_path':rscpath,'rsc_id':scalar(rscpath,'cell-id')})
            result.append({'node':node,'buswidth':scalar(node,'qcom,buswidth'),
                           'ports':scalar(node,'qcom,agg-ports'),'bcms':resources})
        return result
    routes={'ddr':route(ddr),'cnoc':route(cnoc)}
    for b in bins:
        for l in b['levels_descending']:
            if not 0<=l['bus_min']<=l['bus_index']<=l['bus_max']<len(ddr): raise ValueError('bus index out of range')
    smmu=None
    streams={}
    for child in children(gmu):
        if 'iommus' not in nodes[child]: continue
        c=cells(nodes[child]['iommus'])
        if len(c)!=3 or c[0] not in handles: raise ValueError('unsupported GMU iommu spec')
        target=handles[c[0]]
        if 'qcom,qsmmu-v500' not in strings(nodes[target]['compatible']): raise ValueError('unsupported GMU SMMU')
        if smmu and smmu!=target: raise ValueError('GMU contexts on different SMMUs')
        smmu=target;streams[child.rsplit('/',1)[1]]={'sid':c[1],'mask':c[2]}
    if not smmu or not streams or 'qcom,skip-init' not in nodes[smmu]: raise ValueError('missing inherited GMU SMMU')
    actlr=cells(nodes[smmu]['qcom,actlr'])
    if len(actlr)%3: raise ValueError('ACTLR match table malformed')
    for stream in streams.values():
        matches={actlr[i+2] for i in range(0,len(actlr),3)
                 if not ((actlr[i]^stream['sid']) & ~(actlr[i+1]|stream['mask']))}
        if len(matches)!=1: raise ValueError('missing/conflicting ACTLR for GMU stream')
        stream['actlr']=matches.pop()
    acdpath=gmu+'/qcom,gpu-acd-table';acd={'enable_mask':scalar(acdpath,'qcom,acd-enable-by-level'),
        'stride':scalar(acdpath,'qcom,acd-stride'),'levels':scalar(acdpath,'qcom,acd-num-levels'),
        'data':cells(nodes[acdpath]['qcom,acd-data'])}
    if not 1<=acd['stride']<=2 or not 1<=acd['levels']<=6 or len(acd['data'])!=acd['stride']*acd['levels'] or acd['enable_mask'].bit_count()!=acd['levels']:
        raise ValueError('invalid ACD table')
    selected=None
    if speed_bin is not None:
        matches=[b for b in bins if b['bin']==speed_bin]
        if len(matches)!=1: raise ValueError('requested speed bin absent')
        selected=matches[0]
    gmu_levels=[]
    for child in children(gmu+'/qcom,gmu-pwrlevels'):
        if 'qcom,gmu-freq' in nodes[child]: gmu_levels.append((scalar(child,'reg'),scalar(child,'qcom,gmu-freq')))
    gmu_levels.sort()
    if gmu_levels!=[(0,0),(1,200000000)]: raise ValueError('unexpected GMU frequencies')
    mailbox=cells(nodes[gmu]['mboxes'])
    if len(mailbox)!=2 or mailbox[0] not in handles or mailbox[1]!=0:
        raise ValueError('unexpected GMU mailbox specifier')
    qp=handles[mailbox[0]]
    if strings(nodes[gmu]['mbox-names'])!=['aop'] or 'qcom,qmp-mbox' not in strings(nodes[qp]['compatible']):
        raise ValueError('unexpected GMU mailbox controller')
    qmp={'path':qp,'registers':regs(qp),'descriptor_offset':scalar(qp,'mbox-desc-offset'),
         'irq_mask':scalar(qp,'qcom,irq-mask'),'channel':mailbox[1]}
    thermal=[]
    for name in strings(nodes[gpu]['tzone-names']):
        zone='/soc/thermal-zones/'+name
        spec=cells(nodes[zone]['thermal-sensors'])
        if len(spec)!=2 or spec[0] not in handles or spec[1]>15:raise ValueError('invalid GPU thermal specifier')
        controller=handles[spec[0]]
        if 'qcom,tsens24xx' not in strings(nodes[controller]['compatible']):raise ValueError('unsupported GPU thermal controller')
        thermal.append({'zone':name,'controller':controller,'sensor':spec[1],
                        'tm':regs(controller)['tsens_tm_physical'],'srot':regs(controller)['tsens_srot_physical']})
    if len(thermal)!=2:raise ValueError('expected two GPU thermal zones')
    kgsl_iommu=unique('qcom,kgsl-smmu-v2');gx_streams={}
    for child in children(kgsl_iommu):
        if 'iommus' not in nodes[child]:continue
        spec=cells(nodes[child]['iommus'])
        if len(spec)!=3 or handles.get(spec[0])!=smmu:raise ValueError('unexpected GX SMMU')
        gx_streams[child.rsplit('/',1)[1]]={'sid':spec[1],'mask':spec[2]}
    gx={'highest_bank_bit':scalar(gpu,'qcom,highest-bank-bit'),
        'min_access_length':scalar(gpu,'qcom,min-access-length'),
        'ubwc_mode':scalar(gpu,'qcom,ubwc-mode'),
        'macrotiling_channels':scalar(gpu,'qcom,macrotiling-channels') if 'qcom,macrotiling-channels' in nodes[gpu] else None,
        'iommu_path':kgsl_iommu,'streams':gx_streams,
        'hyp_secure_alloc':'qcom,hyp_secure_alloc' in nodes[kgsl_iommu],
        'three_level_tables':'qcom,use-3-lvl-tables' in nodes[smmu]}
    zap_nodes=[path for path,props in nodes.items() if props.get('qcom,firmware-name')==b'a640_zap\0']
    if len(zap_nodes)!=1:raise ValueError('expected one A640 ZAP loader')
    zp=zap_nodes[0]
    if 'qcom,pil-tz-generic' not in strings(nodes[zp]['compatible']):raise ValueError('unexpected ZAP loader')
    zap={'path':zp,'pas_id':scalar(zp,'qcom,pas-id'),'no_auth':'qcom,pil-no-auth' in nodes[zp],
         'memory_region':ref(zp,'memory-region') if 'memory-region' in nodes[zp] else None,
         'subsys_vmid':scalar(zp,'qcom,subsys-vmid') if 'qcom,subsys-vmid' in nodes[zp] else None,
         'clock_names':strings(nodes[zp]['clock-names']) if 'clock-names' in nodes[zp] else [],
         'proxy_clock_names':strings(nodes[zp]['qcom,proxy-clock-names']) if 'qcom,proxy-clock-names' in nodes[zp] else []}
    if zap['pas_id']!=13 or zap['no_auth']:raise ValueError('unsupported/unauthenticated ZAP policy')
    return {'fdt_sha256':hashlib.sha256(blob).hexdigest(),'fdt_bytes':len(blob),
        'chipid':scalar(gpu,'qcom,chipid'),'gpu_path':gpu,'gmu_path':gmu,'opp_path':opppath,
        'gpu_registers':regs(gpu),'gmu_registers':regs(gmu),'gmu_hz':[n[1] for n in gmu_levels],
        'opp_levels':opps,'speed_bins':bins,
        'selected_bin':selected,'speed_bin_evidence':'caller-supplied; not checked by this tool' if selected else 'UNKNOWN; no eFuse read performed',
        'ddr_vectors':ddr,'cnoc_vectors':cnoc,'bus_routes':routes,'streams':streams,'smmu_path':smmu,
        'smmu_actlr_matches':[actlr[i:i+3] for i in range(0,len(actlr),3)],'acd':acd,'qmp':qmp,'thermal':thermal,'gx':gx,'zap':zap,
        'limitations':['saved FDT only, no hardware execution','BCM cmd-db unit/width/VCD values still needed',
                        'not an ARC vote ownership proof','selected OPP phandle must replace generic sm8150 table']}


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('fdt',type=Path);p.add_argument('--speed-bin',type=int)
    a=p.parse_args();print(json.dumps(report(a.fdt.read_bytes(),a.speed_bin),indent=2))


if __name__=='__main__': main()
