# Kennzeichen "nur am Spielerauto": je Auto eigenes Kennzeichen-Material (gleiche Texturen, Auto-Id im Gross/Klein-
# Muster des Texturpfads kodiert), plus Spieler-Material (DURANCE) per verstecktem Mesh in jedem HZN-Modell geladen.
import sys,os,json,zipfile,struct,re,zlib
sys.path.insert(0,'/home/claude/work')
import numpy as np, fbundle, mat2, patch_carrier as pc
LIB='Game:\\Media\\cars\\_library\\materials\\badges\\'
SEG='\\cars\\_library\\textures\\'
def encode(path,ident):
    i=path.lower().index(SEG.lower())+len('\\cars\\_'); s=list(path); k=0
    for j in range(i,i+len('library\\textures')):
        if s[j]=='\\': continue
        s[j]=s[j].upper() if (ident>>k)&1 else s[j].lower(); k+=1
    assert k==15; return ''.join(s)
def decode(path):
    i=path.lower().index(SEG.lower())+len('\\cars\\_'); v=0;k=0
    for ch in path[i:i+len('library\\textures')]:
        if ch=='\\': continue
        v|=(1 if ch.isupper() else 0)<<k; k+=1
    return v
def ltam_with(src,repl,name=None,extra=None):
    out=mat2.ltam_clone(src,repl,extra)
    if name:
        b=fbundle.Bundle.load(out)
        for m in b.blobs[0].metas:
            if m.tag==b'emaN': m.data=name.encode()
        out=b.dump()
    return out
def tex_paths(ltam):
    b=fbundle.Bundle.load(ltam); E=mat2._parse(b.blobs[1].data); out={}
    for e in E:
        if e[0]=='tex':
            raw=e[2]; L=raw[7]; out[e[1]]=raw[8:8+L].decode()     # Laenge < 128 (1 Byte LEB)
    return out
def itam_parent(blob):
    ib=fbundle.Bundle.load(blob.data); d=ib.blobs[0].data; return d[1:1+d[0]].decode(),ib
def set_itam_parent(blob,newpath):
    ib=fbundle.Bundle.load(blob.data); par=newpath.encode(); assert len(par)<256
    ib.blobs[0].data=bytes([len(par)])+par; blob.data=ib.dump()
if __name__=='__main__':
    assert decode(encode('Game:\\Media\\cars\\_library\\textures\\hzn\\hzn_plate_atlas2.swatchbin',2180))==2180
    print('ok')
