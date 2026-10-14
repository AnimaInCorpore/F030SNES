import re,collections,sys
tag=sys.argv[1]; labs=set(sys.argv[2].split(',')); packs=[int(x) for x in sys.argv[3].split(',')] if len(sys.argv)>3 else range(5)
lst=f'{tag}/tc.lst'
addr2={};cur=None
for l in open(lst,errors='replace').read().splitlines():
    m=re.match(r'^(\d+)\s+(?:P:([0-9A-F]{4})(?: P:([0-9A-F]{4}))? [0-9A-F]{6}\s*)?(.*)$',l)
    if not m: continue
    lab=re.match(r'^\s*([A-Za-z_][A-Za-z_0-9]*):',m.group(4))
    if lab and not lab.group(1).startswith('_'): cur=lab.group(1)
    if m.group(2): addr2[int(m.group(2),16)]=(cur,m.group(1),m.group(4).strip())
cyc=collections.Counter();cnt=collections.Counter()
for k in packs:
    for l in open(f'{tag}/prof{k}.txt',errors='replace'):
        m=re.match(r'^p:([0-9a-f]+) .*% \((\d+), (\d+), (\d+)\)\s*$',l)
        if m:
            a=int(m.group(1),16); cyc[a]+=int(m.group(3)); cnt[a]+=int(m.group(2))
tot=0
for a in sorted(cyc):
    lab,ln,src=addr2.get(a,('?','',''))
    if lab in labs:
        tot+=cyc[a]
        print('%7.2f ms %9d x %4.1f cyc  P:%04X L%-5s %s'%(cyc[a]/32084988*1000/225,cnt[a],cyc[a]/max(cnt[a],1),a,ln,src[:60]))
print('total %.1f ms/frame'%(tot/32084988*1000/225))
