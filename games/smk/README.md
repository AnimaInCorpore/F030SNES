# F030SMK

An Atari Falcon 030 port of *Super Mario Kart*, grounded in the supplied
US v1 ROM and the pinned Yoshifanatic1 disassembly. The target display contract
is 256×224 pixels and four Videl planes: 16 colours per frame, chosen
adaptively from the SNES palette in OKLab.

The current executable work is the source-addressed ROM asset extractor. A
source-grounded Falcon gameplay loop is still to be ported. The earlier draft
with invented movement, a divided screen, and a custom HUD has been withdrawn.

## Source and ROM

`third_party/Super-Mario-Kart-Disassembly` is pinned at tag `V1.0`, commit
`f5c5c21f263172f20936893fa339ae0819094954`. Its U1 ROM map declares base-ROM
MD5 `7f25ce5a283d902694c52fb1152fa61a`, which matches the `.sfc` in
`assets/Super Mario Kart (USA).zip`. That confirms this is the matching U.S.
revision. The author notes unresolved bytes and possible missed indirect
trampoline pointers, so the disassembly is a version-matched reverse-engineered
reference rather than a guarantee that every routine has been recovered.
See [analysis/source-verification.md](analysis/source-verification.md).

Initialize the submodule on a fresh checkout with:

```sh
git submodule update --init --recursive
```

## Build

```sh
make smk-assets
make test
make preview
make falcon
make run-hatari
make verify-hatari
```

`make smk-assets` requires the local ROM ZIP at the path above. The ZIP is
ignored by Git. The extractor reads the ROM build hash from the pinned U1 map,
then follows its ROM offsets and routines to produce Mario Circuit 1's
128×128 course map and 1024×1024 indexed course texture. It maps the course
palette to DawnBringer 16 using nearest-color RGB distance, with no dithering.
The current conversion covers course art. Driver poses and their runtime
palette assignments remain undecoded in the original ROM until their source
tables and draw routines are traced.

`make falcon` builds the existing static Videl bring-up viewer from the host
reference frame. It is not the game. `make run-hatari` and
`make verify-hatari` use only the patched Hatari at
`../../../F030Arcade/third_party/hatari/build/src/hatari`, with TOS 4.04 and the
F030Arcade vasm/vlink tools.

The host renderer, format tests and Videl viewer predate the ROM port. Their
procedural artwork remains a renderer test input, not Mario Kart content.

## Translated program

The game's own program is translated to 68030 code from the ROM and the
pinned listing (`tools/smkx`), so gameplay, logic and screen design stay the
original's; only the palette is reduced to 16 colours per frame by the
four-plane display path. The 16 colours are chosen per frame from the
colours on screen, weighted by pixel count, with k-means in OKLab
(`src/palette.c`). The translation and the ROM-derived build stay
untracked.

```sh
make smkx
make run-smkx
```

Mode 7 rows are rendered by the Falcon DSP (sampling, palette and c2p),
streamed over the SSI into DMA record; the 68030 composes the other layers
and the sprites. Checks, all in the patched Hatari or on the host:

```sh
make test-m7          # DSP kernel vs the C reference, random lines
make test-dsp1        # translated DSP-1 vs its interpreter
make test-palette     # adaptive palette
make check-snap       # SNAP.BIN: Falcon planes vs host render
make check-snaps      # every Fnnnnnnn.BIN from SMKX_SNAP_EVERY=n, plus PNGs
```

Design, status and the first runs are in
[analysis/translator.md](analysis/translator.md).

The tile-mode screens (intro, driver, cup and course select) can instead be drawn by the list compositor
taken over from F030SMW (`make smkx SMKX_LC=1`, off by default): the DSP composes only the changed 16-pixel
groups of a scroll ring in eight planes (256 colours = CGRAM), the 68030 walks its list and points the Videl at
a window; the race and every frame it does not cover keep the four-plane path above. Checks:
`make check-dsp-model check-lcd-model check-lc-model test-ppu-tile test-lcd check-lc-play`; record and limits
in [analysis/list-compositor-smk.md](analysis/list-compositor-smk.md).

## Controls

Keyboard as pad 1, by key position on a QWERTY layout (Atari scancodes):
arrows = D-pad, X = B, Z = Y, S = A, A = X, Q = L, W = R, Return = Start,
Space = Select, Esc = quit. `make smkx SMKX_INPUT="1600:8000,1610:0000"`
adds scripted input: from frame 1600 the pad word `$8000` (B), from 1610
nothing (`tools/smkx/input_script.py`, from F030SMW).

## Porting rule

The translator replaces hand-porting of individual routines: every translated
instruction comes from the ROM, placed and checked against the listing.
Platform adapters (Videl output, the DSP Mode 7 and c2p path, input, the
APU and DSP-1 models) are target code and are documented as such. The
256×224 four-plane, 16-colour output remains the platform requirement.

## Pictures

`docs/preview.png`, `docs/hatari-rgb.png` and `docs/hatari-vga.png` show
Super Mario Kart's graphics, © 1992 Nintendo. Super Mario Kart
is a trademark of Nintendo.
