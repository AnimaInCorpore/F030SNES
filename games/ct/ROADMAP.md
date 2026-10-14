# F030CT roadmap

Done (2026-09-24, details in `analysis/ct.md`):

- Diz listing adapter, HiROM 4 MiB, SRAM, direct-page tracking, the WRAM
  interrupt trampolines, the decompressed intro program as a WRAM overlay,
  stack regions for the menu's coroutines, the SPC700 running CT's driver.
- Boot → field init → intro, with the intro's Mode 7 on the DSP kernel.

Next, in order:

1. **Past the name entry** into the first field scene (Crono's room):
   scripted input, the seeding loop for further dispatch targets.
2. **Input timing**: presses are one-NMI pulses the slower main loop can
   miss; hold the NMI back until the main loop has finished its pass
   (F030SMW's `SMKX_SYNC`), and speed.
3. **Take over from the other ports**: forced-blank frames not composed,
   F030SMK's direct register entries in the emitter and its dispatch
   cache. (The DMA fast paths, the smaller frame-start copy, page tables,
   the release switch and timer-C profiling are in, 2026-09-25.)
4. **PPU on the DSP** (tile modes and Mode 7 done, see `analysis/ct.md`:
   `ppu.asm` and its Mode 7 build, eight planes for every scene): next,
   - frames that mix Mode 7 and tile rows still go to the CPU (none seen in
     CT so far);
   - speed: DSP ~165-205 ms per frame now against a 76 ms transport; the
     slivers (~77 cycles each, close to what the layout allows), the
     compose plane loop (pointer arithmetic per plane) and the per-row
     layer setup (cacheable per layer) are what is left
     (`analysis/dsp-renderer-budget.md`); the Mode 7 build's sampling and
     c2p (~34 instructions per pixel) likewise;
   - colour math and windows need the reference first (`src/snes_ppu.c`).
5. **Speed**: without rendering the intro, title, menu and field NMIs run
   at 50 fps (`analysis/ct.md`, 2026-09-25). Left: the field's main loop
   (~46 passes per second in the attract demo: event frames, the SPC700
   running whenever the game polls it, generic memory helpers), then the
   battle engine's own budget. With rendering on, the render work runs in
   the game's idle time (2026-09-25): the game keeps ~50 fps and the
   picture gets what is left (~3.3/s in the field, 1.6/s in the intro).
   Next: plan the next frame while the DSP draws, idle marks for the
   battle engine; the picture rate is then the DSP renderer's
   (`analysis/dsp-renderer-budget.md`).
6. **Sound on the Falcon DSP.** CT is slow-paced and mostly not Mode 7, so
   the DSP is free most of the time:
   - the SPC700 driver statically translated (a new front end for the
     translator; ~7 KiB of SPC code) and run on the 68030 at the driver's
     tick, replacing the interpreter (`src/spc700.c` stays as the
     reference and for the port protocol);
   - the S-DSP on the DSP56001: BRR decoding, pitch, ADSR/GAIN, 8 voices,
     echo/FIR, fed with the driver's register writes and ARAM uploads (the
     per-song instrument set is at most ~40 KiB of BRR);
   - budget: 8 voices at 32 kHz needs roughly 8-10 of the DSP's 16 MIPS;
     16-22 kHz with linear interpolation about a third; echo matters for
     CT's sound and stays;
   - the SSI: the Mode 7 kernel sends its planes over the SSI into DMA
     record, and the DAC needs the SSI transmitter too. Either move the
     kernel's output to the host port (CT has few Mode 7 rows) or share the
     SSI in time; one combined DSP program (audio at interrupt level, c2p in
     the main loop);
   - confirm the budget on a real Falcon (Hatari's DSP timing is not one).
