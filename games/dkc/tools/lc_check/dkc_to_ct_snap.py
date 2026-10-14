import sys, struct
# DKC snapshot (version 4, sram 2048, no planes field/palette blocks) -> F030CT layout
for path in sys.argv[2:]:
    d = open(path,'rb').read()
    hdr = 12+65536+512+544+0x6000+32+4+1536
    out = bytearray(d[:hdr])
    p = hdr
    out += d[p:p+131072]; p += 131072
    out += d[p:p+2048] + bytes(8192-2048); p += 2048
    out += struct.pack('>I', 8)
    out += d[p:p+57344]; p += 57344
    out += bytes(2*(96+48+256+4+2))
    out += d[p:]
    name = path.split('/')[-1]
    open(sys.argv[1]+'/'+name,'wb').write(out)
