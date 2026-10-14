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
GAME_SOURCE := third_party/Donkey-Kong-Country-1-Disassembly/DKC1
# The ROM as the user supplied it: .sfc, .smc or a ZIP holding one.
ROM ?= $(subst ?,$(space),$(firstword $(subst $(space),?,$(wildcard assets/Donkey\ Kong\ Country\ (USA).sfc assets/Donkey\ Kong\ Country\ (USA).smc assets/Donkey\ Kong\ Country\ (USA).zip))))
empty :=
space := $(empty) $(empty)
escape_space = $(subst $(space),\$(space),$(1))
ROM_PREREQUISITE := $(call escape_space,$(ROM))

.PHONY: all dkcx-source dkcx run-dkcx test-m7 test-c2p check-snap check-snaps check-dsp-model test-ppu \
  test-palette clean FORCE
all: dkcx-source

build:
	mkdir -p build

# 65816 -> 68030 translation of the ROM's program (tools/smkx, from the
# F030SMK port). Everything under build/dkcx is derived from the ROM and
# stays untracked.
DKCX := build/dkcx
LC := build/lc
DKCX_FRAMES ?= 0
# 0: no rendering at all (timing and reference runs). Otherwise the render context asks for a composition at every
# VBL and works in the game's idle time, which is the throttle (there is no render interval to set).
DKCX_RENDER ?= 1
DKCX_SNAP_EVERY ?= 0
# ... only from frame n on (a window of consecutive real frames)
DKCX_SNAP_FROM ?= 0
# Dnnnnnnn.BIN before game frame n, then every k frames, c in all, then
# quit (tools/smkx/refcmp.py).
DKCX_DUMP_AT ?= 0
DKCX_DUMP_EVERY ?= 1
DKCX_DUMP_COUNT ?= 1
# Or one dump at the n-th execution of a probed address (decimal; make eats $).
DKCX_DUMP_PC ?= 0
DKCX_DUMP_HIT ?= 1
DKCX_M7_DSP ?= 1
# Blocks of this many groups or more go to the blitter (hog mode); 100000: CPU only
DKCX_LC_BLIT_MIN ?= 24
# 0: the list compositor is off (the DSP renderer draws every frame)
DKCX_LC ?= 1
# 0: release build without per-call diagnostics (call sites, samples)
DKCX_DIAG ?= 1
DKCX_PROFILE_FROM ?= 0
# Scripted joypad input, FRAME:PAD pairs (tools/smkx/input_script.py).
DKCX_INPUT ?=
DKCX_PY := $(wildcard tools/smkx/*.py) tools/smkx/dispatch_targets.txt tools/smkx/probes.txt \
  tools/smkx/marks.txt

$(DKCX)/sources.txt: $(DKCX_PY) $(wildcard $(GAME_SOURCE)/*.asm) $(ROM_PREREQUISITE)
	python3 -m tools.smkx "$(ROM)" $(DKCX)

dkcx-source: $(DKCX)/sources.txt

# The video adapter's C code, cross-compiled freestanding for the 68030.
CROSS_CFLAGS := -DLC_PROF -DLC_ON=$(DKCX_LC) -DLC_BLIT_MIN=$(DKCX_LC_BLIT_MIN) -m68030 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
  -fomit-frame-pointer -nostdlib -Wall -Wextra -Isrc
# 256 colours in eight planes: the DSP renderer (from F030CT: ppu.asm,
# smkx_ppu.c, ppu_dsp_plan.c) composes, ppu_fast.c (from F030SMW) where the
# DSP cannot; the reference renderer (snes_ppu.c) is the checker.
DKCX_C_OBJ := $(DKCX)/snes_ppu.o $(DKCX)/ppu_fast.o $(DKCX)/smkx_video.o $(DKCX)/smkx_m7.o \
  $(DKCX)/m7_plan.o $(DKCX)/smkx_ppu.o $(DKCX)/ppu_dsp_plan.o $(DKCX)/lc_plan.o $(DKCX)/smkx_lc.o

$(DKCX)/smkx_video.o $(DKCX)/smkx_lc.o: $(DKCX)/flags.stamp

$(DKCX)/%.o: src/%.c src/snes_ppu.h src/ppu_internal.h src/ppu_fast.h src/smkx_m7.h src/m7_plan.h \
  src/smkx_ppu.h src/ppu_dsp.h src/smkx_prof.h src/lc_plan.h | $(DKCX)/sources.txt
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

# Build switches are inputs too: the stamp changes whenever they do.
DKCX_FLAGS := FRAMES=$(DKCX_FRAMES) RENDER=$(DKCX_RENDER) \
  SNAP_EVERY=$(DKCX_SNAP_EVERY) SNAP_FROM=$(DKCX_SNAP_FROM) M7_DSP=$(DKCX_M7_DSP) \
  LC_BLIT_MIN=$(DKCX_LC_BLIT_MIN) LC=$(DKCX_LC) PROFILE_FROM=$(DKCX_PROFILE_FROM) DIAG=$(DKCX_DIAG) DUMP_AT=$(DKCX_DUMP_AT) \
  DUMP_EVERY=$(DKCX_DUMP_EVERY) DUMP_COUNT=$(DKCX_DUMP_COUNT) DUMP_PC=$(DKCX_DUMP_PC) \
  DUMP_HIT=$(DKCX_DUMP_HIT)
$(DKCX)/flags.stamp: FORCE | $(DKCX)/sources.txt
	@test "$$(cat $@ 2>/dev/null)" = "$(DKCX_FLAGS)" || echo "$(DKCX_FLAGS)" > $@

FORCE:

$(DKCX)/m7c2p.bin: falcon/dsp/m7c2p.asm tools/smkx/dsp_asm.py
	mkdir -p $(DKCX)
	python3 -m tools.smkx.dsp_asm $< $@

# The Mode 7 kernel in boot format (rt_dsp_boot, no XBIOS Dsp_ExecBoot); m7c2p.bin stays the raw image
# M7TEST hands to Dsp_ExecBoot.
$(DKCX)/m7c2p_boot.bin: falcon/dsp/m7c2p.asm tools/smkx/dsp_asm.py
	mkdir -p $(DKCX)
	python3 -m tools.smkx.dsp_asm --boot $< $@

# The DSP program in its builds (same source): tile frames, Mode 7 frames, tile frames with windows and the
# sub screen, and the list compositor's group composer. Booted by falcon/smkx_dspboot.s.
$(DKCX)/ppu.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(DKCX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=0 --define LCB=0 $< $@

$(DKCX)/ppu7.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(DKCX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=1 --define WIN=0 --define LCB=0 $< $@

$(DKCX)/ppuw.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(DKCX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=0 $< $@

$(DKCX)/ppulc.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(DKCX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=0 --define LCB=1 $< $@

# ... and with the window build in it (frames whose rows have windows masking layers)
$(DKCX)/ppulcw.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(DKCX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=1 $< $@

$(DKCX)/input.s: tools/smkx/input_script.py FORCE | $(DKCX)/sources.txt
	python3 -m tools.smkx.input_script "$(DKCX_INPUT)" $@

$(DKCX)/oklab555.bin: tools/smkx/oklab.py
	mkdir -p $(DKCX)
	python3 -m tools.smkx.oklab $@

$(DKCX)/DKCX.TOS: $(DKCX)/sources.txt falcon/smkx_rt.s falcon/smkx_rt.i \
  falcon/smkx_video.s falcon/smkx_lcput.s falcon/smkx_m7dsp.s falcon/smkx_dspboot.s $(DKCX_C_OBJ) $(DKCX)/flags.stamp \
  $(DKCX)/m7c2p.bin $(DKCX)/m7c2p_boot.bin $(DKCX)/ppu.bin $(DKCX)/ppu7.bin $(DKCX)/ppuw.bin $(DKCX)/ppulc.bin $(DKCX)/ppulcw.bin $(DKCX)/input.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -Ifalcon -I$(DKCX) -DSMKX_FRAMES=$(DKCX_FRAMES) \
	  -DSMKX_RENDER=$(DKCX_RENDER) -DSMKX_SNAP_EVERY=$(DKCX_SNAP_EVERY) -DSMKX_SNAP_FROM=$(DKCX_SNAP_FROM) \
	  -DSMKX_M7_DSP=$(DKCX_M7_DSP) -DSMKX_PROFILE_FROM=$(DKCX_PROFILE_FROM) -DSMKX_DIAG=$(DKCX_DIAG) -DSMKX_DUMP_AT=$(DKCX_DUMP_AT) \
	  -DSMKX_DUMP_EVERY=$(DKCX_DUMP_EVERY) -DSMKX_DUMP_COUNT=$(DKCX_DUMP_COUNT) \
	  -DSMKX_DUMP_PC=$(DKCX_DUMP_PC) -DSMKX_DUMP_HIT=$(DKCX_DUMP_HIT) \
	  -o $(DKCX)/rt.o falcon/smkx_rt.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -o $(DKCX)/tables.o $(DKCX)/smkx_tables.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -o $(DKCX)/input.o $(DKCX)/input.s
	for f in $$(cat $(DKCX)/sources.txt); do \
	  $(VASM) -Felf -m68030 -quiet -nowarn=62 -Ifalcon -o $(DKCX)/$${f%.s}.o $(DKCX)/$$f || exit 1; \
	done
	$(VLINK) -b ataritos -tos-flags 1 -M$(DKCX)/DKCX.map -o $@ $(DKCX)/rt.o \
	  $$(sed 's/\.s$$/.o/;s|^|$(DKCX)/|' $(DKCX)/sources.txt) $(DKCX)/tables.o $(DKCX)/input.o $(DKCX_C_OBJ)

# make 3.81 compares whole seconds: two builds with different switches in
# one second would keep the first link. The stamp is updated first, then a
# sub-make looks at the files afresh (from F030SMW).
dkcx: $(DKCX)/flags.stamp
	@$(MAKE) --no-print-directory $(DKCX)/DKCX.TOS

run-dkcx: dkcx
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $(DKCX)/DKCX.TOS --fast --timeout 1800

# The DSP Mode 7 kernel against the C reference: random VRAM and lines.
$(DKCX)/t_%.o: src/%.c src/snes_ppu.h src/m7_plan.h
	mkdir -p $(DKCX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

M7TEST_OBJ := $(DKCX)/t_m7test.o $(DKCX)/t_m7_plan.o $(DKCX)/t_snes_ppu.o

$(DKCX)/M7TEST.TOS: falcon/m7test.s $(DKCX)/m7c2p.bin $(M7TEST_OBJ)
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(DKCX) -o $(DKCX)/m7test.o falcon/m7test.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(DKCX)/m7test.o $(M7TEST_OBJ)

test-m7: $(DKCX)/M7TEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag m7test --grep M7TEST --fast --timeout 800
	@grep -a "M7TEST lines" $(DKCX)/m7test-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

# The DSP renderer against the host model on real frames (build/dsp_snap
# packs them from level snapshots copied to $(DKCX)/pputest/), then random
# ones (flips, 16x16 tiles, mode 0, ...; build/dsp_rand). From F030CT.
PPUTEST_FRAMES ?= $(sort $(wildcard $(LC)/pt/*.BIN))
PPUTEST_RANDOM ?= 16
DSP_HOST_SRC := tools/dsp_model.c src/ppu_dsp_plan.c src/snes_ppu.c src/m7_plan.c
DSP_HOST_DEP := $(DSP_HOST_SRC) tools/dsp_model.h src/ppu_dsp.h src/snes_ppu.h src/ppu_internal.h \
  src/m7_plan.h

build/dsp_snap: tools/dsp_snap.c $(DSP_HOST_DEP) | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/dsp_snap.c $(DSP_HOST_SRC)

build/dsp_rand: tools/dsp_rand.c $(DSP_HOST_DEP) | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/dsp_rand.c $(DSP_HOST_SRC)

# The DSP renderer's design against the reference, on the snapshots.
check-dsp-model: build/dsp_snap
	build/dsp_snap $(DKCX)/dkc.sfc $(DKCX)/F*.BIN

$(DKCX)/ppu_tests.bin: build/dsp_snap build/dsp_rand $(sort $(wildcard $(DKCX)/pputest/*.BIN))
	mkdir -p $(LC)/pt
	python3 tools/lc_check/dkc_to_ct_snap.py $(LC)/pt $(sort $(wildcard $(DKCX)/pputest/*.BIN))
		build/dsp_snap $(DKCX)/dkc.sfc $(sort $(wildcard $(LC)/pt/*.BIN)) -pack $(DKCX)/ppu_tests_real.bin
	build/dsp_rand $(PPUTEST_RANDOM) 0x5eed $(DKCX)/ppu_tests_rand.bin > $(DKCX)/ppu_tests_rand.txt
	python3 -c "import sys,struct; a,b=(open(f,'rb').read() for f in sys.argv[1:3]); \
	  open(sys.argv[3],'wb').write(b'PPU2'+struct.pack('>I',struct.unpack('>I',a[4:8])[0]+struct.unpack('>I',b[4:8])[0])+a[8:]+b[8:])" \
	  $(DKCX)/ppu_tests_real.bin $(DKCX)/ppu_tests_rand.bin $@

$(DKCX)/t_pputest.o: src/pputest.c src/ppu_dsp.h src/snes_ppu.h src/smkx_prof.h
	mkdir -p $(DKCX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(DKCX)/PPUTEST.TOS: falcon/pputest.s falcon/smkx_dspboot.s $(DKCX)/ppu.bin $(DKCX)/ppu7.bin $(DKCX)/ppuw.bin $(DKCX)/ppu_tests.bin \
  $(DKCX)/t_pputest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(DKCX) -Ifalcon -o $(DKCX)/pputest.o falcon/pputest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(DKCX)/pputest.o $(DKCX)/t_pputest.o

test-ppu: $(DKCX)/PPUTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag pputest --grep PPUTEST --fast --timeout 1500
	@grep -a "PPUTEST frames" $(DKCX)/pputest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

build/palette_test: tools/palette_test.c src/palette.c src/palette.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/palette_test.c src/palette.c

test-palette: build/palette_test $(DKCX)/oklab555.bin
	build/palette_test $(DKCX)/oklab555.bin

build/c2p_test: tools/c2p_test.c src/ppu_fast.c src/snes_ppu.c src/ppu_fast.h src/ppu_internal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/c2p_test.c src/ppu_fast.c src/snes_ppu.c

# The display path's chunky-to-planar packing against the bit-by-bit one.
test-c2p: build/c2p_test
	build/c2p_test

# SNAP.BIN from the runtime: the fast composer against the reference, the
# Falcon's planes against the host.
build/ppu_snap: tools/ppu_snap.c src/snes_ppu.c src/ppu_fast.c src/snes_ppu.h src/ppu_internal.h \
  src/ppu_fast.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/ppu_snap.c src/snes_ppu.c src/ppu_fast.c

check-snap: build/ppu_snap
	build/ppu_snap $(DKCX)/SNAP.BIN $(DKCX)/dkc.sfc

check-snaps: build/ppu_snap
	@fail=0; for f in $(DKCX)/F*.BIN; do \
	  echo "== $$f"; \
	  build/ppu_snap $$f $(DKCX)/dkc.sfc $${f%.BIN}.ppm || fail=1; \
	  magick $${f%.BIN}.ppm $${f%.BIN}.png && rm -f $${f%.BIN}.ppm; \
	done; exit $$fail

clean:
	rm -rf build

# ---- The list compositor's planner on the host (analysis/list-compositor-dkc.md) -------------------------
# tools/lc/lcd_model.c: which groups of the scroll ring each frame writes, from inputs only; exact against the
# reference. Needs snapshots in CT's layout (lc-snaps) of consecutive frames.
LC_SNAPS ?= $(DKCX)

lc-snaps:
	@ls $(LC_SNAPS)/F*.BIN >/dev/null 2>&1 || { echo "no snapshots in $(LC_SNAPS)"; exit 1; }
	mkdir -p $(LC)/snaps
	python3 tools/lc_check/dkc_to_ct_snap.py $(LC)/snaps $(LC_SNAPS)/F*.BIN

$(LC)/lcd_model: tools/lc/lcd_model.c tools/lc/lc_model.c src/lc_plan.c src/lc_plan.h src/snes_ppu.c src/snes_ppu.h | build
	mkdir -p $(LC)
	$(CC) -Isrc $(CPPFLAGS) $(CFLAGS) -Wno-unused $(LDFLAGS) -o $@ tools/lc/lcd_model.c src/lc_plan.c src/snes_ppu.c -lm

# LCDTEST: the LC build of the DSP program on real frames with masks of several patterns; each frame's list against
# the model (build/dsp_snap -packlc).
$(DKCX)/lcd_tests.bin: build/dsp_snap $(DKCX)/dkc.sfc $(sort $(wildcard $(DKCX)/pputest/*.BIN))
	mkdir -p $(LC)/pt
	python3 tools/lc_check/dkc_to_ct_snap.py $(LC)/pt $(sort $(wildcard $(DKCX)/pputest/*.BIN))
	-build/dsp_snap $(DKCX)/dkc.sfc $(sort $(wildcard $(LC)/pt/*.BIN)) -packlc $@ > /dev/null

$(DKCX)/t_lcdtest.o: src/lcdtest.c src/ppu_dsp.h src/smkx_prof.h
	mkdir -p $(DKCX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(DKCX)/LCDTEST.TOS: falcon/lcdtest.s falcon/smkx_dspboot.s $(DKCX)/ppulc.bin $(DKCX)/ppulcw.bin $(DKCX)/lcd_tests.bin $(DKCX)/t_lcdtest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(DKCX) -Ifalcon -o $(DKCX)/lcdtest.o falcon/lcdtest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(DKCX)/lcdtest.o $(DKCX)/t_lcdtest.o

test-lcd: $(DKCX)/LCDTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag lcdtest --grep LCDTEST --fast --timeout 800
	@grep -a "LCDTEST frames" $(DKCX)/lcdtest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'
