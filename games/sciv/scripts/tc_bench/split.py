from pathlib import Path
import struct,sys,collections
a=open(str(Path(__file__).resolve().parents[2]/'build/bench')+'/real.bin','rb').read()
n=struct.unpack('>I',a[4:8])[0]; p=8; frames=[]
for f in range(n):
    s=p; kernel,cnt=struct.unpack('>II',a[p:p+8]); p+=8+cnt*4
    ns=struct.unpack('>I',a[p:p+4])[0]; p+=4+ns*514; p+=224*256*2
    frames.append((a[s:p],kernel,ns,f))
k3=[f for f in frames if f[1]==3 and f[2]<=74]; k4=[f for f in frames if f[1]==4 and f[2]<=74]
print(len(k3),len(k4),'others',len(frames)-len(k3)-len(k4))
groups=[k3[i::5] for i in range(5)]+[k4]      # k3 interleaved into 3 packs, Mode 7 alone
idx={}
for g,ch in enumerate(groups):
    open(fstr(Path(__file__).resolve().parents[2]/'build/bench')+'/pack{g}.bin','wb').write(b'TCP1'+struct.pack('>I',len(ch))+b''.join(c[0] for c in ch))
    idx[g]=[c[3] for c in ch]; print(g,len(ch),sum(len(c[0]) for c in ch)//1e6,'MB')
import json; json.dump(idx,open(str(Path(__file__).resolve().parents[2]/'build/bench')+'/packidx.json','w'))
