# PlateHide.csv: je HZN-modelbin die Index-Bereiche von Schild (HZN_p1), Nieten (HZN_rv), AO-Schleier (HZN_ao)
# Zeile: relpfad;index-daten-offset;index-anzahl;start:anzahl,start:anzahl,...
import sys,struct,zipfile
sys.path.insert(0,'/home/claude/work'); import fbundle
HIDE={'hzn_p1','hzn_rv','hzn_ao'}
def blob_offsets(b):
    mn=b[5]
    if mn>=1: _,hs,ts,cnt=struct.unpack_from('<HIII',b,6); p=20
    else: cnt,=struct.unpack_from('<H',b,6); p=16
    out=[]
    for i in range(cnt):
        tag=b[p+i*24:p+i*24+4]; maj,mn2,mc,mo,do,ds,ds2=struct.unpack_from('<BBHIIII',b,p+i*24+4); out.append((tag,do,ds))
    return out
import json as _j
NATIVE={k.lower():bool(v) and all(e.get('native') for e in v) for k,v in _j.load(open('/home/claude/work/light/fh2_detect.json')).items() if isinstance(v,list)}
def ranges(data):
    B=fbundle.Bundle.load(data); offs=blob_offsets(data)
    ids={}
    for x in B.blobs:
        if x.tag==b'ItaM':
            nm=(fbundle.name(x) or '').lower(); m=x.meta(b'  dI')
            if m and nm in HIDE: ids[struct.unpack('<I',m.data)[0]]=nm
    ib=[i for i,x in enumerate(B.blobs) if x.tag==b'BdnI'][0]
    tag,do,ds=offs[ib]; assert tag==b'BdnI'
    icount,ibytes,isz=struct.unpack_from('<IIH',data,do); assert isz==4
    rr=set(); names={}; ao=set()
    for x in B.blobs:
        if x.tag!=b'hseM': continue
        mid=struct.unpack_from('<H',x.data,0)[0]
        if mid in ids:
            s,n=struct.unpack_from('<I',x.data,25)[0],struct.unpack_from('<I',x.data,33)[0]
            rr.add((s,n)); names.setdefault(ids[mid],0); names[ids[mid]]+=1
            if ids[mid]=='hzn_ao': ao.add((s,n))
    # Positions-Strom (BreV ohne Id bzw. Id ffffffff), 12 Byte je Vertex
    posoff=-1
    for i,x in enumerate(B.blobs):
        if x.tag==b'BreV':
            m=x.meta(b'  dI'); idv=struct.unpack('<I',m.data)[0] if m else 0xffffffff
            if idv==0xffffffff:
                t2,do2,ds2=offs[i]; n,nb,st=struct.unpack_from('<IIH',data,do2)
                if st==12: posoff=do2+16
    lamp=any((fbundle.name(x) or '').lower()=='hzn_lt' for x in B.blobs if x.tag==b'ItaM')
    return do+16,icount,sorted(rr),names,posoff,lamp,sorted(ao)
if __name__=='__main__':
    z=zipfile.ZipFile(sys.argv[1]); out=[]; miss=[]
    for n in sorted(z.namelist()):
        if '/scene/Exterior/HZN/' in n and n.endswith('.modelbin'):
            d=z.read(n); off,ic,rr,names,po,lamp,ao=ranges(d)
            if not rr: miss.append(n); continue
            rel=n[len('media/'):].replace('/','\\').lower()
            # v4: Autos, deren Kennzeichenlampe im Spielmodell selbst leuchtet (fh2_detect: alle Eintraege native), haben kein HZN_lt -> trotzdem Lampe an
            car=rel.split('\\')[1]
            if not lamp and NATIVE.get(car): lamp=True
            out.append('%s;%d;%d;%s;%d;%d;%s'%(rel,off,ic,','.join('%d:%d'%r for r in rr),po,1 if lamp else 0,','.join('%d:%d'%r for r in ao)))
    open(sys.argv[2],'w',newline='\r\n').write('#v3 PlateHide: relative path;index offset;index count;start:count,...;position offset;lamp(0/1);shadow decal ranges start:count,...\n# LicencePlates by Philipp55112 - do not reupload without credit\n'+'\n'.join(out)+'\n')
    print(len(out),'Dateien',len(miss),'ohne Bereiche',miss[:5])
