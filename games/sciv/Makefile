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
ifeq ($(OS),Windows_NT)
HATARI := ../../../F030Arcade/third_party/hatari/build/src/hatari.exe
endif
CROSS_CC ?= ../../../cross-mint/bin/m68k-atari-mintelf-gcc
GAME_SOURCE := third_party/sciv-disassembly-TBD
# The ROM as the user supplied it: .sfc, .smc or a ZIP holding one.
ROM ?= $(subst ?,$(space),$(firstword $(subst $(space),?,$(wildcard assets/Super\ Castlevania\ IV\ (USA).sfc assets/Super\ Castlevania\ IV\ (USA).smc assets/Super\ Castlevania\ IV\ (USA).zip))))
empty :=
space := $(empty) $(empty)
escape_space = $(subst $(space),\$(space),$(1))
ROM_PREREQUISITE := $(call escape_space,$(ROM))

.PHONY: all scivx-source scivx run-scivx test-emit check-tc test-tc test-m7 check-snap check-snaps check-dsp-model test-ppu test-palette test-spc clean FORCE
all: scivx-source

build:
	mkdir -p build

# 65816 -> 68030 translation of the ROM's program (tools/smkx, from the
# F030SMK port, by way of F030CT). Everything under build/scivx is derived from the ROM and
# stays untracked.
SCIVX := build/scivx
SCIVX_FRAMES ?= 0
SCIVX_RENDER_EVERY ?= 1
SCIVX_SNAP_EVERY ?= 0
SCIVX_SNAP_FROM ?= 0
SCIVX_PARTIAL ?= 2                  # partial rendering: 0 off, 1 changed rows only, 2 also skips unchanged pictures
SCIVX_PARTIAL_CHECK ?= 0            # 1: every DSP picture is compared with the CPU's (slow)
SCIVX_M7_DSP ?= 1
SCIVX_PROFILE ?= 0
ifeq ($(SCIVX_PROFILE),1)
SCIVX_PROF_DEF := -DSMKX_PROFILE -DPPU_PROFILE
endif
SCIVX_PROFILE_FROM ?= 0
# 0: release build, without the call-site notes of the generic memory helpers
# (the status line's "from" and the profile's attribution need them).
SCIVX_DIAG ?= 1
# 1: the SPC700 gets 1/60 s per frame (music tempo); 0: only when polled
# (default while sound is deferred: the driver costs ~370 ms per frame).
SCIVX_APU_FRAME ?= 0
# Chrono Trigger's Gfx_DecompressEntry natively (falcon/ct_lz.s, F030CT's;
# off: FF6 has no such routine, see ROADMAP.md). SCIVX_LZ_CHECK=1 also runs
# the translation after it and compares WRAM at every call.
SCIVX_LZ ?= 0
SCIVX_LZ_CHECK ?= 0
# 1: the screen in true colour (RGB565), drawn by the true-colour DSP
# renderer (falcon/dsp/tc.asm, src/smkx_tc.c), the CPU's ppu_render_565 for
# frames it cannot take; 0: the planar renderer and the palette paths.
SCIVX_TC ?= 0
# CPU-planned list compositor is experimental: stage-1 planning stalls the
# CPU. Use the whole-frame DSP path until dirty planning moves to the DSP.
SCIVX_LC ?= 0
SCIVX_LC_BLIT_MIN ?= 24
SCIVX_LC_SYNC ?= 0
SCIVX_LC_DIAG ?= 0
SCIVX_LC_CHECK ?= 0
SCIVX_LC_CHECK_FROM ?= 0
SCIVX_LC_ALLPAGES ?= 0
SCIVX_LC_MATH ?= 1
SCIVX_LC_SPLIT ?= 0
LC_ACTIVE := $(if $(filter 1,$(SCIVX_TC)),0,$(SCIVX_LC))
# 1: no SPC700 once the game's driver would start; port writes are echoed
# (src/smkx_apu.c). FF6's driver costs the 68030 minutes for the title theme.
SCIVX_APU_ECHO ?= 0
# Scripted joypad input, FRAME:PAD pairs (tools/smkx/input_script.py).
SCIVX_INPUT ?=
SCIVX_PY := $(wildcard tools/smkx/*.py) tools/smkx/dispatch_targets.txt tools/smkx/natives.txt tools/smkx/probes.txt tools/smkx/interrupt_targets.txt \
  tools/smkx/marks.txt tools/smkx/tables.txt

$(SCIVX)/sources.txt: $(SCIVX_PY) $(wildcard $(GAME_SOURCE)/src/*/*.asm $(GAME_SOURCE)/include/*.inc) $(ROM_PREREQUISITE)
	python3 -m tools.smkx "$(ROM)" $(SCIVX)

scivx-source: $(SCIVX)/sources.txt

# The video adapter's C code, cross-compiled freestanding for the 68030.
CROSS_CFLAGS := -m68030 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
  -fomit-frame-pointer -nostdlib -Wall -Wextra -Isrc
ifeq ($(SCIVX_TC),1)
SCIVX_DSP_OBJ := $(SCIVX)/smkx_tc.o $(SCIVX)/ppu_rows.o
SCIVX_DSP_BIN := $(SCIVX)/tc.bin $(SCIVX)/tc7.bin
else
SCIVX_DSP_OBJ := $(SCIVX)/smkx_ppu.o
SCIVX_DSP_BIN :=
endif
ifeq ($(LC_ACTIVE),1)
SCIVX_LC_OBJ := $(SCIVX)/lc_plan.o $(SCIVX)/smkx_lc.o
SCIVX_LC_BIN := $(SCIVX)/ppulc.bin $(SCIVX)/ppulcw.bin $(SCIVX)/ppulc7.bin
endif
SCIVX_C_OBJ := $(SCIVX_LC_OBJ) $(SCIVX)/snes_ppu.o $(SCIVX)/smkx_video.o $(SCIVX)/palette.o $(SCIVX)/smkx_m7.o \
  $(SCIVX)/m7_plan.o $(SCIVX)/spc700.o $(SCIVX)/smkx_apu.o $(SCIVX_DSP_OBJ) $(SCIVX)/ppu_dsp_plan.o

# C that translated code calls (the APU glue) runs with the VBL enabled; the
# VBL runs the translated NMI on the interrupted registers, so gcc must not
# borrow a5 (WRAM) or a6 (the 65816 stack) there (F030SMK's finding).
$(SCIVX)/spc700.o $(SCIVX)/smkx_apu.o: SCIVX_FIXED := -ffixed-a5 -ffixed-a6 -DSMKX_APU_FRAME=$(SCIVX_APU_FRAME) -DSMKX_APU_ECHO=$(SCIVX_APU_ECHO)
$(SCIVX_LC_OBJ): SCIVX_FIXED := -DLC_PROF -DLC_BLIT_MIN=$(SCIVX_LC_BLIT_MIN) -DLC_REFUSE_MATH=$(SCIVX_LC_MATH) -DLC_SPLIT_RING=$(SCIVX_LC_SPLIT) -DSMKX_LC_SYNC=$(if $(filter 1,$(SCIVX_LC_CHECK)),1,$(SCIVX_LC_SYNC)) -DSMKX_LC_CHECK=$(SCIVX_LC_CHECK) -DSMKX_SNAP_FROM=$(SCIVX_LC_CHECK_FROM) -DSMKX_LC_ALLPAGES=$(SCIVX_LC_ALLPAGES) -DSMKX_LC_DIAG=$(SCIVX_LC_DIAG)

$(SCIVX)/%.o: src/%.c src/snes_ppu.h src/palette.h src/smkx_m7.h src/m7_plan.h src/smkx_prof.h src/spc700.h \
  src/smkx_ppu.h src/ppu_dsp.h src/ppu_tc.h src/ppu_rows.h src/lc_plan.h $(SCIVX)/flags.stamp | $(SCIVX)/sources.txt
	$(CROSS_CC) $(CROSS_CFLAGS) $(SCIVX_FIXED) $(SCIVX_PROF_DEF) -DSMKX_TC=$(SCIVX_TC) -DSMKX_LC=$(LC_ACTIVE) -DSMKX_PARTIAL_CHECK=$(SCIVX_PARTIAL_CHECK) -DSMKX_PARTIAL=$(SCIVX_PARTIAL) -c -o $@ $<

# Build switches are inputs too: the stamp changes whenever they do.
SCIVX_FLAGS := FRAMES=$(SCIVX_FRAMES) RENDER_EVERY=$(SCIVX_RENDER_EVERY) \
  SNAP_EVERY=$(SCIVX_SNAP_EVERY) SNAP_FROM=$(SCIVX_SNAP_FROM) M7_DSP=$(SCIVX_M7_DSP) PROFILE=$(SCIVX_PROFILE) \
  PROFILE_FROM=$(SCIVX_PROFILE_FROM) PARTIAL_CHECK=$(SCIVX_PARTIAL_CHECK) PARTIAL=$(SCIVX_PARTIAL) APU_FRAME=$(SCIVX_APU_FRAME) APU_ECHO=$(SCIVX_APU_ECHO) DIAG=$(SCIVX_DIAG) LZ=$(SCIVX_LZ) LZ_CHECK=$(SCIVX_LZ_CHECK) TC=$(SCIVX_TC)
SCIVX_FLAGS += LC=$(LC_ACTIVE) LC_BLIT_MIN=$(SCIVX_LC_BLIT_MIN) LC_MATH=$(SCIVX_LC_MATH) LC_SPLIT=$(SCIVX_LC_SPLIT) LC_SYNC=$(SCIVX_LC_SYNC) LC_CHECK=$(SCIVX_LC_CHECK) LC_CHECK_FROM=$(SCIVX_LC_CHECK_FROM) LC_ALLPAGES=$(SCIVX_LC_ALLPAGES) LC_DIAG=$(SCIVX_LC_DIAG)
$(SCIVX)/flags.stamp: FORCE | $(SCIVX)/sources.txt
	@test "$$(cat $@ 2>/dev/null)" = "$(SCIVX_FLAGS)" || { echo "$(SCIVX_FLAGS)" > $@; rm -f $(SCIVX)/SCIVX.TOS; }

FORCE:

$(SCIVX)/m7c2p.bin: falcon/dsp/m7c2p.asm tools/smkx/dsp_asm.py
	mkdir -p $(SCIVX)
	python3 -m tools.smkx.dsp_asm --boot $< $@

$(SCIVX)/input.s: tools/smkx/input_script.py FORCE | $(SCIVX)/sources.txt
	python3 -m tools.smkx.input_script "$(SCIVX_INPUT)" $@

$(SCIVX)/oklab555.bin: tools/smkx/oklab.py
	mkdir -p $(SCIVX)
	python3 -m tools.smkx.oklab $@

# The info screen and the player's files, read at startup (tools/port_info.py,
# tools/games.py): run every time, rewritten only when it changes.
# INFO_WAIT=n keeps the screen n seconds (a key skips); a release uses it.
INFO_WAIT ?= 0
$(SCIVX)/info.i: $(SCIVX)/sources.txt ../../tools/port_info.py ../../tools/games.py FORCE
	python3 ../../tools/port_info.py sciv $@ --rom $(SCIVX)/sciv.sfc --wait $(INFO_WAIT)

$(SCIVX)/SCIVX.TOS: $(SCIVX)/sources.txt falcon/smkx_rt.s falcon/smkx_rt.i falcon/smkx_info.s $(SCIVX)/info.i \
  falcon/smkx_video.s falcon/smkx_lcput.s falcon/ssi_play.s falcon/smkx_m7dsp.s falcon/smkx_dspboot.s $(SCIVX_C_OBJ) $(SCIVX)/flags.stamp \
  $(SCIVX)/oklab555.bin $(SCIVX)/m7c2p.bin $(SCIVX)/ppu.bin $(SCIVX)/ppu7.bin $(SCIVX)/ppuw.bin $(SCIVX)/ppuret.bin $(SCIVX_LC_BIN) $(SCIVX_DSP_BIN) $(SCIVX)/input.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -Ifalcon -I$(SCIVX) -DSMKX_FRAMES=$(SCIVX_FRAMES) \
	  -DSMKX_RENDER_EVERY=$(SCIVX_RENDER_EVERY) -DSMKX_SNAP_EVERY=$(SCIVX_SNAP_EVERY) -DSMKX_SNAP_FROM=$(SCIVX_SNAP_FROM) -DSMKX_LC=$(LC_ACTIVE) \
	  -DSMKX_M7_DSP=$(SCIVX_M7_DSP) -DSMKX_PROFILE_FROM=$(SCIVX_PROFILE_FROM) -DSMKX_DIAG=$(SCIVX_DIAG) -DCT_LZ=$(SCIVX_LZ) -DCT_LZ_CHECK=$(SCIVX_LZ_CHECK) -DSMKX_TC=$(SCIVX_TC) -o $(SCIVX)/rt.o falcon/smkx_rt.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -o $(SCIVX)/tables.o $(SCIVX)/smkx_tables.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -o $(SCIVX)/input.o $(SCIVX)/input.s
	for f in $$(cat $(SCIVX)/sources.txt); do \
	  $(VASM) -Felf -m68030 -quiet -nowarn=62 -Ifalcon -o $(SCIVX)/$${f%.s}.o $(SCIVX)/$$f || exit 1; \
	done
	$(VLINK) -b ataritos -tos-flags 1 -M$(SCIVX)/SCIVX.map -o $@ $(SCIVX)/rt.o \
	  $$(sed 's/\.s$$/.o/;s|^|$(SCIVX)/|' $(SCIVX)/sources.txt) $(SCIVX)/tables.o $(SCIVX)/input.o $(SCIVX_C_OBJ)

scivx: $(SCIVX)/SCIVX.TOS

# The emitter's instruction sequences against a 65816 reference on a 68030
# model (needs vasm and `pip install machine68k`; tools/emit_bench.py).
test-emit:
	VASM=$(VASM) python3 -m tools.emit_tests

run-scivx: scivx
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $(SCIVX)/SCIVX.TOS --fast --timeout 1800

# The DSP Mode 7 kernel against the C reference: random VRAM and lines.
$(SCIVX)/t_%.o: src/%.c src/snes_ppu.h src/m7_plan.h src/ppu_dsp.h
	mkdir -p $(SCIVX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

M7TEST_OBJ := $(SCIVX)/t_m7test.o $(SCIVX)/t_m7_plan.o $(SCIVX)/t_snes_ppu.o

$(SCIVX)/M7TEST.TOS: falcon/m7test.s $(SCIVX)/m7c2p.bin $(M7TEST_OBJ)
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SCIVX) -o $(SCIVX)/m7test.o falcon/m7test.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(SCIVX)/m7test.o $(M7TEST_OBJ)

test-m7: $(SCIVX)/M7TEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag m7test --grep M7TEST --fast --timeout 800
	@grep -a "M7TEST lines" $(SCIVX)/m7test-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

# The DSP renderer against the host model on real frames (build/dsp_snap
# packs them from the snapshots).
PPUTEST_FRAMES ?= $(SCIVX)/pputest/field1.BIN $(SCIVX)/pputest/field2.BIN $(SCIVX)/pputest/menu1.BIN \
  $(SCIVX)/pputest/menu2.BIN $(SCIVX)/pputest/title1.BIN $(SCIVX)/pputest/title2.BIN \
  $(SCIVX)/pputest/intro1.BIN $(SCIVX)/pputest/intro2.BIN $(SCIVX)/pputest/intro3.BIN \
  $(SCIVX)/pputest/wipe1.BIN $(SCIVX)/pputest/wipe2.BIN $(SCIVX)/pputest/wipe3.BIN \
  $(SCIVX)/pputest/reveal1.BIN $(SCIVX)/pputest/edge1.BIN \
  $(SCIVX)/pputest/palace1.BIN $(SCIVX)/pputest/bird1.BIN $(SCIVX)/pputest/fog1.BIN

$(SCIVX)/ppu.bin: falcon/dsp/ppu.asm falcon/dsp/retained.inc tools/smkx/dsp_asm.py
	mkdir -p $(SCIVX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=0 --define LCB=0 $< $@

$(SCIVX)/ppulc.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(SCIVX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=0 --define LCB=1 $< $@

$(SCIVX)/ppulcw.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(SCIVX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=1 $< $@

$(SCIVX)/ppulc7.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	python3 -m tools.smkx.dsp_asm --boot --define M7K=1 --define WIN=0 --define LCB=1 $< $@

# The same source for frames whose visible rows are all Mode 7.
$(SCIVX)/ppu7.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(SCIVX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=1 --define WIN=0 --define LCB=0 $< $@

# And for tile frames with main-screen windows.
$(SCIVX)/ppuw.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(SCIVX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=0 $< $@

PPUTEST_RANDOM ?= 32

build/dsp_rand: tools/dsp_rand.c tools/dsp_model.c tools/dsp_model.h src/ppu_dsp_plan.c src/ppu_dsp.h src/m7_plan.c \
  src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/dsp_rand.c tools/dsp_model.c \
	  src/ppu_dsp_plan.c src/snes_ppu.c src/m7_plan.c

# Real frames first, then random ones (flips, 16x16 tiles, mode 0, ...).
$(SCIVX)/ppu_tests.bin: build/dsp_snap build/dsp_rand $(PPUTEST_FRAMES)
	build/dsp_snap $(SCIVX)/sciv.sfc $(PPUTEST_FRAMES) -pack $(SCIVX)/ppu_tests_real.bin
	build/dsp_rand $(PPUTEST_RANDOM) 0x5eed $(SCIVX)/ppu_tests_rand.bin > $(SCIVX)/ppu_tests_rand.txt
	python3 -c "import sys,struct; a,b=(open(f,'rb').read() for f in sys.argv[1:3]); \
	  open(sys.argv[3],'wb').write(b'PPU2'+struct.pack('>I',struct.unpack('>I',a[4:8])[0]+struct.unpack('>I',b[4:8])[0])+a[8:]+b[8:])" \
	  $(SCIVX)/ppu_tests_real.bin $(SCIVX)/ppu_tests_rand.bin $@

$(SCIVX)/t_pputest.o: src/pputest.c src/ppu_dsp.h src/snes_ppu.h src/smkx_prof.h
	mkdir -p $(SCIVX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(SCIVX)/PPUTEST.TOS: falcon/pputest.s falcon/ssi_play.s falcon/smkx_dspboot.s $(SCIVX)/ppu.bin $(SCIVX)/ppu7.bin $(SCIVX)/ppuw.bin $(SCIVX)/ppu_tests.bin \
  $(SCIVX)/t_pputest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SCIVX) -Ifalcon -o $(SCIVX)/pputest.o falcon/pputest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(SCIVX)/pputest.o $(SCIVX)/t_pputest.o

# The chunky true-colour core, timed (analysis/ct.md, "True colour").
$(SCIVX)/tcbench.bin: falcon/dsp/tcbench.asm tools/smkx/dsp_asm.py
	mkdir -p $(SCIVX)
	python3 -m tools.smkx.dsp_asm --boot $< $@

$(SCIVX)/t_tcbench.o: src/tcbench.c src/smkx_prof.h
	mkdir -p $(SCIVX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(SCIVX)/TCBENCH.TOS: falcon/tcbench.s falcon/smkx_dspboot.s $(SCIVX)/tcbench.bin $(SCIVX)/t_tcbench.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SCIVX) -Ifalcon -o $(SCIVX)/tcbench.o falcon/tcbench.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(SCIVX)/tcbench.o $(SCIVX)/t_tcbench.o

test-tcbench: $(SCIVX)/TCBENCH.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag tcbench --grep TCBENCH --fast --timeout 600

test-ppu: $(SCIVX)/PPUTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag pputest --grep PPUTEST --fast --timeout 800
	@grep -a "PPUTEST frames" $(SCIVX)/pputest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

# The true-colour renderer (src/ppu_tc.h): its host checks and TCTEST.
TC_HOST := tools/tc_model.c tools/tc_pack.c src/ppu_tc.c src/snes_ppu.c src/ppu_dsp_plan.c src/ppu_rows.c src/m7_plan.c
build/tc_snap: tools/tc_snap.c $(TC_HOST) tools/tc_model.h src/ppu_tc.h src/ppu_dsp.h src/ppu_rows.h src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/tc_snap.c $(TC_HOST)
build/tc_rand: tools/tc_rand.c $(TC_HOST) tools/tc_model.h src/ppu_tc.h src/ppu_dsp.h src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/tc_rand.c $(TC_HOST)

check-tc: build/tc_snap build/tc_rand
	build/tc_snap $(SCIVX)/sciv.sfc $(SCIVX)/F*.BIN | tail -1
	build/tc_rand 400 0x5eed | tail -1

$(SCIVX)/tc.bin: falcon/dsp/tc.asm tools/smkx/dsp_asm.py
	mkdir -p $(SCIVX)
	python3 -m tools.smkx.dsp_asm --boot --define TC7=0 $< $@

$(SCIVX)/tc7.bin: falcon/dsp/tc.asm tools/smkx/dsp_asm.py
	mkdir -p $(SCIVX)
	python3 -m tools.smkx.dsp_asm --boot --define TC7=1 $< $@

TCTEST_FRAMES ?= $(PPUTEST_FRAMES)
TCTEST_RANDOM ?= 24
$(SCIVX)/tc_tests.bin: build/tc_snap build/tc_rand $(TCTEST_FRAMES)
	build/tc_snap $(SCIVX)/sciv.sfc $(TCTEST_FRAMES) -pack $(SCIVX)/tc_tests_real.bin > /dev/null
	build/tc_rand $(TCTEST_RANDOM) 0x5eed $(SCIVX)/tc_tests_rand.bin > $(SCIVX)/tc_tests_rand.txt
	python3 -c "import sys,struct; a,b=(open(f,'rb').read() for f in sys.argv[1:3]); \
	  open(sys.argv[3],'wb').write(b'TCP1'+struct.pack('>I',struct.unpack('>I',a[4:8])[0]+struct.unpack('>I',b[4:8])[0])+a[8:]+b[8:])" \
	  $(SCIVX)/tc_tests_real.bin $(SCIVX)/tc_tests_rand.bin $@

$(SCIVX)/t_tctest.o: src/tctest.c src/ppu_dsp.h src/ppu_tc.h src/snes_ppu.h src/smkx_prof.h
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(SCIVX)/TCTEST.TOS: falcon/tctest.s falcon/smkx_dspboot.s $(SCIVX)/tc.bin $(SCIVX)/tc7.bin $(SCIVX)/tc_tests.bin $(SCIVX)/t_tctest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SCIVX) -Ifalcon -o $(SCIVX)/tctest.o falcon/tctest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(SCIVX)/tctest.o $(SCIVX)/t_tctest.o

test-tc: $(SCIVX)/TCTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag tctest --grep TCTEST --fast --timeout 1500
	@grep -a "TCTEST frames" $(SCIVX)/tctest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

build/palette_test: tools/palette_test.c src/palette.c src/palette.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/palette_test.c src/palette.c

# The SPC700 interpreter on CT's own sound driver (host).
build/spc_test: tools/spc_test.c src/spc700.c src/spc700.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/spc_test.c src/spc700.c

test-spc: build/spc_test $(SCIVX)/sources.txt
	build/spc_test $(SCIVX)/sciv.sfc

test-palette: build/palette_test $(SCIVX)/oklab555.bin
	build/palette_test $(SCIVX)/oklab555.bin

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
build/plan_bench: tools/plan_bench.c tools/dsp_model.h src/ppu_dsp_plan.c src/ppu_dsp.h src/m7_plan.c src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) -g $(LDFLAGS) -o $@ tools/plan_bench.c src/ppu_dsp_plan.c src/snes_ppu.c src/m7_plan.c

build/shift_probe: tools/shift_probe.c tools/plan_bench.c tools/dsp_model.c tools/dsp_model.h src/ppu_dsp_plan.c src/ppu_dsp.h src/m7_plan.c src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) -g $(LDFLAGS) -o $@ tools/shift_probe.c tools/dsp_model.c src/ppu_dsp_plan.c src/snes_ppu.c src/m7_plan.c

build/dsp_snap: tools/dsp_snap.c tools/dsp_model.c tools/dsp_model.h src/ppu_dsp_plan.c src/ppu_dsp.h src/m7_plan.c \
  src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/dsp_snap.c tools/dsp_model.c \
	  src/ppu_dsp_plan.c src/snes_ppu.c src/m7_plan.c

check-dsp-model: build/dsp_snap
	build/dsp_snap $(SCIVX)/sciv.sfc $(SCIVX)/F*.BIN

check-snap: build/ppu_snap $(SCIVX)/oklab555.bin
	build/ppu_snap $(SCIVX)/SNAP.BIN $(SCIVX)/sciv.sfc $(SCIVX)/oklab555.bin

check-snaps: build/ppu_snap $(SCIVX)/oklab555.bin
	@fail=0; for f in $(SCIVX)/F*.BIN; do \
	  echo "== $$f"; \
	  build/ppu_snap $$f $(SCIVX)/sciv.sfc $(SCIVX)/oklab555.bin $${f%.BIN}.ppm || fail=1; \
	  magick $${f%.BIN}.ppm $${f%.BIN}.png && rm -f $${f%.BIN}.ppm; \
	done; exit $$fail

clean:
	rm -rf build

# Compositor checks use generated synthetic fixtures by default. Override
# LCD_FRAMES / LC_SNAPS with captured SCIV snapshots for scene coverage.
LC := build/lc
LC_SNAPS ?= $(SCIVX)
LCD_FRAMES ?= $(LC)/synthetic-tile.BIN $(LC)/synthetic-window.BIN $(LC)/synthetic-mixed.BIN $(LC)/synthetic-subhalf.BIN
.PHONY: check-lc check-lc-put check-lcd-model test-lcd lc-fixtures

$(LC)/sciv_lc_test: tools/lc/sciv_lc_test.c tools/lc/lcd_model.c tools/lc/lc_model.c src/lc_plan.c src/lc_plan.h src/snes_ppu.c src/snes_ppu.h src/palette.h src/ppu_dsp.h | build
	mkdir -p $(LC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused $(LDFLAGS) -o $@ tools/lc/sciv_lc_test.c src/lc_plan.c src/snes_ppu.c -lm

lc-fixtures: $(LC)/sciv_lc_test
	$(LC)/sciv_lc_test $(LC)

check-lc: lc-fixtures check-lc-put build/dsp_snap build/ppu_snap $(SCIVX)/oklab555.bin
	build/dsp_snap $(SCIVX)/sciv.sfc $(LCD_FRAMES) -packlc $(LC)/checked-lists.bin
	@for f in $(LCD_FRAMES); do build/ppu_snap $$f $(SCIVX)/sciv.sfc $(SCIVX)/oklab555.bin || exit 1; done

$(LC)/rigid_probe: tools/lc/rigid_probe.c tools/lc/lcd_model.c tools/lc/lc_model.c src/lc_plan.c src/lc_plan.h src/snes_ppu.c src/snes_ppu.h | build
	mkdir -p $(LC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused $(LDFLAGS) -o $@ tools/lc/rigid_probe.c src/lc_plan.c src/snes_ppu.c -lm

$(LC)/lcput_test: tools/lc/lcput_test.c src/smkx_lc.c src/lc_plan.c src/lc_plan.h src/snes_ppu.c src/snes_ppu.h src/smkx_ppu.h src/ppu_dsp.h | build
	mkdir -p $(LC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused -DLC_BLIT_MIN=24 $(LDFLAGS) -o $@ tools/lc/lcput_test.c src/lc_plan.c src/snes_ppu.c -lm

$(LC)/lcput_sync: tools/lc/lcput_test.c src/smkx_lc.c src/lc_plan.c src/lc_plan.h src/snes_ppu.c src/snes_ppu.h src/smkx_ppu.h src/ppu_dsp.h | build
	mkdir -p $(LC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused -DLC_BLIT_MIN=24 -DSMKX_LC_SYNC=1 $(LDFLAGS) -o $@ tools/lc/lcput_test.c src/lc_plan.c src/snes_ppu.c -lm

check-lc-put: $(LC)/lcput_test $(LC)/lcput_sync
	$(LC)/lcput_test
	$(LC)/lcput_sync 0

$(LC)/lcd_model: tools/lc/lcd_model.c tools/lc/lc_model.c src/lc_plan.c src/lc_plan.h src/snes_ppu.c src/snes_ppu.h | build
	mkdir -p $(LC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused $(LDFLAGS) -o $@ tools/lc/lcd_model.c src/lc_plan.c src/snes_ppu.c -lm

$(LC)/lc_why: tools/lc/lc_why.c tools/lc/lc_model.c src/lc_plan.c src/lc_plan.h src/snes_ppu.c src/snes_ppu.h | build
	mkdir -p $(LC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused $(LDFLAGS) -o $@ tools/lc/lc_why.c src/lc_plan.c src/snes_ppu.c -lm

check-lcd-model: $(LC)/lcd_model
	@ls $(LC_SNAPS)/F*.BIN >/dev/null 2>&1 || { echo "no SCIV snapshots in $(LC_SNAPS)"; exit 1; }
	LC_REFUSE_MATH=1 $(LC)/lcd_model $(SCIVX)/sciv.sfc $(sort $(wildcard $(LC_SNAPS)/F*.BIN))

$(SCIVX)/lcd.flags.stamp: FORCE | $(SCIVX)/sources.txt
	@test "$$(cat $@ 2>/dev/null)" = "$(LCD_FRAMES)" || { echo "$(LCD_FRAMES)" > $@; rm -f $(SCIVX)/lcd_tests.bin $(SCIVX)/LCDTEST.TOS; }

$(SCIVX)/lcd_tests.bin: build/dsp_snap $(LCD_FRAMES) $(SCIVX)/lcd.flags.stamp
	build/dsp_snap $(SCIVX)/sciv.sfc $(LCD_FRAMES) -packlc $@

$(LCD_FRAMES): | lc-fixtures
	@test -s $@

$(SCIVX)/t_lcdtest.o: src/lcdtest.c src/ppu_dsp.h src/smkx_prof.h
	mkdir -p $(SCIVX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(SCIVX)/LCDTEST.TOS: falcon/lcdtest.s falcon/smkx_dspboot.s $(SCIVX)/ppulc.bin $(SCIVX)/ppulcw.bin $(SCIVX)/ppulc7.bin $(SCIVX)/lcd_tests.bin $(SCIVX)/t_lcdtest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SCIVX) -Ifalcon -o $(SCIVX)/lcdtest.o falcon/lcdtest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(SCIVX)/lcdtest.o $(SCIVX)/t_lcdtest.o

test-lcd: $(SCIVX)/LCDTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag lcdtest --grep LCDTEST --fast --timeout 800
	@grep -a "LCDTEST frames" $(SCIVX)/lcdtest-stderr.log | tail -1 | grep -q 'words $$00000000'

# Trace-based disassembly of the ROM (no source listing exists for this game;
# see analysis/trace-disassembly.md). Writes build/sciv_trace/.
.PHONY: sciv-trace
sciv-trace: $(ROM_PREREQUISITE) tools/sciv_trace.py tools/smkx/opcodes.py
	mkdir -p build/rom
	case "$(ROM)" in *.zip) unzip -o -q "$(ROM)" -d build/rom; r="$$(ls build/rom/*.sfc | head -1)";; *) r="$(ROM)";; esac; \
	python3 -m tools.sciv_trace "$$r" build/sciv_trace

$(SCIVX)/ppuret.bin: falcon/dsp/ppu.asm falcon/dsp/retained.inc tools/smkx/dsp_asm.py
	mkdir -p $(SCIVX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=1 --define DR=1 $< $@
