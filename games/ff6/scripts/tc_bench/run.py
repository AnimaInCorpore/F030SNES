#!/usr/bin/env python3
"""run.py TAG PACK [--profile]: run build/bench/TAG/T<PACK>.TOS in the patched Hatari; with --profile the DSP
profile (dspprofile) from the first frame to the SSIOFF marker is saved to build/bench/TAG/prof<PACK>.txt"""
import subprocess, sys, time, re, argparse
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]; B = ROOT/'build/bench'
HATARI = ROOT.parents[2] / "F030Arcade/third_party/hatari/build/src/hatari"
TOS = ROOT.parents[2] / "F030Arcade/third_party/tos/tos404.img"
ap = argparse.ArgumentParser(); ap.add_argument('tag'); ap.add_argument('pack'); ap.add_argument('--profile', action='store_true')
ap.add_argument('--timeout', type=float, default=1500); a = ap.parse_args()
D = B/a.tag; prog = D/f'T{a.pack}.TOS'
suffix = 'p' if a.profile else 'r'
fifo = D/f'{suffix}{a.pack}.fifo'; log = D/f'{suffix}{a.pack}.log'; trace = D/f'{suffix}{a.pack}-trace.log'
for f in (fifo, log, trace):
    if f.exists(): f.unlink()
def ssioff(lst):
    L = (D/lst).read_text(errors='replace').splitlines()
    for i, l in enumerate(L):
        if re.match(r'^\d+\s+cmd_ssioff:', l):
            for m in L[i+1:i+6]:
                mm = re.match(r'^\d+\s+P:([0-9A-F]{4})', m)
                if mm: return '0x'+mm.group(1)
cmd = [str(HATARI), "--machine","falcon","--dsp","emu","--memsize","14","--ttram","0","--tos",str(TOS),
       "--fast-boot","true","--cpu-exact","true","--compatible","true","--patch-tos","true","--monitor","rgb",
       "--sound","off","--natfeats","true","--confirm-quit","off","--alert-level","fatal","--fast-forward","true",
       "--trace","event","--trace-file",str(trace),"--cmd-fifo",str(fifo),"--screenshot-dir",str(D), str(prog)]
with log.open('w') as err:
    p = subprocess.Popen(cmd, cwd=ROOT, stdout=err, stderr=subprocess.STDOUT)
def send(s):
    with fifo.open('w') as f: f.write(s+'\n')
t0 = time.monotonic(); started = False; out = D/f'prof{a.pack}.txt'
while time.monotonic()-t0 < a.timeout and p.poll() is None:
    txt = log.read_text(errors='replace')
    if a.profile and not started and 'TCTEST start' in txt:
        time.sleep(0.2)
        (D/f'cmds{a.pack}').write_text(f'dspprofile save {out}\ndspprofile stats\n')
        for l in sorted({ssioff('tc.lst'), ssioff('tc7.lst')}):
            send(f'hatari-debug dspbreak pc={l} :once :file {D/f"cmds{a.pack}"}')
        send('hatari-debug dspprofile on'); started = True
    if 'TCTEST frames' in txt: break
    time.sleep(0.3)
if a.profile: time.sleep(3)
send('hatari-debug quit')
try: p.wait(timeout=20)
except Exception: p.kill()
print(log.read_text(errors='replace').count('render+record'), 'frames timed; tail:')
print(''.join(log.read_text(errors='replace').splitlines(True)[-3:])[-400:])
