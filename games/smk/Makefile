CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Isrc
LDFLAGS ?=
LDLIBS ?= -lm

# This workstation's macOS 27 SDK uses a .tbd architecture unknown to its
# installed linker. Prefer its compatible 26.5 SDK when both are present.
ifeq ($(shell uname -s),Darwin)
ifneq ($(wildcard /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk),)
CPPFLAGS += -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
LDFLAGS += -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
endif
endif

OBJ := build/renderer.o
ARCADE_TOOLS := ../../../F030Arcade/third_party
VASM := $(ARCADE_TOOLS)/vasm/vasmm68k_mot
VLINK := $(ARCADE_TOOLS)/vlink/vlink
HATARI := ../../../F030Arcade/third_party/hatari/build/src/hatari
TOS404 := $(ARCADE_TOOLS)/tos/tos404.img
SMK_SOURCE := third_party/Super-Mario-Kart-Disassembly
SMK_U1_MAP := $(SMK_SOURCE)/SMK/RomMap/ROM_Map_SMK_U1.asm
SMK_ROUTINES := $(SMK_SOURCE)/SMK/Routine_Macros_SMK.asm
SMK_ASSET_MAP := $(SMK_SOURCE)/SMK/AsarScripts/AssetPointersAndFiles.asm
SMK_TRACK_ASSET := $(SMK_SOURCE)/SMK/TrackData/MarioCircuit1.lz5
SMK_PALETTE_ASSET := $(SMK_SOURCE)/SMK/Palettes/MarioCircuitCourse.lz5
ROM_ZIP ?= assets/Super Mario Kart (USA).zip
empty :=
space := $(empty) $(empty)
escape_space = $(subst $(space),\$(space),$(1))
ROM_ZIP_PREREQUISITE := $(call escape_space,$(ROM_ZIP))

.PHONY: check-lc-model check-lc-play bench-lc-play test-ppu-tile test-lcd tile-snaps check-dsp-model check-lcd-model race-snaps bench-race test-ssi test-ppu all preview test falcon run-hatari verify-hatari smk-assets smkx-source smkx run-smkx play-smkx test-dsp1 test-dsp1-hle test-m7 check-snap check-snaps test-palette check-ref clean
all: smk-assets

build:
	mkdir -p build

build/renderer.o: src/renderer.c src/renderer.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build/preview: tools/preview.c $(OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/test_renderer: tests/test_renderer.c $(OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

out/.stamp: build/preview
	mkdir -p out
	build/preview out/frame.ppm out/scene_4bpp.bin out/assets.f3mc out/frame_4bpp.bin
	touch $@

out/.smk.stamp: tools/extract_smk.py $(SMK_U1_MAP) $(SMK_ROUTINES) $(SMK_ASSET_MAP) $(SMK_TRACK_ASSET) $(SMK_PALETTE_ASSET) $(ROM_ZIP_PREREQUISITE)
	python3 tools/extract_smk.py "$(ROM_ZIP)" out
	touch $@

smk-assets: out/.smk.stamp

# 65816 -> 68030 translation of the ROM's program (tools/smkx). Everything
# under build/smkx is derived from the ROM and stays untracked.
SMKX := build/smkx
SMKX_FRAMES ?= 0
SMKX_PY := $(wildcard tools/smkx/*.py) tools/smkx/dispatch_targets.txt tools/smkx/probes.txt \
  tools/smkx/marks.txt tools/smkx/tables.txt

$(SMKX)/sources.txt: $(SMKX_PY) $(SMK_ROUTINES) $(ROM_ZIP_PREREQUISITE)
	python3 -m tools.smkx "$(ROM_ZIP)" $(SMKX)

smkx-source: $(SMKX)/sources.txt

# The DSP-1 runs its own program when assets/dsp1.bin (the uPD77C25 dump,
# MD5 4865ac61cd758b0f9383fe3d4d3b8694) is present, translated by
# tools/smkx/upd2m68k.py; otherwise the geometric model in smkx_dsp1.s.
DSP1_BIN ?= assets/dsp1.bin
UPD_PY := tools/smkx/upd2m68k.py tools/smkx/upd7725.py tools/smkx/upd2c.py
ifneq ($(wildcard $(DSP1_BIN)),)
SMKX_DSP1_OBJ := $(SMKX)/upd_code.o
SMKX_DSP1_DEF := -DSMKX_DSP1_LLE
endif

# DSP-1 regions as value-level C (tools/smkx/upd2c.py), hooked into the
# translated firmware at their heads.
$(SMKX)/dsp1_regions.c: $(DSP1_BIN) tools/smkx/upd2c.py | $(SMKX)/sources.txt
	python3 -m tools.smkx.upd2c $(DSP1_BIN) $@

$(SMKX)/dsp1_regions.o: $(SMKX)/dsp1_regions.c src/dsp1_hle.h
	$(CROSS_CC) $(CROSS_CFLAGS) -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -c -o $@ $<

$(SMKX)/upd_code.s: $(DSP1_BIN) $(UPD_PY)
	python3 -m tools.smkx.upd2m68k $(DSP1_BIN) $(SMKX)

$(SMKX)/upd_code.o: $(SMKX)/upd_code.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SMKX) -o $@ $<

# The video adapter's C code, cross-compiled freestanding for the 68030.
CROSS_CC ?= ../../../cross-mint/bin/m68k-atari-mintelf-gcc
CROSS_CFLAGS := -m68030 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
  -fomit-frame-pointer -nostdlib -Wall -Wextra -Isrc
SMKX_C_OBJ := $(SMKX)/snes_ppu.o $(SMKX)/ppu_fast.o $(SMKX)/smkx_video.o $(SMKX)/palette.o \
  $(SMKX)/smkx_m7.o $(SMKX)/m7_plan.o $(SMKX)/dsp1_regions.o $(SMKX)/ppu_dsp.o $(SMKX)/dsp1_native.o
SMKX_RENDER ?= 1            # 0: no rendering at all (timing and reference runs); otherwise smkx_render is called every frame and
                            # throttles itself (src/smkx_video.c)

$(SMKX)/%.o: src/%.c src/snes_ppu.h src/ppu_internal.h src/ppu_fast.h src/palette.h src/smkx_m7.h src/m7_plan.h src/smkx_prof.h src/dsp1_hle.h src/ppu_dsp.h src/dsp1_native.h \
  $(SMKX)/flags.stamp | $(SMKX)/sources.txt
	$(CROSS_CC) $(CROSS_CFLAGS) $(SMKX_PROF_DEF) -c -o $@ $<

# Build switches are inputs too: the stamp changes whenever they do, so a new
# frame limit or render interval always relinks.
SMKX_SNAP_EVERY ?= 0
SMKX_M7_DSP ?= 1
SMKX_PROFILE ?= 0
SMKX_DSP1_LOG ?= 0
SMKX_DSP1_HLE ?= 1
# SMKX_DSP1_NATIVE=0: every DSP-1 command by the translated firmware
SMKX_DSP1_NATIVE ?= 1
SMKX_DIAG ?= 1
SMKX_WATCH ?=
ifneq ($(SMKX_WATCH),)
SMKX_WATCH_DEF := -DSMKX_WATCH=$(SMKX_WATCH)
endif
ifeq ($(SMKX_DSP1_LOG),1)
SMKX_LOG_DEF := -DSMKX_DSP1_LOG
endif
ifeq ($(SMKX_PROFILE),1)
SMKX_PROF_DEF := -DSMKX_PROFILE -DPPU_PROFILE
endif
# SMKX_IOSTATS=1: count register accesses, print the busiest at the limit
ifeq ($(SMKX_IOSTATS),1)
SMKX_PROF_DEF += -DSMKX_IOSTATS
endif
# SMKX_STATUS_EVERY=n: frames between status lines (printing costs time)
SMKX_STATUS_EVERY ?= 50
SMKX_PROF_DEF += -DSMKX_STATUS_EVERY=$(SMKX_STATUS_EVERY)
# SMKX_RENDER_FROM=n: no renders before frame n (profiles of one scene)
SMKX_RENDER_FROM ?= 0
SMKX_PROF_DEF += -DSMKX_RENDER_FROM=$(SMKX_RENDER_FROM)
# SMKX_REF_RENDER=1: the reference renderer composes the frames (timing A/B)
# SMKX_PACE=0: SNES frames on every VBL, even while SMK's main loop is busy
# (the IRQs then upload half-built OAM buffers; see rt_vbl)
SMKX_PACE ?= 1
# SMKX_LC=1: the list compositor (analysis/list-compositor-smk.md) draws the tile-mode screens it covers in
# eight planes, SMK's own DSP renderer everything else; 0: SMK's renderer alone, as before
SMKX_LC ?= 0
SMKX_LC_BLIT_MIN ?= 24          # blocks of this many groups or more go to the blitter (hog mode); 100000: CPU only
SMKX_LC_WATCHDOG ?= 307200      # a DSP job not done after this many 1/38400 s (8 s) ends the compositor
# SMKX_HOLD=1: no exit at the frame limit; the last picture stays on the Videl (scripts/run_smkx.py --until, --screenshot)
SMKX_HOLD ?= 0
# SMKX_VBLSTAT=1: the status line also prints the VBL count and _hz_200 (emulated time, for speed comparisons)
SMKX_VBLSTAT ?= 0
# SMKX_SUMS=1: log a WRAM sum per frame for tools/ref65816.c (make check-ref)
SMKX_SUMS ?= 0
SMKX_REF_RENDER ?= 0
ifeq ($(SMKX_REF_RENDER),1)
SMKX_PROF_DEF += -DSMKX_REF_RENDER
endif
ifeq ($(SMKX_VBLSTAT),1)
SMKX_PROF_DEF += -DSMKX_VBLSTAT
endif
ifeq ($(SMKX_HOLD),1)
SMKX_PROF_DEF += -DSMKX_HOLD
endif
ifeq ($(SMKX_LC),1)
SMKX_PROF_DEF += -DSMKX_LC
SMKX_LC_OBJ := $(SMKX)/tile.o
SMKX_LC_BINS := $(SMKX)/tile/ppu_tile.bin $(SMKX)/tile/ppu_tilew.bin $(SMKX)/tile/ppu_tilelc.bin $(SMKX)/tile/ppu_tilelcw.bin
endif
SMKX_FLAGS := FRAMES=$(SMKX_FRAMES) RENDER=$(SMKX_RENDER) DSP1=$(SMKX_DSP1_DEF) \
  SNAP_EVERY=$(SMKX_SNAP_EVERY) M7_DSP=$(SMKX_M7_DSP) PROFILE=$(SMKX_PROFILE) DSP1_LOG=$(SMKX_DSP1_LOG) DSP1_HLE=$(SMKX_DSP1_HLE) DSP1_NATIVE=$(SMKX_DSP1_NATIVE) DIAG=$(SMKX_DIAG) WATCH=$(SMKX_WATCH) INPUT=$(SMKX_INPUT) REF_RENDER=$(SMKX_REF_RENDER) RENDER_FROM=$(SMKX_RENDER_FROM) STATUS_EVERY=$(SMKX_STATUS_EVERY) IOSTATS=$(SMKX_IOSTATS) PACE=$(SMKX_PACE) SUMS=$(SMKX_SUMS) LC=$(SMKX_LC) LC_BLIT_MIN=$(SMKX_LC_BLIT_MIN) LC_WATCHDOG=$(SMKX_LC_WATCHDOG) HOLD=$(SMKX_HOLD) VBLSTAT=$(SMKX_VBLSTAT)
# A changed switch also removes what it builds: make 3.81 compares whole
# seconds, and a rebuild within the second of the last link would be skipped.
# `smkx` then builds the TOS in a sub-make, which looks at the files afresh
# (within one run, make does not notice the deletion). From F030SMW.
$(SMKX)/flags.stamp: FORCE | $(SMKX)/sources.txt
	@test "$$(cat $@ 2>/dev/null)" = "$(SMKX_FLAGS)" || { echo "$(SMKX_FLAGS)" > $@; \
	  rm -f $(SMKX)/SMKX.TOS $(SMKX_C_OBJ) $(SMKX)/tile.o $(SMKX)/tilec/*.o; }

.PHONY: FORCE
FORCE:

# Scripted joypad input: SMKX_INPUT="FRAME:PAD,..." (tools/smkx/input_script.py).
SMKX_INPUT ?=
$(SMKX)/input.s: tools/smkx/input_script.py FORCE | $(SMKX)/sources.txt
	python3 -m tools.smkx.input_script "$(SMKX_INPUT)" $@

# The info screen and the player's files, read at startup (tools/port_info.py,
# tools/games.py): run every time, rewritten only when it changes.
# INFO_WAIT=n keeps the screen n seconds (a key skips); a release uses it.
INFO_WAIT ?= 0
$(SMKX)/info.i: $(SMKX)/sources.txt ../../tools/port_info.py ../../tools/games.py FORCE
	python3 ../../tools/port_info.py smk $@ --rom $(SMKX)/smk.sfc --wait $(INFO_WAIT) $(if $(SMKX_DSP1_DEF),--dsp1 $(DSP1_BIN))

$(SMKX)/SMKX.TOS: $(SMKX)/sources.txt falcon/smkx_rt.s falcon/smkx_rt.i falcon/smkx_info.s $(SMKX)/info.i falcon/smkx_dsp1.s \
  falcon/smkx_upd.s falcon/smkx_video.s falcon/smkx_m7dsp.s falcon/smkx_dspboot.s falcon/smkx_lcput.s \
  $(SMKX_LC_OBJ) $(SMKX_LC_BINS) $(SMKX_DSP1_OBJ) $(SMKX_C_OBJ) \
  $(SMKX)/flags.stamp $(SMKX)/oklab555.bin $(SMKX)/ppu.bin $(SMKX)/input.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -Ifalcon -I$(SMKX) -I$(SMKX)/tile -DSMKX_FRAMES=$(SMKX_FRAMES) \
	  -DSMKX_RENDER=$(SMKX_RENDER) -DSMKX_SNAP_EVERY=$(SMKX_SNAP_EVERY) \
	  -DSMKX_M7_DSP=$(SMKX_M7_DSP) -DSMKX_PACE=$(SMKX_PACE) -DSMKX_SUMS=$(SMKX_SUMS) $(SMKX_PROF_DEF) $(SMKX_LOG_DEF) -DUPD_HLE=$(SMKX_DSP1_HLE) -DUPD_NATIVE=$(SMKX_DSP1_NATIVE) -DSMKX_DIAG=$(SMKX_DIAG) $(SMKX_WATCH_DEF) $(SMKX_DSP1_DEF) \
	  -o $(SMKX)/rt.o falcon/smkx_rt.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -o $(SMKX)/tables.o $(SMKX)/smkx_tables.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -o $(SMKX)/input.o $(SMKX)/input.s
	for f in $$(cat $(SMKX)/sources.txt); do \
	  $(VASM) -Felf -m68030 -quiet -nowarn=62 -Ifalcon -o $(SMKX)/$${f%.s}.o $(SMKX)/$$f || exit 1; \
	done
	$(VLINK) -b ataritos -tos-flags 1 -M$(SMKX)/SMKX.map -o $@ $(SMKX)/rt.o \
	  $$(sed 's/\.s$$/.o/;s|^|$(SMKX)/|' $(SMKX)/sources.txt) $(SMKX)/tables.o $(SMKX)/input.o $(SMKX_DSP1_OBJ) \
	  $(SMKX_C_OBJ) $(SMKX_LC_OBJ)

$(SMKX)/oklab555.bin: tools/smkx/oklab.py | $(SMKX)/sources.txt
	python3 -m tools.smkx.oklab $@

build/palette_test: tools/palette_test.c src/palette.c src/palette.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/palette_test.c src/palette.c

test-palette: build/palette_test $(SMKX)/oklab555.bin
	build/palette_test $(SMKX)/oklab555.bin

# Renders a SNAP.BIN on the host with the same C source and compares the
# Falcon's planes byte for byte.
build/ppu_snap: tools/ppu_snap.c src/snes_ppu.c src/snes_ppu.h src/ppu_fast.c src/ppu_fast.h \
  src/ppu_internal.h src/palette.c src/palette.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/ppu_snap.c src/snes_ppu.c src/ppu_fast.c src/palette.c

# ppu_dsp_frame_slots (fast path included) against ppu_dsp_frame, on snapshots.
build/stream_check: tools/stream_check.c src/snes_ppu.c src/snes_ppu.h src/ppu_internal.h \
  src/ppu_dsp.c src/ppu_dsp.h src/palette.c src/palette.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/stream_check.c src/snes_ppu.c src/ppu_dsp.c src/palette.c

# The race census: each row's register state in a snapshot (tools/race_census.c).
build/race_census: tools/race_census.c src/snes_ppu.c src/snes_ppu.h src/ppu_internal.h \
  src/ppu_fast.c src/ppu_fast.h src/palette.c src/palette.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/race_census.c src/snes_ppu.c src/ppu_fast.c src/palette.c

check-snap: build/ppu_snap $(SMKX)/oklab555.bin
	build/ppu_snap $(SMKX)/SNAP.BIN $(SMKX)/smk.sfc $(SMKX)/oklab555.bin

# Every periodic snapshot (SMKX_SNAP_EVERY), with a PNG of each frame.
check-snaps: build/ppu_snap $(SMKX)/oklab555.bin
	@fail=0; for f in $(SMKX)/F*.BIN; do \
	  echo "== $$f"; \
	  build/ppu_snap $$f $(SMKX)/smk.sfc $(SMKX)/oklab555.bin $${f%.BIN}.ppm || fail=1; \
	  test ! -f $${f%.BIN}.ppm || { magick $${f%.BIN}.ppm $${f%.BIN}.png && rm -f $${f%.BIN}.ppm; }; \
	done; exit $$fail

# The translated DSP-1 against the Python interpreter, byte for byte.
$(SMKX)/UPDTEST.TOS: falcon/updtest.s falcon/smkx_upd.s $(SMKX)/upd_code.o $(SMKX)/dsp1_regions.o \
  tools/smkx/updtest.py
	python3 -m tools.smkx.updtest gen $(DSP1_BIN) $(SMKX)
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SMKX) -Ifalcon -o $(SMKX)/updtest.o falcon/updtest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(SMKX)/updtest.o $(SMKX)/upd_code.o $(SMKX)/dsp1_regions.o

# The native DSP-1 hot paths against the firmware, on the host: vectors from
# a recorded game stream (SMKX_DSP1_LOG=1 run) through the interpreter.
build/dsp1_hle_test: tools/dsp1_hle_test.c src/dsp1_hle.c src/dsp1_hle.h $(SMKX)/dsp1_regions.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable \
	  $(LDFLAGS) -o $@ tools/dsp1_hle_test.c src/dsp1_hle.c $(SMKX)/dsp1_regions.c

test-dsp1-hle: build/dsp1_hle_test
	python3 -m tools.smkx.dsp1_hle $(DSP1_BIN) $(SMKX)/DSP1LOG.BIN
	python3 -m tools.smkx.dsp1_hle $(DSP1_BIN) $(SMKX)/DSP1LOG.BIN --vectors $(SMKX)/raster_vectors.bin
	python3 -m tools.smkx.dsp1_hle $(DSP1_BIN) $(SMKX)/DSP1LOG.BIN --vectors $(SMKX)/project_vectors.bin --region project
	build/dsp1_hle_test raster $(SMKX)/raster_vectors.bin $(DSP1_BIN)
	build/dsp1_hle_test region_raster $(SMKX)/raster_vectors.bin $(DSP1_BIN)
	build/dsp1_hle_test region_project $(SMKX)/project_vectors.bin $(DSP1_BIN)

test-dsp1: $(SMKX)/UPDTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $(SMKX)/UPDTEST.TOS --tag updtest --grep UPDTEST --fast --timeout 900
	python3 -m tools.smkx.updtest check $(SMKX) $(SMKX)/updtest-stderr.log

# The DSP Mode 7 kernel (ASM56000 under DOSBox-X) and its test against the
# C reference: random VRAM and lines, planes compared word for word.
$(SMKX)/m7c2p.bin: falcon/dsp/m7c2p.asm tools/smkx/dsp_asm.py | $(SMKX)/sources.txt
	python3 -m tools.smkx.dsp_asm $< $@

$(SMKX)/t_%.o: src/%.c src/snes_ppu.h src/ppu_internal.h src/m7_plan.h src/ppu_dsp.h | $(SMKX)/sources.txt
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

M7TEST_OBJ := $(SMKX)/t_m7test.o $(SMKX)/t_m7_plan.o $(SMKX)/t_snes_ppu.o

$(SMKX)/M7TEST.TOS: falcon/m7test.s $(SMKX)/m7c2p.bin $(M7TEST_OBJ)
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SMKX) -o $(SMKX)/m7test.o falcon/m7test.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(SMKX)/m7test.o $(M7TEST_OBJ)

# CPU -> DSP data by DMA playback into the DSP's SSI receive (scaffold):
# falcon/dsp/ssirx.asm fills a ring from its receive interrupt, SSITEST
# plays a buffer and checks what arrived (with the DSP idle and busy).
$(SMKX)/ssirx.bin: falcon/dsp/ssirx.asm tools/smkx/dsp_asm.py | $(SMKX)/sources.txt
	python3 -m tools.smkx.dsp_asm $< $@

$(SMKX)/SSITEST.TOS: falcon/ssitest.s $(SMKX)/ssirx.bin
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SMKX) -o $(SMKX)/ssitest.o falcon/ssitest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(SMKX)/ssitest.o

test-ssi: $(SMKX)/SSITEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag ssitest --grep SSITEST --fast --timeout 300
	@grep -a "SSITEST errors" $(SMKX)/ssitest-stderr.log | tail -1 | tee /dev/stderr | grep -q "errors (handshake) 00000000"

# The DSP PPU (falcon/dsp/ppu.asm): boot image and the program beyond it.
$(SMKX)/ppu.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py | $(SMKX)/sources.txt
	python3 -m tools.smkx.dsp_asm $< $@

# PT_SNAP=build/smkx/Fnnnnnnn.BIN: PPUTEST also times that frame on the DSP
PT_SNAP ?=
$(SMKX)/PPUTEST.TOS: falcon/pputest.s $(SMKX)/ppu.bin $(SMKX)/t_pputest.o $(SMKX)/t_snes_ppu.o $(SMKX)/t_m7_plan.o $(SMKX)/t_ppu_dsp.o
	$(if $(PT_SNAP),cp $(PT_SNAP) $(SMKX)/PTSNAP.BIN)
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SMKX) $(if $(PT_SNAP),-DPT_SNAP) -o $(SMKX)/pputest.o falcon/pputest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(SMKX)/pputest.o $(SMKX)/t_pputest.o $(SMKX)/t_snes_ppu.o $(SMKX)/t_m7_plan.o $(SMKX)/t_ppu_dsp.o

# Frozen race pictures for the DSP work (one and two players, from the
# census runs on the reference interpreter; derived from the ROM, so they
# stay under build/).
RACE_SNAPS ?= build/racesnap/R1P.BIN build/racesnap/R2P.BIN
race-snaps: build/ref65816 build/race_census
	python3 scripts/race_census_runs.py build/census
	mkdir -p build/racesnap
	cp build/census/gp1/F0000BB8.BIN build/racesnap/R1P.BIN
	cp build/census/p2_0_0/F0000BB8.BIN build/racesnap/R2P.BIN

# PPUTEST's snapshot benchmark on each race picture: the DSP's time per
# frame (as it is, without sprites, without BG1, with neither, with the
# SSI) and the SSI frame checked against the C reference. Emulated time.
bench-race: $(SMKX)/ppu.bin $(SMKX)/t_snes_ppu.o $(SMKX)/t_m7_plan.o $(SMKX)/t_ppu_dsp.o
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	$(CROSS_CC) $(CROSS_CFLAGS) -DPT_SNAP -c -o $(SMKX)/t_pputest_snap.o src/pputest.c
	@for f in $(RACE_SNAPS); do n=$$(basename $$f .BIN); \
	  cp $$f $(SMKX)/PTSNAP.BIN; \
	  $(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SMKX) -DPT_SNAP -o $(SMKX)/pputest_snap.o falcon/pputest.s && \
	  $(VLINK) -b ataritos -tos-flags 1 -o $(SMKX)/PTS$$n.TOS $(SMKX)/pputest_snap.o $(SMKX)/t_pputest_snap.o \
	    $(SMKX)/t_snes_ppu.o $(SMKX)/t_m7_plan.o $(SMKX)/t_ppu_dsp.o 2>/dev/null && \
	  python3 scripts/run_smkx.py --program $(SMKX)/PTS$$n.TOS --tag pts$$n --grep PPUTEST --fast --timeout 1500 >/dev/null 2>&1; \
	  echo "== $$n"; grep -a "PPUTEST snap" $(SMKX)/pts$$n-stderr.log | tail -7; done

test-ppu: $(SMKX)/PPUTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag pputest --grep PPUTEST --fast --timeout 1700
	@grep -a "PPUTEST vram" $(SMKX)/pputest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'mismatches 00000000'
	@grep -a "PPUTEST bg frames" $(SMKX)/pputest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'differing words 00000000'
	@grep -a "PPUTEST obj frames" $(SMKX)/pputest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'differing words 00000000'
	@grep -a "PPUTEST m7 frames" $(SMKX)/pputest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'differing words 00000000'
	@for k in bg obj m7; do grep -a "PPUTEST ssi $$k frames" $(SMKX)/pputest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'differing words 00000000' || exit 1; done
	@grep -a "PPUTEST ssi back on the host port" $(SMKX)/pputest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'ping 00505055'

test-m7: $(SMKX)/M7TEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag m7test --grep M7TEST --fast --timeout 800
	@grep -a "M7TEST lines" $(SMKX)/m7test-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

smkx: $(SMKX)/flags.stamp
	@$(MAKE) --no-print-directory $(SMKX)/SMKX.TOS

run-smkx: smkx
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --fast --timeout 1800

# Play in real time: the release build (no diagnostics, no frame limit) in
# the patched Hatari's window. Keys in falcon/smkx_rt.s (rt_acia).
play-smkx:
	@$(MAKE) --no-print-directory smkx SMKX_FRAMES=0 SMKX_SNAP_EVERY=0 SMKX_DIAG=0 SMKX_INPUT=
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	$(HATARI) --machine falcon --cpuclock 16 --dsp emu --memsize 14 --ttram 0 \
	  --tos $(abspath $(TOS404)) --fast-boot true --cpu-exact true \
	  --compatible true --patch-tos true --monitor rgb --sound off \
	  --natfeats true --confirm-quit off --alert-level fatal \
	  --fast-forward false --zoom 1 $(SMKX)/SMKX.TOS

preview: out/.stamp

build/viewer.o: falcon/viewer.s out/.stamp | build
	$(VASM) -quiet -Felf -m68030 -o $@ $<

build/viewer.tos: build/viewer.o
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $<

falcon: build/viewer.tos

run-hatari: falcon
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	$(HATARI) --machine falcon --cpuclock 16 --dsp emu --memsize 14 --ttram 0 \
	  --tos $(abspath $(TOS404)) --fast-boot true --cpu-exact true \
	  --compatible true --patch-tos true --monitor rgb --sound off \
	  --conout 2 --confirm-quit off --alert-level fatal build/viewer.tos

verify-hatari: falcon
	python3 scripts/verify_hatari.py

test: build/test_renderer
	build/test_renderer

# The translation checked frame by frame (from F030SMW): tools/ref65816.c
# runs the ROM on an interpreted 65816 with the runtime's hardware model
# and the DSP-1's own program; a SMKX_SUMS=1 build logs the same WRAM sum
# per frame. Frames are paced by the main loop on both sides.
build/ref65816: tools/ref65816.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/ref65816.c

REF_FRAMES ?= 3000
check-ref: build/ref65816
	build/ref65816 $(SMKX)/smk.sfc $(DSP1_BIN) $(REF_FRAMES) > $(SMKX)/ref.txt
	@$(MAKE) --no-print-directory smkx SMKX_SUMS=1 SMKX_DIAG=0 SMKX_RENDER=0 \
	  SMKX_SNAP_EVERY=0 SMKX_FRAMES=$$(($(REF_FRAMES) + 100))
	cp $(SMKX)/SMKX.TOS $(SMKX)/REFSUMS.TOS
	python3 scripts/run_smkx.py --program $(SMKX)/REFSUMS.TOS --tag ref --fast --timeout 3400
	python3 scripts/smk_refcompare.py $(SMKX)/ref.txt $(SMKX)/ref-stderr.log

# The tile world for the runtime (SMKX_LC=1): src/tile compiled as it is, linked into one relocatable object
# and its symbols renamed (tl_*) but the interface (tools/tile/interface.txt), so that its snes_ppu.c, planner
# and drivers live next to SMK's own PPU code of the same names.
CROSS_LD ?= ../../../cross-mint/bin/m68k-atari-mintelf-ld
CROSS_NM ?= ../../../cross-mint/bin/m68k-atari-mintelf-nm
CROSS_OBJCOPY ?= ../../../cross-mint/bin/m68k-atari-mintelf-objcopy
TILEC := $(SMKX)/tilec
TILE_SRC := snes_ppu m7_plan ppu_dsp_plan lc_plan smkx_ppu smkx_lc smkx_tile
TILE_OBJ := $(addprefix $(TILEC)/,$(addsuffix .o,$(TILE_SRC)))

$(TILEC)/%.o: src/tile/%.c $(wildcard src/tile/*.h) src/smkx_prof.h $(SMKX)/flags.stamp | $(SMKX)/sources.txt
	mkdir -p $(TILEC)
	$(CROSS_CC) $(CROSS_CFLAGS) -Isrc/tile -DLC_PROF -DLC_BLIT_MIN=$(SMKX_LC_BLIT_MIN) -DLC_WATCHDOG=$(SMKX_LC_WATCHDOG) -c -o $@ $<

$(SMKX)/tile.o: $(TILE_OBJ) tools/tile/interface.txt
	$(CROSS_LD) -r -o $(SMKX)/tile_raw.o $(TILE_OBJ)
	$(CROSS_NM) -g --defined-only $(SMKX)/tile_raw.o | awk '{print $$3}' | grep -vxFf tools/tile/interface.txt \
	  | awk '{print $$1" tl_"$$1}' > $(SMKX)/tile.syms
	$(CROSS_OBJCOPY) --redefine-syms=$(SMKX)/tile.syms $(SMKX)/tile_raw.o $@

# ---- The list compositor's DSP tile stack (analysis/list-compositor-smk.md) -------------------------------
# F030SMW's / F030CT's exact eight-plane DSP tile renderer, its reference renderer and the list compositor,
# taken over as a second world next to SMK's own PPU code: src/tile (reference, planner, drivers, compositor),
# tools/tile (host model and checks), falcon/dsp/ppu_tile.asm, falcon/tile (Hatari tests). SMK's snes_ppu.c,
# ppu_dsp.c and the 4-plane path stay as they are; the tile objects are linked with their symbols renamed
# (tile.syms) and meet the runtime at smkx_tile_* only. Host tools are built against src/tile first.
TL := build/tile
TLHOST := -Isrc/tile -Itools/tile $(CPPFLAGS)

$(TL)/dsp_snap: tools/tile/dsp_snap.c tools/tile/dsp_model.c tools/tile/dsp_model.h src/tile/ppu_dsp_plan.c \
  src/tile/ppu_dsp.h src/tile/m7_plan.c src/tile/snes_ppu.c src/tile/snes_ppu.h | build
	mkdir -p $(TL)
	$(CC) $(TLHOST) $(CFLAGS) $(LDFLAGS) -o $@ tools/tile/dsp_snap.c tools/tile/dsp_model.c \
	  src/tile/ppu_dsp_plan.c src/tile/snes_ppu.c src/tile/m7_plan.c $(LDLIBS)

$(TL)/dsp_rand: tools/tile/dsp_rand.c tools/tile/dsp_model.c tools/tile/dsp_model.h src/tile/ppu_dsp_plan.c \
  src/tile/ppu_dsp.h src/tile/m7_plan.c src/tile/snes_ppu.c src/tile/snes_ppu.h | build
	mkdir -p $(TL)
	$(CC) $(TLHOST) $(CFLAGS) $(LDFLAGS) -o $@ tools/tile/dsp_rand.c tools/tile/dsp_model.c \
	  src/tile/ppu_dsp_plan.c src/tile/snes_ppu.c src/tile/m7_plan.c $(LDLIBS)

# The runtime's eight-plane snapshots (version 4: the list compositor's ring window rebuilt as a plain screen) against
# the reference renderer: Falcon planes and host planes byte for byte.
$(TL)/ppu_snap: tools/tile/ppu_snap.c src/tile/snes_ppu.c src/tile/snes_ppu.h | build
	mkdir -p $(TL)
	$(CC) $(TLHOST) $(CFLAGS) $(LDFLAGS) -o $@ tools/tile/ppu_snap.c src/tile/snes_ppu.c $(LDLIBS)

$(TL)/lcd_model: tools/tile/lcd_model.c tools/tile/lc_model.c src/tile/lc_plan.c src/tile/lc_plan.h \
  src/tile/snes_ppu.c src/tile/snes_ppu.h | build
	mkdir -p $(TL)
	$(CC) $(TLHOST) $(CFLAGS) -Wno-unused $(LDFLAGS) -o $@ tools/tile/lcd_model.c src/tile/lc_plan.c \
	  src/tile/snes_ppu.c $(LDLIBS)

# Snapshots of SMK's tile-mode screens from the reference interpreter (scripts/tile_snaps.py: title and
# intro, mode/driver/cup/course select, then the race start), converted to the shared layout
# (tools/tile/snap_to_ct.py: SMK's version 3 -> version 4): TL/snaps every 4th frame up to the race
# (the DSP model and PPUTILE check), TL/winA, winB and winC windows of consecutive covered frames
# (the title before its windows come in, the title with its windows, the menus), which lcd_model checks.
TILE_RAW := build/tilesnap/seq
$(TL)/.snaps: tools/tile/snap_to_ct.py scripts/tile_snaps.py build/ref65816 build/race_census $(SMKX)/smk.sfc
	mkdir -p $(TL)
	python3 scripts/tile_snaps.py run $(TILE_RAW) 1330 1
	python3 scripts/tile_snaps.py window $(TILE_RAW) 1 1306 $(TL)/snaps 4
	python3 scripts/tile_snaps.py window $(TILE_RAW) 43 300 $(TL)/winA
	python3 scripts/tile_snaps.py window $(TILE_RAW) 743 1306 $(TL)/winB
	python3 scripts/tile_snaps.py window $(TILE_RAW) 301 741 $(TL)/winC
	touch $@

tile-snaps: $(TL)/.snaps

# The DSP design model (tools/tile/dsp_model.c, the planner src/tile/ppu_dsp_plan.c) against the reference
# on the tile-mode snapshots, with the identity colour map and eight planes.
check-dsp-model: $(TL)/dsp_snap $(TL)/.snaps $(SMKX)/smk.sfc
	-$(TL)/dsp_snap $(SMKX)/smk.sfc $(TL)/snaps/F*.BIN > $(TL)/dsp_snap.txt
	@echo "$$(grep -c 'diff 0 words in 0 rows' $(TL)/dsp_snap.txt) of $$(ls $(TL)/snaps/F*.BIN | wc -l | tr -d ' ') snapshots: model equals the reference"
	@echo "$$(grep -c 'does not fit' $(TL)/dsp_snap.txt) not taken by the DSP planner (none of them may be a tile frame)"
	@test "$$(grep -c 'diff [1-9]\|READ OUTSIDE' $(TL)/dsp_snap.txt)" = 0

# The planner of the list compositor (src/tile/lc_plan.c) against the reference: which groups of the scroll
# ring each frame writes, two pages, the shown picture equal to the reference on every frame.
check-lcd-model: $(TL)/lcd_model $(TL)/.snaps $(SMKX)/smk.sfc
	$(TL)/lcd_model $(SMKX)/smk.sfc $(TL)/winA/F*.BIN | tail -1 | tee $(TL)/lcd_winA.txt
	$(TL)/lcd_model $(SMKX)/smk.sfc $(TL)/winB/F*.BIN | tail -1 | tee $(TL)/lcd_winB.txt
	$(TL)/lcd_model $(SMKX)/smk.sfc $(TL)/winC/F*.BIN | tail -1 | tee $(TL)/lcd_winC.txt
	@for w in A B C; do grep -q ' 0 frames wrong, 0 wrong pixels' $(TL)/lcd_win$$w.txt || exit 1; done

# The DSP program in its builds (same source): tile frames, Mode 7 frames, tile frames with windows and the
# sub screen, and the list compositor's group composer (LCB). Booted by falcon/smkx_dspboot.s (no XBIOS).
TLX := $(SMKX)/tile
$(TLX)/ppu_tile.bin: falcon/dsp/ppu_tile.asm tools/smkx/dsp_asm.py
	mkdir -p $(TLX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=0 --define LCB=0 $< $@
$(TLX)/ppu_tile7.bin: falcon/dsp/ppu_tile.asm tools/smkx/dsp_asm.py
	mkdir -p $(TLX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=1 --define WIN=0 --define LCB=0 $< $@
$(TLX)/ppu_tilew.bin: falcon/dsp/ppu_tile.asm tools/smkx/dsp_asm.py
	mkdir -p $(TLX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=0 $< $@
$(TLX)/ppu_tilelc.bin: falcon/dsp/ppu_tile.asm tools/smkx/dsp_asm.py
	mkdir -p $(TLX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=0 --define LCB=1 $< $@

# ... and with the window build in it (frames whose rows have windows masking layers)
$(TLX)/ppu_tilelcw.bin: falcon/dsp/ppu_tile.asm tools/smkx/dsp_asm.py
	mkdir -p $(TLX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=1 $< $@

# PPUTILE: real tile-mode frames of SMK, then random ones (flips, 16x16 tiles, mode 0, windows, the sub
# screen), each composed by the DSP in Hatari and compared with the reference (test-ppu-tile; SMK's own
# DSP renderer has test-ppu).
PPUTILE_RANDOM ?= 32
PPUTILE_FRAMES ?= $(addprefix $(TL)/snaps/,F0000014.BIN F000003C.BIN F00000C8.BIN F000012C.BIN F0000190.BIN \
  F00001E0.BIN F0000268.BIN F00002E8.BIN F00003E8.BIN F00004B0.BIN F0000514.BIN)

$(TLX)/tile_tests.bin: $(TL)/dsp_snap $(TL)/dsp_rand $(TL)/.snaps
	mkdir -p $(TLX)
	$(TL)/dsp_snap $(SMKX)/smk.sfc $(PPUTILE_FRAMES) -pack $(TLX)/tile_real.bin
	$(TL)/dsp_rand $(PPUTILE_RANDOM) 0x5eed $(TLX)/tile_rand.bin > $(TLX)/tile_rand.txt
	python3 -c "import sys,struct; a,b=(open(f,'rb').read() for f in sys.argv[1:3]); \
	  open(sys.argv[3],'wb').write(b'PPU2'+struct.pack('>I',struct.unpack('>I',a[4:8])[0]+struct.unpack('>I',b[4:8])[0])+a[8:]+b[8:])" \
	  $(TLX)/tile_real.bin $(TLX)/tile_rand.bin $@

TILE_CFLAGS := $(CROSS_CFLAGS) -Isrc/tile
$(TLX)/t_%.o: src/tile/%.c src/tile/ppu_dsp.h src/tile/snes_ppu.h src/smkx_prof.h | $(SMKX)/sources.txt
	mkdir -p $(TLX)
	$(CROSS_CC) $(TILE_CFLAGS) -c -o $@ $<

$(TLX)/PPUTILE.TOS: falcon/tile/pputest.s falcon/smkx_dspboot.s $(TLX)/ppu_tile.bin $(TLX)/ppu_tile7.bin \
  $(TLX)/ppu_tilew.bin $(TLX)/tile_tests.bin $(TLX)/t_pputest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(TLX) -Ifalcon -o $(TLX)/pputile.o falcon/tile/pputest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(TLX)/pputile.o $(TLX)/t_pputest.o

test-ppu-tile: $(TLX)/PPUTILE.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag pputile --grep PPUTEST --fast --timeout 1700
	@grep -a "PPUTEST frames" $(SMKX)/pputile-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

# LCDTEST: the LC build on real SMK frames with masks of several patterns; each frame's list against the model.
$(TLX)/lcd_tests.bin: $(TL)/dsp_snap $(TL)/.snaps
	mkdir -p $(TLX)
	-$(TL)/dsp_snap $(SMKX)/smk.sfc $(PPUTILE_FRAMES) -packlc $@ > /dev/null

$(TLX)/LCDTEST.TOS: falcon/tile/lcdtest.s falcon/smkx_dspboot.s $(TLX)/ppu_tilelc.bin $(TLX)/ppu_tilelcw.bin $(TLX)/lcd_tests.bin $(TLX)/t_lcdtest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(TLX) -Ifalcon -o $(TLX)/lcdtest.o falcon/tile/lcdtest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(TLX)/lcdtest.o $(TLX)/t_lcdtest.o

test-lcd: $(TLX)/LCDTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag lcdtest --grep LCDTEST --fast --timeout 800
	@grep -a "LCDTEST frames" $(SMKX)/lcdtest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

# ---- The blitter renderer: F030CT's list-compositor model and Hatari player on SMK's tile-mode frames -----
# (analysis/list-compositor-smk.md): tools/tile/lc_model.c against src/tile/snes_ppu.c on the consecutive
# tile-mode snapshots of TL/lcsnaps (frames 1 to 1,306 of a 1P GP run: title, intro, menus), then the
# longest run of consecutive frames it covers recorded as a stream, which falcon/tile/lcplay.s plays in
# Hatari on the 030 and the blitter (every frame's page window summed against the stream's checksum).
# Host model and emulator only; the model skips Mode 7 frames, and the sprite/overlay producer exists
# as this model only, not in the game's runtime.
LCM := $(TL)/lc
$(LCM)/lc_model: tools/tile/lc_model.c src/tile/snes_ppu.c src/tile/snes_ppu.h | build
	mkdir -p $(LCM)
	$(CC) $(TLHOST) $(CFLAGS) $(LDFLAGS) -o $@ tools/tile/lc_model.c src/tile/snes_ppu.c -lm

$(TL)/.lcsnaps: $(TL)/.snaps scripts/tile_snaps.py
	python3 scripts/tile_snaps.py window $(TILE_RAW) 1 1306 $(TL)/lcsnaps
	touch $@

# The longest window is recorded whole (LC_PLAY_FRAMES: cut it shorter for a quick run; the 300 frames of the
# course-select menu are an 11 MB stream and fit the emulated 14 MB).
LC_PLAY_FRAMES ?= 100000
LC_ENV := LC_SMW=1 LC_CHOOSER=exact LC_OVERLAY=ring LC_CACHE_KB=64

check-lc-model: $(LCM)/lc_model $(SMKX)/smk.sfc $(TL)/.lcsnaps
	LC_SMW=1 $(LCM)/lc_model census $(SMKX)/smk.sfc $(TL)/lcsnaps/F*.BIN | tee $(LCM)/census.txt | tail -1
	LC_SMW=1 $(LCM)/lc_model seq $(SMKX)/smk.sfc $(TL)/lcsnaps/F*.BIN | tail -2
	LC_SMW=1 $(LCM)/lc_model blitfuzz 50000 1 | tail -1
	python3 tools/tile/lc_longest.py $(TL)/lcsnaps < $(LCM)/census.txt > $(LCM)/window.txt
	@test -s $(LCM)/window.txt || { echo "no frame the model covers"; exit 1; }
	@echo "longest covered run: $$(wc -l < $(LCM)/window.txt | tr -d ' ') frames"
	$(LC_ENV) $(LCM)/lc_model hreal $(SMKX)/smk.sfc $$(cat $(LCM)/window.txt) | tail -5
	LC_REC=$(LCM)/smk.lcp $(LC_ENV) $(LCM)/lc_model hreal $(SMKX)/smk.sfc $$(head -n $(LC_PLAY_FRAMES) $(LCM)/window.txt) | tail -6
	LC_SMW=1 $(LCM)/lc_model play $(LCM)/smk.lcp

check-lc-play: check-lc-model
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_lcplay.py check $(LCM)/smk.lcp

bench-lc-play: check-lc-model
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_lcplay.py bench $(LCM)/smk.lcp

clean:
	rm -rf build out
