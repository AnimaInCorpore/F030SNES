#!/usr/bin/env python3
"""prof1.py TAG PACK: complete DSP profile of a pack (profile on from the start, saved at SSIOFF)"""
import subprocess, sys, re, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]; D=ROOT/'build/bench'/sys.argv[1]; k=sys.argv[2]
HATARI=ROOT.parents[2] / "F030Arcade/third_party/hatari/build/src/hatari"; TOS=ROOT.parents[2] / "F030Arcade/third_party/tos/tos404.img"
def ssioff(lst):
    L=(D/lst).read_text(errors='replace').splitlines()
    for i,l in enumerate(L):
        if re.match(r'^\d+\s+cmd_ssioff:',l):
            for m in L[i+1:i+6]:
                mm=re.match(r'^\d+\s+P:([0-9A-F]{4})',m)
                if mm: return '0x'+mm.group(1)
out=D/f'prof{k}.txt'
(D/f'cm{k}').write_text(f'dspprofile save {out}\ndspprofile stats\nquit\n')
def lab(lst,name):
    L=(D/lst).read_text(errors='replace').splitlines()
    for i,l in enumerate(L):
        if re.match(r'^\d+\s+'+name+r':',l):
            for m in L[i+1:i+6]:
                mm=re.match(r'^\d+\s+P:([0-9A-F]{4})',m)
                if mm: return '0x'+mm.group(1)
(D/f'on{k}').write_text('dspprofile on\n')
(D/f'pa{k}').write_text('\n'.join(f'dspbreak pc={a} :once :file {D/f"cm{k}"}' for a in sorted({ssioff("tc.lst"),ssioff("tc7.lst")}))+'\n'+'\n'.join(f'dspbreak pc={a} :once :file {D/f"on{k}"}' for a in sorted({lab("tc.lst","render"),lab("tc7.lst","render")}))+'\n')
log=D/f'q{k}.log'
cmd=[str(HATARI),"--machine","falcon","--dsp","emu","--memsize","14","--ttram","0","--tos",str(TOS),"--fast-boot","true","--cpu-exact","true","--compatible","true","--patch-tos","true","--monitor","rgb","--sound","off","--natfeats","true","--confirm-quit","off","--alert-level","fatal","--fast-forward","true","--parse",str(D/f'pa{k}'),str(D/f'T{k}.TOS')]
with log.open('w') as e: p=subprocess.Popen(cmd,cwd=ROOT,stdin=subprocess.DEVNULL,stdout=e,stderr=subprocess.STDOUT)
try: p.wait(timeout=1500)
except Exception: p.kill()
print(log.read_text(errors='replace').count('render+record'),'frames')
