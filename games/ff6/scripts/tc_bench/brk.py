import re,collections,sys
tag=sys.argv[1]; nfr=225
def load(lst):
    addr2={};cur=None
    for l in open(lst,errors='replace').read().splitlines():
        m=re.match(r'^(\d+)\s+(?:P:([0-9A-F]{4})(?: P:([0-9A-F]{4}))? [0-9A-F]{6}\s*)?(.*)$',l)
        if not m: continue
        lab=re.match(r'^\s*([A-Za-z_][A-Za-z_0-9]*):',m.group(4))
        if lab and not lab.group(1).startswith('_'): cur=lab.group(1)
        if m.group(2): addr2[int(m.group(2),16)]=cur
    return addr2
a2=load(f'{tag}/tc.lst'); by=collections.Counter()
for k in range(5):
    for l in open(f'{tag}/prof{k}.txt',errors='replace'):
        m=re.match(r'^p:([0-9a-f]+) .*% \((\d+), (\d+), (\d+)\)\s*$',l)
        if m: by[a2.get(int(m.group(1),16),'?')]+=int(m.group(3))
idle=sum(by[x] for x in ('g_h','get','getw','gx_s1','gx_s2','put','main'))
tot=sum(by.values())-idle
print('non-idle %.0f ms/frame'%(tot/32084988*1000/nfr))
top=int(sys.argv[2]) if len(sys.argv)>2 else 30
base=None
if len(sys.argv)>3:
    b2=load(f'{sys.argv[3]}/tc.lst'); base=collections.Counter()
    for k in range(5):
        for l in open(f'{sys.argv[3]}/prof{k}.txt',errors='replace'):
            m=re.match(r'^p:([0-9a-f]+) .*% \((\d+), (\d+), (\d+)\)\s*$',l)
            if m: base[b2.get(int(m.group(1),16),'?')]+=int(m.group(3))
for lab,c in by.most_common(top):
    if lab in ('g_h','get','getw','gx_s1','gx_s2','put','main'): continue
    extra='' if base is None else '   (base %.1f)'%(base[lab]/32084988*1000/nfr)
    print('  %-12s %6.1f ms/frame%s'%(lab,c/32084988*1000/nfr,extra))
