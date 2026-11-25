import struct
class Meta:
    def __init__(s,tag,ver,data): s.tag=tag;s.ver=ver;s.data=data
class Blob:
    def __init__(s,tag,maj,mn,metas,data): s.tag=tag;s.maj=maj;s.mn=mn;s.metas=metas;s.data=data
    def meta(s,t):
        for m in s.metas:
            if m.tag==t: return m
class Bundle:
    def __init__(s): s.maj=1;s.mn=0;s.blobs=[]
    @staticmethod
    def load(b,base=0):
        B=Bundle(); assert b[base:base+4]==b'burG'
        B.maj,B.mn=b[base+4],b[base+5]
        if B.mn>=1:
            _,hs,ts,cnt=struct.unpack_from('<HIII',b,base+6); p=base+20
        else:
            cnt,=struct.unpack_from('<H',b,base+6); p=base+16
        for i in range(cnt):
            tag=b[p+i*24:p+i*24+4]
            maj,mn,mc,mo,do,ds,ds2=struct.unpack_from('<BBHIIII',b,p+i*24+4)
            metas=[]
            for j in range(mc):
                q=base+mo+j*8
                mt=b[q:q+4]; fl,off=struct.unpack_from('<HH',b,q+4)
                size=fl>>4; ver=fl&15
                metas.append(Meta(mt,ver,b[q+off:q+off+size]))
            B.blobs.append(Blob(tag,maj,mn,metas,b[base+do:base+do+ds]))
        return B
    def dump(s):
        out=bytearray(b'\0'*20)  # header 0x14 (minor>=1 layout) ; we write both layouts below
        n=len(s.blobs)
        hdr=20 if s.mn>=1 else 16
        out=bytearray(hdr)+bytearray(24*n)
        info=[]
        for bl in s.blobs:
            mo=len(out); mb=bytearray(8*len(bl.metas)); dat=bytearray()
            last=len(mb)
            ents=[]
            for j,m in enumerate(bl.metas):
                off=last-8*j; ents.append((m,off)); mb+=m.data; last+=len(m.data)
            for j,(m,off) in enumerate(ents):
                struct.pack_into('<4sHH',mb,8*j,m.tag,(len(m.data)<<4)|(m.ver&15),off)
            out+=mb; info.append([mo,0,0])
        while len(out)%4: out+=b'\0'
        hs=len(out)
        for i,bl in enumerate(s.blobs):
            info[i][1]=len(out); out+=bl.data; info[i][2]=len(bl.data)
            while len(out)%4: out+=b'\0'
        for i,bl in enumerate(s.blobs):
            struct.pack_into('<4sBBHIIII',out,hdr+24*i,bl.tag,bl.maj,bl.mn,len(bl.metas),info[i][0],info[i][1],info[i][2],info[i][2])
        if s.mn>=1: struct.pack_into('<4sBBHIII',out,0,b'burG',s.maj,s.mn,0,hs,len(out),n) if False else None
        if s.mn>=1:
            out[0:4]=b'burG'; out[4]=s.maj; out[5]=s.mn; struct.pack_into('<HIII',out,6,0,hs,len(out),n)
        else:
            out[0:4]=b'burG'; out[4]=s.maj; out[5]=s.mn; struct.pack_into('<HII',out,6,n,hs,len(out))
        return bytes(out)
def name(bl):
    m=bl.meta(b'emaN') or bl.meta(b'Name')
    return m.data.decode('latin1') if m else None
