from pathlib import Path
import re,sys,statistics as st,json
base,tag=sys.argv[1],sys.argv[2]
idx=json.load(open(str(Path(__file__).resolve().parents[2]/'build/bench')+'/packidx.json'))
def load(t):
    res={}
    for k in range(6):
        for suf in ('p','r'):
            try: txt=open(fstr(Path(__file__).resolve().parents[2]/'build/bench')+'/{t}/{suf}{k}.log',errors='replace').read()
            except FileNotFoundError: continue
            if 'TCTEST frames' not in txt: continue
            cur=None
            for l in txt.splitlines():
                m=re.match(r'TCTEST frame \$([0-9A-F]+) kernel .*differing words \$([0-9A-F]+) in rows',l)
                if m: cur=[int(m.group(1),16),int(m.group(2),16)]; continue
                m=re.match(r'TCTEST timing .*render\+record \$([0-9A-F]+)',l)
                if m and cur is not None:
                    res[idx[str(k)][cur[0]]]=(int(m.group(1),16)*1000/38400,cur[1]); cur=None
            break
    return res
b=load(base); n=load(tag)
common=sorted(set(b)&set(n))
print('frames',len(common),'of',sum(len(v) for v in idx.values()))
badn=sum(1 for f in common if n[f][1]); print('frames with differing words:',badn)
def grp(name,sel):
    fr=[f for f in common if sel(f)]
    if not fr: return
    tb=sum(b[f][0] for f in fr); tn=sum(n[f][0] for f in fr)
    print('%-8s n=%3d  base %6.1f ms  new %6.1f ms  speedup %.3fx'%(name,len(fr),tb/len(fr),tn/len(fr),tb/tn))
k3=set(f for k in '012 34'.replace(' ','') for f in idx[k]); k4=set(idx['5'])
grp('tile',lambda f:f in k3); grp('mode7',lambda f:f in k4); grp('all',lambda f:True)
grp('tile>=400',lambda f:f in k3 and b[f][0]>=400); grp('tile<200',lambda f:f in k3 and b[f][0]<200)
