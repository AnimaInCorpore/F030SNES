# F030DKC roadmap

Done (2026-09-24, details in analysis/dkc.md):

1. Listing placement: all 53,624 instructions ROM-checked (macros,
   conditionals, 64 banks).
2. Translation: 50,770 listing instructions reached, 0 unsupported, 0
   traps; D register tracked; table jumps as tail calls; hand-written JSLs.
3. Memory: 4 MiB ROM without the word view; DKCX.TOS 8.4 MB.
4. Runtime: NMI taken at WAI (the game lives in its NMI), DKC's APU loader
   protocol, WRAM-port DMA, F030SMK's page tables and dispatch cache.
5. Input: keyboard as pad 1, scripted input; `$4016` pad presence.
6. Display: F030SMW's 256-colour path (8 planes, palette = CGRAM).
7. First runs: logos, intro, file select, player-mode menu; no trap.

8. Reference interpreter (`tools/smkx/ref65816.py`, `refcmp.py`): the ROM
   run in Python with the runtime's hardware model, compared with Falcon
   dumps (`DKCX_DUMP_AT/_EVERY/_COUNT`, or `DKCX_DUMP_PC/_HIT` at the n-th
   execution of a probed address). Scripted input is keyed by game frames.
   Falcon and reference agree through game frame 3131 (logos, intro,
   menus, map) with input
   `2100:1000,2110:0000,2500:1000,2510:0000,2700:1000,2710:0000,2900:1000,2910:0000,3100:1000,3110:0000`.

9. Level-load bug fixed (2026-09-25): Jungle Hijinxs shows its
   foreground, the Kongs, bananas, enemies and the HUD; the Kongs walk
   and jump with scripted input; Falcon and reference agree through game
   frame 4693. Four causes, each found by bisecting against the
   reference (details in `analysis/dkc.md`, "The level-load bug"): the
   65816 V flag (gone with F030SMK's `rt_vf` change), `LDA.l
   DATA&$FF0000,x` read as WRAM, the OAM address not reloaded at VBlank
   (both models), VRAM reads without the prefetch latch (both models).

Next, in order:

1. **Play through levels**: long runs with scripted input (Jungle
   Hijinxs to its end, then the next levels, and the attract demo if the
   title has one), for traps, dispatch misses and differences from the
   reference. The input script above reaches the level at game frame
   3132; `3300:0100` holds Right.
2. **Measure**: game time per frame with rendering off (the frame
   counter at `$2A`, NMIs vs VBLs), and render time per level frame.
   Render cadence and game cadence are separate figures: time game/NMI
   code, CPU planning, VRAM uploads and presentation each on its own.
3. **Renderer**: F030CT's DSP renderer is in (2026-09-25; see
   `analysis/dkc.md`, "The DSP renderer" and "Rendering in idle time"):
   every level frame goes to the DSP, the 68030's share runs while the
   game waits at WAI. Jungle Hijinxs, release build (emulated): 43.9 game
   frames and 3.9 frames shown per second, all snapshots exact. Next:
   - Pipeline: plan the next frame while the DSP renders the current one
     (`smkx_ppu_plan`, `smkx_ppu_stale`, `smkx_ppu_send` are there),
     sending when the DSP is free; the display now waits for planning in
     the game's idle time (~8 ms a frame) on top of the DSP's ~200 ms.
   - The game code itself: 12-15 ms a frame, spikes above 20 ms cost
     frames (43.9, not 50); profile the level.
   - The DSP needs ~190-210 ms per jungle frame; the SSI floor is 76 ms.
     Take over F030CT's budget work as it lands (ring starvation profile,
     cached layer setup and tile rows, leaner `comp_group`, sprite
     buckets); DKC frames are useful stress cases for CT's tests.
   - Budget, from `games/ct/analysis/dsp-renderer-budget.md`: DKC keeps
     eight planes (up to 134 colours per frame), so SSI allows ~13 frames
     per second at best; below 40 ms needs another output path
     (host-port readback, to be benchmarked).
   - Modes 2-6 are drawn as mode 1 by the reference, the fast composer
     and the DSP alike; the logo screen uses mode 5 below row 150.
4. **PPU features DKC uses**: colour math (translucency, fades through
   the fixed colour), windows, HDMA gradients (the levels rewrite CGRAM
   on every line; every renderer shows the frame-start colours), mosaic
   in transitions; each goes into the reference renderer first, then the
   DSP.
5. **Sound**: the SPC700 driver and BRR samples (as in the other ports,
   later).
