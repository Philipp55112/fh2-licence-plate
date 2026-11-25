import sys,struct,zlib,json,os,copy
sys.path.insert(0,'/home/claude/work')
import numpy as np, fbundle
O='/home/claude/work/fh5plate/'
PARENT='Game:\\Media\\cars\\_library\\materials\\badges\\hzn_plate.materialbin'
def bend(V,N,T):
    # Wölbung in x wie die Karosserie: z += c*(x-cx)^2  (T['curv'])
    c=float(T.get('curv',0.0))
    V=np.array(V,np.float64); N=np.array(N,np.float64)
    if not c: return V,N
    dx=V[:,0]-T['cx']; V[:,2]+=c*dx*dx
    a=np.arctan(2*c*dx); ca,sa=np.cos(a),np.sin(a)
    nx=N[:,0]*ca-N[:,2]*sa; nz=N[:,0]*sa+N[:,2]*ca; N[:,0]=nx; N[:,2]=nz
    return V,N
def tilt(V,N,T):
    V,N=bend(V,N,T)
    # Neigung um die x-Achse durch die Schildmitte: T['slope'] = dz/dy der Karosserie (z wächst nach vorn)
    k=float(T.get('slope',0.0))
    if not k: return np.asarray(V,np.float32),np.asarray(N,np.float32)
    th=np.arctan(k); s,c=np.sin(th),np.cos(th)
    V=np.array(V,np.float64); N=np.array(N,np.float64); cy,cz=T['cy'],T['cz']
    dy=V[:,1]-cy; dz=V[:,2]-cz
    V[:,1]=cy+dy*c-dz*s; V[:,2]=cz+dy*s+dz*c
    ny=N[:,1]*c-N[:,2]*s; nz=N[:,1]*s+N[:,2]*c; N[:,1]=ny; N[:,2]=nz
    return V.astype(np.float32),N.astype(np.float32)
def plate_geom(T):
    # Eigene Schildgeometrie (Fahrzeugkoordinaten): Vorderseite zeigt nach hinten (-Z), u steigt mit +x
    sc=float(os.environ.get('SCALE','1'))
    W,H,TH,R=0.520*sc,0.110*sc,0.006*sc,0.012*sc
    cx,cy,cz=T['cx'],T['cy'],T['cz']
    ring=[]
    for (ox,oy,a0) in ((W/2-R,H/2-R,0),(-W/2+R,H/2-R,90),(-W/2+R,-H/2+R,180),(W/2-R,-H/2+R,270)):
        for k in range(9):
            a=np.deg2rad(a0+k*90/8); ring.append((ox+R*np.cos(a),oy+R*np.sin(a)))
    ring=np.array(ring); n=len(ring)
    V=[];UV=[];N=[];F=[]
    def add(p,u,nn): V.append(p);UV.append(u);N.append(nn);return len(V)-1
    zf=cz-TH/2; zb=cz+TH/2
    edge=(0.5,0.004)
    c0=add((cx,cy,zf),(0.5,0.5),(0,0,-1)); fr=[add((cx+x,cy+y,zf),(x/W+0.5,0.5-y/H),(0,0,-1)) for x,y in ring]
    c1=add((cx,cy,zb),edge,(0,0,1)); bk=[add((cx+x,cy+y,zb),edge,(0,0,1)) for x,y in ring]
    for i in range(n):
        j=(i+1)%n
        F.append((c0,fr[i],fr[j])); F.append((c0,fr[j],fr[i]))          # beide Wicklungen
        F.append((c1,bk[j],bk[i])); F.append((c1,bk[i],bk[j]))
        nx,ny=ring[i]/np.linalg.norm(ring[i])
        a1=add((cx+ring[i][0],cy+ring[i][1],zf),edge,(nx,ny,0)); a2=add((cx+ring[j][0],cy+ring[j][1],zf),edge,(nx,ny,0))
        b1=add((cx+ring[i][0],cy+ring[i][1],zb),edge,(nx,ny,0)); b2=add((cx+ring[j][0],cy+ring[j][1],zb),edge,(nx,ny,0))
        F+= [(a1,a2,b2),(a1,b2,b1),(a1,b2,a2),(a1,b1,b2)]
    V=np.array(V,np.float32); uv=np.array(UV,np.float32); nrm=np.array(N,np.float32)
    V,nrm=tilt(V,nrm,T)
    tan=np.tile(np.array([1,0,0],np.float32),(len(V),1))
    idx=np.array([v for f in F for v in f],np.uint32)
    h=lambda a: np.asarray(a,np.float16).view(np.uint16)
    # Layout wie Auto-Badges: NORMAL, TEXCOORD0..3, TANGENT0, TANGENT1  (Stride 40)
    st=np.zeros((len(V),20),np.uint16)
    st[:,0:3]=h(nrm); u=np.clip(np.round(uv*65535),0,65535).astype(np.uint16)
    for k in range(4): st[:,4+2*k:6+2*k]=u
    st[:,12:15]=h(tan); st[:,15]=h([1.0]*len(V)); st[:,16:19]=h(tan); st[:,19]=h([1.0]*len(V))
    return V,st.tobytes(),idx

def pack_attr(V,uv,nrm,tan):
    h=lambda a: np.asarray(a,np.float16).view(np.uint16)
    st=np.zeros((len(V),20),np.uint16)
    st[:,0:3]=h(nrm); u=np.clip(np.round(uv*65535),0,65535).astype(np.uint16)
    for k in range(4): st[:,4+2*k:6+2*k]=u
    st[:,12:15]=h(tan); st[:,15]=h([1.0]*len(V)); st[:,16:19]=h(tan); st[:,19]=h([1.0]*len(V))
    return st.tobytes()
def screw_geom(T):
    # zwei Nieten wie FH5 (Positionen aus PlateEU_a Material 3), flache Kuppel 8 mm, 1.6 mm hoch
    sc=float(os.environ.get('SCALE','1')); W,H,TH=0.520*sc,0.110*sc,0.006*sc
    cx,cy,cz=T['cx'],T['cy'],T['cz']; zf=cz-TH/2
    R=0.004*sc; Hh=0.0016*sc; seg=16; rings=5
    V=[];UV=[];N=[];F=[]
    for (u,v) in ((0.125,0.402),(0.8555,0.3995)):
        px,py=cx+(u-0.5)*W, cy+(0.5-v)*H
        base=len(V)
        V.append((px,py,zf-Hh)); UV.append((0.5,0.5)); N.append((0,0,-1))
        for r in range(1,rings+1):
            t=r/rings; a=t*np.pi/2*0.95
            rr=R*np.sin(a)/np.sin(np.pi/2*0.95); z=zf-Hh*np.cos(a)/1.0
            for k in range(seg):
                p=2*np.pi*k/seg; dx,dy=np.cos(p),np.sin(p)
                n=np.array([dx*np.sin(a),dy*np.sin(a),-np.cos(a)]); n/=np.linalg.norm(n)
                V.append((px+rr*dx,py+rr*dy,z)); UV.append((0.5+0.5*t*dx,0.5+0.5*t*dy)); N.append(tuple(n))
        for k in range(seg):
            j=(k+1)%seg
            F+=[(base,base+1+k,base+1+j),(base,base+1+j,base+1+k)]
            for r in range(rings-1):
                a0=base+1+r*seg; a1=a0+seg
                q=[(a0+k,a1+k,a1+j),(a0+k,a1+j,a0+j)]
                F+=q+[(x,z_,y) for x,y,z_ in q]
    V=np.array(V,np.float32); uv=np.array(UV,np.float32); nrm=np.array(N,np.float32)
    tan=np.tile(np.array([1,0,0],np.float32),(len(V),1))
    return V,pack_attr(V,uv,nrm,tan),np.array([v for f in F for v in f],np.uint32)
def patch(carrier_bytes,T,tpl_vlay,mname='HZN_plate',parent=None,off=None,rp=None,geom=None,shadow=True):
    parent=parent or PARENT
    b=fbundle.Bundle.load(carrier_bytes); bl=b.blobs
    V,attr,idx=(geom or plate_geom)(T); nv=len(V); ni=len(idx)
    # Traeger-Teil kann knochen-lokal sein: Versatz = Welt-BBox (Modl) - Vertex-BBox
    _p=[x for x in bl if x.tag==b'BreV' and x.meta(b'  dI').data==b'\xff\xff\xff\xff'][0]
    _c=struct.unpack_from('<I',_p.data,0)[0]; _P=np.frombuffer(_p.data[16:16+_c*12],'<f4').reshape(-1,3)
    _m=[x for x in bl if x.tag==b'ldoM'][0].meta(b'xoBB').data; _w=np.frombuffer(_m[:24],'<f4')
    if off is None: off=((_w[:3]+_w[3:])/2-(_P.min(0)+_P.max(0))/2).astype(np.float32)
    print('Traeger-Versatz',off)
    V=V-off
    idof=lambda x:(lambda m:struct.unpack('<I',m.data)[0] if m else None)(x.meta(b'  dI'))
    mats=[x for x in bl if x.tag==b'ItaM']
    newid=max(idof(m) for m in mats)+1
    # Material
    # Material-Instanz (ITAM) wie bei allen Auto-Materialien: verweist auf unsere Bibliotheks-materialbin
    def _isitam(x):
        try: return fbundle.Bundle.load(x.data).blobs[0].tag==b'ITAM'
        except Exception: return False
    tplm=[x for x in mats if _isitam(x)][0]
    ib_=fbundle.Bundle.load(tplm.data)
    par=parent.encode()
    ib_.blobs[0].data=bytes([len(par)])+par
    for y in ib_.blobs[0].metas:
        if y.tag==b'emaN': y.data=mname.lower().encode()
    ib_.blobs[1].data=rp if rp else bytes(13)
    nm=fbundle.Blob(b'ItaM',1,0,[fbundle.Meta(b'emaN',0,mname.encode()),fbundle.Meta(b'  dI',0,struct.pack('<I',newid))],ib_.dump())
    # Stream-Basis: Anzahl bestehender Positionsvertices
    pos=[x for x in bl if x.tag==b'BreV' and idof(x)==0xffffffff][0]
    base=struct.unpack_from('<I',pos.data,0)[0]
    pos.data=struct.pack('<IIHBBI',base+nv,(base+nv)*12,12,1,0,6)+pos.data[16:]+V.astype('<f4').tobytes()
    # Indizes
    ib=[x for x in bl if x.tag==b'BdnI'][0]
    ic=struct.unpack_from('<I',ib.data,0)[0]
    ib.data=struct.pack('<IIHBBI',ic+ni,(ic+ni)*4,4,1,0,42)+ib.data[16:]+(idx+base).astype('<u4').tobytes()
    # neues VLay + VerB
    # Mesh-Feld 49 ist der INDEX in der VLay-Liste (nicht die Id) -> Id = Index setzen
    lidx=sum(1 for x in bl if x.tag==b'yaLV')
    vl=fbundle.Blob(b'yaLV',1,1,[fbundle.Meta(b'  dI',0,struct.pack('<I',lidx))],tpl_vlay)
    bid=max([idof(x) for x in bl if x.tag in (b'BreV',b'fuBM') and idof(x) is not None and idof(x)!=0xffffffff]+[newid-1])+1
    vb=fbundle.Blob(b'BreV',1,0,[fbundle.Meta(b'  dI',0,struct.pack('<I',bid))],struct.pack('<IIHBBI',nv,nv*40,40,7,0,10)+attr)
    # Mesh-Eintraege: je LOD ein Eintrag (Kopie des ersten Materials-Eintrags der LOD), Material -> newid
    bb=np.concatenate([V.min(0),V.max(0)]).astype('<f4').tobytes()
    meshes=[x for x in bl if x.tag==b'hseM']
    seen={};new=[]
    for m in meshes:
        nme=fbundle.name(m)
        if nme=='Shadow' or nme in seen or fbundle.name(m) is None: continue
        if struct.unpack_from('<H',m.data,0)[0]>=newid-0 and False: continue
        seen[nme]=1
        d=bytearray(m.data); struct.pack_into('<H',d,0,newid)
        struct.pack_into('<I',d,25,ic); struct.pack_into('<I',d,33,ni); struct.pack_into('<I',d,37,ni//3); struct.pack_into('<I',d,45,nv); struct.pack_into('<i',d,29,-base); struct.pack_into('<I',d,49,lidx); struct.pack_into('<I',d,53,2); struct.pack_into('<I',d,69,base*12); struct.pack_into('<I',d,73,bid); struct.pack_into('<I',d,77,1); struct.pack_into('<I',d,81,40); struct.pack_into('<20f',d,105,*([0.0,1.0,0.0,1.0]*5)); d[12]=0; d[13]=0; struct.pack_into('<I',d,89,0xffffffff)
        mm=[fbundle.Meta(x.tag,x.ver,bb if x.tag==b'xoBB' else x.data) for x in m.metas]
        new.append(fbundle.Blob(b'hseM',m.maj,m.mn,mm,bytes(d)))
    # Schatten-Meshes (Material ffff, Positions-Layout) je LOD, damit das Teil Schatten wirft
    if shadow:
        sl={}
        for m in meshes:
            if fbundle.name(m)=='Shadow' and m.data[4:8] not in sl: sl[m.data[4:8]]=m
        for m in sl.values():
            d=bytearray(m.data); struct.pack_into('<I',d,25,ic); struct.pack_into('<I',d,33,ni); struct.pack_into('<I',d,37,ni//3)
            mm=[fbundle.Meta(x.tag,x.ver,bb if x.tag==b'xoBB' else x.data) for x in m.metas]
            new.append(fbundle.Blob(b'hseM',m.maj,m.mn,mm,bytes(d)))
    # Einsortieren
    def last(tag): return max(i for i,x in enumerate(bl) if x.tag==tag)
    bl.insert(last(b'BreV')+1,vb)
    bl.insert(last(b'yaLV')+1,vl)
    for n in reversed(new): bl.insert(last(b'hseM')+1,n)
    bl.insert(last(b'ItaM')+1,nm)
    modl=[x for x in bl if x.tag==b'ldoM'][0]
    p=json.load(open(O+'paths.json'))
    cr=[zlib.crc32(p[k].encode().lower()) for k in ('diff','nrml','glos')]
    import re as _re
    if rp: cr+= [zlib.crc32(x.lower()) for x in _re.findall(rb'Game:[^\x00]*?swatchbin',rp)]
    for m in modl.metas:
        if m.tag==b'feRT':
            n,=struct.unpack_from('<I',m.data,0); old=list(struct.unpack_from('<%dI'%n,m.data,4))
            allc=old+[c for c in dict.fromkeys(cr) if c not in old]; m.data=struct.pack('<I',len(allc))+struct.pack('<%dI'%len(allc),*allc)
        if m.tag==b'xoBB':
            o=np.frombuffer(m.data[:24],'<f4'); u=np.concatenate([np.minimum(o[:3],V.min(0)),np.maximum(o[3:],V.max(0))]).astype('<f4'); m.data=u.tobytes()
    md=bytearray(modl.data); c=struct.unpack_from('<4H',md,0)
    struct.pack_into('<4H',md,0,c[0]+len(new),c[1]+1,c[2]+1,c[3]+1); modl.data=bytes(md)
    return b.dump(),len(new),newid,off
if __name__=='__main__':
    car,src,dst=sys.argv[1:4]
    T=json.load(open('/mnt/user-data/outputs/b/plate_table_eu.json'))[car]
    for k in ('cx','cy','cz'):
        if os.environ.get('D'+k.upper()): T[k]=float(os.environ['D'+k.upper()])
    tv=open(O+'vlay_badge40.bin','rb').read()
    out,n,nid,_=patch(open(src,'rb').read(),T,tv)
    open(dst,'wb').write(out); print('ok',n,'Meshes, Material-Id',nid,len(out))
