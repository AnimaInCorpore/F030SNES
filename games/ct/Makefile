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
GAME_SOURCE := third_party/ct_disassembly
# The ROM as the user supplied it: .sfc, .smc or a ZIP holding one.
ROM ?= $(subst ?,$(space),$(firstword $(subst $(space),?,$(wildcard assets/Chrono\ Trigger\ (USA).sfc assets/Chrono\ Trigger\ (USA).smc assets/Chrono\ Trigger\ (USA).zip))))
empty :=
space := $(empty) $(empty)
escape_space = $(subst $(space),\$(space),$(1))
ROM_PREREQUISITE := $(call escape_space,$(ROM))

.PHONY: all ctx-source ctx run-ctx test-emit check-tc test-tc test-m7 check-snap check-snaps check-dsp-model check-lc-model check-lc-real check-lc-play bench-lc-play lc-snaps check-lcd-model check-lcd-fuzz check-lc-put test-ppu test-lcd test-palette test-spc clean FORCE
all: ctx-source

build:
	mkdir -p build

# 65816 -> 68030 translation of the ROM's program (tools/smkx, from the
# F030SMK port). Everything under build/ctx is derived from the ROM and
# stays untracked.
CTX := build/ctx
CTX_FRAMES ?= 0
# 0: no rendering at all (timing and reference runs). Otherwise the render context asks for a composition at every VBL
# and works in the game's idle time, paced by smkx_render_poll (src/smkx_video.c): there is no render interval to set.
CTX_RENDER ?= 1
CTX_SNAP_EVERY ?= 0
CTX_SNAP_FROM ?= 0                # with CTX_SNAP_EVERY: from this frame on (a window of consecutive snapshots)
CTX_PARTIAL ?= 2                  # partial rendering: 0 off, 1 changed rows only, 2 also skips unchanged pictures
CTX_PARTIAL_CHECK ?= 0            # 1: every DSP picture is compared with the CPU's (slow)
CTX_M7_DSP ?= 1
CTX_PROFILE ?= 0
ifeq ($(CTX_PROFILE),1)
CTX_PROF_DEF := -DSMKX_PROFILE -DPPU_PROFILE
endif
CTX_PROFILE_FROM ?= 0
# 0: release build, without the call-site notes of the generic memory helpers
# (the status line's "from" and the profile's attribution need them).
CTX_DIAG ?= 1
# 1: the SPC700 gets 1/60 s per frame (music tempo); 0: only when polled
# (default while sound is deferred: the driver costs ~370 ms per frame).
CTX_APU_FRAME ?= 0
# Gfx_DecompressEntry natively (falcon/ct_lz.s); CTX_LZ_CHECK=1 also runs the
# translation after it and compares WRAM at every call.
CTX_LZ ?= 1
CTX_LZ_CHECK ?= 0
# 1: the screen in true colour (RGB565), drawn by the true-colour DSP
# renderer (falcon/dsp/tc.asm, src/smkx_tc.c), the CPU's ppu_render_565 for
# frames it cannot take; 0: the planar renderer and the palette paths.
CTX_TC ?= 0
# 1: the list compositor (src/lc_plan.c, src/smkx_lc.c, analysis/list-compositor-ct.md) draws the eight-plane tile
# frames it covers; the others go the old way. Has no effect with CTX_TC=1.
CTX_LC ?= 1
CTX_LC_BLIT_MIN ?= 24             # blocks of this many groups or more go to the blitter (hog mode)
CTX_LC_SYNC ?= 0                  # 1: one compositor job at a time (no planning ahead; for checking the pipeline)
CTX_LC_DIAG ?= 0                  # 1: the compositor's job log (SMKX LCLOG lines at the end), list-sentinel and copy checks (lc statistics 16-18)
CTX_LC_CHECK ?= 0                 # 1: a debug build: every compositor picture is compared with the CPU reference first (implies CTX_LC_SYNC=1; slow)
CTX_LC_ALLPAGES ?= 0              # 1: the planner compares every VRAM page instead of trusting the written-pages flags (a check)
CTX_LC_MATH ?= 1                  # 1: frames with colour math are not drawn by the compositor (it leaves colour math out); 0: it draws them without
LC_ACTIVE := $(if $(filter 1,$(CTX_TC)),0,$(CTX_LC))
CTX_HOLD ?= 0                     # 1: at the frame limit write SNAP.BIN and keep the picture up (scripts/lc_shot.py)
# Scripted joypad input, FRAME:PAD pairs (tools/smkx/input_script.py).
CTX_INPUT ?=
CTX_PY := $(wildcard tools/smkx/*.py) tools/smkx/dispatch_targets.txt tools/smkx/natives.txt tools/smkx/probes.txt tools/smkx/interrupt_targets.txt \
  tools/smkx/marks.txt tools/smkx/tables.txt

$(CTX)/sources.txt: $(CTX_PY) $(wildcard $(GAME_SOURCE)/*.asm) $(ROM_PREREQUISITE)
	python3 -m tools.smkx "$(ROM)" $(CTX)

ctx-source: $(CTX)/sources.txt

# The video adapter's C code, cross-compiled freestanding for the 68030.
CROSS_CFLAGS := -m68030 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
  -fomit-frame-pointer -nostdlib -Wall -Wextra -Isrc
ifeq ($(CTX_TC),1)
CTX_DSP_OBJ := $(CTX)/smkx_tc.o $(CTX)/ppu_rows.o
CTX_DSP_BIN := $(CTX)/tc.bin $(CTX)/tc7.bin
else
CTX_DSP_OBJ := $(CTX)/smkx_ppu.o
CTX_DSP_BIN :=
endif
ifeq ($(LC_ACTIVE),1)
CTX_LC_OBJ := $(CTX)/lc_plan.o $(CTX)/smkx_lc.o
CTX_LC_BIN := $(CTX)/ppulc.bin $(CTX)/ppulcw.bin
endif
CTX_C_OBJ := $(CTX_LC_OBJ) $(CTX)/snes_ppu.o $(CTX)/smkx_video.o $(CTX)/palette.o $(CTX)/smkx_m7.o \
  $(CTX)/m7_plan.o $(CTX)/spc700.o $(CTX)/smkx_apu.o $(CTX_DSP_OBJ) $(CTX)/ppu_dsp_plan.o

# C that translated code calls (the APU glue) runs with the VBL enabled; the
# VBL runs the translated NMI on the interrupted registers, so gcc must not
# borrow a5 (WRAM) or a6 (the 65816 stack) there (F030SMK's finding).
$(CTX_LC_OBJ): CTX_FIXED := -DLC_PROF -DLC_BLIT_MIN=$(CTX_LC_BLIT_MIN) -DLC_REFUSE_MATH=$(CTX_LC_MATH) -DSMKX_LC_SYNC=$(if $(filter 1,$(CTX_LC_CHECK)),1,$(CTX_LC_SYNC)) -DSMKX_LC_CHECK=$(CTX_LC_CHECK) -DSMKX_LC_ALLPAGES=$(CTX_LC_ALLPAGES) -DSMKX_LC_DIAG=$(CTX_LC_DIAG) -DSMKX_SNAP_FROM=$(CTX_SNAP_FROM)
$(CTX)/spc700.o $(CTX)/smkx_apu.o: CTX_FIXED := -ffixed-a5 -ffixed-a6 -DSMKX_APU_FRAME=$(CTX_APU_FRAME)

$(CTX)/%.o: src/%.c src/snes_ppu.h src/palette.h src/smkx_m7.h src/m7_plan.h src/smkx_prof.h src/spc700.h \
  src/smkx_ppu.h src/ppu_dsp.h src/ppu_tc.h src/ppu_rows.h src/lc_plan.h $(CTX)/flags.stamp | $(CTX)/sources.txt
	$(CROSS_CC) $(CROSS_CFLAGS) $(CTX_FIXED) $(CTX_PROF_DEF) -DSMKX_TC=$(CTX_TC) -DSMKX_LC=$(LC_ACTIVE) -DSMKX_PARTIAL_CHECK=$(CTX_PARTIAL_CHECK) -DSMKX_PARTIAL=$(CTX_PARTIAL) -c -o $@ $<

# Build switches are inputs too: the stamp changes whenever they do.
CTX_FLAGS := FRAMES=$(CTX_FRAMES) RENDER=$(CTX_RENDER) \
  SNAP_EVERY=$(CTX_SNAP_EVERY) M7_DSP=$(CTX_M7_DSP) PROFILE=$(CTX_PROFILE) \
  PROFILE_FROM=$(CTX_PROFILE_FROM) PARTIAL_CHECK=$(CTX_PARTIAL_CHECK) PARTIAL=$(CTX_PARTIAL) APU_FRAME=$(CTX_APU_FRAME) DIAG=$(CTX_DIAG) LZ=$(CTX_LZ) LZ_CHECK=$(CTX_LZ_CHECK) TC=$(CTX_TC) \
  SNAP_FROM=$(CTX_SNAP_FROM) LC=$(LC_ACTIVE) LC_BLIT_MIN=$(CTX_LC_BLIT_MIN) LC_MATH=$(CTX_LC_MATH) LC_SYNC=$(CTX_LC_SYNC) LC_CHECK=$(CTX_LC_CHECK) LC_ALLPAGES=$(CTX_LC_ALLPAGES) LC_DIAG=$(CTX_LC_DIAG) HOLD=$(CTX_HOLD)
$(CTX)/flags.stamp: FORCE | $(CTX)/sources.txt
	@test "$$(cat $@ 2>/dev/null)" = "$(CTX_FLAGS)" || echo "$(CTX_FLAGS)" > $@

FORCE:

$(CTX)/m7c2p.bin: falcon/dsp/m7c2p.asm tools/smkx/dsp_asm.py
	mkdir -p $(CTX)
	python3 -m tools.smkx.dsp_asm --boot $< $@

$(CTX)/input.s: tools/smkx/input_script.py FORCE | $(CTX)/sources.txt
	python3 -m tools.smkx.input_script "$(CTX_INPUT)" $@

$(CTX)/oklab555.bin: tools/smkx/oklab.py
	mkdir -p $(CTX)
	python3 -m tools.smkx.oklab $@

# The info screen and the player's files, read at startup (tools/port_info.py,
# tools/games.py): run every time, rewritten only when it changes.
# INFO_WAIT=n keeps the screen n seconds (a key skips); a release uses it.
INFO_WAIT ?= 0
$(CTX)/info.i: $(CTX)/sources.txt ../../tools/port_info.py ../../tools/games.py FORCE
	python3 ../../tools/port_info.py ct $@ --rom $(CTX)/ct.sfc --wait $(INFO_WAIT)

$(CTX)/CTX.TOS: $(CTX)/sources.txt falcon/smkx_rt.s falcon/smkx_rt.i falcon/smkx_info.s $(CTX)/info.i falcon/ct_lz.s \
  falcon/smkx_video.s falcon/smkx_lcput.s falcon/smkx_m7dsp.s falcon/smkx_dspboot.s $(CTX_C_OBJ) $(CTX)/flags.stamp \
  $(CTX)/oklab555.bin $(CTX)/m7c2p.bin $(CTX)/ppu.bin $(CTX)/ppu7.bin $(CTX)/ppuw.bin $(CTX_LC_BIN) $(CTX_DSP_BIN) $(CTX)/input.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -Ifalcon -I$(CTX) -DSMKX_FRAMES=$(CTX_FRAMES) \
	  -DSMKX_RENDER=$(CTX_RENDER) -DSMKX_SNAP_EVERY=$(CTX_SNAP_EVERY) -DSMKX_SNAP_FROM=$(CTX_SNAP_FROM) -DSMKX_LC=$(LC_ACTIVE) -DSMKX_HOLD=$(CTX_HOLD) \
	  -DSMKX_M7_DSP=$(CTX_M7_DSP) -DSMKX_PROFILE_FROM=$(CTX_PROFILE_FROM) -DSMKX_DIAG=$(CTX_DIAG) -DCT_LZ=$(CTX_LZ) -DCT_LZ_CHECK=$(CTX_LZ_CHECK) -DSMKX_TC=$(CTX_TC) -o $(CTX)/rt.o falcon/smkx_rt.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -o $(CTX)/tables.o $(CTX)/smkx_tables.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -o $(CTX)/input.o $(CTX)/input.s
	for f in $$(cat $(CTX)/sources.txt); do \
	  $(VASM) -Felf -m68030 -quiet -nowarn=62 -Ifalcon -o $(CTX)/$${f%.s}.o $(CTX)/$$f || exit 1; \
	done
	$(VLINK) -b ataritos -tos-flags 1 -M$(CTX)/CTX.map -o $@ $(CTX)/rt.o \
	  $$(sed 's/\.s$$/.o/;s|^|$(CTX)/|' $(CTX)/sources.txt) $(CTX)/tables.o $(CTX)/input.o $(CTX_C_OBJ)

ctx: $(CTX)/CTX.TOS

# The emitter's instruction sequences against a 65816 reference on a 68030
# model (needs vasm and `pip install machine68k`; tools/emit_bench.py).
test-emit:
	VASM=$(VASM) python3 -m tools.emit_tests

run-ctx: ctx
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $(CTX)/CTX.TOS --fast --timeout 1800

# The DSP Mode 7 kernel against the C reference: random VRAM and lines.
$(CTX)/t_%.o: src/%.c src/snes_ppu.h src/m7_plan.h src/ppu_dsp.h
	mkdir -p $(CTX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

M7TEST_OBJ := $(CTX)/t_m7test.o $(CTX)/t_m7_plan.o $(CTX)/t_snes_ppu.o

$(CTX)/M7TEST.TOS: falcon/m7test.s $(CTX)/m7c2p.bin $(M7TEST_OBJ)
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(CTX) -o $(CTX)/m7test.o falcon/m7test.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(CTX)/m7test.o $(M7TEST_OBJ)

test-m7: $(CTX)/M7TEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag m7test --grep M7TEST --fast --timeout 800
	@grep -a "M7TEST lines" $(CTX)/m7test-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

# The DSP renderer against the host model on real frames (build/dsp_snap
# packs them from the snapshots).
PPUTEST_FRAMES_DIR ?= $(CTX)/pputest/field1.BIN $(CTX)/pputest/field2.BIN $(CTX)/pputest/menu1.BIN \
  $(CTX)/pputest/menu2.BIN $(CTX)/pputest/title1.BIN $(CTX)/pputest/title2.BIN \
  $(CTX)/pputest/intro1.BIN $(CTX)/pputest/intro2.BIN $(CTX)/pputest/intro3.BIN \
  $(CTX)/pputest/wipe1.BIN $(CTX)/pputest/wipe2.BIN $(CTX)/pputest/wipe3.BIN \
  $(CTX)/pputest/reveal1.BIN $(CTX)/pputest/edge1.BIN \
  $(CTX)/pputest/palace1.BIN $(CTX)/pputest/bird1.BIN $(CTX)/pputest/fog1.BIN
# Without that directory (the frames of one machine's run): a selection of the snapshots at hand (F*.BIN of a
# runtime built with CTX_SNAP_EVERY): tile, Mode 7 and sprite frames of several scenes.
PPUTEST_FRAMES ?= $(if $(wildcard $(CTX)/pputest/field1.BIN),$(PPUTEST_FRAMES_DIR),$(wildcard $(addprefix $(CTX)/F,0000040.BIN 0000182.BIN 0000202.BIN 0000904.BIN 0000944.BIN 0000B80.BIN 0000E80.BIN 0001388.BIN 000140E.BIN 0001547.BIN 0001640.BIN 00016CF.BIN 0001840.BIN 0001902.BIN 0001A02.BIN)))

$(CTX)/ppu.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(CTX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=0 --define LCB=0 $< $@

# The same source for frames whose visible rows are all Mode 7.
$(CTX)/ppu7.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(CTX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=1 --define WIN=0 --define LCB=0 $< $@

# And for tile frames with main-screen windows.
$(CTX)/ppuw.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(CTX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=0 $< $@

# The list compositor's group composer (LCB=1, falcon/dsp/ppu.asm; src/smkx_lc.c).
$(CTX)/ppulc.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(CTX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=0 --define LCB=1 $< $@

# ... and with the window build in it (frames whose rows have windows masking layers)
$(CTX)/ppulcw.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(CTX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=1 $< $@

PPUTEST_RANDOM ?= 32

build/dsp_rand: tools/dsp_rand.c tools/dsp_model.c tools/dsp_model.h src/ppu_dsp_plan.c src/ppu_dsp.h src/m7_plan.c \
  src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/dsp_rand.c tools/dsp_model.c \
	  src/ppu_dsp_plan.c src/snes_ppu.c src/m7_plan.c

# Real frames first, then random ones (flips, 16x16 tiles, mode 0, ...).
$(CTX)/ppu_tests.bin: build/dsp_snap build/dsp_rand $(PPUTEST_FRAMES)
	build/dsp_snap $(CTX)/ct.sfc $(PPUTEST_FRAMES) -pack $(CTX)/ppu_tests_real.bin
	build/dsp_rand $(PPUTEST_RANDOM) 0x5eed $(CTX)/ppu_tests_rand.bin > $(CTX)/ppu_tests_rand.txt
	python3 -c "import sys,struct; a,b=(open(f,'rb').read() for f in sys.argv[1:3]); \
	  open(sys.argv[3],'wb').write(b'PPU2'+struct.pack('>I',struct.unpack('>I',a[4:8])[0]+struct.unpack('>I',b[4:8])[0])+a[8:]+b[8:])" \
	  $(CTX)/ppu_tests_real.bin $(CTX)/ppu_tests_rand.bin $@

$(CTX)/t_pputest.o: src/pputest.c src/ppu_dsp.h src/snes_ppu.h src/smkx_prof.h
	mkdir -p $(CTX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(CTX)/PPUTEST.TOS: falcon/pputest.s falcon/smkx_dspboot.s $(CTX)/ppu.bin $(CTX)/ppu7.bin $(CTX)/ppuw.bin $(CTX)/ppu_tests.bin \
  $(CTX)/t_pputest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(CTX) -Ifalcon -o $(CTX)/pputest.o falcon/pputest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(CTX)/pputest.o $(CTX)/t_pputest.o

# The chunky true-colour core, timed (analysis/ct.md, "True colour").
$(CTX)/tcbench.bin: falcon/dsp/tcbench.asm tools/smkx/dsp_asm.py
	mkdir -p $(CTX)
	python3 -m tools.smkx.dsp_asm --boot $< $@

$(CTX)/t_tcbench.o: src/tcbench.c src/smkx_prof.h
	mkdir -p $(CTX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(CTX)/TCBENCH.TOS: falcon/tcbench.s falcon/smkx_dspboot.s $(CTX)/tcbench.bin $(CTX)/t_tcbench.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(CTX) -Ifalcon -o $(CTX)/tcbench.o falcon/tcbench.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(CTX)/tcbench.o $(CTX)/t_tcbench.o

test-tcbench: $(CTX)/TCBENCH.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag tcbench --grep TCBENCH --fast --timeout 600

test-ppu: $(CTX)/PPUTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag pputest --grep PPUTEST --fast --timeout 800
	@grep -a "PPUTEST frames" $(CTX)/pputest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

# The true-colour renderer (src/ppu_tc.h): its host checks and TCTEST.
TC_HOST := tools/tc_model.c tools/tc_pack.c src/ppu_tc.c src/snes_ppu.c src/ppu_dsp_plan.c src/ppu_rows.c src/m7_plan.c
build/tc_snap: tools/tc_snap.c $(TC_HOST) tools/tc_model.h src/ppu_tc.h src/ppu_dsp.h src/ppu_rows.h src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/tc_snap.c $(TC_HOST)
build/tc_rand: tools/tc_rand.c $(TC_HOST) tools/tc_model.h src/ppu_tc.h src/ppu_dsp.h src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/tc_rand.c $(TC_HOST)

check-tc: build/tc_snap build/tc_rand
	build/tc_snap $(CTX)/ct.sfc $(CTX)/F*.BIN | tail -1
	build/tc_rand 400 0x5eed | tail -1

$(CTX)/tc.bin: falcon/dsp/tc.asm tools/smkx/dsp_asm.py
	mkdir -p $(CTX)
	python3 -m tools.smkx.dsp_asm --boot --define TC7=0 $< $@

$(CTX)/tc7.bin: falcon/dsp/tc.asm tools/smkx/dsp_asm.py
	mkdir -p $(CTX)
	python3 -m tools.smkx.dsp_asm --boot --define TC7=1 $< $@

TCTEST_FRAMES ?= $(PPUTEST_FRAMES)
TCTEST_RANDOM ?= 24
$(CTX)/tc_tests.bin: build/tc_snap build/tc_rand $(TCTEST_FRAMES)
	build/tc_snap $(CTX)/ct.sfc $(TCTEST_FRAMES) -pack $(CTX)/tc_tests_real.bin > /dev/null
	build/tc_rand $(TCTEST_RANDOM) 0x5eed $(CTX)/tc_tests_rand.bin > $(CTX)/tc_tests_rand.txt
	python3 -c "import sys,struct; a,b=(open(f,'rb').read() for f in sys.argv[1:3]); \
	  open(sys.argv[3],'wb').write(b'TCP1'+struct.pack('>I',struct.unpack('>I',a[4:8])[0]+struct.unpack('>I',b[4:8])[0])+a[8:]+b[8:])" \
	  $(CTX)/tc_tests_real.bin $(CTX)/tc_tests_rand.bin $@

$(CTX)/t_tctest.o: src/tctest.c src/ppu_dsp.h src/ppu_tc.h src/snes_ppu.h src/smkx_prof.h
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(CTX)/TCTEST.TOS: falcon/tctest.s falcon/smkx_dspboot.s $(CTX)/tc.bin $(CTX)/tc7.bin $(CTX)/tc_tests.bin $(CTX)/t_tctest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(CTX) -Ifalcon -o $(CTX)/tctest.o falcon/tctest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(CTX)/tctest.o $(CTX)/t_tctest.o

test-tc: $(CTX)/TCTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag tctest --grep TCTEST --fast --timeout 1500
	@grep -a "TCTEST frames" $(CTX)/tctest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

build/palette_test: tools/palette_test.c src/palette.c src/palette.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/palette_test.c src/palette.c

# The SPC700 interpreter on CT's own sound driver (host).
build/spc_test: tools/spc_test.c src/spc700.c src/spc700.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/spc_test.c src/spc700.c

test-spc: build/spc_test $(CTX)/sources.txt
	build/spc_test $(CTX)/ct.sfc

test-palette: build/palette_test $(CTX)/oklab555.bin
	build/palette_test $(CTX)/oklab555.bin

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

# The list compositor model (analysis/list-compositor.md): pages, dirty groups, restore runs and
# blitter rows against the exact renderer.
build/lc_model: tools/lc/lc_model.c src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/lc/lc_model.c src/snes_ppu.c

# The DSP renderer's design against the reference, on the snapshots.
build/dsp_snap: tools/dsp_snap.c tools/dsp_model.c tools/dsp_model.h src/ppu_dsp_plan.c src/ppu_dsp.h src/m7_plan.c \
  src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/dsp_snap.c tools/dsp_model.c \
	  src/ppu_dsp_plan.c src/snes_ppu.c src/m7_plan.c

check-dsp-model: build/dsp_snap
	build/dsp_snap $(CTX)/ct.sfc $(CTX)/F*.BIN

check-lc-model: build/lc_model
	build/lc_model census $(CTX)/ct.sfc $(CTX)/F*.BIN
	build/lc_model seq $(CTX)/ct.sfc $(CTX)/F*.BIN
	build/lc_model blitfuzz 50000 1
	build/lc_model hsim $(CTX)/ct.sfc $(CTX)/F0001B01.BIN 200 4 1 0 3 2
	build/lc_model hsim $(CTX)/ct.sfc $(CTX)/F0001C01.BIN 200 12 -1 0 1 0
	LC_LAYER_SX=3,2,1,0 LC_LAYER_SY=1,0,2,0 build/lc_model hsim $(CTX)/ct.sfc $(CTX)/F0002142.BIN 200 6 1 1 0 0
	LC_LAYER_SX=2,1,0,0 build/lc_model hsim $(CTX)/ct.sfc $(CTX)/F0001B01.BIN 200 4 1 0 0 0
	LC_CACHE_KB=64 build/lc_model blitfuzz 50000 2
	LC_CACHE_KB=64 build/lc_model seq $(CTX)/ct.sfc $(CTX)/F*.BIN
	LC_CACHE_KB=64 LC_LAYER_SX=3,2,1,0 LC_LAYER_SY=1,0,2,0 build/lc_model hsim $(CTX)/ct.sfc $(CTX)/F0002142.BIN 200 6 1 1 0 0
	LC_OVERLAY=ring LC_CACHE_KB=64 LC_LWORLD=200,380 LC_LAYER_SX=2,1,0,0 LC_LAYER_SY=0,1,0,0 build/lc_model hsim $(CTX)/ct.sfc $(CTX)/F0001B01.BIN 200 4 -1 0 0 0
	LC_OVERLAY=ring LC_LAYER_SX=3,2,1,0 LC_LAYER_SY=1,0,2,0 build/lc_model hsim $(CTX)/ct.sfc $(CTX)/F0002142.BIN 200 6 1 1 0 0
	LC_INSTANCE=1 LC_PROG_KB=256 LC_OVERLAY=ring LC_CACHE_KB=256 LC_LAYER_SX=1,1,0,0 build/lc_model hsim $(CTX)/ct.sfc $(CTX)/F0001B01.BIN 200 0 0 0 0 0
	LC_INSTANCE=1 LC_PROG_KB=24 LC_OVERLAY=ring LC_CACHE_KB=16 LC_LAYER_SX=2,1,0,0 build/lc_model hsim $(CTX)/ct.sfc $(CTX)/F0001B01.BIN 300 38 1 1 0 0
	LC_INSTANCE=2 LC_PROG_LRU=1 LC_PROG_POLICY=random LC_PROG_KB=64 LC_OVERLAY=ring LC_CACHE_KB=32 LC_LAYER_SX=2,1,0,0 build/lc_model hsim $(CTX)/ct.sfc $(CTX)/F0001B01.BIN 300 38 1 1 0 0
	LC_INSTANCE=1 LC_PROG_LRU=1 LC_PROG_KB=128 LC_OVERLAY=ring LC_CACHE_KB=256 LC_LAYER_SX=3,3,3,0 build/lc_model hsim $(CTX)/ct.sfc $(CTX)/F0002142.BIN 300 12 1 1 0 0
	LC_CHOOSER=exact LC_OVERLAY=ring LC_CACHE_KB=64 LC_LAYER_SX=0,2,2,0 build/lc_model hsim $(CTX)/ct.sfc $(CTX)/F0002142.BIN 150 0 0 0 0 0
	LC_CHOOSER=cost LC_OVERLAY=ring LC_CACHE_KB=64 LC_LAYER_SX=2,1,0,0 build/lc_model hsim $(CTX)/ct.sfc $(CTX)/F0001B01.BIN 150 0 0 0 0 0

check-snap: build/ppu_snap $(CTX)/oklab555.bin
	build/ppu_snap $(CTX)/SNAP.BIN $(CTX)/ct.sfc $(CTX)/oklab555.bin

check-snaps: build/ppu_snap $(CTX)/oklab555.bin
	@fail=0; for f in $(CTX)/F*.BIN; do \
	  echo "== $$f"; \
	  build/ppu_snap $$f $(CTX)/ct.sfc $(CTX)/oklab555.bin $${f%.BIN}.ppm || fail=1; \
	  magick $${f%.BIN}.ppm $${f%.BIN}.png && rm -f $${f%.BIN}.ppm; \
	done; exit $$fail

# The ring model (the blitter renderer, analysis/list-compositor.md; the model and the Hatari player only, no game
# runtime draws with it) on consecutive real frames: LC_SNAPS, snapshots of a runtime built with CTX_SNAP_EVERY=1 and
# CTX_SNAP_FROM=n (`make lc-snaps LC_FROM=.. LC_TO=..`, scripts/lc_snaps.sh). Every snapshot through the model's census,
# then the longest run of consecutive frames it covers through the ring model (hreal), recorded as the stream
# falcon/lcplay.s plays in Hatari (check-lc-play: every frame's page window against the stream's checksum;
# bench-lc-play: time per update, emulated).
LC_ENV := LC_CHOOSER=exact LC_INSTANCE=1 LC_PROG_KB=128 LC_OVERLAY=ring LC_CACHE_KB=64
check-lc-real: build/lc_model
	@ls $(LC_SNAPS)/F*.BIN >/dev/null 2>&1 || { echo "no real frames in $(LC_SNAPS): make lc-snaps"; exit 1; }
	mkdir -p $(LC)
	build/lc_model census $(CTX)/ct.sfc $(sort $(wildcard $(LC_SNAPS)/F*.BIN)) | tee $(LC)/census.txt | tail -1
	python3 tools/lc/lc_longest.py $(LC_SNAPS) < $(LC)/census.txt > $(LC)/window.txt
	@test -s $(LC)/window.txt || { echo "no frame the model covers"; exit 1; }
	@echo "longest covered run: $$(wc -l < $(LC)/window.txt | tr -d ' ') frames"
	build/lc_model seq $(CTX)/ct.sfc $$(cat $(LC)/window.txt) | tail -2
	LC_REC=$(LC)/ct.lcp $(LC_ENV) build/lc_model hreal $(CTX)/ct.sfc $$(cat $(LC)/window.txt) | tail -6
	build/lc_model play $(LC)/ct.lcp

check-lc-play: check-lc-real
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_lcplay.py check $(LC)/ct.lcp

bench-lc-play: check-lc-real
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_lcplay.py bench $(LC)/ct.lcp

# ---- The list compositor (analysis/list-compositor-ct.md) -----------------------------------------------
# tools/lc/lcd_model.c: the planner (src/lc_plan.c) against the reference on consecutive snapshots: which groups of
# the scroll ring each frame writes, from inputs only; the shown page must equal the reference picture.
# LC_SNAPS: a directory of consecutive snapshots (the runtime built with CTX_SNAP_EVERY=1, CTX_SNAP_FROM).
LC := build/lc
LC_SNAPS ?= $(LC)/field1
LC_FROM ?= 6640
LC_TO ?= 6840

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
	$(LC)/lcd_fuzz $(CTX)/ct.sfc $(CTX)/F0001A02.BIN 400 1
	$(LC)/lcd_fuzz $(CTX)/ct.sfc $(CTX)/F0002400.BIN 400 2
	$(LC)/lcd_fuzz $(CTX)/ct.sfc $(CTX)/F0001388.BIN 400 3

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
	$(LC)/lcd_model $(CTX)/ct.sfc $(sort $(wildcard $(LC_SNAPS)/F*.BIN)) | tail -1

# LCDTEST: the LC build of the DSP program on real frames with masks of several patterns; each frame's list against
# the model (build/dsp_snap -packlc; LCD_PATS = mask of the 8 mask patterns to pack). LCD_FRAMES: snapshots of tile
# frames (a frame that starts in forced blank, a no-BG sprite frame, frames with a forced blank at line 212, windows (the LCW
# build), 82-91 pages, ...).
LCD_FRAMES ?= $(wildcard $(addprefix $(CTX)/F,0000040.BIN 0000182.BIN 0000944.BIN 0000E80.BIN 00013C9.BIN 000140E.BIN 0001547.BIN 00016CF.BIN 0001840.BIN 0001902.BIN 0001A02.BIN 0001D50.BIN 0002040.BIN 0002102.BIN 0002142.BIN 0002400.BIN 00024C0.BIN 00025D4.BIN 0002613.BIN 00031C9.BIN))

$(CTX)/lcd_tests.bin: build/dsp_snap $(LCD_FRAMES)
	mkdir -p $(CTX)
	-build/dsp_snap $(CTX)/ct.sfc $(LCD_FRAMES) -packlc $@ > /dev/null

$(CTX)/t_lcdtest.o: src/lcdtest.c src/ppu_dsp.h src/smkx_prof.h
	mkdir -p $(CTX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(CTX)/LCDTEST.TOS: falcon/lcdtest.s falcon/smkx_dspboot.s $(CTX)/ppulc.bin $(CTX)/ppulcw.bin $(CTX)/lcd_tests.bin $(CTX)/t_lcdtest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(CTX) -Ifalcon -o $(CTX)/lcdtest.o falcon/lcdtest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(CTX)/lcdtest.o $(CTX)/t_lcdtest.o

test-lcd: $(CTX)/LCDTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag lcdtest --grep LCDTEST --fast --timeout 800
	@grep -a "LCDTEST frames" $(CTX)/lcdtest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

clean:
	rm -rf build
