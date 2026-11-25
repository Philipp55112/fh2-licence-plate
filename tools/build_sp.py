import sys,os,json,zipfile,re
sys.path.insert(0,'/home/claude/work')
import numpy as np, fbundle, patch_carrier as pc, light.spieler_mat as S
BASE=sys.argv[1]; LTOUT=sys.argv[2]; OUT=sys.argv[3]
ORD=json.load(open('/home/claude/work/light/ordinals.json'))
src=zipfile.ZipFile(BASE)
def get(n):
    p=os.path.join(LTOUT,n)
    return open(p,'rb').read() if os.path.exists(p) else src.read(n)
LIBM='media/cars/_library/materials/badges/'
EU_LT=src.read(LIBM+'hzn_plate_lp3.materialbin'); US_LT=src.read(LIBM+'hzn_plate_us.materialbin')
PAR={'eu':S.LIB+'hzn_plate_lp3.materialbin','us':S.LIB+'hzn_plate_us.materialbin'}
SP={'eu':S.LIB+'hzn_plate_lp3_sp.materialbin','us':S.LIB+'hzn_plate_us_sp.materialbin'}
XH=('a1b2c301','a1b2c302','a1b2c303')
TX='Game:\\Media\\cars\\_library\\textures\\hzn\\'
SPTEX={'eu':{'d98cd96d':TX+'hzn_plate_atlas2_sp.swatchbin','9424d990':TX+'hzn_plate_nrml3_sp.swatchbin','6e7a2ec0':TX+'hzn_plate_glos3_sp.swatchbin'},
       'us':{'d98cd96d':TX+'hzn_us_atlas_sp.swatchbin','9424d990':TX+'hzn_us_nrml_sp.swatchbin','6e7a2ec0':TX+'hzn_us_glos_sp.swatchbin'}}
newfiles={}; log=[]
tv=open(pc.O+'vlay_badge40.bin','rb').read()
FIT=json.load(open('/home/claude/work/v32/fit_v36.json'))
CEN={}
for v in FIT.values():
    CEN.setdefault(v['car'],[]).append((np.array(v['c'],np.float32),np.array(v['n'],np.float32)))
cars=sorted({n.split('/')[2] for n in src.namelist() if n.startswith('media/cars/') and '/scene/Exterior/HZN/' in n})
for car in cars:
    hz=[n for n in src.namelist() if n.startswith('media/cars/%s/scene/Exterior/HZN/'%car) and n.endswith('.modelbin')]
    cj=[n for n in src.namelist() if n.startswith('media/cars/%s/'%car) and n.endswith('.carbin.json')][0]
    ident=ORD.get(car)
    if not ident: log.append((car,'keine Id')); continue
    typ=None; done=[]
    for h in hz:
        b=fbundle.Bundle.load(get(h)); bl=[x for x in b.blobs if x.tag==b'ItaM' and fbundle.name(x)=='HZN_p1']
        if not bl: continue
        par,_=S.itam_parent(bl[0]); t='eu' if par.lower()==PAR['eu'].lower() else 'us' if par.lower()==PAR['us'].lower() else None
        if not t: log.append((car,h,'unbekanntes Material',par)); continue
        typ=t; own=S.LIB+'hzn_plate_%s_%s.materialbin'%(t,car.lower())
        S.set_itam_parent(bl[0],own)
        # verstecktes Mesh (3 gleiche Punkte) mit dem Spieler-Material, damit dessen Texturen geladen sind
        data=b.dump()
        newfiles[h]=data; done.append(h)
    if not done: log.append((car,'kein HZN_p1')); continue
    lt=EU_LT if typ=='eu' else US_LT
    newfiles[LIBM+'hzn_plate_%s_%s.materialbin'%(typ,car.lower())]=S.ltam_with(lt,{k:S.encode(v,ident) for k,v in S.tex_paths(lt).items()},extra=list(zip(XH,[SPTEX[typ]['d98cd96d'],SPTEX[typ]['9424d990'],SPTEX[typ]['6e7a2ec0']])))
    newfiles[cj]=get(cj)
    log.append((car,typ,ident,len(done)))
json.dump({k:None for k in newfiles},open(OUT+'.list.json','w'))
import pickle; pickle.dump(newfiles,open(OUT+'.pkl','wb'))
print(len(cars),'Autos', sum(1 for l in log if len(l)==4),'mit eigenem Material'); [print(*l) for l in log if len(l)!=4][:20]
