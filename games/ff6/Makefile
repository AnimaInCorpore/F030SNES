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
GAME_SOURCE := third_party/ff6
# The ROM as the user supplied it: .sfc, .smc or a ZIP holding one.
ROM ?= $(subst ?,$(space),$(firstword $(subst $(space),?,$(wildcard assets/Final\ Fantasy\ VI\ (Japan).sfc assets/Final\ Fantasy\ VI\ (Japan).smc assets/Final\ Fantasy\ VI\ (Japan).zip))))
empty :=
space := $(empty) $(empty)
escape_space = $(subst $(space),\$(space),$(1))
ROM_PREREQUISITE := $(call escape_space,$(ROM))

.PHONY: all ff6x-source ff6x run-ff6x test-emit check-tc test-tc test-m7 check-snap check-snaps check-dsp-model check-lc-model check-lc-real check-lc-play bench-lc-play lc-snaps check-lcd-model test-ppu test-lcd test-palette test-spc clean FORCE
all: ff6x-source

build:
	mkdir -p build

# 65816 -> 68030 translation of the ROM's program (tools/smkx, from the
# F030SMK port, by way of F030CT). Everything under build/ff6x is derived from the ROM and
# stays untracked.
FF6X := build/ff6x
FF6X_FRAMES ?= 0
# 0: no rendering at all (timing and reference runs). Otherwise the render context asks for a composition at every VBL
# and works in the game's idle time, paced by smkx_render_poll (src/smkx_video.c): there is no render interval to set.
FF6X_RENDER ?= 1
FF6X_SNAP_EVERY ?= 0
FF6X_SNAP_FROM ?= 0               # with FF6X_SNAP_EVERY: from this frame on (a window of consecutive snapshots)
FF6X_PARTIAL ?= 2                  # partial rendering: 0 off, 1 changed rows only, 2 also skips unchanged pictures
FF6X_PARTIAL_CHECK ?= 0            # 1: every DSP picture is compared with the CPU's (slow)
FF6X_M7_DSP ?= 1
FF6X_PROFILE ?= 0
ifeq ($(FF6X_PROFILE),1)
FF6X_PROF_DEF := -DSMKX_PROFILE -DPPU_PROFILE
endif
FF6X_PROFILE_FROM ?= 0
# 0: release build, without the call-site notes of the generic memory helpers
# (the status line's "from" and the profile's attribution need them).
FF6X_DIAG ?= 1
# 1: the SPC700 gets 1/60 s per frame (music tempo); 0: only when polled
# (default while sound is deferred: the driver costs ~370 ms per frame).
FF6X_APU_FRAME ?= 0
# Chrono Trigger's Gfx_DecompressEntry natively (falcon/ct_lz.s, F030CT's;
# off: FF6 has no such routine, see ROADMAP.md). FF6X_LZ_CHECK=1 also runs
# the translation after it and compares WRAM at every call.
FF6X_LZ ?= 0
FF6X_LZ_CHECK ?= 0
# 1: the screen in true colour (RGB565), drawn by the true-colour DSP
# renderer (falcon/dsp/tc.asm, src/smkx_tc.c), the CPU's ppu_render_565 for
# frames it cannot take; 0: the planar renderer and the palette paths.
FF6X_TC ?= 0
# 1: the list compositor (src/lc_plan.c, src/smkx_lc.c, analysis/list-compositor-ffvi.md) draws the eight-plane tile
# frames it covers; the others go the old way. Has no effect with FF6X_TC=1. UNVERIFIED in the game: see the analysis.
FF6X_LC ?= 0
FF6X_LC_BLIT_MIN ?= 24            # blocks of this many groups or more go to the blitter (hog mode)
FF6X_LC_SYNC ?= 0                 # 1: one compositor job at a time (no planning ahead; for checking the pipeline)
FF6X_LC_DIAG ?= 0                 # 1: the compositor's job log (SMKX LCLOG lines at the end), list-sentinel and copy checks (lc statistics 16-18)
FF6X_LC_CHECK ?= 0                # 1: a debug build: every compositor picture is compared with the CPU reference first (implies FF6X_LC_SYNC=1; slow)
FF6X_LC_ALLPAGES ?= 0             # 1: the planner compares every VRAM page instead of trusting the written-pages flags (a check)
FF6X_LC_MATH ?= 1                 # 1: frames with colour math are not drawn by the compositor (it leaves colour math out); 0: it draws them without
LC_ACTIVE := $(if $(filter 1,$(FF6X_TC)),0,$(FF6X_LC))
FF6X_HOLD ?= 0                    # 1: at the frame limit write SNAP.BIN and keep the picture up (scripts/lc_shot.py)
# 1: no SPC700 once the game's driver would start; port writes are echoed
# (src/smkx_apu.c). FF6's driver costs the 68030 minutes for the title theme.
FF6X_APU_ECHO ?= 0
# Scripted joypad input, FRAME:PAD pairs (tools/smkx/input_script.py).
FF6X_INPUT ?=
FF6X_PY := $(wildcard tools/smkx/*.py) tools/smkx/dispatch_targets.txt tools/smkx/natives.txt tools/smkx/probes.txt tools/smkx/interrupt_targets.txt \
  tools/smkx/marks.txt tools/smkx/tables.txt

$(FF6X)/sources.txt: $(FF6X_PY) $(wildcard $(GAME_SOURCE)/src/*/*.asm $(GAME_SOURCE)/include/*.inc) $(ROM_PREREQUISITE)
	python3 -m tools.smkx "$(ROM)" $(FF6X)

ff6x-source: $(FF6X)/sources.txt

# The video adapter's C code, cross-compiled freestanding for the 68030.
CROSS_CFLAGS := -m68030 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
  -fomit-frame-pointer -nostdlib -Wall -Wextra -Isrc
ifeq ($(FF6X_TC),1)
FF6X_DSP_OBJ := $(FF6X)/smkx_tc.o $(FF6X)/ppu_rows.o
FF6X_DSP_BIN := $(FF6X)/tc.bin $(FF6X)/tc7.bin
else
FF6X_DSP_OBJ := $(FF6X)/smkx_ppu.o
FF6X_DSP_BIN :=
endif
ifeq ($(LC_ACTIVE),1)
FF6X_LC_OBJ := $(FF6X)/lc_plan.o $(FF6X)/smkx_lc.o
FF6X_LC_BIN := $(FF6X)/ppulc.bin $(FF6X)/ppulcw.bin
endif
FF6X_C_OBJ := $(FF6X_LC_OBJ) $(FF6X)/snes_ppu.o $(FF6X)/smkx_video.o $(FF6X)/palette.o $(FF6X)/smkx_m7.o \
  $(FF6X)/m7_plan.o $(FF6X)/spc700.o $(FF6X)/smkx_apu.o $(FF6X_DSP_OBJ) $(FF6X)/ppu_dsp_plan.o

# C that translated code calls (the APU glue) runs with the VBL enabled; the
# VBL runs the translated NMI on the interrupted registers, so gcc must not
# borrow a5 (WRAM) or a6 (the 65816 stack) there (F030SMK's finding).
$(FF6X_LC_OBJ): FF6X_FIXED := -DLC_PROF -DLC_BLIT_MIN=$(FF6X_LC_BLIT_MIN) -DLC_REFUSE_MATH=$(FF6X_LC_MATH) -DSMKX_LC_SYNC=$(if $(filter 1,$(FF6X_LC_CHECK)),1,$(FF6X_LC_SYNC)) -DSMKX_LC_CHECK=$(FF6X_LC_CHECK) -DSMKX_LC_ALLPAGES=$(FF6X_LC_ALLPAGES) -DSMKX_LC_DIAG=$(FF6X_LC_DIAG) -DSMKX_SNAP_FROM=$(FF6X_SNAP_FROM)
$(FF6X)/spc700.o $(FF6X)/smkx_apu.o: FF6X_FIXED := -ffixed-a5 -ffixed-a6 -DSMKX_APU_FRAME=$(FF6X_APU_FRAME) -DSMKX_APU_ECHO=$(FF6X_APU_ECHO)

$(FF6X)/%.o: src/%.c src/snes_ppu.h src/palette.h src/smkx_m7.h src/m7_plan.h src/smkx_prof.h src/spc700.h \
  src/smkx_ppu.h src/ppu_dsp.h src/ppu_tc.h src/ppu_rows.h src/lc_plan.h $(FF6X)/flags.stamp | $(FF6X)/sources.txt
	$(CROSS_CC) $(CROSS_CFLAGS) $(FF6X_FIXED) $(FF6X_PROF_DEF) -DSMKX_TC=$(FF6X_TC) -DSMKX_LC=$(LC_ACTIVE) -DSMKX_PARTIAL_CHECK=$(FF6X_PARTIAL_CHECK) -DSMKX_PARTIAL=$(FF6X_PARTIAL) -c -o $@ $<

# Build switches are inputs too: the stamp changes whenever they do.
FF6X_FLAGS := FRAMES=$(FF6X_FRAMES) RENDER=$(FF6X_RENDER) \
  SNAP_EVERY=$(FF6X_SNAP_EVERY) M7_DSP=$(FF6X_M7_DSP) PROFILE=$(FF6X_PROFILE) \
  PROFILE_FROM=$(FF6X_PROFILE_FROM) PARTIAL_CHECK=$(FF6X_PARTIAL_CHECK) PARTIAL=$(FF6X_PARTIAL) APU_FRAME=$(FF6X_APU_FRAME) APU_ECHO=$(FF6X_APU_ECHO) DIAG=$(FF6X_DIAG) LZ=$(FF6X_LZ) LZ_CHECK=$(FF6X_LZ_CHECK) TC=$(FF6X_TC) \
  SNAP_FROM=$(FF6X_SNAP_FROM) LC=$(LC_ACTIVE) LC_BLIT_MIN=$(FF6X_LC_BLIT_MIN) LC_MATH=$(FF6X_LC_MATH) LC_SYNC=$(FF6X_LC_SYNC) LC_CHECK=$(FF6X_LC_CHECK) LC_ALLPAGES=$(FF6X_LC_ALLPAGES) LC_DIAG=$(FF6X_LC_DIAG) HOLD=$(FF6X_HOLD)
$(FF6X)/flags.stamp: FORCE | $(FF6X)/sources.txt
	@test "$$(cat $@ 2>/dev/null)" = "$(FF6X_FLAGS)" || echo "$(FF6X_FLAGS)" > $@

FORCE:

$(FF6X)/m7c2p.bin: falcon/dsp/m7c2p.asm tools/smkx/dsp_asm.py
	mkdir -p $(FF6X)
	python3 -m tools.smkx.dsp_asm --boot $< $@

$(FF6X)/input.s: tools/smkx/input_script.py FORCE | $(FF6X)/sources.txt
	python3 -m tools.smkx.input_script "$(FF6X_INPUT)" $@

$(FF6X)/oklab555.bin: tools/smkx/oklab.py
	mkdir -p $(FF6X)
	python3 -m tools.smkx.oklab $@

# The info screen and the player's files, read at startup (tools/port_info.py,
# tools/games.py): run every time, rewritten only when it changes.
# INFO_WAIT=n keeps the screen n seconds (a key skips); a release uses it.
INFO_WAIT ?= 0
$(FF6X)/info.i: $(FF6X)/sources.txt ../../tools/port_info.py ../../tools/games.py FORCE
	python3 ../../tools/port_info.py ff6 $@ --rom $(FF6X)/ff6.sfc --wait $(INFO_WAIT)

$(FF6X)/FF6X.TOS: $(FF6X)/sources.txt falcon/smkx_rt.s falcon/smkx_rt.i falcon/smkx_info.s $(FF6X)/info.i falcon/ct_lz.s \
  falcon/smkx_video.s falcon/smkx_lcput.s falcon/smkx_m7dsp.s falcon/smkx_dspboot.s $(FF6X_C_OBJ) $(FF6X)/flags.stamp \
  $(FF6X)/oklab555.bin $(FF6X)/m7c2p.bin $(FF6X)/ppu.bin $(FF6X)/ppu7.bin $(FF6X)/ppuw.bin $(FF6X_LC_BIN) $(FF6X_DSP_BIN) $(FF6X)/input.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -Ifalcon -I$(FF6X) -DSMKX_FRAMES=$(FF6X_FRAMES) \
	  -DSMKX_RENDER=$(FF6X_RENDER) -DSMKX_SNAP_EVERY=$(FF6X_SNAP_EVERY) -DSMKX_SNAP_FROM=$(FF6X_SNAP_FROM) -DSMKX_LC=$(LC_ACTIVE) -DSMKX_HOLD=$(FF6X_HOLD) \
	  -DSMKX_M7_DSP=$(FF6X_M7_DSP) -DSMKX_PROFILE_FROM=$(FF6X_PROFILE_FROM) -DSMKX_DIAG=$(FF6X_DIAG) -DCT_LZ=$(FF6X_LZ) -DCT_LZ_CHECK=$(FF6X_LZ_CHECK) -DSMKX_TC=$(FF6X_TC) -o $(FF6X)/rt.o falcon/smkx_rt.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -o $(FF6X)/tables.o $(FF6X)/smkx_tables.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -o $(FF6X)/input.o $(FF6X)/input.s
	for f in $$(cat $(FF6X)/sources.txt); do \
	  $(VASM) -Felf -m68030 -quiet -nowarn=62 -Ifalcon -o $(FF6X)/$${f%.s}.o $(FF6X)/$$f || exit 1; \
	done
	$(VLINK) -b ataritos -tos-flags 1 -M$(FF6X)/FF6X.map -o $@ $(FF6X)/rt.o \
	  $$(sed 's/\.s$$/.o/;s|^|$(FF6X)/|' $(FF6X)/sources.txt) $(FF6X)/tables.o $(FF6X)/input.o $(FF6X_C_OBJ)

ff6x: $(FF6X)/FF6X.TOS

# The emitter's instruction sequences against a 65816 reference on a 68030
# model (needs vasm and `pip install machine68k`; tools/emit_bench.py).
test-emit:
	VASM=$(VASM) python3 -m tools.emit_tests

run-ff6x: ff6x
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $(FF6X)/FF6X.TOS --fast --timeout 1800

# The DSP Mode 7 kernel against the C reference: random VRAM and lines.
$(FF6X)/t_%.o: src/%.c src/snes_ppu.h src/m7_plan.h src/ppu_dsp.h
	mkdir -p $(FF6X)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

M7TEST_OBJ := $(FF6X)/t_m7test.o $(FF6X)/t_m7_plan.o $(FF6X)/t_snes_ppu.o

$(FF6X)/M7TEST.TOS: falcon/m7test.s $(FF6X)/m7c2p.bin $(M7TEST_OBJ)
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(FF6X) -o $(FF6X)/m7test.o falcon/m7test.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(FF6X)/m7test.o $(M7TEST_OBJ)

test-m7: $(FF6X)/M7TEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag m7test --grep M7TEST --fast --timeout 800
	@grep -a "M7TEST lines" $(FF6X)/m7test-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

# The DSP renderer against the host model on real frames (build/dsp_snap
# packs them from the snapshots).
PPUTEST_FRAMES_DIR ?= $(FF6X)/pputest/field1.BIN $(FF6X)/pputest/field2.BIN $(FF6X)/pputest/menu1.BIN \
  $(FF6X)/pputest/menu2.BIN $(FF6X)/pputest/title1.BIN $(FF6X)/pputest/title2.BIN \
  $(FF6X)/pputest/intro1.BIN $(FF6X)/pputest/intro2.BIN $(FF6X)/pputest/intro3.BIN \
  $(FF6X)/pputest/wipe1.BIN $(FF6X)/pputest/wipe2.BIN $(FF6X)/pputest/wipe3.BIN \
  $(FF6X)/pputest/reveal1.BIN $(FF6X)/pputest/edge1.BIN \
  $(FF6X)/pputest/palace1.BIN $(FF6X)/pputest/bird1.BIN $(FF6X)/pputest/fog1.BIN
# Without that directory: the snapshots at hand (F*.BIN of a runtime built with FF6X_SNAP_EVERY).
PPUTEST_FRAMES ?= $(if $(wildcard $(FF6X)/pputest/field1.BIN),$(PPUTEST_FRAMES_DIR),$(sort $(wildcard $(FF6X)/F*.BIN)))

$(FF6X)/ppu.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(FF6X)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=0 --define LCB=0 $< $@

# The same source for frames whose visible rows are all Mode 7.
$(FF6X)/ppu7.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(FF6X)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=1 --define WIN=0 --define LCB=0 $< $@

# And for tile frames with main-screen windows.
$(FF6X)/ppuw.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(FF6X)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=0 $< $@

# The list compositor's group composer (LCB=1, falcon/dsp/ppu.asm; src/smkx_lc.c), and with the window build in it.
$(FF6X)/ppulc.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(FF6X)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=0 --define LCB=1 $< $@

$(FF6X)/ppulcw.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(FF6X)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=1 $< $@

PPUTEST_RANDOM ?= 32

build/dsp_rand: tools/dsp_rand.c tools/dsp_model.c tools/dsp_model.h src/ppu_dsp_plan.c src/ppu_dsp.h src/m7_plan.c \
  src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/dsp_rand.c tools/dsp_model.c \
	  src/ppu_dsp_plan.c src/snes_ppu.c src/m7_plan.c

# Real frames first, then random ones (flips, 16x16 tiles, mode 0, ...).
$(FF6X)/ppu_tests.bin: build/dsp_snap build/dsp_rand $(PPUTEST_FRAMES)
	build/dsp_snap $(FF6X)/ff6.sfc $(PPUTEST_FRAMES) -pack $(FF6X)/ppu_tests_real.bin
	build/dsp_rand $(PPUTEST_RANDOM) 0x5eed $(FF6X)/ppu_tests_rand.bin > $(FF6X)/ppu_tests_rand.txt
	python3 -c "import sys,struct; a,b=(open(f,'rb').read() for f in sys.argv[1:3]); \
	  open(sys.argv[3],'wb').write(b'PPU2'+struct.pack('>I',struct.unpack('>I',a[4:8])[0]+struct.unpack('>I',b[4:8])[0])+a[8:]+b[8:])" \
	  $(FF6X)/ppu_tests_real.bin $(FF6X)/ppu_tests_rand.bin $@

$(FF6X)/t_pputest.o: src/pputest.c src/ppu_dsp.h src/snes_ppu.h src/smkx_prof.h
	mkdir -p $(FF6X)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(FF6X)/PPUTEST.TOS: falcon/pputest.s falcon/smkx_dspboot.s $(FF6X)/ppu.bin $(FF6X)/ppu7.bin $(FF6X)/ppuw.bin $(FF6X)/ppu_tests.bin \
  $(FF6X)/t_pputest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(FF6X) -Ifalcon -o $(FF6X)/pputest.o falcon/pputest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(FF6X)/pputest.o $(FF6X)/t_pputest.o

# The chunky true-colour core, timed (analysis/ct.md, "True colour").
$(FF6X)/tcbench.bin: falcon/dsp/tcbench.asm tools/smkx/dsp_asm.py
	mkdir -p $(FF6X)
	python3 -m tools.smkx.dsp_asm --boot $< $@

$(FF6X)/t_tcbench.o: src/tcbench.c src/smkx_prof.h
	mkdir -p $(FF6X)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(FF6X)/TCBENCH.TOS: falcon/tcbench.s falcon/smkx_dspboot.s $(FF6X)/tcbench.bin $(FF6X)/t_tcbench.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(FF6X) -Ifalcon -o $(FF6X)/tcbench.o falcon/tcbench.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(FF6X)/tcbench.o $(FF6X)/t_tcbench.o

test-tcbench: $(FF6X)/TCBENCH.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag tcbench --grep TCBENCH --fast --timeout 600

test-ppu: $(FF6X)/PPUTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag pputest --grep PPUTEST --fast --timeout 800
	@grep -a "PPUTEST frames" $(FF6X)/pputest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

# The true-colour renderer (src/ppu_tc.h): its host checks and TCTEST.
TC_HOST := tools/tc_model.c tools/tc_pack.c src/ppu_tc.c src/snes_ppu.c src/ppu_dsp_plan.c src/ppu_rows.c src/m7_plan.c
build/tc_snap: tools/tc_snap.c $(TC_HOST) tools/tc_model.h src/ppu_tc.h src/ppu_dsp.h src/ppu_rows.h src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/tc_snap.c $(TC_HOST)
build/tc_rand: tools/tc_rand.c $(TC_HOST) tools/tc_model.h src/ppu_tc.h src/ppu_dsp.h src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/tc_rand.c $(TC_HOST)

check-tc: build/tc_snap build/tc_rand
	build/tc_snap $(FF6X)/ff6.sfc $(FF6X)/F*.BIN | tail -1
	build/tc_rand 400 0x5eed | tail -1

$(FF6X)/tc.bin: falcon/dsp/tc.asm tools/smkx/dsp_asm.py
	mkdir -p $(FF6X)
	python3 -m tools.smkx.dsp_asm --boot --define TC7=0 $< $@

$(FF6X)/tc7.bin: falcon/dsp/tc.asm tools/smkx/dsp_asm.py
	mkdir -p $(FF6X)
	python3 -m tools.smkx.dsp_asm --boot --define TC7=1 $< $@

TCTEST_FRAMES ?= $(PPUTEST_FRAMES)
TCTEST_RANDOM ?= 24
$(FF6X)/tc_tests.bin: build/tc_snap build/tc_rand $(TCTEST_FRAMES)
	build/tc_snap $(FF6X)/ff6.sfc $(TCTEST_FRAMES) -pack $(FF6X)/tc_tests_real.bin > /dev/null
	build/tc_rand $(TCTEST_RANDOM) 0x5eed $(FF6X)/tc_tests_rand.bin > $(FF6X)/tc_tests_rand.txt
	python3 -c "import sys,struct; a,b=(open(f,'rb').read() for f in sys.argv[1:3]); \
	  open(sys.argv[3],'wb').write(b'TCP1'+struct.pack('>I',struct.unpack('>I',a[4:8])[0]+struct.unpack('>I',b[4:8])[0])+a[8:]+b[8:])" \
	  $(FF6X)/tc_tests_real.bin $(FF6X)/tc_tests_rand.bin $@

$(FF6X)/t_tctest.o: src/tctest.c src/ppu_dsp.h src/ppu_tc.h src/snes_ppu.h src/smkx_prof.h
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(FF6X)/TCTEST.TOS: falcon/tctest.s falcon/smkx_dspboot.s $(FF6X)/tc.bin $(FF6X)/tc7.bin $(FF6X)/tc_tests.bin $(FF6X)/t_tctest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(FF6X) -Ifalcon -o $(FF6X)/tctest.o falcon/tctest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(FF6X)/tctest.o $(FF6X)/t_tctest.o

test-tc: $(FF6X)/TCTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag tctest --grep TCTEST --fast --timeout 1500
	@grep -a "TCTEST frames" $(FF6X)/tctest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

build/palette_test: tools/palette_test.c src/palette.c src/palette.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/palette_test.c src/palette.c

# The SPC700 interpreter on CT's own sound driver (host).
build/spc_test: tools/spc_test.c src/spc700.c src/spc700.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/spc_test.c src/spc700.c

test-spc: build/spc_test $(FF6X)/sources.txt
	build/spc_test $(FF6X)/ff6.sfc

test-palette: build/palette_test $(FF6X)/oklab555.bin
	build/palette_test $(FF6X)/oklab555.bin

# SNAP.BIN from the runtime: Falcon planes and palette against the host.
build/ppu_snap: tools/ppu_snap.c src/snes_ppu.c src/snes_ppu.h src/palette.c src/palette.h src/ppu_dsp_plan.c \
  src/ppu_dsp.h src/m7_plan.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/ppu_snap.c src/snes_ppu.c src/palette.c src/ppu_dsp_plan.c \
	  src/m7_plan.c

build/win_test: tools/win_test.c src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/win_test.c src/snes_ppu.c

# snes9x as the reference: its libretro core, from the 1.63 sources
# (fetched from GitHub, built under build/snes9x, untracked), and
# tools/s9x_ref.c, which runs it headless and pairs its frames with the
# port's snapshots by their work RAM.
S9X := build/snes9x
$(S9X)/src/libretro/snes9x_libretro.dylib:
	test -d $(S9X)/src || git clone --depth 1 --branch 1.63 https://github.com/snes9xgit/snes9x.git $(S9X)/src
	$(MAKE) -C $(S9X)/src/libretro platform=osx CXXFLAGS="$(LDFLAGS)" LDFLAGS="$(LDFLAGS)"

build/s9x_ref: tools/s9x_ref.c $(S9X)/src/libretro/snes9x_libretro.dylib | build
	$(CC) $(CPPFLAGS) -I$(S9X)/src/libretro $(CFLAGS) $(LDFLAGS) -o $@ tools/s9x_ref.c

build/ppu_rgb: tools/ppu_rgb.c src/snes_ppu.c src/snes_ppu.h src/ppu_dsp_plan.c src/ppu_dsp.h src/m7_plan.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/ppu_rgb.c src/snes_ppu.c src/ppu_dsp_plan.c src/m7_plan.c

build/ppu_census: tools/ppu_census.c src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/ppu_census.c src/snes_ppu.c

build/win_census: tools/win_census.c src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/win_census.c src/snes_ppu.c

# The DSP renderer's design against the reference, on the snapshots.
build/dsp_snap: tools/dsp_snap.c tools/dsp_model.c tools/dsp_model.h src/ppu_dsp_plan.c src/ppu_dsp.h src/m7_plan.c \
  src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/dsp_snap.c tools/dsp_model.c \
	  src/ppu_dsp_plan.c src/snes_ppu.c src/m7_plan.c

check-dsp-model: build/dsp_snap
	build/dsp_snap $(FF6X)/ff6.sfc $(FF6X)/F*.BIN

check-snap: build/ppu_snap $(FF6X)/oklab555.bin
	build/ppu_snap $(FF6X)/SNAP.BIN $(FF6X)/ff6.sfc $(FF6X)/oklab555.bin

check-snaps: build/ppu_snap $(FF6X)/oklab555.bin
	@fail=0; for f in $(FF6X)/F*.BIN; do \
	  echo "== $$f"; \
	  build/ppu_snap $$f $(FF6X)/ff6.sfc $(FF6X)/oklab555.bin $${f%.BIN}.ppm || fail=1; \
	  magick $${f%.BIN}.ppm $${f%.BIN}.png && rm -f $${f%.BIN}.ppm; \
	done; exit $$fail

# The list compositor model (analysis/list-compositor.md): pages, dirty groups, restore runs and
# blitter rows against the exact renderer.
build/lc_model: tools/lc/lc_model.c src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/lc/lc_model.c src/snes_ppu.c


check-lc-model: build/lc_model
	build/lc_model census $(FF6X)/ff6.sfc $(sort $(wildcard $(FF6X)/F*.BIN))
	build/lc_model seq $(FF6X)/ff6.sfc $(sort $(wildcard $(FF6X)/F*.BIN))
	build/lc_model blitfuzz 50000 1
	LC_CACHE_KB=64 build/lc_model blitfuzz 50000 2
	LC_CACHE_KB=64 build/lc_model seq $(FF6X)/ff6.sfc $(sort $(wildcard $(FF6X)/F*.BIN))
	@if [ -n "$(LC_HSIM_FRAME)" ]; then \
	  build/lc_model hsim $(FF6X)/ff6.sfc $(LC_HSIM_FRAME) 200 4 1 0 3 2 && \
	  LC_OVERLAY=ring LC_CACHE_KB=64 LC_LAYER_SX=2,1,0,0 build/lc_model hsim $(FF6X)/ff6.sfc $(LC_HSIM_FRAME) 200 4 1 0 0 0; \
	fi

# The ring model (the blitter renderer, analysis/list-compositor.md; the model and the Hatari player only, no game
# runtime draws with it) on consecutive real frames: LC_SNAPS, snapshots of a runtime built with FF6X_SNAP_EVERY=1 and
# FF6X_SNAP_FROM=n (`make lc-snaps LC_FROM=.. LC_TO=..`, scripts/lc_snaps.sh). Every snapshot through the model's census,
# then the longest run of consecutive frames it covers through the ring model (hreal), recorded as the stream
# falcon/lcplay.s plays in Hatari (check-lc-play: every frame's page window against the stream's checksum;
# bench-lc-play: time per update, emulated).
LC_ENV := LC_CHOOSER=exact LC_INSTANCE=1 LC_PROG_KB=128 LC_OVERLAY=ring LC_CACHE_KB=64
check-lc-real: build/lc_model
	@ls $(LC_SNAPS)/F*.BIN >/dev/null 2>&1 || { echo "no real frames in $(LC_SNAPS): make lc-snaps"; exit 1; }
	mkdir -p $(LC)
	build/lc_model census $(FF6X)/ff6.sfc $(sort $(wildcard $(LC_SNAPS)/F*.BIN)) | tee $(LC)/census.txt | tail -1
	python3 tools/lc/lc_longest.py $(LC_SNAPS) < $(LC)/census.txt > $(LC)/window.txt
	@test -s $(LC)/window.txt || { echo "no frame the model covers"; exit 1; }
	@echo "longest covered run: $$(wc -l < $(LC)/window.txt | tr -d ' ') frames"
	build/lc_model seq $(FF6X)/ff6.sfc $$(cat $(LC)/window.txt) | tail -2
	LC_REC=$(LC)/ff6.lcp $(LC_ENV) build/lc_model hreal $(FF6X)/ff6.sfc $$(cat $(LC)/window.txt) | tail -6
	build/lc_model play $(LC)/ff6.lcp

check-lc-play: check-lc-real
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_lcplay.py check $(LC)/ff6.lcp

bench-lc-play: check-lc-real
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_lcplay.py bench $(LC)/ff6.lcp

# ---- The list compositor (analysis/list-compositor-ffvi.md) -----------------------------------------------
# tools/lc/lcd_model.c: the planner (src/lc_plan.c) against the reference on consecutive snapshots: which groups of
# the scroll ring each frame writes, from inputs only; the shown page must equal the reference picture.
# LC_SNAPS: a directory of consecutive snapshots (the runtime built with FF6X_SNAP_EVERY=1, FF6X_SNAP_FROM).
LC := build/lc
LC_SNAPS ?= $(LC)/snaps
LC_FROM ?= 1000
LC_TO ?= 1200

# Consecutive snapshots of frames LC_FROM..LC_TO into LC_SNAPS (the game in the patched Hatari, ~3-6 minutes).
lc-snaps:
	scripts/lc_snaps.sh $(LC_SNAPS) $(LC_FROM) $(LC_TO) 1

$(LC)/lcd_model: tools/lc/lcd_model.c tools/lc/lc_model.c src/lc_plan.c src/lc_plan.h src/snes_ppu.c src/snes_ppu.h | build
	mkdir -p $(LC)
	$(CC) -Isrc $(CPPFLAGS) $(CFLAGS) -Wno-unused $(LDFLAGS) -o $@ tools/lc/lcd_model.c src/lc_plan.c src/snes_ppu.c -lm

$(LC)/lcd_fuzz: tools/lc/lcd_fuzz.c tools/lc/lcd_model.c tools/lc/lc_model.c src/lc_plan.c src/lc_plan.h src/snes_ppu.c src/snes_ppu.h | build
	mkdir -p $(LC)
	$(CC) -Isrc $(CPPFLAGS) $(CFLAGS) -Wno-unused $(LDFLAGS) -o $@ tools/lc/lcd_fuzz.c src/lc_plan.c src/snes_ppu.c -lm

# The planner's blank-row logic on mutated frames (a blank from a random row, or none, every frame; scroll): tools/lc/lcd_fuzz.c
check-lcd-fuzz: $(LC)/lcd_fuzz
	$(LC)/lcd_fuzz $(FF6X)/ff6.sfc $(FF6X)/F00005DD.BIN 400 1
	$(LC)/lcd_fuzz $(FF6X)/ff6.sfc $(FF6X)/F00005DD.BIN 400 2
	$(LC)/lcd_fuzz $(FF6X)/ff6.sfc $(FF6X)/F00005DD.BIN 400 3

# The block writer of the compositor (src/smkx_lc.c) on the host: tools/lc/lcput_test.c
$(LC)/lcput_test: tools/lc/lcput_test.c src/smkx_lc.c src/lc_plan.c src/lc_plan.h src/snes_ppu.c src/snes_ppu.h src/smkx_ppu.h src/ppu_dsp.h | build
	mkdir -p $(LC)
	$(CC) -Isrc -DLC_BLIT_MIN=24 $(CPPFLAGS) $(CFLAGS) -Wno-unused $(LDFLAGS) -o $@ tools/lc/lcput_test.c src/lc_plan.c src/snes_ppu.c -lm

check-lc-put: $(LC)/lcput_test
	$(LC)/lcput_test

$(LC)/lc_why: tools/lc/lc_why.c tools/lc/lc_model.c src/lc_plan.c src/lc_plan.h src/snes_ppu.c src/snes_ppu.h | build
	mkdir -p $(LC)
	$(CC) -Isrc $(CPPFLAGS) $(CFLAGS) -Wno-unused $(LDFLAGS) -o $@ tools/lc/lc_why.c src/lc_plan.c src/snes_ppu.c -lm

check-lcd-model: $(LC)/lcd_model
	@ls $(LC_SNAPS)/F*.BIN >/dev/null 2>&1 || { echo "no snapshots in $(LC_SNAPS)"; exit 1; }
	$(LC)/lcd_model $(FF6X)/ff6.sfc $(sort $(wildcard $(LC_SNAPS)/F*.BIN)) | tail -1

# LCDTEST: the LC build of the DSP program on real frames with masks of several patterns; each frame's list against
# the model (build/dsp_snap -packlc; LCD_PATS = mask of the 8 mask patterns to pack). LCD_FRAMES: snapshots of tile
# frames (FF6's snapshots in build/ff6x).
LCD_FRAMES ?= $(wordlist 1,12,$(sort $(wildcard $(FF6X)/F*.BIN)))

$(FF6X)/lcd_tests.bin: build/dsp_snap $(LCD_FRAMES)
	mkdir -p $(FF6X)
	-build/dsp_snap $(FF6X)/ff6.sfc $(LCD_FRAMES) -packlc $@ > /dev/null

$(FF6X)/t_lcdtest.o: src/lcdtest.c src/ppu_dsp.h src/smkx_prof.h
	mkdir -p $(FF6X)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(FF6X)/LCDTEST.TOS: falcon/lcdtest.s falcon/smkx_dspboot.s $(FF6X)/ppulc.bin $(FF6X)/ppulcw.bin $(FF6X)/lcd_tests.bin $(FF6X)/t_lcdtest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(FF6X) -Ifalcon -o $(FF6X)/lcdtest.o falcon/lcdtest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(FF6X)/lcdtest.o $(FF6X)/t_lcdtest.o

test-lcd: $(FF6X)/LCDTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag lcdtest --grep LCDTEST --fast --timeout 800
	@grep -a "LCDTEST frames" $(FF6X)/lcdtest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'


clean:
	rm -rf build
