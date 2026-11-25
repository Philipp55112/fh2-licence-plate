# Leuchtende Kennzeichen-Lampenglaeser: Glas-Dreiecke der erkannten Lampen aus dem FH2-Automodell kopieren,
# 0,4 mm nach aussen versetzen und als eigenes Mesh (Material gls_lightbulb, Licht-ID 13 = Abblendlicht)
# in die HZN-Modelle des Kennzeichen-Pakets einbauen. Ergebnis: Glas leuchtet, sobald das Fahrzeuglicht an ist.
import sys,os,json,glob,zipfile,struct,re
sys.path.insert(0,'/home/claude/work'); sys.path.insert(0,'/home/claude/work/light')
import numpy as np, fbundle, patch_carrier as pc, light.fh2mat as M
PAR='Game:\\Media\\cars\\_library\\materials\\basematerials\\gls_lightbulb.materialbin'
# RPTM einer echten Abblendlicht-Birne (PAG_Huayra_12 headlightLBulbs_a, gls_lightbulb, Licht-ID 13, weiss)
RP=bytes.fromhex('040200a0733049040d0000000200bfd92519010000803f0000803f0000803f0000803f02008dba0ec2020ad7a33c02006ed4ce9b010000803f0000803f0000803f0000803fa2f2eac30000000000000000')
MAT='HZN_lt'; OFFS=0.0004
from lightcolor import car_rgb
def rp_for(car):
    r,g,b=car_rgb(car); w=struct.pack('<4f',r,g,b,1.0); one=struct.pack('<4f',1,1,1,1)
    assert RP.count(one)==2
    return RP.replace(one,w)
rows={}
for f in sorted(glob.glob('/tmp/a37_*.jsonl')):
    for l in open(f):
        try: d=json.loads(l)
        except: continue
        if d.get('c'): rows.setdefault(d['car'],d)
src={}
for p in glob.glob('/mnt/user-data/uploads/Forza Horizon 2/Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount/media/cars/*.zip')+glob.glob('/mnt/user-data/uploads/Kennzeichen_Arbeit/plate_export/fh2_cars/*.zip'):
    src.setdefault(os.path.basename(p)[:-4].lower(),p)
det=json.load(open('/home/claude/work/light/fh2_detect.json'))
def frame(car):
    r=rows[car]; c=np.array(r['c']); n=np.array(r['n'],float); n/=np.linalg.norm(n)
    right=np.array([1.0,0,0]); right-=n*right@n; right/=np.linalg.norm(right); up=np.cross(n,right)
    if up[1]<0: up=-up
    return c,right,up,n
LAMPISH=re.compile(r'(?i)(^gls|lens|bulb|clear|licen|platelight|reflector|illum)')
def synth(car,cl):
    """Lampe ohne eigenes Glas im FH2-Modell (Gehaeuse): kleines leuchtendes Glas, zur Platte gerichtet"""
    c,right,up,n=frame(car); cu=np.array(cl['c'])
    P=c+cu[0]*right+cu[1]*up+cu[2]*n
    r=json.loads(open('/home/claude/work/light/_hw.json').read()) if False else None
    W2=rows[car]['W']/2000.0; H2=rows[car]['H']/2000.0
    single=abs(cu[0])<0.03
    a=0.032 if single else 0.014; b=0.0045                # Halbachsen (m), klein: im Aus-Zustand kein auffaelliger Punkt
    if cl.get('pos')=='seite': fdir=-np.sign(cu[0])*right; ax1=up                 # zur Platte hin
    elif cu[1]<0: fdir=up; ax1=right                                                    # unter der Platte: nach oben
    else: fdir=-up; ax1=right                                                           # ueber der Platte: nach unten
    fdir=fdir*0.9+n*0.3; fdir/=np.linalg.norm(fdir)           # fast flach zur Platte: von hinten kaum sichtbar, beleuchtet aber das Schild
    ax2=np.cross(fdir,ax1); ax2/=np.linalg.norm(ax2); ax1=np.cross(ax2,fdir)
    ctr=P-0.0015*n-0.001*fdir; k=16; pts=[ctr+a*np.cos(2*np.pi*i/k)*ax1+b*np.sin(2*np.pi*i/k)*ax2 for i in range(k)]   # leicht ins Gehaeuse versenkt
    return np.array([[ctr,pts[i],pts[(i+1)%k]] for i in range(k)])
def facedir(cl,right,up,n):
    cu=cl['c']
    if cl.get('pos')=='seite': f=-np.sign(cu[0])*right
    elif cu[1]<0: f=up
    else: f=-up
    v=f+0.5*n; return v/np.linalg.norm(v)
def lamp_tris(car):
    """-> {datei_stem: [(Dreiecke (k,3,3), cluster)]}"""
    c,right,up,n=frame(car); per={}
    allt=M.car(src[car.lower()])
    for cl in det[car]:
        if cl.get('native'): continue                   # Glas leuchtet im Spiel schon (Ruecklicht-Funktion)
        if cl.get('pick'):                               # v72: echte Flaechen eines Materials in einem Welt-Kasten, nach Richtung gefiltert, am Kasten abgeschnitten
            out=[]
            for pk in (cl['pick'] if isinstance(cl['pick'],list) else [cl['pick']]):
             B=np.array(pk['box'],float).reshape(3,2); dv=np.array(pk['dir'],float); dv/=np.linalg.norm(dv)
             for f,mn,me,T in allt:
                 if mn.lower()!=pk['mat'].lower() or not f.lower().startswith(pk['file'].lower()): continue
                 C=T.mean(1); nn=np.cross(T[:,1]-T[:,0],T[:,2]-T[:,0]); l=np.linalg.norm(nn,axis=1); ok=l>1e-12; nn[ok]/=l[ok,None]
                 m=ok&(np.abs(nn@dv)>pk.get('cos',0.85))
                 for a in range(3): m&=(T[:,:,a].max(1)>B[a,0])&(T[:,:,a].min(1)<B[a,1])
                 for t in T[m]:
                     poly=[p for p in t]
                     for a in range(3):
                         for lim,sg in ((B[a,0],1),(B[a,1],-1)):
                             new=[]
                             for i in range(len(poly)):
                                 p,q=poly[i],poly[(i+1)%len(poly)]; ip=sg*(p[a]-lim)>=0; iq=sg*(q[a]-lim)>=0
                                 if ip: new.append(p)
                                 if ip!=iq: tt=(lim-p[a])/(q[a]-p[a]); new.append(p+tt*(q-p))
                             poly=new
                             if len(poly)<3: break
                         if len(poly)<3: break
                     sh=np.array(pk.get('push',[0,0,0]),float)       # v80: gegen Z-Fighting von der Flaeche weg schieben
                     for i in range(1,len(poly)-1): out.append([poly[0]+sh,poly[i]+sh,poly[i+1]+sh])
            if out: per.setdefault('__synth__',[]).append((np.array(out),cl)); continue
        if cl.get('poly'):                               # v65: Leuchtflaeche aus gemessenen Eckpunkten (Welt), 0,5 mm vor der Flaeche
            Q=np.array(cl['poly'],float); ctr=Q.mean(0); nn=np.cross(Q[1]-Q[0],Q[2]-Q[0]); nn/=np.linalg.norm(nn)
            if nn@facedir(cl,right,up,n)<0: nn=-nn
            Q=Q+0.0005*nn; ctr=Q.mean(0)
            per.setdefault('__synth__',[]).append((np.array([[ctr,Q[i],Q[(i+1)%len(Q)]] for i in range(len(Q))]),cl)); continue
        cu=np.array(cl['c']); ext=np.array(cl['ext']); ms=set(cl.get('m') or [])
        if any(LAMPISH.search(m.split('@')[0]) for m in ms): ms={m for m in ms if LAMPISH.search(m.split('@')[0])}   # v64: Chrom/Gehaeuse nicht leuchten lassen, wenn Glas bekannt
        manual=cl.get('n',0)==0 or not ext.any()
        want=None; wfile=None
        if cl.get('opening') or cl.get('synth'): manual='synth'
        if manual is True:
            m0=(cl.get('m') or [''])[0]
            force=m0.startswith('!'); m0=m0.lstrip('!')
            if '@' in m0:
                want,wfile=m0.split('@',1); want=want.strip().lower(); wfile=wfile.strip().lower()
                if not force and not LAMPISH.search(want): want=None; wfile=None; manual='synth'     # Gehaeuse/Lack: nicht leuchten lassen
        found=False
        for f,mn,me,T in allt:
            C=T.mean(1)-c; q=np.stack([C@right,C@up,C@n],1)
            if not manual:
                if ms and mn not in ms and not any(m.split('@')[0]==mn and f.lower().startswith(m.split('@')[1].lower()) for m in ms if '@' in m): continue
                sel=np.all(np.abs(q-cu)<=ext/2+0.012,axis=1)
            elif want:
                if mn.lower()!=want or not f.lower().startswith(wfile): continue
                sel=np.linalg.norm(q-cu,axis=1)<(cl.get('r') or (0.03 if force else 0.06))
            elif manual=='synth': break
            else:
                if not LAMPISH.search(mn) or NOGROW.search(mn): continue
                sel=np.linalg.norm(q-cu,axis=1)<0.035
            if sel.any() and cl.get('comp'):          # v67: nur das zusammenhaengende Teil, das der Lampe am naechsten liegt
                Ts=T[sel]; P0=c+cu[0]*right+cu[1]*up+cu[2]*n
                key=np.round(Ts.reshape(-1,3)*5000).astype(np.int64); _,inv=np.unique(key,axis=0,return_inverse=True); inv=inv.ravel()
                from scipy.sparse import coo_matrix; from scipy.sparse.csgraph import connected_components
                k=len(Ts); g=coo_matrix((np.ones(3*k),(np.repeat(np.arange(k),3),inv)),shape=(k,inv.max()+1)).tocsr()
                _,lab=connected_components(g@g.T,directed=False)
                i0=np.argmin(np.linalg.norm(Ts.mean(1)-P0,axis=1)); keep=lab==lab[i0]
                sel=sel.copy(); sel[np.nonzero(sel)[0][~keep]]=False
            if sel.any() and cl.get('half'):          # v70: nur Flaechen, die zur Platte zeigen (Knubbel ohne Glas: eingearbeitete Lampe)
                Ts=T[sel]; vd=facedir(cl,right,up,n); ctr=Ts.reshape(-1,3).mean(0)
                nn=np.cross(Ts[:,1]-Ts[:,0],Ts[:,2]-Ts[:,0]); ln=np.linalg.norm(nn,axis=1); nn=nn/np.maximum(ln,1e-12)[:,None]
                nn*=np.sign(((Ts.mean(1)-ctr)*nn).sum(1))[:,None]           # nach aussen (weg von der Knubbel-Mitte)
                keep=(nn@vd>cl.get('half_cos',0.25))&(ln>1e-12)
                if cl.get('halfpos'):                  # v73: Knubbel mit Ebene quer zur Platte schneiden -> gerade Kante statt Zacken
                    lat=vd-(vd@n)*n; lat/=max(np.linalg.norm(lat),1e-9)
                    pr=Ts.reshape(-1,3)@lat; cut=pr.min()+(pr.max()-pr.min())*cl.get('cut',0.55)
                    out=[]
                    for t in Ts:
                        poly=list(t); new=[]
                        for i in range(3):
                            p,q=poly[i],poly[(i+1)%3]; ip=p@lat>=cut; iq=q@lat>=cut
                            if ip: new.append(p)
                            if ip!=iq: tt=(cut-p@lat)/((q-p)@lat); new.append(p+tt*(q-p))
                        for i in range(1,len(new)-1): out.append([new[0],new[i],new[i+1]])
                    if out: per.setdefault(f.lower()[:-9],[]).append((np.array(out),cl)); found=True
                    continue
                sel=sel.copy(); sel[np.nonzero(sel)[0][~keep]]=False
            if sel.any(): per.setdefault(f.lower()[:-9],[]).append((T[sel] if cl.get('nogrow') or manual else grow(T[sel],f,allt,cl),cl)); found=True
        if not found:
            op=opening(car,cl,allt) if (manual and not cl.get('synth')) else None
            per.setdefault('__synth__',[]).append((op if op is not None else synth(car,cl),cl))
    return per
NOGROW=re.compile(r'(?i)(red|amber|orange|tail|limo|projector|smoke|window|windshield|tint|mirror|backlight)')
def grow(seed,f,allt,cl):
    """v64: ganzes Glas statt nur der erkannten Teilflaeche: Glas-Dreiecke (alle Dateien, ohne Rot/Rueckleuchte/Scheibe),
    die an die Saat grenzen (Ecke <= 2,5 mm), schrittweise, max. 5 cm um die Saat"""
    if cl.get('nogrow'): return seed
    from scipy.spatial import cKDTree
    pool=[T for ff,mn,me,T in allt if LAMPISH.search(mn) and not NOGROW.search(mn)]
    if not pool: return seed
    P=np.concatenate(pool); ctr=seed.reshape(-1,3).mean(0)
    P=P[np.linalg.norm(P.mean(1)-ctr,axis=1)<0.08]
    if not len(P): return seed
    V=P.reshape(-1,3); tri_of=np.repeat(np.arange(len(P)),3); kd=cKDTree(V)
    front=seed.reshape(-1,3); inc=np.zeros(len(P),bool)
    for _ in range(60):
        hits=set()
        for lst in kd.query_ball_point(front,0.0025):
            hits.update(lst)
        new=np.unique(tri_of[list(hits)]) if hits else np.array([],int)
        new=new[~inc[new]]
        if not len(new): break
        inc[new]=True; front=P[new].reshape(-1,3)
    if not inc.any(): return seed
    return np.concatenate([seed,P[inc]])
def opening(car,cl,allt):
    """Phil 24.11.: Lampe ohne Glas = Oeffnung/Gehaeuse -> flache Leuchtflaeche buendig auf die zur Platte zeigende Gehaeuseflaeche"""
    c,right,up,n=frame(car); cu=np.array(cl['c']); P=c+cu[0]*right+cu[1]*up+cu[2]*n
    if cl.get('pos')=='seite': fdir=-np.sign(cu[0])*right
    elif cu[1]<0: fdir=up
    else: fdir=-up
    R=cl.get('r',0.025); cand=[]
    for f,mn,me,T in allt:
        if 'paint' in mn.lower() and not cl.get('paint'): continue
        if cl.get('omat') and mn.lower()!=cl['omat'].lower(): continue
        C=T.mean(1); m=np.linalg.norm(C-P,axis=1)<R
        if not m.any(): continue
        Tm=T[m]; nn=np.cross(Tm[:,1]-Tm[:,0],Tm[:,2]-Tm[:,0]); l=np.linalg.norm(nn,axis=1); ok=l>1e-10; nn=nn[ok]/l[ok][:,None]; Tm=Tm[ok]
        k=np.abs(nn@fdir)>0.6
        if k.any(): cand.append(Tm[k])
    if not cand: return None
    T=np.concatenate(cand); Pp=T.reshape(-1,3); ctr=Pp.mean(0)
    U,S_,Vt=np.linalg.svd(Pp-ctr); e1,e2,nn=Vt
    if nn@fdir<0: nn=-nn
    from scipy.spatial import ConvexHull
    Q=np.stack([(Pp-ctr)@e1,(Pp-ctr)@e2],1)
    try: hull=ConvexHull(Q)
    except Exception: return None
    if hull.volume>cl.get('amax',0.0025) or hull.volume<2e-5: return None          # Flaeche 0,2..25 cm2
    dev=((Pp-ctr)@nn).max(); off=ctr+(dev+0.0005)*nn
    ring=[off+a*e1+b*e2 for a,b in Q[hull.vertices]]
    return np.array([[off,ring[i],ring[(i+1)%len(ring)]] for i in range(len(ring))])
def geom(tris,car):
    c,right,up,n=frame(car)
    V=[];N=[]
    for T,cl in tris:
        cu=np.array(cl['c']); vd=facedir(cl,right,up,n)     # v64: Glas zeigt zur Platte/nach aussen (statt Referenzpunkt)
        for t in T:
            nn=np.cross(t[1]-t[0],t[2]-t[0]); l=np.linalg.norm(nn)
            if l<1e-12: continue
            nn/=l
            if nn@vd<0: nn=-nn
            for sg in (1.0,):                     # v64: aussen (Phil: Glas soll mitleuchten), 0,4 mm, folgt der Glasform
                p=t+sg*OFFS*nn
                V+= [p[0],p[1],p[2]]; N+=[sg*nn,sg*nn,sg*nn]
    # Volle Linse: konvexe Huelle je Cluster als leuchtende Flaeche (manche Glaeser sind nur ein Rand, z. B. RS6)
    from scipy.spatial import ConvexHull
    for T,cl in tris:
        P=T.reshape(-1,3)
        if len(P)<3 or cl.get('nohull'): continue
        ctr=P.mean(0); U,S_,Vt=np.linalg.svd(P-ctr); e1,e2,nn=Vt[0],Vt[1],Vt[2]
        cu=np.array(cl['c']); vd=facedir(cl,right,up,n)
        if nn@vd<0: nn=-nn                                   # nn zeigt nach aussen
        Q=np.stack([(P-ctr)@e1,(P-ctr)@e2],1)
        try: hull=ConvexHull(Q)
        except Exception: continue
        ring=Q[hull.vertices]; dev=np.abs((P-ctr)@nn).max()
        for sg in ((1.0,) if dev<0.002 else (-1.0,)):          # flaches Glas: Flaeche aussen, gewoelbtes: innen (steht nicht raus)
            off=ctr+sg*(dev+OFFS)*nn
            pts=[off+a*e1+b*e2 for a,b in ring]; m=off
            for i in range(len(pts)):
                V+=[m,pts[i],pts[(i+1)%len(pts)]]; N+=[sg*nn]*3
    V=np.array(V,np.float32); N=np.array(N,np.float32); k=len(V)//3
    idx=[]
    for i in range(k): idx+= [3*i,3*i+1,3*i+2, 3*i,3*i+2,3*i+1]     # beide Wicklungen
    uv=np.full((len(V),2),0.5,np.float32)
    tan=np.cross(N,np.array([0,1,0],np.float32)); bad=np.linalg.norm(tan,axis=1)<1e-3; tan[bad]=np.cross(N[bad],np.array([1,0,0],np.float32))
    tan/=np.linalg.norm(tan,axis=1)[:,None]
    return V,pc.pack_attr(V,uv,N,tan.astype(np.float32)),np.array(idx,np.uint32),k
def process(car,zin,outdir,log):
    names=[x for x in zin.namelist() if x.lower().startswith(('media/cars/'+car+'/').lower())]
    hz=[x for x in names if x.endswith('.modelbin')]
    cj=[x for x in names if x.endswith('.carbin.json')]
    if not hz or not cj: log.append((car,'kein HZN-Modell im Paket')); return []
    per=lamp_tris(car)
    if not per: log.append((car,'keine Glas-Dreiecke gefunden')); return []
    stems={h.split('/')[-1][4:-9]:h for h in hz}          # hzn_<stem>.modelbin
    assign={h:[] for h in hz}
    common=[]
    for st,lst in per.items():
        if st in stems: assign[stems[st]]+=lst
        else: common+=lst
    if common:
        # Lampen in Teilen ohne eigenes HZN-Modell: nur eine Quelldatei je Cluster verwenden (keine Doppel)
        # v135: Schluessel = Cluster + Teil ohne Varianten-Endung -> Birne UND Streuscheibe (verschiedene Teile) bleiben, nur _a/_b-Doppel fallen weg
        chosen={}; uniq=[]
        for st,lst in per.items():
            if st in stems: continue
            for T,cl in lst:
                key=(tuple(cl['c']),st.rsplit('_',1)[0])
                if chosen.setdefault(key,st)!=st: continue      # gleiche Lampe aus einer anderen Variante (_b, _c ...) -> Doppel
                uniq.append((T,cl))
        for h in hz: assign[h]+=uniq
    # v64: Tuning-Varianten (z. B. bumperr_g zu bumperr_a): Glas uebernehmen, wenn das Variantenteil an der Lampe Geometrie hat (<= 1 cm)
    from scipy.spatial import cKDTree
    allt=None
    for h in hz:
        if assign[h]: continue
        st=h.split('/')[-1][4:-9]; base=st.rsplit('_',1)[0]
        sib=[x for x in per if x!=st and x.rsplit('_',1)[0]==base]
        other=[x for x in per if x!='__synth__' and x.rsplit('_',1)[0]!=base and x not in stems]
        for x in other:                                   # Lampe sitzt an einem anderen Teil ohne eigenes HZN-Modell -> immer mitnehmen
            pass
        if not sib:
            # Lampe an anderem Teil (z. B. Stossstange), Platte an Tuning-Kofferraum: Glas der Stamm-Variante uebernehmen
            src_st=[x for x in per if x in stems and x.rsplit('_',1)[0]!=base]
            for x in src_st:
                for T,cl in per[x]: assign[h].append((T,cl)); log.append((car,'Fremdteil-Lampe',st,'von',x))
            continue
        if allt is None: allt=M.car(src[car.lower()])
        mine=[T for f,mn,me,T in allt if f.lower()[:-9]==st]
        if not mine: continue
        kd=cKDTree(np.concatenate(mine).reshape(-1,3))
        for x in sib:
            for T,cl in per[x]:
                dd,_=kd.query(T.reshape(-1,3).mean(0))
                if dd<=0.01: assign[h].append((T,cl)); log.append((car,'Tuning-Variante',st,'von',x))
    done=[]
    for h,lst in assign.items():
        if not lst: continue
        data=zin.read(h)
        V,attr,idx,k=geom(lst,car)
        if k==0: continue
        out,nm,nid,_=pc.patch(data,{},open(pc.O+'vlay_badge40.bin','rb').read(),MAT,PAR,np.zeros(3,np.float32),rp_for(car),geom=lambda T,_g=(V,attr,idx):_g,shadow=False)
        op=os.path.join(outdir,h); os.makedirs(os.path.dirname(op),exist_ok=True); open(op,'wb').write(out)
        done.append(h); log.append((car,'Lampenglas',h.split('/')[-1],'%d Dreiecke'%k))
    if done:
        j=json.loads(zin.read(cj[0]))
        paths={('game:\\media\\cars\\%s\\scene\\exterior\\hzn\\%s'%(car.lower(),h.split('/')[-1].lower())) for h in done}
        for sec in ('parts','groups'):
            for p in j.get(sec,[]):
                for m in p.get('models',[]):
                    v=m['value']
                    if v['path'].lower() in paths and not any(x['name']==MAT for x in v['material_indices']):
                        v['material_indices'].append({'name':MAT,'value':0})
        op=os.path.join(outdir,cj[0]); os.makedirs(os.path.dirname(op),exist_ok=True)
        json.dump(j,open(op,'w'),indent=2)
    return done
if __name__=='__main__':
    zin=zipfile.ZipFile(sys.argv[1]); outdir=sys.argv[2]; cars=sys.argv[3:] or sorted(det)
    log=[]
    for car in cars:
        try: process(car,zin,outdir,log)
        except Exception as e:
            import traceback; traceback.print_exc(); log.append((car,'FEHLER',repr(e)[:200]))
    for l in log: print(*l)
