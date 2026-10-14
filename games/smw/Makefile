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
SMW_SOURCE := third_party/SMWDisX
# The ROM as the user supplied it: .sfc, .smc or a ZIP holding one.
ROM ?= $(subst ?,$(space),$(firstword $(subst $(space),?,$(wildcard assets/Super\ Mario\ World\ (USA).sfc assets/Super\ Mario\ World\ (USA).smc assets/Super\ Mario\ World\ (USA).zip))))
empty :=
space := $(empty) $(empty)
escape_space = $(subst $(space),\$(space),$(1))
ROM_PREREQUISITE := $(call escape_space,$(ROM))

.PHONY: check-lcd bench-lcw test-ppu check-dsp-model lc-snaps check-lc-model check-lc-play bench-lc-play all smwx-source smwx run-smwx test-m7 test-c2p check-ref check-snap check-snaps test-palette clean FORCE
all: smwx-source

build:
	mkdir -p build

# 65816 -> 68030 translation of the ROM's program (tools/smkx, from the
# F030SMK port). Everything under build/smwx is derived from the ROM and
# stays untracked.
SMWX := build/smwx
SMWX_FRAMES ?= 0
SMWX_RENDER ?= 1            # 0: no rendering at all (timing and reference runs); the renderer throttles itself otherwise
SMWX_SNAP_EVERY ?= 0
SMWX_SNAP_FROM ?= 0
SMWX_LC_BLIT_MIN ?= 24        # blocks of this many groups or more go to the blitter (hog mode); 100000: CPU only
SMWX_LC_SYNC ?= 0          # 1: the list compositor waits for the DSP in the frame handler (snapshots match the picture)
SMWX_M7_DSP ?= 1
SMWX_PROFILE_FROM ?= 0
SMWX_SLOW ?= 0
# Test switches for the reference comparison (tools/ref65816.c).
SMWX_SYNC ?= 0
SMWX_SUMS ?= 0
SMWX_TRACE_FROM ?= 0x7fffffff
# Scripted joypad input, FRAME:PAD pairs (tools/smkx/input_script.py).
SMWX_INPUT ?=
SMWX_PY := $(wildcard tools/smkx/*.py) tools/smkx/dispatch_targets.txt tools/smkx/probes.txt \
  tools/smkx/traces.txt

$(SMWX)/sources.txt: $(SMWX_PY) $(wildcard $(SMW_SOURCE)/*.asm) $(ROM_PREREQUISITE)
	python3 -m tools.smkx "$(ROM)" $(SMWX)

smwx-source: $(SMWX)/sources.txt

# The video adapter's C code, cross-compiled freestanding for the 68030.
CROSS_CFLAGS := -DLC_PROF $(LC_EXTRA) -DLC_BLIT_MIN=$(SMWX_LC_BLIT_MIN) -DSMKX_LC_SYNC=$(SMWX_LC_SYNC) -m68030 -O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
  -fomit-frame-pointer -nostdlib -Wall -Wextra -Isrc
SMWX_C_OBJ := $(SMWX)/snes_ppu.o $(SMWX)/ppu_fast.o $(SMWX)/smkx_video.o $(SMWX)/smkx_m7.o \
  $(SMWX)/m7_plan.o $(SMWX)/smkx_ppu.o $(SMWX)/ppu_dsp_plan.o $(SMWX)/lc_plan.o $(SMWX)/smkx_lc.o

$(SMWX)/smkx_video.o: $(SMWX)/flags.stamp

$(SMWX)/%.o: src/%.c src/snes_ppu.h src/ppu_internal.h src/ppu_fast.h src/smkx_m7.h src/m7_plan.h src/ppu_dsp.h \
  src/smkx_ppu.h src/smkx_prof.h src/lc_plan.h | $(SMWX)/sources.txt
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

# Build switches are inputs too: the stamp changes whenever they do.
SMWX_FLAGS := FRAMES=$(SMWX_FRAMES) RENDER=$(SMWX_RENDER) \
  SNAP_EVERY=$(SMWX_SNAP_EVERY) SNAP_FROM=$(SMWX_SNAP_FROM) LC_SYNC=$(SMWX_LC_SYNC) LC_BLIT_MIN=$(SMWX_LC_BLIT_MIN) M7_DSP=$(SMWX_M7_DSP) \
  PROFILE_FROM=$(SMWX_PROFILE_FROM) SLOW=$(SMWX_SLOW) SYNC=$(SMWX_SYNC) SUMS=$(SMWX_SUMS) TRACE_FROM=$(SMWX_TRACE_FROM) INPUT=$(SMWX_INPUT)
# A changed switch also removes SMWX.TOS: make compares whole seconds, and a
# rebuild within the same second as the last link would otherwise be skipped.
$(SMWX)/flags.stamp: FORCE | $(SMWX)/sources.txt
	@test "$$(cat $@ 2>/dev/null)" = "$(SMWX_FLAGS)" || { echo "$(SMWX_FLAGS)" > $@; rm -f $(SMWX)/SMWX.TOS; }

FORCE:

$(SMWX)/m7c2p.bin: falcon/dsp/m7c2p.asm tools/smkx/dsp_asm.py
	mkdir -p $(SMWX)
	python3 -m tools.smkx.dsp_asm $< $@

# The Mode 7 kernel in boot format (rt_dsp_boot, no XBIOS Dsp_ExecBoot); $(SMWX)/m7c2p.bin stays the raw
# image M7TEST hands to Dsp_ExecBoot.
$(SMWX)/m7c2p_boot.bin: falcon/dsp/m7c2p.asm tools/smkx/dsp_asm.py
	mkdir -p $(SMWX)
	python3 -m tools.smkx.dsp_asm --boot $< $@

$(SMWX)/input.s: tools/smkx/input_script.py FORCE | $(SMWX)/sources.txt
	python3 -m tools.smkx.input_script "$(SMWX_INPUT)" $@

$(SMWX)/oklab555.bin: tools/smkx/oklab.py
	mkdir -p $(SMWX)
	python3 -m tools.smkx.oklab $@

# The info screen and the player's files, read at startup (tools/port_info.py,
# tools/games.py): run every time, rewritten only when it changes.
# INFO_WAIT=n keeps the screen n seconds (a key skips); a release uses it.
INFO_WAIT ?= 0
$(SMWX)/info.i: $(SMWX)/sources.txt ../../tools/port_info.py ../../tools/games.py FORCE
	python3 ../../tools/port_info.py smw $@ --rom $(SMWX)/smw.sfc --wait $(INFO_WAIT)

$(SMWX)/SMWX.TOS: $(SMWX)/sources.txt falcon/smkx_rt.s falcon/smkx_rt.i falcon/smkx_info.s $(SMWX)/info.i \
  falcon/smkx_video.s falcon/smkx_lcput.s falcon/smkx_m7dsp.s $(SMWX_C_OBJ) $(SMWX)/flags.stamp \
  $(SMWX)/m7c2p.bin $(SMWX)/m7c2p_boot.bin $(SMWX)/ppu.bin $(SMWX)/ppu7.bin $(SMWX)/ppuw.bin $(SMWX)/ppulc.bin $(SMWX)/ppulcw.bin $(SMWX)/input.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -Ifalcon -I$(SMWX) -DSMKX_FRAMES=$(SMWX_FRAMES) \
	  -DSMKX_RENDER=$(SMWX_RENDER) -DSMKX_SNAP_EVERY=$(SMWX_SNAP_EVERY) -DSMKX_SNAP_FROM=$(SMWX_SNAP_FROM) \
	  -DSMKX_M7_DSP=$(SMWX_M7_DSP) -DSMKX_PROFILE_FROM=$(SMWX_PROFILE_FROM) -DSMKX_SLOW=$(SMWX_SLOW) -DSMKX_SYNC=$(SMWX_SYNC) -DSMKX_SUMS=$(SMWX_SUMS) -DSMKX_TRACE_FROM=$(SMWX_TRACE_FROM) -o $(SMWX)/rt.o falcon/smkx_rt.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -o $(SMWX)/tables.o $(SMWX)/smkx_tables.s
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -o $(SMWX)/input.o $(SMWX)/input.s
	for f in $$(cat $(SMWX)/sources.txt); do \
	  $(VASM) -Felf -m68030 -quiet -nowarn=62 -Ifalcon -o $(SMWX)/$${f%.s}.o $(SMWX)/$$f || exit 1; \
	done
	$(VLINK) -b ataritos -tos-flags 1 -M$(SMWX)/SMWX.map -o $@ $(SMWX)/rt.o \
	  $$(sed 's/\.s$$/.o/;s|^|$(SMWX)/|' $(SMWX)/sources.txt) $(SMWX)/tables.o $(SMWX)/input.o $(SMWX_C_OBJ)

# The stamp first, then a sub-make: a stamp change deletes SMWX.TOS, and
# make 3.81 would not look at the file again within the same run.
smwx: $(SMWX)/flags.stamp
	@$(MAKE) --no-print-directory $(SMWX)/SMWX.TOS

run-smwx: smwx
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $(SMWX)/SMWX.TOS --fast --timeout 1800

# The DSP Mode 7 kernel against the C reference: random VRAM and lines.
$(SMWX)/t_%.o: src/%.c src/snes_ppu.h src/m7_plan.h
	mkdir -p $(SMWX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

M7TEST_OBJ := $(SMWX)/t_m7test.o $(SMWX)/t_m7_plan.o $(SMWX)/t_snes_ppu.o

$(SMWX)/M7TEST.TOS: falcon/m7test.s $(SMWX)/m7c2p.bin $(M7TEST_OBJ)
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SMWX) -o $(SMWX)/m7test.o falcon/m7test.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(SMWX)/m7test.o $(M7TEST_OBJ)

test-m7: $(SMWX)/M7TEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag m7test --grep M7TEST --fast --timeout 800
	@grep -a "M7TEST lines" $(SMWX)/m7test-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

# Reference 65816 interpreter with the runtime's hardware model, and the
# frame-for-frame check of the translation against it (analysis/smw.md):
# a SMKX_SYNC/SMKX_SUMS build logs a WRAM sum per game frame.
build/ref65816: tools/ref65816.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/ref65816.c

REF_FRAMES ?= 5000
check-ref: build/ref65816
	$(MAKE) --no-print-directory smwx SMWX_SYNC=1 SMWX_SUMS=1 SMWX_RENDER=0 \
	  SMWX_FRAMES=$(REF_FRAMES) SMWX_INPUT="$(SMWX_INPUT)"
	python3 scripts/run_smkx.py --fast --timeout 3400 --tag ref
	python3 scripts/smw_refcompare.py $(SMWX)/ref-stderr.log "$(SMWX_INPUT)"

build/c2p_test: tools/c2p_test.c src/ppu_fast.c src/snes_ppu.c src/ppu_fast.h src/ppu_internal.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/c2p_test.c src/ppu_fast.c src/snes_ppu.c

# The display path's chunky-to-planar packing against the bit-by-bit one.
test-c2p: build/c2p_test
	build/c2p_test

build/palette_test: tools/palette_test.c src/palette.c src/palette.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/palette_test.c src/palette.c

test-palette: build/palette_test $(SMWX)/oklab555.bin
	build/palette_test $(SMWX)/oklab555.bin

# SNAP.BIN from the runtime: Falcon planes and palette against the host.
build/ppu_snap: tools/ppu_snap.c src/snes_ppu.c src/ppu_fast.c src/snes_ppu.h src/ppu_internal.h \
  src/ppu_fast.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/ppu_snap.c src/snes_ppu.c src/ppu_fast.c

check-snap: build/ppu_snap
	build/ppu_snap $(SMWX)/SNAP.BIN $(SMWX)/smw.sfc

check-snaps: build/ppu_snap
	@fail=0; for f in $(SMWX)/F*.BIN; do \
	  echo "== $$f"; \
	  build/ppu_snap $$f $(SMWX)/smw.sfc $${f%.BIN}.ppm || fail=1; \
	  magick $${f%.BIN}.ppm $${f%.BIN}.png && rm -f $${f%.BIN}.ppm; \
	done; exit $$fail

clean:
	rm -rf build

# ---- List compositor, the blitter renderer (analysis/list-compositor-smw.md) ---------------------
# F030CT's model and player, with SMW's rules (tools/lc/lc_model.c, LC_SMW=1: LoROM bus, colour math
# that changes no index). Built against F030CT's src/snes_ppu.c: SMW's own copy has no colour math,
# windows or ppu_walk_all. The snapshots are the ones a build with SMWX_SNAP_EVERY=1 and
# SMWX_SNAP_FROM=n writes (consecutive frames, see "How the window was made" in the record).
LC_CT ?= ../ct
LC := build/lc
LC_SNAPS ?= $(SMWX)
LC_ENV := LC_SMW=1 LC_CHOOSER=exact LC_OVERLAY=ring LC_CACHE_KB=64

$(LC)/lc_model: tools/lc/lc_model.c $(LC_CT)/src/snes_ppu.c $(LC_CT)/src/snes_ppu.h | build
	mkdir -p $(LC)
	$(CC) -I$(LC_CT)/src $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tools/lc/lc_model.c $(LC_CT)/src/snes_ppu.c -lm

lc-snaps:
	@ls $(LC_SNAPS)/F*.BIN >/dev/null 2>&1 || { echo "no snapshots in $(LC_SNAPS)"; exit 1; }
	python3 tools/lc/smw_to_ct_snap.py $(LC)/snaps $(LC_SNAPS)/F*.BIN

# Every snapshot through the model (which frames it covers), then the longest run of consecutive
# covered frames through the ring model (hreal), recorded as the stream the player plays.
check-lc-model: $(LC)/lc_model $(SMWX)/smw.sfc lc-snaps
	LC_SMW=1 $(LC)/lc_model census $(SMWX)/smw.sfc $(LC)/snaps/F*.BIN | tee $(LC)/census.txt | tail -1
	LC_SMW=1 $(LC)/lc_model seq $(SMWX)/smw.sfc $(LC)/snaps/F*.BIN | tail -2
	LC_SMW=1 $(LC)/lc_model blitfuzz 50000 1 | tail -1
	python3 tools/lc/lc_longest.py $(LC)/snaps < $(LC)/census.txt > $(LC)/window.txt
	@test -s $(LC)/window.txt || { echo "no frame the model covers"; exit 1; }
	@echo "longest covered run: $$(wc -l < $(LC)/window.txt | tr -d ' ') frames"
	LC_REC=$(LC)/smw.lcp $(LC_ENV) $(LC)/lc_model hreal $(SMWX)/smw.sfc $$(cat $(LC)/window.txt) | tail -6
	LC_SMW=1 $(LC)/lc_model play $(LC)/smw.lcp

# The recorded stream played by falcon/lcplay.s in the patched Hatari: every frame's page window
# against the stream's checksum.
check-lc-play: check-lc-model
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_lcplay.py check $(LC)/smw.lcp

bench-lc-play: check-lc-model
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_lcplay.py bench $(LC)/smw.lcp

# ---- The DSP tile renderer, taken over from F030CT (analysis/list-compositor-dsp-plan.md, step 1) ---
# Host model of the DSP design (tools/dsp_model.c, the planner in src/ppu_dsp_plan.c) against the
# reference, on SMW's snapshots in CT's layout (lc-snaps). LoROM bus.
build/dsp_snap: tools/dsp_snap.c tools/dsp_model.c tools/dsp_model.h src/ppu_dsp_plan.c src/ppu_dsp.h \
  src/m7_plan.c src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/dsp_snap.c tools/dsp_model.c \
	  src/ppu_dsp_plan.c src/snes_ppu.c src/m7_plan.c

build/dsp_rand: tools/dsp_rand.c tools/dsp_model.c tools/dsp_model.h src/ppu_dsp_plan.c src/ppu_dsp.h \
  src/m7_plan.c src/snes_ppu.c src/snes_ppu.h | build
	$(CC) $(CPPFLAGS) -Itools $(CFLAGS) $(LDFLAGS) -o $@ tools/dsp_rand.c tools/dsp_model.c \
	  src/ppu_dsp_plan.c src/snes_ppu.c src/m7_plan.c

check-dsp-model: build/dsp_snap lc-snaps
	-build/dsp_snap $(SMWX)/smw.sfc $(LC)/snaps/F*.BIN > $(LC)/dsp_snap.txt
	@echo "$$(grep -c 'diff 0 words in 0 rows' $(LC)/dsp_snap.txt) of $$(ls $(LC)/snaps/F*.BIN | wc -l | tr -d ' ') snapshots: model equals the reference"
	@echo "$$(grep -c "Falcon's eight planes vs the model: [1-9]" $(LC)/dsp_snap.txt) snapshots where the Falcon's own frame differs (the fast composer has no windows or sub screen)"
	@test "$$(grep -c 'diff [1-9]\|READ OUTSIDE' $(LC)/dsp_snap.txt)" = 0

# The DSP program in its three builds (same source): tile frames, Mode 7 frames, tile frames with
# windows and the sub screen. Booted by falcon/smkx_dspboot.s (no XBIOS Dsp_ExecBoot).
$(SMWX)/ppu.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(SMWX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=0 --define LCB=0 $< $@

$(SMWX)/ppu7.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(SMWX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=1 --define WIN=0 --define LCB=0 $< $@

$(SMWX)/ppuw.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(SMWX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=0 $< $@

# The list compositor's group composer (LCB=1): only the groups a per-row mask names, as list records.
$(SMWX)/ppulc.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(SMWX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=0 --define LCB=1 $< $@

# ... and with the window build in it (frames whose rows have windows masking layers)
$(SMWX)/ppulcw.bin: falcon/dsp/ppu.asm tools/smkx/dsp_asm.py
	mkdir -p $(SMWX)
	python3 -m tools.smkx.dsp_asm --boot --define M7K=0 --define WIN=1 --define LCB=1 $< $@

# PPUTEST: real SMW frames (a chosen set of snapshots in CT's layout), then random ones (flips, 16x16
# tiles, mode 0, windows, the sub screen), each composed by the DSP in Hatari and compared with the
# reference. PPUTEST_FRAMES: title, a masking window, levels with math, the longest scroll window.
PPUTEST_RANDOM ?= 32
PPUTEST_FRAMES ?= $(addprefix $(LC)/snaps/,F00001F4.BIN F00003E8.BIN F00005DC.BIN F00009C4.BIN F0000BB8.BIN \
  F0000FA0.BIN F0001194.BIN F0001388.BIN F00010B9.BIN F0001100.BIN F0001200.BIN F0001250.BIN)

$(SMWX)/ppu_tests.bin: build/dsp_snap build/dsp_rand $(SMWX)/smw.sfc
	$(MAKE) --no-print-directory lc-snaps
	build/dsp_snap $(SMWX)/smw.sfc $(PPUTEST_FRAMES) -pack $(SMWX)/ppu_tests_real.bin
	build/dsp_rand $(PPUTEST_RANDOM) 0x5eed $(SMWX)/ppu_tests_rand.bin > $(SMWX)/ppu_tests_rand.txt
	python3 -c "import sys,struct; a,b=(open(f,'rb').read() for f in sys.argv[1:3]); \
	  open(sys.argv[3],'wb').write(b'PPU2'+struct.pack('>I',struct.unpack('>I',a[4:8])[0]+struct.unpack('>I',b[4:8])[0])+a[8:]+b[8:])" \
	  $(SMWX)/ppu_tests_real.bin $(SMWX)/ppu_tests_rand.bin $@

$(SMWX)/t_pputest.o: src/pputest.c src/ppu_dsp.h src/snes_ppu.h src/smkx_prof.h
	mkdir -p $(SMWX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(SMWX)/PPUTEST.TOS: falcon/pputest.s falcon/smkx_dspboot.s $(SMWX)/ppu.bin $(SMWX)/ppu7.bin $(SMWX)/ppuw.bin \
  $(SMWX)/ppu_tests.bin $(SMWX)/t_pputest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SMWX) -Ifalcon -o $(SMWX)/pputest.o falcon/pputest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(SMWX)/pputest.o $(SMWX)/t_pputest.o

test-ppu: $(SMWX)/PPUTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag pputest --grep PPUTEST --fast --timeout 800
	@grep -a "PPUTEST frames" $(SMWX)/pputest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'

# ---- The orchestrator model and the CPU walker (analysis/list-compositor-dsp-plan.md) ------------------
# tools/lc/lcd_model.c: which groups of the scroll ring each frame writes, from inputs only (VRAM writes, OAM,
# registers, scroll), two pages, exact against the reference; LCD_REC writes the walker's stream.
# falcon/lcwalk.s plays it in Hatari: the CPU only stores, copies and points the Videl at the window.
$(LC)/lcd_model: tools/lc/lcd_model.c tools/lc/lc_model.c src/lc_plan.c src/lc_plan.h src/snes_ppu.c src/snes_ppu.h | build
	mkdir -p $(LC)
	$(CC) -Isrc $(CPPFLAGS) $(CFLAGS) -Wno-unused $(LDFLAGS) -o $@ tools/lc/lcd_model.c src/lc_plan.c src/snes_ppu.c -lm

check-lcd: $(LC)/lcd_model check-lc-model
	LCD_REC=$(LC)/smw.lcw $(LC)/lcd_model $(SMWX)/smw.sfc $$(cat $(LC)/window.txt) | tail -1
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_lcwalk.py check $(LC)/smw.lcw

bench-lcw: check-lcd
	python3 scripts/run_lcwalk.py bench $(LC)/smw.lcw

# LCDTEST: the LC build on real SMW frames with masks of several patterns; each frame's list against the model.
$(SMWX)/lcd_tests.bin: build/dsp_snap $(SMWX)/smw.sfc
	$(MAKE) --no-print-directory lc-snaps
	-build/dsp_snap $(SMWX)/smw.sfc $(PPUTEST_FRAMES) -packlc $@ > /dev/null

$(SMWX)/t_lcdtest.o: src/lcdtest.c src/ppu_dsp.h src/smkx_prof.h
	mkdir -p $(SMWX)
	$(CROSS_CC) $(CROSS_CFLAGS) -c -o $@ $<

$(SMWX)/LCDTEST.TOS: falcon/lcdtest.s falcon/smkx_dspboot.s $(SMWX)/ppulc.bin $(SMWX)/ppulcw.bin $(SMWX)/lcd_tests.bin $(SMWX)/t_lcdtest.o
	$(VASM) -Felf -m68030 -quiet -nowarn=62 -I$(SMWX) -Ifalcon -o $(SMWX)/lcdtest.o falcon/lcdtest.s
	$(VLINK) -b ataritos -tos-flags 1 -o $@ $(SMWX)/lcdtest.o $(SMWX)/t_lcdtest.o

test-lcd: $(SMWX)/LCDTEST.TOS
	@test -x $(HATARI) || { echo "Patched F030Arcade Hatari missing: $(HATARI)"; exit 1; }
	python3 scripts/run_smkx.py --program $< --tag lcdtest --grep LCDTEST --fast --timeout 800
	@grep -a "LCDTEST frames" $(SMWX)/lcdtest-stderr.log | tail -1 | tee /dev/stderr | grep -q 'words $$00000000'
