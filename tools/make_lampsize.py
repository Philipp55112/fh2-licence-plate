# PlateLampSize.csv: Breite der leuchtenden Lampenglaeser je Lampe (m), fuer den Lichtkegel
import sys,zipfile,struct,numpy as np
sys.path.insert(0,'/tmp'); sys.path.insert(0,'/home/claude/work')
import fbundle
def lt_vertices(data):
    B=fbundle.Bundle.load(data); ids={}
    for x in B.blobs:
        if x.tag==b'ItaM':
            m=x.meta(b'  dI')
            if m: ids[struct.unpack('<I',m.data)[0]]=(fbundle.name(x) or '').lower()
    lt=[k for k,v in ids.items() if v=='hzn_lt']
    if not lt: return None
    ib=[x for x in B.blobs if x.tag==b'BdnI'][0].data; ic=struct.unpack_from('<I',ib,0)[0]; I=np.frombuffer(ib[16:16+ic*4],'<u4')
    pos=None
    for x in B.blobs:
        if x.tag==b'BreV':
            m=x.meta(b'  dI'); idv=struct.unpack('<I',m.data)[0] if m else 0xffffffff
            n,nb,st=struct.unpack_from('<IIH',x.data,0)
            if idv==0xffffffff and st==12: pos=np.frombuffer(x.data[16:16+n*12],'<f4').reshape(-1,3)
    vs=set()
    for x in B.blobs:
        if x.tag==b'hseM' and struct.unpack_from('<H',x.data,0)[0] in lt:
            s,c=struct.unpack_from('<I',x.data,25)[0],struct.unpack_from('<I',x.data,33)[0]; vs|=set(I[s:s+c].tolist())
    return pos[sorted(vs)]
z=zipfile.ZipFile(sys.argv[1]); names=z.namelist()
out=['# PlateLampSize: MediaName;count;width per lamp (m, size of the glowing glass) - used to widen the light cone of wide lamps',
     '# LicencePlates by Philipp55112 - do not reupload without credit']
allw=[]
for l in z.read('plugins/PlateLightCars.csv').decode().splitlines():
    if not l or l[0]=='#' or '@' not in l: continue
    f=l.split(';'); car=f[0].split('@')[0]; n=int(f[1]); glass=[np.array([float(x) for x in f[2+i*11:5+i*11]]) for i in range(n)]
    files=[x for x in names if x.lower().startswith(('media/cars/'+car+'/scene/exterior/hzn/').lower()) and x.endswith('.modelbin')]
    P=None
    for fn in sorted(files):
        P=lt_vertices(z.read(fn))
        if P is not None and len(P): break
    if P is None or not len(P): continue
    d=np.stack([np.linalg.norm(P-g,axis=1) for g in glass],1)
    if np.median(d.min(1))>0.25:   # HZN-Modell haengt an einem Traeger mit Versatz -> Schwerpunkte angleichen (Breite aendert sich dadurch nicht)
        P=P+(np.mean(glass,0)-P.mean(0)); d=np.stack([np.linalg.norm(P-g,axis=1) for g in glass],1)
    own=d.argmin(1)
    ws=[]
    for i in range(n):
        Q=P[(own==i)&(d.min(1)<0.25)]
        if len(Q)<3: ws.append(0.0); continue
        ext=Q.max(0)-Q.min(0); ws.append(float(max(ext[0],ext[1])))
    allw+=ws
    out.append('%s;%d;%s'%(car,n,';'.join('%.3f'%w for w in ws)))
open(sys.argv[2],'w',newline='\r\n').write('\n'.join(out)+'\n')
a=np.array(allw); print(len(out)-2,'Autos', 'Breiten min/median/max', a.min(), np.median(a), a.max()); print(np.percentile(a,[10,25,50,75,90]))
