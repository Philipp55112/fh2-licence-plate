import sys,struct,zipfile; sys.path.insert(0,'/home/claude/work')
import numpy as np, fbundle, skel
def mat_tris(data):
    b=fbundle.Bundle.load(data); B=skel.bones(b)
    mats={struct.unpack('<I',x.meta(b'  dI').data)[0]:(fbundle.name(x) or '?') for x in b.blobs if x.tag==b'ItaM'}
    ps=[x for x in b.blobs if x.tag==b'BreV' and x.meta(b'  dI').data==b'\xff\xff\xff\xff']
    if not ps: return []
    p=ps[0]; c,tb,stride=struct.unpack_from('<IIH',p.data,0)
    if stride!=12: return []
    P=np.frombuffer(p.data[16:16+c*12],'<f4').reshape(-1,3).astype(np.float64)
    ib=[x for x in b.blobs if x.tag==b'BdnI'][0]; nI,tb,wd=struct.unpack_from('<IIH',ib.data,0)
    I=np.frombuffer(ib.data[16:16+tb],'<u4' if wd==4 else '<u2').astype(np.int64)
    out=[]
    for m in b.blobs:
        if m.tag!=b'hseM': continue
        nm=fbundle.name(m) or ''
        if not (nm.endswith('LODS0') or nm.endswith('LOD0')): continue
        if m.data[:2]==b'\xff\xff': continue
        mid,bi=struct.unpack_from('<HH',m.data,0)
        fs,fc=struct.unpack_from('<I',m.data,25)[0],struct.unpack_from('<I',m.data,33)[0]
        if fs+fc>len(I) or fc==0: continue
        V=P[I[fs:fs+fc]]
        if V.size==0: continue
        if bi<len(B): M=B[bi][3]; V=V@M[:3,:3]+M[3,:3]
        out.append((mats.get(mid,'?'),nm,V.reshape(-1,3,3)))
    return out
def car(zp):
    z=zipfile.ZipFile(zp); res=[]
    for n in z.namelist():
        if not n.endswith('.modelbin') or 'SLOD' in n or 'nterior' in n: continue
        try:
            for mn,me,T in mat_tris(z.read(n)): res.append((n.split('/')[-1],mn,me,T))
        except Exception as e: pass
    return res
