# v1: PlateParts.csv = relpfad;feld-offset im Auto-Objekt;upgrade-ids (nur Teile mit mehreren HZN-Varianten)
import zipfile,json,collections,sys
z=zipfile.ZipFile(sys.argv[1])
FIELD={'BumperR':0x1B4,'BumperF':0x1B0,'Hood':0x1B8,'SideSkirts':0x1BC,'Trunk':0x14C}  # Trunk = RearWing (ungeprueft)
groups=collections.defaultdict(list)
for n in z.namelist():
    if not n.endswith('.carbin.json'): continue
    d=json.loads(z.read(n))
    for g in d['groups']:
        for m in g.get('models',[]):
            v=m.get('value') or {}; p=v.get('path','').lower()
            if '\\hzn\\' in p and v.get('upgrade_ids'):
                rel=p.split('game:\\media\\',1)[1]
                groups[(d['ordinal'],v.get('part_name'))].append((rel,v['upgrade_ids']))
out=['#v1 PlateParts: relpfad;feld;upgrade-ids']
for (o,pn),L in sorted(groups.items()):
    if len({r for r,_ in L})<2: continue
    if pn not in FIELD: print('unbekannt',o,pn,L); continue
    for r,ids in L: out.append('%s;%d;%s'%(r,FIELD[pn],','.join(map(str,ids))))
open(sys.argv[2],'w',newline='\r\n').write('\n'.join(out)+'\n')
print(len(out)-1,'Zeilen')
for (o,pn),L in sorted(groups.items()):
    if len({r for r,_ in L})>=2 and pn!='BumperR': print(o,pn,L)
