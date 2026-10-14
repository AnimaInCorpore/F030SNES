CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Isrc
LDFLAGS ?=

# This workstation's macOS 27 SDK uses a .tbd architecture unknown to its
# installed linker. Prefer its compatible 26.5 SDK when both are present.
ifeq ($(shell uname -s),Darwin)
ifneq ($(wildcard /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk),)
CPPFLAGS += -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
LDFLAGS += -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
endif
endif

ARCADE_TOOLS := ../../../F030Arcade/third_party
VASM := $(ARCADE_TOOLS)/vasm/vasmm68k_mot
VLINK := $(ARCADE_TOOLS)/vlink/vlink
HATARI := ../../../F030Arcade/third_party/hatari/build/src/hatari
CROSS_CC ?= ../../../cross-mint/bin/m68k-atari-mintelf-gcc
GAME_SOURCE := third_party/Yoshi-s-Island-Disassembly/YI
# The ROM as the user supplied it: .sfc, .smc or a ZIP holding one.
ROM ?= $(subst ?,$(space),$(firstword $(subst $(space),?,$(wildcard assets/Super\ Mario\ World\ 2\ -\ Yoshi's\ Island\ (USA).sfc assets/Super\ Mario\ World\ 2\ -\ Yoshi's\ Island\ (USA).smc assets/Super\ Mario\ World\ 2\ -\ Yoshi's\ Island\ (USA).zip))))
empty :=
space := $(empty) $(empty)
escape_space = $(subst $(space),\$(space),$(1))
ROM_PREREQUISITE := $(call escape_space,$(ROM))

.PHONY: check-lc-model check-lc-play bench-lc-play test-lcd check-lcd test-ppu check-dsp-model all yix-source yix run-yix test-m7 test-c2p test-gsu check-snap check-snaps test-palette clean FORCE
all: yix-source

build:
	mkdir -p build

# 65816 -> 68030 translation of the ROM's program (tools/smkx, from the
# F030SMK port via F030SMW). Everything under build/yix is derived from the
# ROM and stays untracked.
YIX := build/yix
YIX_FRAMES ?= 0
YIX_RENDER ?= 1            # 0: no rendering at all (timing and reference runs); the renderer throttles itself otherwise
YIX_SNAP_EVERY ?= 0
YIX_SNAP_FROM ?= 0
YIX_LC ?= 1               # 0: no list compositor and no throttle (the DSP or the CPU draws every frame: consecutive snapshots)
YIX_LC_BLIT_MIN ?= 24     # blocks of this many groups or more go to the blitter (hog mode); 100000: CPU only
YIX_LC_SYNC ?= 0          # 1: the list compositor waits for the DSP in the frame handler (snapshots match the picture)
YIX_M7_DSP ?= 1
YIX_PROFILE_FROM ?= 0
YIX_SLOW ?= 0
# Scripted joypad input, FRAME:PAD pairs (tools/smkx/input_script.py).
YIX_INPUT ?=
# SuperFX: interp (src/gsu.c only), xlat (translated program, the
# interpreter for what it does not cover) or check (both, compared per run).
YIX_GSU ?= xlat
YIX_GSU_DEF_interp :=
YIX_GSU_DEF_xlat := -DYIX_GSU_XLAT
YIX_GSU_DEF_check := -DYIX_GSU_XLAT -DYIX_GSU_CHECK
YIX_GSU_OBJ_interp :=
YIX_GSU_OBJ_xlat := $(YIX)/gsu_code.o
YIX_GSU_OBJ_check := $(YIX)/gsu_code.o
YIX_PY := $(wildcard tools/smkx/*.py) tools/smkx/dispatch_targets.txt tools/smkx/probes.txt \
  tools/smkx/jumps.txt

$(YIX)/sources.txt: $(YIX_PY) $(wildcard $(GAME_SOURCE)/*.asm) $(ROM_PREREQUISITE)
	python3 -m tools.smkx "$(ROM)" $(YIX)

yix-source: $(YIX)/sources.txt

# The video adapter's C code, cross-compiled freestanding for the 68030.
CROSS_CFLAGS := -DLC_PROF $(LC_EXTRA) -DLC_BLIT_MIN=$(YIX_LC_BLIT_MIN) -DSMKX_LC_SYNC=$(YIX_LC_SYNC) -DSMKX_LC=$(YIX_LC) \
  -m68030 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
  -fomit-frame-pointer -nostdlib -Wall -Wextra -Isrc
YIX_C_OBJ := $(YIX)/snes_ppu.o $(YIX)/ppu_fast.o $(YIX)/smkx_video.o $(YIX)/smkx_m7.o \
  $(YIX)/m7_plan.o $(YIX)/smkx_ppu.o $(YIX)/ppu_dsp_plan.o $(YIX)/lc_plan.o $(YIX)/smkx_lc.o \
  $(YIX)/gsu.o $(YIX)/yix_gsu.o $(YIX_GSU_OBJ_$(YIX_GSU))

$(YIX)/%.o: src/%.c src/snes_ppu.h src/ppu_internal.h src/ppu_fast.h src/smkx_m7.h src/m7_plan.h \
  src/ppu_dsp.h src/smkx_ppu.h src/smkx_prof.h src/lc_plan.h src/gsu.h | $(YIX)/sources.txt
	$(CROSS_CC) $(CROSS_CFLAGS) $(YIX_GSU_DEF_$(YIX_GSU)) -c -o $@ $<

$(YIX)/yix_gsu.o: $(YIX)/flags.stamp
$(YIX)/smkx_video.o $(YIX)/smkx_lc.o: $(YIX)/flags.stamp

# The SuperFX program as C (tools/smkx/gsu2c.py), compiled for the 68030.
$(YIX)/gsu_code.c: tools/smkx/gsu2c.py $(YIX)/sources.txt
	python3 -m tools.smkx.gsu2c $(YIX)/yi.sfc $@

$(YIX)/gsu_code.o: $(YIX)/gsu_code.c src/gsu.h
	$(CROSS_CC) $(CROSS_CFLAGS) -w -c -o $@ $<

# Build switches are inputs too: the stamp changes whenever they do.
YIX_FLAGS := FRAMES=$(YIX_FRAMES) RENDER=$(YIX_RENDER) \
  SNAP_EVERY=$(YIX_SNAP_EVERY) SNAP_FROM=$(YIX_SNAP_FROM) LC=$(YIX_LC) LC_SYNC=$(YIX_LC_SYNC) LC_BLIT_MIN=$(YIX_LC_BLIT_MIN) \
  M7_DSP=$(YIX_M7_DSP) \
  PROFILE_FROM=$(YIX_PROFILE_FROM) SLOW=$(YIX_SLOW) INPUT=$(YIX_INPUT) GSU=$(YIX_GSU)
# A changed switch also removes YIX.TOS: make compares whole seconds, and a
# rebuild within the same second as the last link would otherwise be skipped.
$(YIX)/flags.stamp: FORCE | $(YIX)/sources.txt
	@test "$$(cat $@ 2>/dev/null)" = "$(YIX_FLAGS)" || { echo "$(YIX_FLAGS)" > $@; rm -f $(YIX)/YIX.TOS; }

FORCE:

$(YIX)/m7c2p.bin: falcon/dsp/m7c2p.asm tools/smkx/dsp_asm.py
	mkdir -p $(YIX)
	python3 -m tools.smkx.dsp_asm $< $@

# The Mode 7 kernel in boot format (rt_dsp_boot, no XBIOS Dsp_ExecBoot); $(YIX)/m7c2p.bin stays the raw
# image M7TEST hands to Dsp_ExecBoot.
$(YIX)/m7c2p_boot.bin: falcon/dsp/m7c2p.asm tools/smkx/dsp_asm.py
	mkdir -p $(YIX)
	python3 -m tools.smkx.dsp_asm --boot $< $@

$(YIX)/input.s: tools/smkx/input_script.py FORCE | $(YIX)/sources.txt
	python3 -m tools.smkx.input_script "$(YIX_INPUT)" $@

$(YIX)/oklab555.bin: tools/smkx/oklab.py
	mkdir -p $(YIX)
	python3 -m tools.smkx.oklab $@

# The info screen and the player's files, read at startup (tools/port_info.py,
# tools/games.py): run every time, rewritten only when it changes.
# INFO_WAIT=n keeps the screen n seconds (a key skips); a release uses it.
INFO_WAIT ?= 0
$(YIX)/info.i: $(YIX)/sources.txt ../../tools/port_info.py ../../tools/games.py FORCE
	python3 ../../tools/port_info.py smw2 $@ --rom $(YIX)/yi.sfc --wait $(INFO_WAIT)

$(YIX)/YIX.TOS: $(YIX)/sources.txt falcon/smkx_rt.s falcon/smkx_rt.i falcon/smkx_info.s $(YIX)/info.i \
  falcon/smkx_video.s falcon/smkx_lcput.s falcon/smkx_m7dsp.s $(YIX_C_OBJ) $(YIX)/flags.stamp \
  $(YIX)/m7c2p.bin $(YIX)/m7c2p_boot.bin $(YIX)/ppu.bin $(YIX)/ppu7.bin $(YIX)/ppuw.bin $(YIX)/ppulc.bin $(YIX)/ppulcw.bin \
  $(YIX)/input.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -Ifalcon -I$(YIX) -DSMKX_FRAMES=$(YIX_FRAMES) \
	  -DSMKX_RENDER=$(YIX_RENDER) -DSMKX_SNAP_EVERY=$(YIX_SNAP_EVERY) -DSMKX_SNAP_FROM=$(YIX_SNAP_FROM) \
	  -DSMKX_M7_DSP=$(YIX_M7_DSP) -DSMKX_PROFILE_FROM=$(YIX_PROFILE_FROM) -DSMKX_SLOW=$(YIX_SLOW) -o $(YIX)/rt.o falcon/smkx_rt.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -o $(YIX)/tables.o $(YIX)/smkx_tables.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -o $(YIX)/input.o $(YIX)/input.s
	for f in $$(cat $(YIX)/sources.txt); do \
	  $(VASM) -Felf -m68030 -quiet -nowarn=62 -Ifalcon -o $(YIX)/$${f%.s}.o $(YIX)/$$f || exit 1; \
	done
	$(VLINK) -b ataritos -tos-flags 1 -s -M$(YIX)/YIX.map -o $@ $(YIX)/rt.o \
	  $$(sed 's/\.s$$/.o/;s|^|$(YIX)/|' $(YIX)/sources.txt) $(YIX)/tables.o $(YIX)/input.o $(YIX_C_OBJ)

# The stamp first, then a sub-make: a stamp change deletes YIX.TOS, and
# make 3.81 would not look at the file again within the same run.
yix: $(YIX)/flags.stamp
	@$(MAKE) --no-print-directory $(YIX)/YIX.TOS

run-yix: yix
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $(YIX)/YIX.TOS --fast --timeout 1800

# The DSP Mode 7 kernel against the C reference: random VRAM and lines.
$(YIX)/t_%.o: src/%.c src/snes_ppu.h src/m7_plan.h
	mkdir -p $(YIX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

M7TEST_OBJ := $(YIX)/t_m7test.o $(YIX)/t_m7_plan.o $(YIX)/t_snes_ppu.o

$(YIX)/M7TEST.TOS: falcon/m7test.s $(YIX)/m7c2p.bin $(M7TEST_OBJ)
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(YIX) -o $(YIX)/m7test.o falcon/m7test.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(YIX)/m7test.o $(M7TEST_OBJ)

test-m7: $(YIX)/M7TEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag m7test --grep M7TEST --fast --timeout 800
	@grep -a "M7TEST lines" $(YIX)/m7test-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

build/c2p_test: tools/c2p_test.c src/ppu_fast.c src/snes_ppu.c src/ppu_fast.h src/ppu_internal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/c2p_test.c src/ppu_fast.c src/snes_ppu.c

# The display path's chunky-to-planar packing against the bit-by-bit one.
test-c2p: build/c2p_test
	build/c2p_test

# The SuperFX core on the host: run one routine from the ROM.
build/gsu_run: tools/gsu_run.c src/gsu.c src/gsu.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/gsu_run.c src/gsu.c

test-gsu: build/gsu_run
	build/gsu_run $(YIX)/yi.sfc 08:A97B

build/palette_test: tools/palette_test.c src/palette.c src/palette.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/palette_test.c src/palette.c

test-palette: build/palette_test $(YIX)/oklab555.bin
	build/palette_test $(YIX)/oklab555.bin

# SNAP.BIN from the runtime: Falcon planes and palette against the host.
build/ppu_snap: tools/ppu_snap.c src/snes_ppu.c src/ppu_fast.c src/snes_ppu.h src/ppu_internal.h \
  src/ppu_fast.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/ppu_snap.c src/snes_ppu.c src/ppu_fast.c

check-snap: build/ppu_snap
	build/ppu_snap $(YIX)/SNAP.BIN $(YIX)/yi.sfc

check-snaps: build/ppu_snap
	@fail=0; for f in $(YIX)/F*.BIN; do \
	  echo "== $$f"; \
	  build/ppu_snap $$f $(YIX)/yi.sfc $${f%.BIN}.ppm || fail=1; \
	  magick $${f%.BIN}.ppm $${f%.BIN}.png && rm -f $${f%.BIN}.ppm; \
	done; exit $$fail


clean:
	rm -rf build

# ---- The DSP tile renderer, taken over from games/smw/F030CT (analysis/list-compositor-smw2.md) ------
# Host model of the DSP design (tools/dsp_model.c, the planner in src/ppu_dsp_plan.c) against the
# reference, on the runtime's snapshots (YI's own layout, version 5). SuperFX bus (ppu_abus_yi).
SNAPS ?= $(YIX)
build/dsp_snap: tools/dsp_snap.c tools/dsp_model.c tools/dsp_model.h src/ppu_dsp_plan.c src/ppu_dsp.h \
  src/m7_plan.c src/snes_ppu.c src/snes_ppu.h src/lc_plan.c src/lc_plan.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/dsp_snap.c tools/dsp_model.c \
	  src/ppu_dsp_plan.c src/snes_ppu.c src/m7_plan.c src/lc_plan.c

build/dsp_rand: tools/dsp_rand.c tools/dsp_model.c tools/dsp_model.h src/ppu_dsp_plan.c src/ppu_dsp.h \
  src/m7_plan.c src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/dsp_rand.c tools/dsp_model.c \
	  src/ppu_dsp_plan.c src/snes_ppu.c src/m7_plan.c

check-dsp-model: build/dsp_snap
	@mkdir -p build/dsp
	-build/dsp_snap $(YIX)/yi.sfc $(SNAPS)/F*.BIN > build/dsp/dsp_snap.txt
	@echo "$$(grep -c 'diff 0 words in 0 rows' build/dsp/dsp_snap.txt) of $$(ls $(SNAPS)/F*.BIN | wc -l | tr -d ' ') snapshots: model equals the reference"
	@echo "$$(grep -c 'stream does not fit' build/dsp/dsp_snap.txt) snapshots the DSP cannot take (the CPU draws them: Mode 7 rows, mixed rows, too long a stream)"
	@echo "$$(grep -c "Falcon's eight planes vs the model: [1-9]" build/dsp/dsp_snap.txt) snapshots where the Falcon's own frame differs (a build before the DSP drew them)"
	@test "$$(grep -c 'diff [1-9]\|READ OUTSIDE' build/dsp/dsp_snap.txt)" = 0

# The DSP program in its builds (same source): tile frames, Mode 7 frames, tile frames with windows and
# the sub screen, the list compositor's group composer. Booted by falcon/smkx_dspboot.s (no XBIOS Dsp_ExecBoot).
$(YIX)/ppu.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(YIX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=0 --define LCB=0 $< $@

$(YIX)/ppu7.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(YIX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=1 --define WIN=0 --define LCB=0 $< $@

$(YIX)/ppuw.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(YIX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=0 $< $@

$(YIX)/ppulc.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(YIX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=0 --define LCB=1 $< $@

# ... and with the window build in it (frames whose rows have windows masking layers)
$(YIX)/ppulcw.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(YIX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=1 $< $@

# PPUTEST: real YI frames (the snapshots named in PPUTEST_FRAMES), then random ones (flips, 16x16 tiles,
# mode 0, windows, the sub screen), each composed by the DSP in Hatari and compared with the reference.
PPUTEST_RANDOM ?= 32
PPUTEST_FRAMES ?= $(sort $(wildcard $(SNAPS)/F*.BIN))

$(YIX)/ppu_tests.bin: build/dsp_snap build/dsp_rand $(YIX)/yi.sfc $(PPUTEST_FRAMES)
	-build/dsp_snap $(YIX)/yi.sfc $(PPUTEST_FRAMES) -pack $(YIX)/ppu_tests_real.bin > $(YIX)/ppu_tests_real.txt   # (frames the program cannot hold exit 1: left out of the pack)
	build/dsp_rand $(PPUTEST_RANDOM) 0x5eed $(YIX)/ppu_tests_rand.bin > $(YIX)/ppu_tests_rand.txt
	python3 -c "import sys,struct; a,b=(open(f,'rb').read() for f in sys.argv[1:3]); \
	  open(sys.argv[3],'wb').write(b'PPU2'+struct.pack('>I',struct.unpack('>I',a[4:8])[0]+struct.unpack('>I',b[4:8])[0])+a[8:]+b[8:])" \
	  $(YIX)/ppu_tests_real.bin $(YIX)/ppu_tests_rand.bin $@

$(YIX)/t_pputest.o: src/pputest.c src/ppu_dsp.h src/snes_ppu.h src/smkx_prof.h
	mkdir -p $(YIX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(YIX)/PPUTEST.TOS: falcon/pputest.s falcon/smkx_dspboot.s $(YIX)/ppu.bin $(YIX)/ppu7.bin $(YIX)/ppuw.bin \
  $(YIX)/ppu_tests.bin $(YIX)/t_pputest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(YIX) -Ifalcon -o $(YIX)/pputest.o falcon/pputest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(YIX)/pputest.o $(YIX)/t_pputest.o

test-ppu: $(YIX)/PPUTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag pputest --grep PPUTEST --fast --timeout 800
	@grep -a "PPUTEST frames" $(YIX)/pputest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

# ---- The list compositor (analysis/list-compositor-smw2.md) --------------------------------------------
# tools/lc/lcd_model.c: which groups of the scroll ring each frame writes, from inputs only (VRAM writes, OAM,
# registers, scroll), two pages, exact against the reference (colour math off, as the compositor draws).
# The snapshots must be consecutive frames (YIX_LC=0 YIX_SNAP_EVERY=1 YIX_SNAP_FROM=n: every frame drawn; the model reads
# the snapshots' inputs only, so the compositor's own build is not needed).
LC := build/lc
LCD_SNAPS ?= $(sort $(wildcard $(SNAPS)/F*.BIN))

$(LC)/lcd_model: tools/lc/lcd_model.c tools/lc/lc_model.c src/lc_plan.c src/lc_plan.h src/snes_ppu.c src/snes_ppu.h | build
	mkdir -p $(LC)
	$(CC) -Isrc $(CPPFLAGS) $(CFLAGS) -Wno-unused $(LDFLAGS) -o $@ tools/lc/lcd_model.c src/lc_plan.c src/snes_ppu.c -lm

check-lcd: $(LC)/lcd_model
	@$(LC)/lcd_model $(YIX)/yi.sfc $(LCD_SNAPS) | tee $(LC)/lcd_model.txt | tail -1
	@test "$$(tail -1 $(LC)/lcd_model.txt | grep -c ' 0 frames wrong, 0 wrong pixels')" = 1

# LCDTEST: the LC build of the DSP program on real frames with masks of several patterns; each frame's list against
# the model.
$(YIX)/lcd_tests.bin: build/dsp_snap $(YIX)/yi.sfc $(PPUTEST_FRAMES)
	-build/dsp_snap $(YIX)/yi.sfc $(PPUTEST_FRAMES) -packlc $@ > /dev/null

$(YIX)/t_lcdtest.o: src/lcdtest.c src/ppu_dsp.h src/smkx_prof.h
	mkdir -p $(YIX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(YIX)/LCDTEST.TOS: falcon/lcdtest.s falcon/smkx_dspboot.s $(YIX)/ppulc.bin $(YIX)/ppulcw.bin $(YIX)/lcd_tests.bin $(YIX)/t_lcdtest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(YIX) -Ifalcon -o $(YIX)/lcdtest.o falcon/lcdtest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(YIX)/lcdtest.o $(YIX)/t_lcdtest.o

test-lcd: $(YIX)/LCDTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag lcdtest --grep LCDTEST --fast --timeout 800
	@grep -a "LCDTEST frames" $(YIX)/lcdtest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

# ---- F030CT's blitter renderer: the list-compositor model and its Hatari player ------------------------
# (analysis/list-compositor-smw2.md). tools/lc/lc_model.c is F030CT's model with YI's rules (LC_SMW=1); it reads the
# runtime's snapshots directly (tools/lc/smw2_to_ct_snap.py converts them to F030CT's layout for CT's own tools).
# Only the model and the player exist: nothing here runs in the game. Snapshots of consecutive frames:
#   make yix YIX_LC=0 YIX_SNAP_EVERY=1 YIX_SNAP_FROM=n YIX_FRAMES=m
LC_ENV := LC_SMW=1 LC_CHOOSER=exact LC_OVERLAY=ring LC_CACHE_KB=64

$(LC)/lc_model: tools/lc/lc_model.c src/snes_ppu.c src/snes_ppu.h src/lc_plan.h | build
	mkdir -p $(LC)
	$(CC) -Isrc $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/lc/lc_model.c src/snes_ppu.c -lm

# Every snapshot through the model (which frames it covers), then the longest run of consecutive covered frames
# through the ring model (hreal), recorded as the stream the player plays.
check-lc-model: $(LC)/lc_model $(YIX)/yi.sfc
	@LC_SMW=1 $(LC)/lc_model census $(YIX)/yi.sfc $(LCD_SNAPS) | tee $(LC)/census.txt | tail -1
	@LC_SMW=1 $(LC)/lc_model seq $(YIX)/yi.sfc $(LCD_SNAPS) | tail -2
	LC_SMW=1 $(LC)/lc_model blitfuzz 50000 1 | tail -1
	python3 tools/lc/lc_longest.py $(SNAPS) < $(LC)/census.txt > $(LC)/window.txt
	@test -s $(LC)/window.txt || { echo "no frame the model covers"; exit 1; }
	@echo "longest covered run: $$(wc -l < $(LC)/window.txt | tr -d ' ') frames"
	@LC_REC=$(LC)/yi.lcp $(LC_ENV) $(LC)/lc_model hreal $(YIX)/yi.sfc $$(cat $(LC)/window.txt) | tail -6
	LC_SMW=1 $(LC)/lc_model play $(LC)/yi.lcp

# The recorded stream played by falcon/lcplay.s in the patched Hatari: every frame's page window against the
# stream's checksum.
check-lc-play: check-lc-model
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_lcplay.py check $(LC)/yi.lcp

bench-lc-play: check-lc-model
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_lcplay.py bench $(LC)/yi.lcp
