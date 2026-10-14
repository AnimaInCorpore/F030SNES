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
GAME_SOURCE := third_party/SMW-SMAS-SMASW-Disassembly
# The ROM as the user supplied it: .sfc, .smc or a ZIP holding one.
ROM ?= $(subst ?,$(space),$(firstword $(subst $(space),?,$(wildcard assets/Super\ Mario\ All-Stars\ (USA).sfc assets/Super\ Mario\ All-Stars\ (USA).smc assets/Super\ Mario\ All-Stars\ (USA).zip))))
empty :=
space := $(empty) $(empty)
escape_space = $(subst $(space),\$(space),$(1))
ROM_PREREQUISITE := $(call escape_space,$(ROM))

.PHONY: check-lcd bench-lcw test-ppu test-lcd check-dsp-model lc-snaps check-lc-model check-lc-fuzz check-lc-play bench-lc-play ref-snaps all smasx-source smasx smasx-parts run-smasx test-m7 test-c2p check-snap check-snaps test-palette clean FORCE
all: smasx-source

build:
	mkdir -p build

# 65816 -> 68030 translation of the ROM's program (tools/smkx, from the
# F030SMK port). Everything under build/smasx is derived from the ROM and
# stays untracked.
SMASX := build/smasx
SMASX_FRAMES ?= 0
SMASX_RENDER ?= 1            # 0: no rendering at all (timing and reference runs); the renderer throttles itself otherwise
SMASX_SNAP_EVERY ?= 0
SMASX_SNAP_FROM ?= 0
SMASX_DSP ?= 1             # 0: the CPU renderer only (ppu_fast), the DSP is not booted for pictures
SMASX_LC ?= 1              # 0: no list compositor, the blocking DSP tile renderer (needs SMASX_DSP=1) draws the frames
SMASX_LC_BLIT_MIN ?= 24        # blocks of this many groups or more go to the blitter (hog mode); 100000: CPU only
SMASX_LC_SYNC ?= 0          # 1: the list compositor waits for the DSP in the frame handler (snapshots match the picture)
SMASX_M7_DSP ?= 1
SMASX_PROFILE_FROM ?= 0
SMASX_SLOW ?= 0
# Test switches for the reference comparison (tools/ref65816.c).
SMASX_SYNC ?= 0
SMASX_SUMS ?= 0
SMASX_TRACE_FROM ?= 0x7fffffff
# Scripted joypad input, FRAME:PAD pairs (tools/smkx/input_script.py).
SMASX_INPUT ?=
SMASX_PY := $(wildcard tools/smkx/*.py) tools/smkx/dispatch_targets.txt tools/smkx/probes.txt \
  tools/smkx/traces.txt

$(SMASX)/sources.txt: $(SMASX_PY) $(wildcard $(GAME_SOURCE)/SMAS/*.asm $(GAME_SOURCE)/SMAS/RomMap/*.asm $(GAME_SOURCE)/SMB1/*.asm \
  $(GAME_SOURCE)/SMBLL/*.asm $(GAME_SOURCE)/SMB2U/*.asm $(GAME_SOURCE)/SMB3/*.asm) $(ROM_PREREQUISITE)
	python3 -m tools.smkx "$(ROM)" $(SMASX)

smasx-source: $(SMASX)/sources.txt

# The video adapter's C code, cross-compiled freestanding for the 68030.
CROSS_CFLAGS := -DLC_PROF $(LC_EXTRA) -DLC_BLIT_MIN=$(SMASX_LC_BLIT_MIN) -DSMKX_LC_SYNC=$(SMASX_LC_SYNC) -DSMKX_DSP=$(SMASX_DSP) -DSMKX_LC=$(SMASX_LC) -m68030 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
  -fomit-frame-pointer -nostdlib -Wall -Wextra -Isrc
SMASX_C_OBJ := $(SMASX)/snes_ppu.o $(SMASX)/ppu_fast.o $(SMASX)/smkx_video.o $(SMASX)/smkx_m7.o \
  $(SMASX)/m7_plan.o $(SMASX)/smkx_ppu.o $(SMASX)/ppu_dsp_plan.o $(SMASX)/lc_plan.o $(SMASX)/smkx_lc.o

$(SMASX)/smkx_video.o $(SMASX)/smkx_lc.o: $(SMASX)/flags.stamp

$(SMASX)/%.o: src/%.c src/snes_ppu.h src/ppu_internal.h src/ppu_fast.h src/smkx_m7.h src/m7_plan.h src/ppu_dsp.h \
  src/smkx_ppu.h src/smkx_prof.h src/lc_plan.h
	@mkdir -p $(SMASX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

# Build switches are inputs too: the stamp changes whenever they do.
SMASX_FLAGS := FRAMES=$(SMASX_FRAMES) RENDER=$(SMASX_RENDER) \
  SNAP_EVERY=$(SMASX_SNAP_EVERY) SNAP_FROM=$(SMASX_SNAP_FROM) DSP=$(SMASX_DSP) LC=$(SMASX_LC) LC_SYNC=$(SMASX_LC_SYNC) LC_BLIT_MIN=$(SMASX_LC_BLIT_MIN) M7_DSP=$(SMASX_M7_DSP) \
  PROFILE_FROM=$(SMASX_PROFILE_FROM) SLOW=$(SMASX_SLOW) SYNC=$(SMASX_SYNC) SUMS=$(SMASX_SUMS) TRACE_FROM=$(SMASX_TRACE_FROM) INPUT=$(SMASX_INPUT)
# A changed switch also removes SMASX.TOS: make compares whole seconds, and a
# rebuild within the same second as the last link would otherwise be skipped.
$(SMASX)/flags.stamp: FORCE
	@mkdir -p $(SMASX)
	@test "$$(cat $@ 2>/dev/null)" = "$(SMASX_FLAGS)" || { echo "$(SMASX_FLAGS)" > $@; rm -f $(SMASX)/SMASX.TOS $(SMASX)/rt.o $(SMASX)/smkx_video.o $(SMASX)/smkx_lc.o; }

FORCE:

$(SMASX)/m7c2p.bin: falcon/dsp/m7c2p.asm tools/smkx/dsp_asm.py
	mkdir -p $(SMASX)
	python3 -m tools.smkx.dsp_asm $< $@

# The Mode 7 kernel in boot format (rt_dsp_boot, no XBIOS Dsp_ExecBoot); $(SMASX)/m7c2p.bin stays the raw
# image M7TEST hands to Dsp_ExecBoot.
$(SMASX)/m7c2p_boot.bin: falcon/dsp/m7c2p.asm tools/smkx/dsp_asm.py
	mkdir -p $(SMASX)
	python3 -m tools.smkx.dsp_asm --boot $< $@

$(SMASX)/input.s: tools/smkx/input_script.py FORCE
	mkdir -p $(SMASX)
	python3 -m tools.smkx.input_script "$(SMASX_INPUT)" $@

$(SMASX)/oklab555.bin: tools/smkx/oklab.py
	mkdir -p $(SMASX)
	python3 -m tools.smkx.oklab $@

# The ROM as the translator checks it (MD5 of the U.S. release), for the runtime and the host tools.
$(SMASX)/smas.sfc: $(ROM_PREREQUISITE)
	mkdir -p $(SMASX)
	python3 -c "import sys; from pathlib import Path; from tools.smkx.__main__ import load_rom; Path(sys.argv[2]).write_bytes(load_rom(Path(sys.argv[1])))" "$(ROM)" $@

# The runtime (falcon/smkx_rt.s with its video and DSP driver includes and the incbin'd DSP programs and ROM).
# It does not need the translation: `make smasx-parts` builds it and the C objects while step 1 of ROADMAP.md
# (the placement, hence sources.txt) is missing.
$(SMASX)/rt.o: falcon/smkx_rt.s falcon/smkx_rt.i falcon/smkx_video.s falcon/smkx_lcput.s falcon/smkx_m7dsp.s \
  $(SMASX)/flags.stamp $(SMASX)/smas.sfc $(SMASX)/m7c2p.bin $(SMASX)/m7c2p_boot.bin $(SMASX)/ppu.bin \
  $(SMASX)/ppu7.bin $(SMASX)/ppuw.bin $(SMASX)/ppulc.bin $(SMASX)/ppulcw.bin $(SMASX)/input.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -Ifalcon -I$(SMASX) -DSMKX_FRAMES=$(SMASX_FRAMES) \
	  -DSMKX_RENDER=$(SMASX_RENDER) -DSMKX_SNAP_EVERY=$(SMASX_SNAP_EVERY) -DSMKX_SNAP_FROM=$(SMASX_SNAP_FROM) \
	  -DSMKX_M7_DSP=$(SMASX_M7_DSP) -DSMKX_PROFILE_FROM=$(SMASX_PROFILE_FROM) -DSMKX_SLOW=$(SMASX_SLOW) -DSMKX_SYNC=$(SMASX_SYNC) -DSMKX_SUMS=$(SMASX_SUMS) -DSMKX_TRACE_FROM=$(SMASX_TRACE_FROM) -o $@ falcon/smkx_rt.s

smasx-parts: $(SMASX)/flags.stamp
	@$(MAKE) --no-print-directory $(SMASX)/rt.o $(SMASX_C_OBJ)

$(SMASX)/SMASX.TOS: $(SMASX)/sources.txt $(SMASX)/rt.o $(SMASX_C_OBJ) $(SMASX)/flags.stamp $(SMASX)/input.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -o $(SMASX)/tables.o $(SMASX)/smkx_tables.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -o $(SMASX)/input.o $(SMASX)/input.s
	for f in $$(cat $(SMASX)/sources.txt); do \
	  $(VASM) -Felf -m68030 -quiet -nowarn=62 -Ifalcon -o $(SMASX)/$${f%.s}.o $(SMASX)/$$f || exit 1; \
	done
	$(VLINK) -b ataritos -tos-flags 1 -M$(SMASX)/SMASX.map -o $@ $(SMASX)/rt.o \
	  $$(sed 's/\.s$$/.o/;s|^|$(SMASX)/|' $(SMASX)/sources.txt) $(SMASX)/tables.o $(SMASX)/input.o $(SMASX_C_OBJ)

# The stamp first, then a sub-make: a stamp change deletes SMASX.TOS, and
# make 3.81 would not look at the file again within the same run.
smasx: $(SMASX)/flags.stamp
	@$(MAKE) --no-print-directory $(SMASX)/SMASX.TOS

run-smasx: smasx
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $(SMASX)/SMASX.TOS --fast --timeout 1800

# The DSP Mode 7 kernel against the C reference: random VRAM and lines.
$(SMASX)/t_%.o: src/%.c src/snes_ppu.h src/m7_plan.h
	mkdir -p $(SMASX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

M7TEST_OBJ := $(SMASX)/t_m7test.o $(SMASX)/t_m7_plan.o $(SMASX)/t_snes_ppu.o

$(SMASX)/M7TEST.TOS: falcon/m7test.s $(SMASX)/m7c2p.bin $(M7TEST_OBJ)
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SMASX) -o $(SMASX)/m7test.o falcon/m7test.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(SMASX)/m7test.o $(M7TEST_OBJ)

test-m7: $(SMASX)/M7TEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag m7test --grep M7TEST --fast --timeout 800
	@grep -a "M7TEST lines" $(SMASX)/m7test-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

build/c2p_test: tools/c2p_test.c src/ppu_fast.c src/snes_ppu.c src/ppu_fast.h src/ppu_internal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/c2p_test.c src/ppu_fast.c src/snes_ppu.c

# The display path's chunky-to-planar packing against the bit-by-bit one.
test-c2p: build/c2p_test
	build/c2p_test

build/palette_test: tools/palette_test.c src/palette.c src/palette.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/palette_test.c src/palette.c

test-palette: build/palette_test $(SMASX)/oklab555.bin
	build/palette_test $(SMASX)/oklab555.bin

# SNAP.BIN from the runtime: Falcon planes and palette against the host.
build/ppu_snap: tools/ppu_snap.c src/snes_ppu.c src/ppu_fast.c src/snes_ppu.h src/ppu_internal.h \
  src/ppu_fast.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/ppu_snap.c src/snes_ppu.c src/ppu_fast.c

check-snap: build/ppu_snap
	build/ppu_snap $(SMASX)/SNAP.BIN $(SMASX)/smas.sfc

check-snaps: build/ppu_snap
	@fail=0; for f in $(SMASX)/F*.BIN; do \
	  echo "== $$f"; \
	  build/ppu_snap $$f $(SMASX)/smas.sfc $${f%.BIN}.ppm || fail=1; \
	  magick $${f%.BIN}.ppm $${f%.BIN}.png && rm -f $${f%.BIN}.ppm; \
	done; exit $$fail

clean:
	rm -rf build

# ---- Reference run: the ROM on an interpreted 65816 (tools/ref65816.c) -----------------------------------
# Real SMAS frames while the translation does not exist. NOT frames of the translation or of the Falcon:
# tools/ref65816.c runs the game on an interpreter with the runtime's hardware model and writes the
# renderer's inputs of a frame the way falcon/smkx_rt.s's snapshot does (Falcon planes zero).
# tools/ref_tour.py scripts the pad through the select screen into each of the four games.
build/ref65816: tools/ref65816.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/ref65816.c

REF := build/ref
REF_FRAMES ?= 4500
REF_SPARSE ?= 100               # every Nth frame of each game's run: titles, menus, levels (mixed scenes)
REF_WINDOW ?= 3000:3119         # consecutive frames of each run (gameplay), for the models that need them
# Frames of game k are numbered k * 65536 + n in the file names: one directory, four runs.
$(REF)/stamp: build/ref65816 tools/ref_tour.py $(SMASX)/smas.sfc
	rm -rf $(REF)
	mkdir -p $(REF)/sparse $(REF)/window
	for g in 0 1 2 3; do \
	  REF_INPUT="$$(python3 tools/ref_tour.py $$g $(REF_FRAMES))" REF_SNAP_BASE=$$((g * 65536)) \
	    REF_SNAP_DIR=$(REF)/sparse REF_SNAP_FROM=$(REF_SPARSE) REF_SNAP_EVERY=$(REF_SPARSE) \
	    build/ref65816 $(SMASX)/smas.sfc - $(REF_FRAMES) || exit 1; \
	  REF_INPUT="$$(python3 tools/ref_tour.py $$g $(REF_FRAMES))" REF_SNAP_BASE=$$((g * 65536)) \
	    REF_SNAP_DIR=$(REF)/window REF_SNAP_FROM=100000 REF_SNAP_WINDOW=$(REF_WINDOW) \
	    build/ref65816 $(SMASX)/smas.sfc - $(REF_FRAMES) || exit 1; \
	done
	touch $@

ref-snaps: $(REF)/stamp

# ---- List compositor, the blitter renderer (analysis/list-compositor-smas.md) ---------------------
# F030CT's model and player with F030SMW's rules (tools/lc/lc_model.c, LC_SMW=1: LoROM bus, here with
# SMAS's 2 MiB; colour math that changes no index), built against this tree's src/snes_ppu.c. The snapshots
# are the runtime's (SMASX_SNAP_EVERY=1 and SMASX_SNAP_FROM=n write consecutive frames) or, by default, the
# reference run's ($(REF), `make ref-snaps`): the real ROM's frames, not the translation's.
LC := build/lc
LC_SNAPS ?= $(REF)/sparse $(REF)/window
LC_ENV := LC_SMW=1 LC_CHOOSER=exact LC_OVERLAY=ring LC_CACHE_KB=64

$(LC)/lc_model: tools/lc/lc_model.c src/snes_ppu.c src/snes_ppu.h | build
	mkdir -p $(LC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/lc/lc_model.c src/snes_ppu.c -lm

# The snapshots in CT's layout, one flat directory. LC_SNAPS names directories; the reference run is made
# when it is the one asked for and does not exist.
lc-snaps: $(if $(findstring $(REF),$(LC_SNAPS)),$(REF)/stamp)
	@ls $(addsuffix /F*.BIN,$(LC_SNAPS)) >/dev/null 2>&1 || { echo "no snapshots in $(LC_SNAPS)"; exit 1; }
	rm -rf $(LC)/snaps
	python3 tools/lc/smas_to_ct_snap.py $(LC)/snaps $(wildcard $(addsuffix /F*.BIN,$(LC_SNAPS)))

# Every snapshot through the model (which frames it covers), then the longest run of consecutive
# covered frames through the ring model (hreal), recorded as the stream the player plays.
check-lc-model: $(LC)/lc_model $(SMASX)/smas.sfc lc-snaps
	LC_SMW=1 $(LC)/lc_model census $(SMASX)/smas.sfc $(LC)/snaps/F*.BIN | tee $(LC)/census.txt | tail -1
	LC_SMW=1 $(LC)/lc_model seq $(SMASX)/smas.sfc $(LC)/snaps/F*.BIN | tail -2
	LC_SMW=1 $(LC)/lc_model blitfuzz 50000 1 | tail -1
	python3 tools/lc/lc_longest.py $(LC)/snaps < $(LC)/census.txt > $(LC)/window.txt
	@test -s $(LC)/window.txt || { echo "no frame the model covers"; exit 1; }
	@echo "longest covered run: $$(wc -l < $(LC)/window.txt | tr -d ' ') frames"
	LC_REC=$(LC)/smas.lcp $(LC_ENV) $(LC)/lc_model hreal $(SMASX)/smas.sfc $$(cat $(LC)/window.txt) | tail -6
	LC_SMW=1 $(LC)/lc_model play $(LC)/smas.lcp

# The model's synthetic self-test alone (no ROM, no snapshot): random pieces through the blit-row generator.
check-lc-fuzz: $(LC)/lc_model
	LC_SMW=1 $(LC)/lc_model blitfuzz 50000 1 | tail -1

# The recorded stream played by falcon/lcplay.s in the patched Hatari: every frame's page window
# against the stream's checksum.
check-lc-play: check-lc-model
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_lcplay.py check $(LC)/smas.lcp

bench-lc-play: check-lc-model
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_lcplay.py bench $(LC)/smas.lcp

# ---- The DSP tile renderer, taken over from F030CT (analysis/list-compositor-dsp-plan.md, step 1) ---
# Host model of the DSP design (tools/dsp_model.c, the planner in src/ppu_dsp_plan.c) against the
# reference, on SMAS snapshots in CT's layout (lc-snaps). LoROM bus.
build/dsp_snap: tools/dsp_snap.c tools/dsp_model.c tools/dsp_model.h src/ppu_dsp_plan.c src/ppu_dsp.h \
  src/m7_plan.c src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/dsp_snap.c tools/dsp_model.c \
	  src/ppu_dsp_plan.c src/snes_ppu.c src/m7_plan.c

build/dsp_rand: tools/dsp_rand.c tools/dsp_model.c tools/dsp_model.h src/ppu_dsp_plan.c src/ppu_dsp.h \
  src/m7_plan.c src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/dsp_rand.c tools/dsp_model.c \
	  src/ppu_dsp_plan.c src/snes_ppu.c src/m7_plan.c

check-dsp-model: build/dsp_snap lc-snaps
	-build/dsp_snap $(SMASX)/smas.sfc $(LC)/snaps/F*.BIN > $(LC)/dsp_snap.txt
	@echo "$$(grep -c 'diff 0 words in 0 rows' $(LC)/dsp_snap.txt) of $$(ls $(LC)/snaps/F*.BIN | wc -l | tr -d ' ') snapshots: model equals the reference"
	@echo "$$(grep -c "Falcon's eight planes vs the model: [1-9]" $(LC)/dsp_snap.txt) snapshots where the Falcon's own frame differs (the fast composer has no windows or sub screen)"
	@test "$$(grep -c 'diff [1-9]\|READ OUTSIDE' $(LC)/dsp_snap.txt)" = 0

# The DSP program in its three builds (same source): tile frames, Mode 7 frames, tile frames with
# windows and the sub screen. Booted by falcon/smkx_dspboot.s (no XBIOS Dsp_ExecBoot).
$(SMASX)/ppu.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(SMASX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=0 --define LCB=0 $< $@

$(SMASX)/ppu7.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(SMASX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=1 --define WIN=0 --define LCB=0 $< $@

$(SMASX)/ppuw.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(SMASX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=0 $< $@

# The list compositor's group composer (LCB=1): only the groups a per-row mask names, as list records.
$(SMASX)/ppulc.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(SMASX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=0 --define LCB=1 $< $@

# ... and with the window build in it (frames whose rows have windows masking layers)
$(SMASX)/ppulcw.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(SMASX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=1 $< $@

# PPUTEST: real frames, then random ones (flips, 16x16 tiles, mode 0, windows, the sub screen), each composed by the
# DSP in Hatari and compared with the reference. The real frames are PPUTEST_FRAMES (snapshots in CT's layout),
# by default the reference run's sparse frames of all four games (PPUTEST_REAL=1; frames the DSP planner does not
# take, such as the Mode 3 title and menus, go to the CPU renderer and are left out, as in the game). PPUTEST_REAL=0:
# random frames only, nothing needs the game or the reference run. LCDTEST takes every PPUTEST_LCD_STEP-th real frame.
PPUTEST_RANDOM ?= 32
PPUTEST_REAL ?= 1
PPUTEST_LCD_STEP ?= 25
PPUTEST_FRAMES ?=

# $(SMASX)/ppu_tests.bin and lcd_tests.bin come from one rule; the real frames' list is made in the recipe.
$(SMASX)/ppu_tests.bin: build/dsp_snap build/dsp_rand $(SMASX)/smas.sfc FORCE
	build/dsp_rand $(PPUTEST_RANDOM) 0x5eed $(SMASX)/ppu_tests_rand.bin $(SMASX)/lcd_tests_rand.bin > $(SMASX)/ppu_tests_rand.txt
	@frames="$(PPUTEST_FRAMES)"; \
	if [ "$(PPUTEST_REAL)" = 1 ]; then \
	  $(MAKE) --no-print-directory lc-snaps || exit 1; \
	  [ -n "$$frames" ] || frames="$$(ls $(REF)/sparse/F*.BIN | sed 's|.*/|$(LC)/snaps/|')"; \
	fi; \
	if [ -n "$$frames" ]; then \
	  build/dsp_snap $(SMASX)/smas.sfc $$frames -pack $(SMASX)/ppu_tests_real.bin > $(SMASX)/ppu_tests_real.txt; \
	  ! grep -q 'diff [1-9]\|READ OUTSIDE' $(SMASX)/ppu_tests_real.txt || { echo "the DSP model differs on a real frame"; exit 1; }; \
	  echo "PPUTEST: $$(echo $$frames | wc -w | tr -d ' ') real frames, $$(grep -c 'diff 0 words' $(SMASX)/ppu_tests_real.txt) planned by the DSP, $$(grep -c 'does not fit' $(SMASX)/ppu_tests_real.txt) left to the CPU renderer"; \
	  { echo "$$frames" | tr ' ' '\n' | awk 'NR % $(PPUTEST_LCD_STEP) == 0'; \
	    LC_DEBUG=1 build/dsp_snap $(SMASX)/smas.sfc $$frames -packlc /dev/null 2>&1 >/dev/null | awk '/^packlc/ && $$4 == 2 { print "$(LC)/snaps/" $$2 }'; \
	  } | sort -u > $(SMASX)/lcd_frames.txt; \
	  python3 -c "import sys,struct; a,b=(open(f,'rb').read() for f in sys.argv[1:3]); \
	    open(sys.argv[3],'wb').write(b'PPU2'+struct.pack('>I',struct.unpack('>I',a[4:8])[0]+struct.unpack('>I',b[4:8])[0])+a[8:]+b[8:])" \
	    $(SMASX)/ppu_tests_real.bin $(SMASX)/ppu_tests_rand.bin $@.new; \
	else cp $(SMASX)/ppu_tests_rand.bin $@.new; : > $(SMASX)/lcd_frames.txt; echo "PPUTEST: random frames only"; fi
	@cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@

$(SMASX)/t_pputest.o: src/pputest.c src/ppu_dsp.h src/snes_ppu.h src/smkx_prof.h
	mkdir -p $(SMASX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(SMASX)/PPUTEST.TOS: falcon/pputest.s falcon/smkx_dspboot.s $(SMASX)/ppu.bin $(SMASX)/ppu7.bin $(SMASX)/ppuw.bin \
  $(SMASX)/ppu_tests.bin $(SMASX)/t_pputest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SMASX) -Ifalcon -o $(SMASX)/pputest.o falcon/pputest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(SMASX)/pputest.o $(SMASX)/t_pputest.o

test-ppu: $(SMASX)/PPUTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag pputest --grep PPUTEST --fast --timeout 800
	@grep -a "PPUTEST frames" $(SMASX)/pputest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

# ---- The orchestrator model and the CPU walker (analysis/list-compositor-dsp-plan.md) ------------------
# tools/lc/lcd_model.c: which groups of the scroll ring each frame writes, from inputs only (VRAM writes, OAM,
# registers, scroll), two pages, exact against the reference; LCD_REC writes the walker's stream.
# falcon/lcwalk.s plays it in Hatari: the CPU only stores, copies and points the Videl at the window.
$(LC)/lcd_model: tools/lc/lcd_model.c tools/lc/lc_model.c src/lc_plan.c src/lc_plan.h src/snes_ppu.c src/snes_ppu.h | build
	mkdir -p $(LC)
	$(CC) -Isrc $(CPPFLAGS) $(CFLAGS) -Wno-unused $(LDFLAGS) -o $@ tools/lc/lcd_model.c src/lc_plan.c src/snes_ppu.c -lm

check-lcd: $(LC)/lcd_model check-lc-model
	LCD_REC=$(LC)/smas.lcw $(LC)/lcd_model $(SMASX)/smas.sfc $$(cat $(LC)/window.txt) | tail -1
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_lcwalk.py check $(LC)/smas.lcw

bench-lcw: check-lcd
	python3 scripts/run_lcwalk.py bench $(LC)/smas.lcw

# LCDTEST: the LC build on frames with masks of several patterns; each frame's list against the model.
# Real frames as selected by ppu_tests.bin (lcd_frames.txt: every PPUTEST_LCD_STEP-th, and every frame the window build LCW takes; of
# those the ones the tile or window build holds in 77 or 73 slots, colour math left out as the compositor does),
# then the random ones of tools/dsp_rand.c. Hatari does not load a LCDTEST.TOS above about 13 MB.
$(SMASX)/lcd_tests.bin: build/dsp_snap $(SMASX)/ppu_tests.bin $(SMASX)/smas.sfc FORCE
	@if [ -s $(SMASX)/lcd_frames.txt ]; then \
	  build/dsp_snap $(SMASX)/smas.sfc $$(cat $(SMASX)/lcd_frames.txt) -packlc $(SMASX)/lcd_tests_real.bin > /dev/null; \
	  python3 -c "import sys,struct; a,b=(open(f,'rb').read() for f in sys.argv[1:3]); \
	    open(sys.argv[3],'wb').write(b'LCD1'+struct.pack('>I',struct.unpack('>I',a[4:8])[0]+struct.unpack('>I',b[4:8])[0])+a[8:]+b[8:])" \
	    $(SMASX)/lcd_tests_real.bin $(SMASX)/lcd_tests_rand.bin $@.new; \
	else cp $(SMASX)/lcd_tests_rand.bin $@.new; echo "LCDTEST: random frames only"; fi
	@cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@

$(SMASX)/t_lcdtest.o: src/lcdtest.c src/ppu_dsp.h src/smkx_prof.h
	mkdir -p $(SMASX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(SMASX)/LCDTEST.TOS: falcon/lcdtest.s falcon/smkx_dspboot.s $(SMASX)/ppulc.bin $(SMASX)/ppulcw.bin $(SMASX)/lcd_tests.bin $(SMASX)/t_lcdtest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SMASX) -Ifalcon -o $(SMASX)/lcdtest.o falcon/lcdtest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(SMASX)/lcdtest.o $(SMASX)/t_lcdtest.o

test-lcd: $(SMASX)/LCDTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag lcdtest --grep LCDTEST --fast --timeout 800
	@grep -a "LCDTEST frames" $(SMASX)/lcdtest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'
