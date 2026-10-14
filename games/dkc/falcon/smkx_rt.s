; SMKX runtime: the Falcon side of the translated Super Mario Kart program.
;
; The translated code (build/<game>x/smk_code_NNN.s) runs in supervisor mode with
; the register mapping documented in tools/smkx/emit.py. This file provides:
;   - startup: ROM word view, bank tables, entry into the reset procedure
;   - the VBL hook standing in for the SNES NMI (and the frame's IRQ)
;   - memory helpers for accesses whose target is decided at runtime
;   - the SNES hardware registers the program touches: PPU ports, DMA,
;     multiply/divide, the APU handshake (echo model, no sound yet) and a
;     DSP-1 stub (always ready, reads zero)
;   - traps, which print the 65816 address via NatFeats and quit to TOS
;
; Emulator runs: F030Arcade's patched Hatari with --natfeats true.

	xref	smkx_reset,smkx_nmi,smkx_irq,smkx_dispatch,smkx_dispatch_count
	xref	smwx_input
	xref	smkx_video_init,smkx_render,smkx_c2p_ticks,smkx_ppu_stat
	xref	smkx_lc_active,smkx_lc_window,smkx_lc_stat,lc_prof,smkx_vid_stat
	xref	smkx_render_poll,smkx_render_start,smkx_render_present
	xref	smkx_m7_init,smkx_m7_on,smkx_m7_rows,smkx_m7_pages,smkx_m7_timeouts
	xdef	rt_m7_dirty,rt_plan_dirty,rt_m7_record,rt_m7_record_done
	xdef	rt_snapdue
	xdef	smkx_vid_set_window,smkx_lc_cpu,smkx_lc_blit,smkx_lc_blit_busy,smkx_lc_dcache_clear
	xdef	rt_dsp_boot,m7_image,m7_image_bytes,ppu_image,ppu_image_bytes,rt_m7_route
	xdef	ppu7_image,ppu7_image_bytes,ppuw_image,ppuw_image_bytes,ppulc_image,ppulc_image_bytes,ppulcw_image,ppulcw_image_bytes
	xdef	rt_clut,rt_clut_ready,rt_vram_dirty
	xdef	rt_vram,rt_cgram,rt_oam,rt_sram,rt_io_start,rt_irqlog,rt_nirqlog
	xdef	rt_cgram_start,rt_noamseg,rt_oamseg
	xdef	rt_bgofs_start,rt_m7_start,rt_m7ofs_start

	xdef	rt_rb,rt_rw,rt_wb,rt_ww,rt_iorb,rt_iorw,rt_iowb,rt_ioww
	xdef	rt_setdbr,rt_bit8,rt_bit16,rt_mvn,rt_mvp,rt_sei,rt_cli,rt_vf
	xdef	rt_dispatch,rt_probe,rt_trap,rt_unsupported,rt_badtable,rt_badexit,rt_noreturn
	xdef	smkx_wram,smkx_rom
	xdef	rt_dp,rt_sp_base,rt_wai

	ifnd	SMKX_NMI_AT_WAI
SMKX_NMI_AT_WAI	equ	1		; DKC: the NMI runs at WAI (rt_wai)
	endif
	ifnd	SMKX_DUMP_AT
SMKX_DUMP_AT	equ	0		; write Dnnnnnnn.BIN before game frame n (0 = off),
	endif
	ifnd	SMKX_DUMP_EVERY
SMKX_DUMP_EVERY	equ	1		; then every k game frames,
	endif
	ifnd	SMKX_DUMP_PC
SMKX_DUMP_PC	equ	0		; or: dump at the n-th execution of this probed
	endif				; 65816 address (tools/smkx/probes.txt)
	ifnd	SMKX_DUMP_HIT
SMKX_DUMP_HIT	equ	1
	endif
	ifnd	SMKX_DUMP_COUNT
SMKX_DUMP_COUNT	equ	1		; c dumps in all; then quit
	endif
	ifnd	SMKX_FRAMES
SMKX_FRAMES	equ	0		; quit after this many frames (0 = never)
	endif

ROM_SIZE	equ	$400000	; Donkey Kong Country
SAMPLES		equ	20000
IRQLOG_MAX	equ	256
IRQ_CHAIN_MAX	equ	8		; V-IRQs per frame
OAMSEG_MAX	equ	IRQ_CHAIN_MAX+1	; snes_ppu.h: PPU_OAMSEG_MAX
OAMSEG_BYTES	equ	4+544
SCREEN_BYTES	equ	256*224		; 8 planes: one byte per pixel (smkx_video.s)
	ifnd	SMKX_RENDER
SMKX_RENDER	equ	1		; 0: no rendering at all (timing and reference runs); otherwise the render context (rt_render_thread)
					; asks for a composition at every VBL and works in the game's idle time: that is the throttle
	endif
	ifnd	SMKX_SNAP_EVERY
SMKX_SNAP_EVERY	equ	0	; also write Fnnnnnnn.BIN every Nth frame (0 = off)
	endif
	ifnd	SMKX_SNAP_FROM
SMKX_SNAP_FROM	equ	0	; ... only from this frame on (a window of consecutive frames)
	endif
	ifnd	SMKX_DIAG
SMKX_DIAG	equ	1	; 0: release build without per-call diagnostics
	endif
DCACHE_SIZE	equ	1024		; dispatch cache slots
LOG_BYTES	equ	196608		; rt_print's text without NatFeats (DKCXLOG.TXT)
LOG_RESERVE	equ	8192		; ... of which the last bytes are for a crash report
VBL_VECTOR	equ	$70
ACIA_VECTOR	equ	$118		; MFP GPIP4: keyboard and MIDI ACIAs

; NOTE_CALLER: remember the translated call site for status lines and the
; profile (diagnostics builds only). From F030SMK.
NOTE_CALLER	macro
	ifne	SMKX_DIAG
	bsr	rt_note_caller
	endif
	endm

; M7DIRTY: after a VRAM byte write at byte offset d4.l, mark its 256-word
; page for the fast composer's tile-row cache (rt_vram_dirty) and for the
; DSP programs' page caches (rt_m7_dirty), all 128 pages. Clobbers d4, a0.
M7DIRTY	macro
	lsr.l	#8,d4
	lsr.l	#1,d4
	lea	rt_vram_dirty,a0
	st	(a0,d4.w)
	lea	rt_m7_dirty,a0
	st	(a0,d4.w)
	lea	rt_plan_dirty,a0	; the planner's own (it clears what it used)
	st	(a0,d4.w)
	endm


	include	"info.i"		; tools/port_info.py: info texts, the player's files

	section	text,code

; ---------------------------------------------------------------------------
; Startup

start:
	lea	rt_ustack_top,sp
	bsr	info_start		; the info screen; reads and checks the player's files
	bsr	rt_m7_boot		; XBIOS DSP and sound calls: user mode
	clr.l	-(sp)
	move.w	#$20,-(sp)		; Super(0)
	trap	#1
	addq.l	#6,sp
	move.l	d0,rt_old_ssp
	lea	rt_sstack_top,sp

	bsr	rt_nf_init
	move.l	VBL_VECTOR.w,rt_old_vbl		; saved here: rt_exit can run from
	; (any point below, a crash included)
	move.l	ACIA_VECTOR.w,rt_old_acia
	bsr	rt_catch_exceptions
	lea	msg_start,a0
	bsr	rt_print
	lea	msg_wram_at,a0		; host addresses, for debugger breakpoints
	bsr	rt_print
	move.l	#smkx_wram,d7
	bsr	rt_print_hex8
	lea	msg_text_at,a0
	bsr	rt_print
	move.l	#start,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print

	bsr	rt_build_banks
	bsr	rt_build_pages
	jsr	smkx_video_init		; C: palette state
	bsr	rt_video_on
	lea	msg_m7off,a0		; the DSP route; the programs boot when a
	tst.b	rt_m7_route		; scene first needs one (smkx_video.c)
	beq.s	.m7msg
	lea	msg_m7on,a0
.m7msg:	bsr	rt_print

	move.w	#$2700,sr
	move.l	#rt_vbl,VBL_VECTOR.w
	move.l	#rt_acia,ACIA_VECTOR.w
	move.l	#smwx_input,rt_script_at
	move.w	#$2300,sr

	; SNES reset state: emulation mode, S = $01FF, DBR = 0, registers 0.
	moveq	#0,d0
	moveq	#0,d1
	moveq	#0,d2
	lea	smkx_wram,a5
	lea	($1FF,a5),a6
	moveq	#0,d7
	bsr	rt_setdbr
	move.w	#$0000,ccr
	clr.w	rt_dp
	move.l	sp,d7			; the translated program's 68k stack base:
	subq.l	#4,d7			; TCS/TXS return here (smkx_reset's frame)
	move.l	d7,rt_sp_base
	move.l	d0,-(sp)
	bsr	rt_now
	move.l	d0,rt_t_start
	move.l	(sp)+,d0
	moveq	#0,d7
	jsr	smkx_reset
	lea	msg_reset_returned,a0
	bsr	rt_print
	moveq	#2,d7
	bra	rt_exit

; rt_exit: d7.w = TOS return code.
rt_exit:
	move.w	#$2700,sr
	move.l	rt_old_vbl,VBL_VECTOR.w
	move.l	rt_old_acia,ACIA_VECTOR.w
	bsr	rt_release_exceptions
	bsr	rt_m7_stop
	bsr	rt_video_off
	move.w	d7,rt_exit_code
	lea	rt_sstack_top,sp
	move.w	#$2300,sr
	move.l	rt_old_ssp,-(sp)
	move.w	#$20,-(sp)		; back to user mode
	trap	#1
	addq.l	#6,sp
	lea	rt_ustack_top,sp
	bsr	rt_m7_release
	bsr	rt_log_flush
	move.w	rt_exit_code,-(sp)
	move.w	#$4c,-(sp)		; Pterm
	trap	#1

; Bank tables. lotab[b] is the host address of b:$0000 and hitab[b] of
; b:$8000; banksys[b] marks the system banks whose low half is split into
; WRAM, I/O and expansion space.
rt_build_banks:
	lea	rt_lotab,a0
	lea	rt_hitab,a1
	lea	rt_banksys,a2
	moveq	#0,d7
.bank:	move.l	d7,d6
	andi.w	#$7f,d6
	cmpi.w	#$7e,d7
	beq.s	.w0
	cmpi.w	#$7f,d7
	beq.s	.w1
	cmpi.w	#$40,d6
	bcs.s	.sys
	; linear ROM bank ($40-$7D, $C0-$FF)
	sf	(a2)+
	move.l	d7,d6
	andi.l	#$3f,d6
	swap	d6
	andi.l	#ROM_SIZE-1,d6
	add.l	#smkx_rom,d6
	move.l	d6,(a0)+
	add.l	#$8000,d6
	move.l	d6,(a1)+
	bra.s	.next
.sys:	st	(a2)+
	move.l	#smkx_wram,(a0)+
	move.l	d7,d6
	andi.l	#$3f,d6
	swap	d6
	ori.w	#$8000,d6
	andi.l	#ROM_SIZE-1,d6
	add.l	#smkx_rom,d6
	move.l	d6,(a1)+
	bra.s	.next
.w0:	sf	(a2)+
	move.l	#smkx_wram,(a0)+
	move.l	#smkx_wram+$8000,(a1)+
	bra.s	.next
.w1:	sf	(a2)+
	move.l	#smkx_wram+$10000,(a0)+
	move.l	#smkx_wram+$18000,(a1)+
.next:	addq.w	#1,d7
	cmpi.w	#256,d7
	bne	.bank
	rts

; ---------------------------------------------------------------------------
; Frame: the Falcon VBL stands in for the SNES vertical blank.

rt_vbl:
	move.l	2(sp),rt_last_pc		; the interrupted PC, for rt_status
	tst.b	rt_clut_ready		; the palette of the frame that shows from now on
	beq.s	.noclut
	movem.l	d0/a0-a1,-(sp)
	lea	rt_clut,a0
	lea	$ffff9800.w,a1
	move.w	#255,d0
.clut:	move.l	(a0)+,(a1)+
	dbra	d0,.clut
	sf	rt_clut_ready
	movem.l	(sp)+,d0/a0-a1
.noclut:
	bsr	rt_sample
	addq.l	#1,rt_vblcount
	tst.b	rt_in_frame
	bne.s	.busy
	st	rt_in_frame
	movem.l	d0-d7/a0-a6,-(sp)
	bsr	rt_now
	move.l	d0,-(sp)
	move.w	#$2300,sr		; let the MFP (keyboard, timers) run
	bsr	rt_frame
	move.w	#$2700,sr
	bsr	rt_now
	sub.l	(sp)+,d0
	add.l	d0,rt_t_vbl
	tst.b	rt_in_render		; over the render context: back to the
	bne.s	.suspend		; game, which takes the NMI at its WAI
	movem.l	(sp)+,d0-d7/a0-a6
	sf	rt_in_frame
.chain:	move.l	rt_old_vbl,-(sp)
	rts
.suspend:
	move.l	sp,rt_rctx_sp		; the render context as interrupted
	sf	rt_in_render
	sf	rt_in_frame
	bsr	rt_chain_tos
	bra	rt_to_main
.busy:	addq.l	#1,rt_stuck		; a frame handler is still running
	move.l	rt_stuck,-(sp)
	andi.l	#127,(sp)+
	bne.s	.chain
	move.l	a0,-(sp)
	lea	msg_busy,a0
	bsr	rt_print
	move.l	(sp)+,a0
	bsr	rt_status
	bra.s	.chain

;---------------------------------------------------------------------------
; Render work in the game's idle time (from F030CT, simpler here: DKC's
; VBL never runs game code, the game takes its NMI at WAI). Composing a
; frame (planning and uploads for the DSP renderer, or the CPU composer)
; takes the 68030 10 ms to a second; done in the VBL it cost game frames.
; The VBL now only shows a DSP frame that has arrived and asks for the next
; composition (rt_rstate 1); the work runs in its own context
; (rt_render_thread on rt_rstack), started or resumed from rt_wai while the
; game waits for its NMI. A VBL over the render context runs its frame and
; returns to the game in rt_wai, which takes the NMI and resumes the work
; at its next WAI. While work is pending the frame's inputs stay as they
; were asked for (no frame-start copy); VRAM stays live, and the planner
; has its own dirty flags (rt_plan_dirty). Snapshots and the frame limit
; wait until no work is pending.

; rt_idle_render: from rt_wai; returns when a VBL hands the CPU back or the
; work is done. All registers kept.
rt_idle_render:
	move.w	sr,-(sp)
	ori.w	#$0700,sr
	movem.l	d0-d7/a0-a6,-(sp)
	move.l	sp,rt_main_sp		; the game: back by MOVEM, MOVE SR, RTS
; At IPL 7, the game saved at rt_main_sp: start or resume the work.
rt_to_render:
	tst.b	rt_rstate
	beq.s	rt_to_main		; finished meanwhile
	st	rt_in_render
	cmpi.b	#2,rt_rstate
	beq.s	.resume
	move.b	#2,rt_rstate
	lea	rt_rstack_top,sp
	move.w	#$2300,sr
	bra.s	rt_render_thread
.resume:
	movea.l	rt_rctx_sp,sp		; as the VBL left it: registers, then
	movem.l	(sp)+,d0-d7/a0-a6	; the exception frame
	rte

; At IPL 7: back into the game in rt_wai.
rt_to_main:
	movea.l	rt_main_sp,sp
	movem.l	(sp)+,d0-d7/a0-a6
	move.w	(sp)+,sr
	rts

rt_render_thread:
	bsr	rt_now
	move.l	d0,-(sp)
	move.l	rt_back,-(sp)
	jsr	smkx_render_start	; C: 1 when the frame is in rt_back
	addq.l	#4,sp
	move.l	d0,d7
	bsr	rt_now
	sub.l	(sp)+,d0
	move.w	#$2700,sr
	add.l	d0,rt_t_render		; (includes VBLs that preempted it)
	tst.l	d7
	beq.s	.done
	bsr	rt_flip
.done:	clr.b	rt_rstate
	sf	rt_in_render
	bra.s	rt_to_main

; The VBL chain (TOS) for this VBL, called like the exception it is: a
; format 0 frame whose RTE comes back here.
rt_chain_tos:
	move.w	#$0070,-(sp)
	pea	.back(pc)
	move.w	sr,-(sp)
	move.l	rt_old_vbl,-(sp)
	rts
.back:	rts

; Show rt_back: the palette the frame was composed with (C:
; smkx_render_present, loaded at the next VBL), then the Videl base.
rt_flip:
	movem.l	d0-d1/a0-a1,-(sp)
	jsr	smkx_render_present
	movem.l	(sp)+,d0-d1/a0-a1
rt_flip_base:
	addq.l	#1,rt_renders
	movem.l	d0-d1,-(sp)
	move.l	rt_back,d0
	move.l	rt_screen,rt_back
	move.l	d0,rt_screen
	bsr	vid_set_base
	clr.b	$ffff8265.w		; no HSCROLL and no line offset for a plain screen
	clr.w	$ffff820e.w
	movem.l	(sp)+,d0-d1
	rts

; In the VBL: the DSP renderer's frame shown when it is there; asks for the
; next composition when nothing is in flight. rt_rstate is 0 here.
rt_render_poll:
	tst.l	rt_screen
	beq.s	.x
	movem.l	d0-d7/a0-a6,-(sp)
	jsr	smkx_render_poll	; -1 nothing in flight, 0 not there, 1 there
	tst.l	d0
	beq.s	.busy
	bmi.s	.ask
	bsr	rt_flip
.ask:	move.b	#1,rt_rstate		; this frame's inputs, from now kept
.busy:	movem.l	(sp)+,d0-d7/a0-a6
.x:	rts

; Profile sample: interrupted PC and the last runtime call site, as text
; offsets. A ring of the latest SAMPLES, printed by rt_dump_samples when the
; frame limit is reached.
rt_sample:
	movem.l	d7/a0,-(sp)
	move.l	rt_sampidx,d7
	addq.l	#1,rt_sampidx
	cmpi.l	#SAMPLES-1,d7
	bcs.s	.in
	clr.l	rt_sampidx
.in:	lsl.l	#3,d7
	lea	rt_samples,a0
	adda.l	d7,a0
	move.l	rt_last_pc,d7
	sub.l	#start,d7
	move.l	d7,(a0)+
	move.l	rt_io_caller,d7
	sub.l	#start,d7
	move.l	d7,(a0)
	cmpi.l	#SAMPLES,rt_nsamples
	bcc.s	.full
	addq.l	#1,rt_nsamples
.full:	movem.l	(sp)+,d7/a0
	rts

rt_dump_samples:
	lea	msg_samples,a0
	bsr	rt_print
	lea	rt_samples,a1
	move.l	rt_nsamples,d5
	bra.s	.next
.one:	move.l	(a1)+,d7
	bsr	rt_print_hex8
	lea	msg_sp,a0
	bsr	rt_print
	move.l	(a1)+,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print
.next:	subq.l	#1,d5
	bpl.s	.one
	rts

rt_frame:
	addq.l	#1,rt_frames
	tst.b	rt_quit			; Esc: leave cleanly
	beq.s	.run
	lea	msg_quit,a0
	bsr	rt_print
	bsr	rt_status
	moveq	#0,d7
	bra	rt_exit
.run:
	ifeq	SMKX_NMI_AT_WAI
	movea.l	rt_script_at,a0		; scripted input (DKCX_INPUT): frame, pad
	move.l	(a0),d7
	bmi.s	.pad
	cmp.l	rt_frames,d7
	bhi.s	.pad
	move.w	6(a0),rt_script_pad
	addq.l	#8,rt_script_at
	endif
.pad:	move.w	rt_pad,d7		; auto-joypad read: $4218/$4219 latch here
	or.w	rt_script_pad,d7
	move.b	d7,rt_io+$2218
	lsr.w	#8,d7
	move.b	d7,rt_io+$2219
	move.l	rt_frames,d7
	divu.w	#50,d7
	swap	d7
	tst.w	d7
	bne.s	.nolog
	bsr	rt_status
.nolog:
	tst.b	rt_rstate		; render work pending: its inputs stay
	bne.s	.kept
	bsr	rt_frame_start
.kept:
	; V-count IRQs, when enabled and unmasked, run before the NMI, in line
	; order: a handler that sets VTIME to a later line gets called again
	; there (SMK chains its screen splits). Their PPU writes are logged
	; with the line they start at.
	clr.w	rt_irqline
	moveq	#IRQ_CHAIN_MAX-1,d7
	move.w	d7,rt_irqleft
	ifne	SMKX_NMI_AT_WAI
	bra	.noirq			; DKC's IRQ handler is SEI : RTI (it only ends WAI)
	endif
.irq:	move.b	rt_nmitimen,d7
	andi.b	#$30,d7
	beq	.noirq
	tst.b	rt_iflag
	bne	.noirq
	moveq	#0,d7
	move.b	rt_io+$220A,d7		; VTIME, 9 bits
	andi.w	#1,d7
	lsl.w	#8,d7
	move.b	rt_io+$2209,d7
	addq.w	#1,d7
	cmp.w	rt_irqline,d7
	bls	.noirq			; not later than the last one: next frame
	cmpi.w	#262,d7
	bcc	.noirq
	move.w	d7,rt_irqline
	st	rt_timeup
	addq.l	#1,rt_irqs
	st	rt_logging
	bsr	rt_now
	move.l	d0,rt_t0
	andi.l	#$ffff,d1		; X and Y are 16-bit: the translated
	andi.l	#$ffff,d2		; code indexes with d1.l and d2.l
	subq.l	#4,a6
	move.b	rt_vf,-(sp)		; the interrupted code's 65816 V (RTI)
	jsr	smkx_irq
	move.b	(sp)+,rt_vf
	bsr	rt_now
	sub.l	rt_t0,d0
	add.l	d0,rt_t_irq
	sf	rt_logging
	bsr	rt_oam_segment
	subq.w	#1,rt_irqleft
	bpl	.irq
.noirq:
	move.l	#SMKX_FRAMES,d7
	beq.s	.render
	cmp.l	rt_frames,d7
	bhi.s	.render
	tst.b	rt_rstate		; render work pending: stop after it
	bne.s	.render
	bsr	rt_render_sync
	lea	snap_name,a0
	move.l	#'SNAP',(a0)+
	move.l	#'.BIN',(a0)+
	clr.b	(a0)
	lea	msg_frames,a0
	bsr	rt_print
	bsr	rt_status
	bsr	rt_dump_samples
	bsr	rt_write_snapshot
	moveq	#0,d7
	bra	rt_exit
.render:
	ifne	SMKX_SNAP_EVERY
	move.l	rt_frames,d7		; a snapshot frame: composed at once, as
	ifne	SMKX_SNAP_FROM
	cmp.l	#SMKX_SNAP_FROM,d7
	blo.s	.nosnap
	endif
	divu.w	#SMKX_SNAP_EVERY,d7	; soon as no work is pending
	swap	d7
	tst.w	d7
	bne.s	.nosnap
	st	rt_snapdue
.nosnap:
	endif
	ifeq	SMKX_RENDER
	bra.s	.nmi
	endif
	tst.b	rt_rstate		; render work pending or running
	bne.s	.nmi
	ifne	SMKX_SNAP_EVERY
	tst.b	rt_snapdue
	beq.s	.async
	sf	rt_snapdue
	st	rt_snapnow
	bsr	rt_render_sync
	sf	rt_snapnow
	bra.s	.nmi
.async:
	endif
	bsr	rt_render_poll
.nmi:	st	rt_rdnmi
	tst.b	rt_nmitimen
	bpl.s	.nonmi
	addq.l	#1,rt_nmis
	ifne	SMKX_NMI_AT_WAI
	st	rt_nmi_pending		; rt_wai takes it
	rts
	endif
	bsr	rt_now
	move.l	d0,rt_t0
	andi.l	#$ffff,d1
	andi.l	#$ffff,d2
	subq.l	#4,a6
	move.b	rt_vf,-(sp)		; the interrupted code's 65816 V (RTI)
	jsr	smkx_nmi
	move.b	(sp)+,rt_vf
	bsr	rt_now
	sub.l	rt_t0,d0
	add.l	d0,rt_t_nmi
.nonmi:
	rts

; WAI: DKC runs its whole game in the NMI. The main program only waits
; (`WAI : BRA *`), and the NMI (CODE_80A96C) saves A, X and Y, forces blank
; and jumps through $1C: to CODE_80A980 (INIDISP from its RAM copy, then
; RTI) while a frame is still running, or to the frame handler, which
; resets S (TCS/TXS, rt_sp_base) and ends in the next WAI. The VBL only
; flags the NMI; it is taken here, from the main program, in the widths
; the frame handlers are analysed in (16-bit A and index, D = 0).
; A lag handler returns with every register and the CCR kept.
NMI_LAG		equ	$A980		; CODE_80A980
NMI_RETURN	equ	$A98A		; CODE_80A98A
INIDISP_COPY	equ	$051A		; !RAM_DKC1_Global_ScreenDisplayRegister
rt_wai:
	move.w	ccr,-(sp)
	move.l	d0,-(sp)		; clock the wait, minus the VBL time in it
	bsr	rt_now
	move.l	d0,rt_wait_t0
	move.l	rt_t_vbl,rt_wait_vbl0
	move.l	(sp)+,d0
.wait:	tst.b	rt_nmi_pending
	bne.s	.nmi
	tst.b	rt_rstate		; render work: done here, in the game's
	beq.s	.stop			; idle time, until a VBL hands back
	bsr	rt_idle_render
	bra.s	.wait
.stop:	stop	#$2300			; until the next interrupt
	bra.s	.wait
.nmi:	sf	rt_nmi_pending
	move.l	d0,-(sp)
	bsr	rt_now
	sub.l	rt_wait_t0,d0
	sub.l	rt_t_vbl,d0
	add.l	rt_wait_vbl0,d0
	add.l	d0,rt_t_main		; DKC: time waiting at WAI
	move.l	(sp)+,d0
	cmpi.l	#16,rt_nmi_logged	; the first NMIs: $1C and the WAI site
	bcc.s	.nolog
	addq.l	#1,rt_nmi_logged
	move.l	a0,-(sp)
	lea	msg_nmilog,a0
	bsr	rt_print
	moveq	#0,d7
	move.w	$1c(a5),d7
	ror.w	#8,d7
	bsr	rt_print_hex8
	lea	msg_from,a0
	bsr	rt_print
	move.l	6(sp),d7
	sub.l	#start,d7
	bsr	rt_print_hex8
	lea	msg_dbr,a0
	bsr	rt_print
	move.l	d3,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print
	move.l	(sp)+,a0
.nolog:
	sf	rt_rdnmi		; LDA $4210
	btst	#7,rt_io+$100		; VBlank starts: unless in forced blank,
	bne.s	.oamkeep		; the PPU reloads the OAM address from
	moveq	#0,d7			; $2102/$2103 (DKC's OAM DMA at $808C6F
	move.b	rt_io+$103,d7		; relies on it and never sets $2102)
	andi.w	#1,d7
	lsl.w	#8,d7
	move.b	rt_io+$102,d7
	add.w	d7,d7
	move.w	d7,rt_oamadd
.oamkeep:
	move.b	#$8f,rt_io+$100		; forced blank
	moveq	#0,d7
	move.w	$1c(a5),d7
	ror.w	#8,d7
	cmpi.w	#NMI_LAG,d7
	beq.s	.lag
	cmpi.w	#NMI_RETURN,d7
	beq.s	.ret
	addq.l	#1,rt_nmi_frames
	addq.l	#1,rt_n_main
	bsr	rt_game_frame_input
	ifne	SMKX_DUMP_AT
	move.l	rt_dump_next,d6
	bne.s	.dnext
	move.l	#SMKX_DUMP_AT,d6
	move.l	d6,rt_dump_next
.dnext:	cmp.l	rt_nmi_frames,d6
	bne.s	.nodump
	bsr	rt_write_dump
.nodump:
	endif
	ori.l	#$800000,d7
	movea.l	d7,a0
	moveq	#0,d6			; m16 x16 binary
	bsr	rt_dispatch
	move.b	#$8f,d0			; C = B:$8F after LDA #$8F
	andi.l	#$ffff,d1
	andi.l	#$ffff,d2
	lea	-10(a6),a6		; interrupt frame, then A, X, Y
	movea.l	rt_sp_base,sp
	move.w	#$0008,ccr		; N from LDA #$8F
	jmp	(a0)
.lag:	move.b	INIDISP_COPY(a5),rt_io+$100
.ret:	move.w	(sp)+,ccr
	rts

; DKC: scripted input is keyed by game frames (frame handlers started,
; rt_nmi_frames), so a run and tools/smkx/ref65816.py see the same pad at
; the same game frame however long rendering takes. The pad latches here,
; as the frame handler starts. Preserves all registers.
rt_game_frame_input:
	movem.l	d7/a0,-(sp)
	movea.l	rt_script_at,a0
.next:	move.l	(a0),d7
	bmi.s	.pad
	cmp.l	rt_nmi_frames,d7
	bhi.s	.pad
	move.w	6(a0),rt_script_pad
	addq.l	#8,a0
	move.l	a0,rt_script_at
	bra.s	.next
.pad:	move.w	rt_pad,d7
	or.w	rt_script_pad,d7
	move.b	d7,rt_io+$2218
	lsr.w	#8,d7
	move.b	d7,rt_io+$2219
	movem.l	(sp)+,d7/a0
	rts

	ifne	SMKX_DUMP_AT|SMKX_DUMP_PC
; Dnnnnnnn.BIN at a game-frame boundary (before frame handler n starts;
; n in hex), for tools/smkx/refcmp.py: "DKCXDUMP", the game frame (long),
; WRAM, VRAM, CGRAM, OAM, the I/O shadow ($2000-$7FFF), SRAM. After
; SMKX_DUMP_COUNT dumps the run ends.
rt_write_dump:
	move.l	d7,-(sp)		; the frame handler's address
	move.l	rt_nmi_frames,d7
	bsr.s	rt_write_dump_as
	move.l	(sp)+,d7
	rts
; d7 = the number the dump carries (bit 31: an execution count, not a frame)
rt_write_dump_as:
	movem.l	d0-d7/a0-a6,-(sp)
	move.l	d7,dump_frame
	lea	dump_name+8,a0		; D + seven hex digits of the game frame
	moveq	#6,d6
.dig:	move.b	d7,d5
	andi.b	#$f,d5
	addi.b	#'0',d5
	cmpi.b	#'9',d5
	bls.s	.put
	addq.b	#7,d5
.put:	move.b	d5,-(a0)
	lsr.l	#4,d7
	dbra	d6,.dig
	addi.l	#SMKX_DUMP_EVERY,rt_dump_next
	addq.l	#1,rt_dumps
	clr.w	-(sp)
	pea	dump_name
	move.w	#$3c,-(sp)		; Fcreate
	trap	#1
	addq.l	#8,sp
	tst.l	d0
	bmi	.fail
	move.w	d0,d7
	lea	dump_blocks,a3
.blk:	move.l	(a3)+,d6
	beq.s	.close
	move.l	(a3)+,a4
	move.l	a4,-(sp)
	move.l	d6,-(sp)
	move.w	d7,-(sp)
	move.w	#$40,-(sp)		; Fwrite
	trap	#1
	lea	12(sp),sp
	bra.s	.blk
.close:	move.w	d7,-(sp)
	move.w	#$3e,-(sp)		; Fclose
	trap	#1
	addq.l	#4,sp
	lea	msg_dump,a0
	bsr	rt_print
.fail:	movem.l	(sp)+,d0-d7/a0-a6
	cmpi.l	#SMKX_DUMP_COUNT,rt_dumps
	bcs.s	.more
	bsr	rt_status
	moveq	#0,d7
	bra	rt_exit
.more:	rts
	endif

; The state the frame starts from: the register shadows and the latched
; scroll and Mode 7 values as the previous NMI left them.
rt_frame_start:
	movem.l	d0/a0-a1,-(sp)
	; only what the renderer reads (from F030SMK): PPU $2100-$21FF,
	; $4200-$421F and the DMA/HDMA channels $4300-$437F
	lea	rt_io+$100,a0
	lea	rt_io_start+$100,a1
	moveq	#$100/4-1,d0
.cp:	move.l	(a0)+,(a1)+
	dbra	d0,.cp
	lea	rt_io+$2200,a0
	lea	rt_io_start+$2200,a1
	moveq	#$20/4-1,d0
.cp1:	move.l	(a0)+,(a1)+
	dbra	d0,.cp1
	lea	rt_io+$2300,a0
	lea	rt_io_start+$2300,a1
	moveq	#$80/4-1,d0
.cp4:	move.l	(a0)+,(a1)+
	dbra	d0,.cp4
	lea	rt_bgofs,a0
	lea	rt_bgofs_start,a1
	moveq	#(16+12+4)/4-1,d0	; bgofs, m7 and m7ofs are contiguous
.cp2:	move.l	(a0)+,(a1)+
	dbra	d0,.cp2
	clr.w	rt_nirqlog
	lea	rt_cgram,a0		; the renderer starts from these
	lea	rt_cgram_start,a1
	move.w	#512/4-1,d0
.cp3:	move.l	(a0)+,(a1)+
	dbra	d0,.cp3
	clr.l	rt_noamseg
	clr.w	rt_irqline
	movem.l	(sp)+,d0/a0-a1
	bsr	rt_oam_segment		; OAM from line 0
	rts

; rt_oam_segment: OAM as it is now, used from rt_irqline on.
rt_oam_segment:
	movem.l	d0/a0-a1,-(sp)
	move.l	rt_noamseg,d0
	cmpi.l	#OAMSEG_MAX,d0
	bcc.s	.x
	mulu.w	#OAMSEG_BYTES,d0
	lea	rt_oamseg,a1
	adda.l	d0,a1
	move.w	rt_irqline,(a1)+
	clr.w	(a1)+
	lea	rt_oam,a0
	move.w	#544/4-1,d0
.cp:	move.l	(a0)+,(a1)+
	dbra	d0,.cp
	addq.l	#1,rt_noamseg
.x:	movem.l	(sp)+,d0/a0-a1
	rts

; Compose the frame into the back buffer (C: smkx_render), then show it:
; the Videl takes the new base at its next frame. The DSP renderer works
; on while the game runs: smkx_render returns 0 until its frame is in the
; back buffer. rt_render_sync waits for it (snapshots: the inputs and the
; planes are then the same frame's; from F030CT).
rt_render_sync:
	st	rt_sync
rt_render:
	tst.l	rt_screen
	beq	.no
	movem.l	d0-d7/a0-a6,-(sp)
	move.l	$4ba.w,-(sp)		; _hz_200: timer C runs above the VBL
	bsr	rt_now
	move.l	d0,-(sp)
	moveq	#0,d0
	move.b	rt_sync,d0
	move.l	d0,-(sp)
	move.l	rt_back,-(sp)
	jsr	smkx_render
	addq.l	#8,sp
	sf	rt_sync
	move.l	d0,d1			; 0: nothing to show (yet)
	bsr	rt_now
	sub.l	(sp)+,d0
	add.l	d0,rt_t_render
	move.l	$4ba.w,d0
	sub.l	(sp)+,d0
	add.l	d0,rt_render_ticks
	tst.l	d1
	beq	.blank
	cmpi.l	#2,d1			; 2: the list compositor's ring window is on the screen (its palette is set)
	beq.s	.snapc
	bsr	rt_flip_base		; smkx_render set the palette itself
.snapc:
	ifne	SMKX_SNAP_EVERY
	tst.b	rt_snapnow
	beq.s	.nosnap
	move.l	rt_frames,d7		; name: F + seven hex digits of the frame
	lea	snap_name+8,a0
	move.l	#'.BIN',(a0)
	clr.b	4(a0)
	moveq	#6,d6
.dig:	move.b	d7,d5
	andi.b	#$f,d5
	addi.b	#'0',d5
	cmpi.b	#'9',d5
	bls.s	.put
	addq.b	#7,d5
.put:	move.b	d5,-(a0)
	lsr.l	#4,d7
	dbra	d6,.dig
	move.b	#'F',-(a0)
	bsr	rt_write_snapshot
.nosnap:
	endif
.blank:	movem.l	(sp)+,d0-d7/a0-a6
.no:	rts


; ---------------------------------------------------------------------------
; Game-code timing, all in 1/38400 s (_hz_200 x 192 + MFP Timer C's count;
; timer C interrupts above the VBL, so the clock runs inside it).

; rt_now: d0.l = the clock, never stepping back. Preserves the rest.
; Runs with interrupts masked: it is called from translated code, and the
; VBL runs the translated NMI and IRQs on the interrupted registers, so
; nothing interruptible may leave a temporary in d1 or d2.
rt_now:
	move.w	sr,-(sp)
	ori.w	#$0700,sr
	move.l	d1,-(sp)
.again:	move.l	$4ba.w,d1
	moveq	#0,d0
	move.b	$fffffa23.w,d0
	cmp.l	$4ba.w,d1
	bne.s	.again
	mulu.l	#192,d1
	neg.l	d0
	add.l	#192,d0
	add.l	d1,d0
	cmp.l	rt_now_last,d0
	bcc.s	.up
	move.l	rt_now_last,d0
.up:	move.l	d0,rt_now_last
	move.l	(sp)+,d1
	move.w	(sp)+,sr
	rts

; Marks from tools/smkx/marks.txt. The main loop's frame work is timed
; from main_start to main_end, minus the VBL handler's time in between
; (NMI, IRQs, rendering). Preserve every register and the CCR.
	xdef	rt_mark_main_start,rt_mark_main_end
rt_mark_main_start:
	move.w	sr,-(sp)
	move.w	#$2700,sr
	move.l	d0,-(sp)
	bsr	rt_now
	move.l	d0,rt_main_t0
	move.l	rt_t_vbl,rt_main_vbl0
	st	rt_main_open
	move.l	(sp)+,d0
	move.w	(sp)+,sr
	rts

rt_mark_main_end:
	move.w	sr,-(sp)
	move.w	#$2700,sr
	tst.b	rt_main_open
	beq.s	.x
	sf	rt_main_open
	move.l	d0,-(sp)
	bsr	rt_now
	sub.l	rt_main_t0,d0
	sub.l	rt_t_vbl,d0
	add.l	rt_main_vbl0,d0
	add.l	d0,rt_t_main
	addq.l	#1,rt_n_main
	move.l	(sp)+,d0
.x:	move.w	(sp)+,sr
	rts

; "SMKX time" frames, then cumulative, in 1/38400 s. DKC (NMI at WAI):
; frame handlers started, time waiting at WAI (VBL time excluded), NMI,
; IRQ (both 0), time since start, rendering, whole VBL handler. Game time
; = since start - waiting - VBL handler.
rt_print_times:
	movem.l	d5-d7/a0-a1,-(sp)
	move.l	d0,-(sp)
	bsr	rt_now
	sub.l	rt_t_start,d0
	move.l	d0,rt_t_dsp1		; DKC: elapsed since start (no DSP-1)
	move.l	(sp)+,d0
	lea	msg_times,a0
	bsr	rt_print
	move.l	rt_frames,d7
	bsr	rt_print_hex8
	lea	rt_n_main,a1
	moveq	#6,d5
.one:	lea	msg_space,a0
	bsr	rt_print
	move.l	(a1)+,d7
	bsr	rt_print_hex8
	dbra	d5,.one
	lea	msg_nl,a0
	bsr	rt_print
	movem.l	(sp)+,d5-d7/a0-a1
	rts

; One status line: frames, NMIs, main-loop state, DMA bytes, APU writes.
rt_status:
	movem.l	d0-d7/a0-a6,-(sp)
	lea	msg_status,a0
	bsr	rt_print
	move.l	rt_frames,d7
	bsr	rt_print_hex8
	lea	msg_nmi,a0
	bsr	rt_print
	move.l	rt_nmis,d7
	bsr	rt_print_hex8
	lea	msg_state,a0
	bsr	rt_print
	lea	smkx_wram,a0
	move.w	$1c(a0),d7		; DKC: NMI handler ($1C) : frame counter ($2A)
	ror.w	#8,d7
	swap	d7
	move.w	$2a(a0),d7
	ror.w	#8,d7
	bsr	rt_print_hex8
	lea	msg_dma,a0
	bsr	rt_print
	move.l	rt_dmabytes,d7
	bsr	rt_print_hex8
	lea	msg_apu,a0
	bsr	rt_print
	move.l	rt_apuwrites,d7
	bsr	rt_print_hex8
	lea	msg_dsp,a0
	bsr	rt_print
	move.l	rt_nmi_frames,d7		; NMIs that started a frame handler
	bsr	rt_print_hex8
	lea	msg_pc,a0
	bsr	rt_print
	move.l	rt_last_pc,d7
	sub.l	#start,d7		; offset from the start of the text segment
	bsr	rt_print_hex8
	lea	msg_ppu,a0
	bsr	rt_print
	moveq	#0,d7
	move.b	rt_io+$100,d7		; INIDISP: force blank, brightness
	lsl.w	#8,d7
	move.b	rt_io+$105,d7		; BGMODE
	swap	d7
	move.b	rt_io+$12C,d7		; TM: main-screen layers
	bsr	rt_print_hex8
	lea	msg_cg,a0
	bsr	rt_print
	lea	rt_cgram,a0
	moveq	#0,d7
	move.w	#255,d6
.cgs:	add.w	(a0)+,d7
	rol.w	#1,d7
	dbra	d6,.cgs
	bsr	rt_print_hex8
	lea	msg_m7rows,a0
	bsr	rt_print
	move.l	smkx_m7_rows,d7
	bsr	rt_print_hex8
	lea	msg_ppu_dsp,a0		; DSP renderer: frames, pages, fallbacks,
	bsr	rt_print		; then plan/upload/send time (1/38400 s)
	lea	smkx_ppu_stat,a1	; src/smkx_ppu.h
	moveq	#5,d6
.pc:	move.l	(a1)+,d7
	bsr	rt_print_hex8
	lea	msg_slash,a0
	bsr	rt_print
	dbra	d6,.pc
	lea	msg_rtime,a0
	bsr	rt_print
	move.l	rt_renders,d7
	bsr	rt_print_hex8
	lea	msg_slash,a0
	bsr	rt_print
	move.l	rt_render_ticks,d7
	bsr	rt_print_hex8
	lea	msg_c2p,a0
	bsr	rt_print
	move.l	smkx_c2p_ticks,d7
	bsr	rt_print_hex8
	lea	msg_lc,a0		; list compositor (src/smkx_lc.c): frames shown, planned, cold, groups, copies,
	bsr	rt_print		; fallbacks, plan ticks, walk ticks, list/copy/window ticks (1/38400 s)
	lea	smkx_lc_stat,a1
	moveq	#15,d6
.lcs:	move.l	(a1)+,d7
	bsr	rt_print_hex8
	lea	msg_slash,a0
	bsr	rt_print
	dbra	d6,.lcs
	lea	msg_vid,a0		; video adapter (src/smkx_video.c): render_start calls, blank skips, lc, lc refused, dsp, cpu
	bsr	rt_print
	lea	smkx_vid_stat,a1
	moveq	#7,d6
.vds:	move.l	(a1)+,d7
	bsr	rt_print_hex8
	lea	msg_slash,a0
	bsr	rt_print
	dbra	d6,.vds
	lea	msg_lcp,a0		; lc_plan checkpoints (src/lc_plan.c)
	bsr	rt_print
	lea	lc_prof,a1
	moveq	#9,d6
.lcq:	move.l	(a1)+,d7
	bsr	rt_print_hex8
	lea	msg_slash,a0
	bsr	rt_print
	dbra	d6,.lcq
	lea	msg_io,a0
	bsr	rt_print
	move.l	rt_io_addr,d7
	bsr	rt_print_hex8
	lea	msg_from,a0
	bsr	rt_print
	move.l	rt_io_caller,d7
	sub.l	#start,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print
	bsr	rt_print_times
	movem.l	(sp)+,d0-d7/a0-a6
	rts

; ---------------------------------------------------------------------------
; SNAP.BIN (format in src/snes_ppu.h): the renderer's inputs for the last
; frame and the planes the Falcon composed from them. GEMDOS is safe here:
; the interrupted code is the translated program, never TOS.
rt_write_snapshot:
	movem.l	d0-d7/a0-a6,-(sp)
	jsr	smkx_lc_active		; the ring window: the picture as a plain screen
	tst.l	d0
	beq.s	.plain
	pea	rt_screen_copy
	jsr	smkx_lc_window
	addq.l	#4,sp
	bra.s	.noscr
.plain:	move.l	rt_screen,d0
	beq.s	.noscr
	movea.l	d0,a0
	lea	rt_screen_copy,a1
	move.w	#SCREEN_BYTES/4-1,d0
.cp:	move.l	(a0)+,(a1)+
	dbra	d0,.cp
.noscr:	clr.w	-(sp)
	pea	snap_name
	move.w	#$3c,-(sp)		; Fcreate
	trap	#1
	addq.l	#8,sp
	tst.l	d0
	bmi	.fail
	move.w	d0,d7
	lea	snap_blocks,a3
.blk:	move.l	(a3)+,d6		; length
	beq.s	.close
	move.l	(a3)+,a4		; address
	move.l	a4,-(sp)
	move.l	d6,-(sp)
	move.w	d7,-(sp)
	move.w	#$40,-(sp)		; Fwrite
	trap	#1
	lea	12(sp),sp
	bra.s	.blk
.close:	move.w	d7,-(sp)
	move.w	#$3e,-(sp)		; Fclose
	trap	#1
	addq.l	#4,sp
	lea	msg_snap,a0
	bsr	rt_print
.fail:	movem.l	(sp)+,d0-d7/a0-a6
	rts

; ---------------------------------------------------------------------------
; Data bank. d7.b = bank; sets d3 = bank << 16, a3 = b:$8000, a4 = b:$0000.
; Preserves every other register and the CCR.

rt_setdbr:
	move.w	ccr,-(sp)
	move.l	d7,-(sp)
	andi.l	#$ff,d7
	move.l	d7,d3
	swap	d3
	lsl.w	#2,d7
	movea.l	(rt_lotab,d7.w),a4
	movea.l	(rt_hitab,d7.w),a3
	move.l	(sp)+,d7
	move.w	(sp)+,ccr
	rts

; ---------------------------------------------------------------------------
; Memory helpers. a0 = SNES address; d6 = value (write) or result (read).
; Everything else, the CCR included, is preserved.

; rt_resolve: a1 = host byte address, or a1 = 0 and d7 = address for I/O.
rt_resolve:
	move.l	a0,d7
	andi.l	#$ffffff,d7
	swap	d7
	tst.b	(rt_banksys,d7.w)
	bne.s	.sys
	lsl.w	#2,d7
	movea.l	(rt_lotab,d7.w),a1
	move.l	a0,d7
	andi.l	#$ffff,d7
	adda.l	d7,a1
	rts
.sys:	move.l	a0,d7
	andi.l	#$ffff,d7
	cmpi.w	#$2000,d7
	bcs.s	.wram
	tst.w	d7
	bmi.s	.rom
	cmpi.w	#$6000,d7
	bcs.s	.io
	move.l	a0,d7			; $6000-$7FFF: DSP-1 in $00-$1F, SRAM in $20-$3F
	swap	d7
	andi.w	#$3f,d7
	cmpi.w	#$20,d7
	bcs.s	.io
	move.l	a0,d7
	andi.l	#$7ff,d7
	lea	rt_sram,a1
	adda.l	d7,a1
	rts
.wram:	lea	smkx_wram,a1
	adda.l	d7,a1
	rts
.rom:	andi.l	#$7fff,d7
	move.l	d7,-(sp)
	move.l	a0,d7
	swap	d7
	andi.w	#$ff,d7
	lsl.w	#2,d7
	movea.l	(rt_hitab,d7.w),a1
	move.l	(sp)+,d7
	adda.l	d7,a1
	rts
.io:	suba.l	a1,a1
	move.l	a0,d7
	andi.l	#$ffffff,d7
	rts

; Generic accesses go through page tables built from rt_resolve's rules
; (from F030SMK): one entry per 256-byte page of the 24-bit address space,
; the host address of the page (0 = I/O). Writes use their own table: ROM
; pages point at a scratch page (writes to ROM are dropped).
rt_rb:
	NOTE_CALLER
	move.w	ccr,-(sp)
	movem.l	d7/a1,-(sp)
	move.l	a0,d7
	lsr.l	#8,d7
	andi.l	#$ffff,d7
	movea.l	(rt_pages_r,d7.l*4),a1
	cmpa.w	#0,a1
	beq.s	.io
	move.l	a0,d7
	andi.l	#$ff,d7
	moveq	#0,d6
	move.b	(a1,d7.l),d6
.done:	movem.l	(sp)+,d7/a1
	move.w	(sp)+,ccr
	rts
.io:	move.l	a0,d7
	andi.l	#$ffffff,d7
	bsr	rt_io_read
	bra.s	.done

rt_rw:
	NOTE_CALLER
	move.w	ccr,-(sp)
	move.l	d7,-(sp)
	bsr	rt_rb			; low byte from the address, first
	move.b	d6,-(sp)
	addq.l	#1,a0
	bsr	rt_rb			; then the high byte from address + 1
	subq.l	#1,a0
	lsl.w	#8,d6
	move.b	(sp)+,d6
	move.l	(sp)+,d7
	move.w	(sp)+,ccr
	rts

rt_wb:
	NOTE_CALLER
	move.w	ccr,-(sp)
	movem.l	d6-d7/a1,-(sp)
	move.l	a0,d7
	lsr.l	#8,d7
	andi.l	#$ffff,d7
	movea.l	(rt_pages_w,d7.l*4),a1
	cmpa.w	#0,a1
	beq.s	.io
	move.l	a0,d7
	andi.l	#$ff,d7
	move.b	d6,(a1,d7.l)
.done:	movem.l	(sp)+,d6-d7/a1
	move.w	(sp)+,ccr
	rts
.io:	move.l	a0,d7
	andi.l	#$ffffff,d7
	bsr	rt_io_write
	bra.s	.done

; The page tables, from rt_resolve and rt_wb's former rules: writes land
; in WRAM ($7E/$7F, and the low 8 KiB of the system banks) and SRAM;
; I/O pages are 0 in both tables.
rt_build_pages:
	movem.l	d0-d7/a0-a6,-(sp)
	lea	rt_pages_r,a2
	lea	rt_pages_w,a3
	moveq	#0,d0			; page
.pg:	move.l	d0,d1
	lsl.l	#8,d1
	movea.l	d1,a0
	bsr	rt_resolve		; a1 = host address or 0 for I/O
	move.l	a1,(a2)+
	move.l	a1,d2
	beq.s	.put			; I/O in both
	move.l	d0,d3
	lsr.l	#8,d3			; bank
	tst.b	(rt_banksys,d3.w)
	bne.s	.sys
	andi.w	#$fe,d3			; linear bank: only WRAM takes writes
	cmpi.w	#$7e,d3
	beq.s	.put
	bra.s	.drop
.sys:	btst	#7,d0			; system bank: $8000-$FFFF is ROM
	beq.s	.put
.drop:	lea	rt_trash,a1
.put:	move.l	a1,(a3)+
	addq.l	#1,d0
	cmpi.l	#$10000,d0
	bcs.s	.pg
	lea	rt_dcache,a0		; empty dispatch cache: no key matches -1
	move.w	#DCACHE_SIZE-1,d0
.dc:	move.l	#-1,(a0)+
	clr.l	(a0)+
	dbra	d0,.dc
	movem.l	(sp)+,d0-d7/a0-a6
	rts

rt_ww:
	NOTE_CALLER
	move.w	ccr,-(sp)
	move.l	d6,-(sp)
	bsr	rt_wb			; low byte at the address
	addq.l	#1,a0
	lsr.w	#8,d6
	bsr	rt_wb			; high byte at address + 1
	subq.l	#1,a0
	move.l	(sp)+,d6
	move.w	(sp)+,ccr
	rts

; Fixed-address I/O, emitted for every static register access: a0.w is the
; register ($2000-$5FFF, or the DSP-1 at $6000-$7FFF). No bank resolution.
; d6 = value / result; everything else, the CCR included, is preserved.
rt_iorb:
	move.w	ccr,-(sp)
	move.l	d7,-(sp)
	move.l	a0,d7
	bsr	rt_io_read
	move.l	(sp)+,d7
	move.w	(sp)+,ccr
	rts

rt_iowb:
	move.w	ccr,-(sp)
	movem.l	d6-d7,-(sp)
	move.l	a0,d7
	bsr	rt_io_write
	movem.l	(sp)+,d6-d7
	move.w	(sp)+,ccr
	rts

rt_iorw:
	move.w	ccr,-(sp)
	movem.l	d5/d7,-(sp)
	move.l	a0,d7
	bsr	rt_io_read		; low byte first
	move.b	d6,d5
	move.l	a0,d7
	addq.l	#1,d7
	bsr	rt_io_read
	lsl.w	#8,d6
	move.b	d5,d6
	movem.l	(sp)+,d5/d7
	move.w	(sp)+,ccr
	rts

rt_ioww:
	move.w	ccr,-(sp)
	movem.l	d6-d7,-(sp)
	move.l	a0,d7
	bsr	rt_io_write		; low byte first
	move.l	(sp),d6
	lsr.w	#8,d6
	move.l	a0,d7
	addq.l	#1,d7
	bsr	rt_io_write
	movem.l	(sp)+,d6-d7
	move.w	(sp)+,ccr
	rts

; Diagnostics: the translated call site and SNES address of the last access
; that went through the runtime. 4(sp) is the helper's own return address.
rt_note_caller:
	move.w	ccr,-(sp)
	cmpi.l	#rt_text_end,6(sp)
	bls.s	.skip
	move.l	6(sp),rt_io_caller
	move.l	a0,rt_io_addr
.skip:	move.w	(sp)+,ccr
	rts

; ---------------------------------------------------------------------------
; BIT with a memory operand: d7 = operand, d0 = A.
; N = operand bit 7 (15), V = bit 6 (14), Z = (A & operand) == 0, X kept.

rt_bit8:
	move.w	ccr,d6
	andi.w	#$10,d6
	btst	#7,d7
	beq.s	.n
	ori.w	#$08,d6
.n:	btst	#6,d7		; V: the byte rt_vf, not the CCR
	sne	rt_vf
	and.b	d0,d7
	bne.s	.z
	ori.w	#$04,d6
.z:	move.w	d6,ccr
	rts

rt_bit16:
	move.w	ccr,d6
	andi.w	#$10,d6
	btst	#15,d7
	beq.s	.n
	ori.w	#$08,d6
.n:	btst	#14,d7		; V: the byte rt_vf, not the CCR
	sne	rt_vf
	and.w	d0,d7
	bne.s	.z
	ori.w	#$04,d6
.z:	move.w	d6,ccr
	rts

; ---------------------------------------------------------------------------
; MVN/MVP: d6 = source bank << 16, d7 = destination bank << 16.
; Moves C+1 bytes from source:X to destination:Y; C ends at $FFFF and the
; data bank at the destination bank.

rt_mvn:
	move.w	ccr,-(sp)
	movem.l	d4-d5/a1,-(sp)
	move.l	d6,d4
	move.l	d7,d5
.loop:	move.l	d4,d7
	move.w	d1,d7
	movea.l	d7,a0
	bsr	rt_rb
	move.l	d5,d7
	move.w	d2,d7
	movea.l	d7,a0
	bsr	rt_wb
	addq.w	#1,d1
	addq.w	#1,d2
	subq.w	#1,d0
	cmpi.w	#$ffff,d0
	bne.s	.loop
	move.l	d5,d7
	swap	d7
	bsr	rt_setdbr
	movem.l	(sp)+,d4-d5/a1
	move.w	(sp)+,ccr
	rts

rt_mvp:
	move.w	ccr,-(sp)
	movem.l	d4-d5/a1,-(sp)
	move.l	d6,d4
	move.l	d7,d5
.loop:	move.l	d4,d7
	move.w	d1,d7
	movea.l	d7,a0
	bsr	rt_rb
	move.l	d5,d7
	move.w	d2,d7
	movea.l	d7,a0
	bsr	rt_wb
	subq.w	#1,d1
	subq.w	#1,d2
	subq.w	#1,d0
	cmpi.w	#$ffff,d0
	bne.s	.loop
	move.l	d5,d7
	swap	d7
	bsr	rt_setdbr
	movem.l	(sp)+,d4-d5/a1
	move.w	(sp)+,ccr
	rts

rt_sei:	st	rt_iflag
	rts
rt_cli:	sf	rt_iflag
	rts

; ---------------------------------------------------------------------------
; Pointer dispatch: a0 = 24-bit target, d6.w = width context.
; Binary search of smkx_dispatch (key = context << 24 | address).

rt_dispatch:
	move.w	ccr,-(sp)
	movem.l	d4-d5/d7/a1,-(sp)
	move.l	a0,d7
	andi.l	#$ffffff,d7
	moveq	#0,d4
	move.b	d6,d4
	ror.l	#8,d4
	or.l	d4,d7			; key
	; direct-mapped cache in front of the binary search (from F030SMK); a
	; slot's key and handler move with one movem each
	move.l	d7,d4
	moveq	#10,d5
	lsr.l	d5,d4
	eor.l	d7,d4
	andi.l	#DCACHE_SIZE-1,d4
	lsl.l	#3,d4
	lea	rt_dcache,a1
	adda.l	d4,a1
	movem.l	(a1),d4-d5
	cmp.l	d4,d7
	bne.s	.lookup
	movea.l	d5,a0
	movem.l	(sp)+,d4-d5/d7/a1
	move.w	(sp)+,ccr
	rts
.lookup:
	move.l	a1,-(sp)		; the slot
	lea	smkx_dispatch,a1
	moveq	#0,d4			; lo
	move.l	#smkx_dispatch_count,d5	; hi (exclusive)
.search:
	cmp.l	d4,d5
	bls.s	.miss
	move.l	d4,d6
	add.l	d5,d6
	lsr.l	#1,d6
	move.l	d6,-(sp)
	lsl.l	#3,d6
	cmp.l	(a1,d6.l),d7
	beq.s	.hit
	bcs.s	.lower
	move.l	(sp)+,d4
	addq.l	#1,d4
	bra.s	.search
.lower:	move.l	(sp)+,d5
	bra.s	.search
.hit:	addq.l	#4,sp
	movea.l	4(a1,d6.l),a0
	movea.l	(sp)+,a1		; fill the slot
	movem.l	d7/a0,(a1)
	movem.l	(sp)+,d4-d5/d7/a1
	move.w	(sp)+,ccr
	rts
.miss:	move.l	d7,rt_fault_arg
	move.l	22(sp),rt_io_caller	; the translated dispatch site
	lea	msg_dispatch,a0
	bra	rt_fatal_arg

; ---------------------------------------------------------------------------
; Probes (tools/smkx/probes.txt): 4(sp) = 65816 address. Reports each
; address once, with the frame number. Preserves registers and the CCR.
rt_probe:
	move.w	ccr,-(sp)
	movem.l	d6-d7/a0,-(sp)
	move.l	18(sp),d7
	ifne	SMKX_DUMP_PC
	cmpi.l	#SMKX_DUMP_PC,d7	; the n-th execution of this address: dump
	bne.s	.nodump
	addq.l	#1,rt_dump_hits
	cmpi.l	#SMKX_DUMP_HIT,rt_dump_hits
	bne.s	.nodump
	move.l	#$80000000+SMKX_DUMP_HIT,d7
	bsr	rt_write_dump_as
.nodump:
	move.l	18(sp),d7
	endif
	lea	rt_probed,a0
	move.w	rt_nprobed,d6
	bra.s	.find
.cmp:	cmp.l	(a0)+,d7
	beq.s	.seen
.find:	dbra	d6,.cmp
	cmpi.w	#64,rt_nprobed
	bcc.s	.seen
	move.l	d7,(a0)
	addq.w	#1,rt_nprobed
	lea	msg_probe,a0
	bsr	rt_print
	bsr	rt_print_hex8
	lea	msg_at,a0
	bsr	rt_print
	move.l	rt_frames,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print
.seen:	movem.l	(sp)+,d6-d7/a0
	move.w	(sp)+,ccr
	rts

; ---------------------------------------------------------------------------
; The translated DSP-1 program reached a branch the translator does not model;
; (sp) = the uPD77C25 program address.
rt_upd_trap:
	lea	msg_updtrap,a0
	bra	rt_fatal_sp

; ---------------------------------------------------------------------------
; Traps. Each is entered with the 65816 address pushed by `pea`.

rt_trap:
	lea	msg_trap,a0
	bra.s	rt_fatal_sp
rt_unsupported:
	lea	msg_unsupported,a0
	bra.s	rt_fatal_sp
rt_noreturn:
	lea	msg_noreturn,a0
	bra.s	rt_fatal_sp
rt_badtable:
	move.l	d1,rt_fault_arg
	lea	msg_badtable,a0
	bra	rt_fatal_arg
rt_badexit:				; (sp) = the 65816 call site, a0 = exit id
	move.l	a0,rt_fault_arg
	move.l	(sp),rt_io_caller
	lea	msg_badexit,a0
	bsr	rt_print
	move.l	rt_fault_arg,d7
	bsr	rt_print_hex8
	lea	msg_at_site,a0
	move.l	rt_io_caller,rt_fault_arg
	bra	rt_fatal_arg
rt_fatal_sp:
	move.l	(sp),rt_fault_arg
; ---------------------------------------------------------------------------
; CPU exceptions (bus and address error, illegal instruction, division by
; zero, CHK, TRAPV, privilege, line A/F): report the vector and the PC as
; a text offset over NatFeats, then leave through rt_exit instead of
; letting TOS show bombs.
EXC_FIRST	equ	2
EXC_LAST	equ	11

rt_catch_exceptions:
	lea	EXC_FIRST*4.w,a0
	lea	rt_old_exc,a1
	lea	rt_exc_stub,a2
	moveq	#EXC_LAST-EXC_FIRST,d0
.v:	cmpa.w	#9*4,a0			; leave trace (9) to TOS
	beq.s	.skip
	move.l	(a0),(a1)
	move.l	a2,(a0)
.skip:	addq.l	#4,a0
	addq.l	#4,a1
	dbra	d0,.v
	rts

rt_release_exceptions:
	lea	EXC_FIRST*4.w,a0
	lea	rt_old_exc,a1
	moveq	#EXC_LAST-EXC_FIRST,d0
.v:	cmpa.w	#9*4,a0
	beq.s	.skip
	move.l	(a1),(a0)
.skip:	addq.l	#4,a0
	addq.l	#4,a1
	dbra	d0,.v
	rts

; 68030 exception frame: SR (sp), PC 2(sp), format/vector offset 6(sp).
rt_exc_stub:
	move.w	#$2700,sr
	move.l	#rt_log_end,rt_log_lim		; the reserve is for this report
	movem.l	d0-d7/a0-a7,rt_exc_regs
	moveq	#0,d7
	move.w	6(sp),d7
	andi.w	#$0fff,d7
	lsr.w	#2,d7
	move.l	d7,rt_fault_arg
	move.l	2(sp),d7
	sub.l	#start,d7
	move.l	d7,rt_exc_pc
	lea	msg_exc,a0
	bsr	rt_print
	move.l	rt_fault_arg,d7
	bsr	rt_print_hex8
	lea	msg_exc_pc,a0
	bsr	rt_print
	move.l	rt_exc_pc,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print
	; d0-d7 a0-a7, then the 8 longs above the exception frame, as text
	; offsets where they point into the program (scripts/smkx_pc.py)
	lea	msg_exc_regs,a0
	bsr	rt_print
	lea	rt_exc_regs,a1
	moveq	#15,d5
.reg:	move.l	(a1)+,d7
	bsr	rt_print_hex8
	lea	msg_space,a0
	bsr	rt_print
	dbra	d5,.reg
	lea	msg_nl,a0
	bsr	rt_print
	lea	msg_exc_stack,a0
	bsr	rt_print
	movea.l	rt_exc_regs+15*4,a1	; a7 at the exception
	lea	8(a1),a1		; above SR, PC, format word
	moveq	#7,d5
.stk:	move.l	(a1)+,d7
	sub.l	#start,d7
	bsr	rt_print_hex8
	lea	msg_space,a0
	bsr	rt_print
	dbra	d5,.stk
	lea	msg_nl,a0
	bsr	rt_print
	bsr	rt_status
	moveq	#3,d7
	bra	rt_exit

rt_fatal_arg:
	move.w	#$2700,sr
	move.l	#rt_log_end,rt_log_lim
	bsr	rt_print
	move.l	rt_fault_arg,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print
	bsr	rt_status
	moveq	#1,d7
	bra	rt_exit

; ---------------------------------------------------------------------------
; SNES I/O. d7 = 24-bit address; d6.b = value (write) or result (read).
; Callers (rt_rb/rt_wb) have saved the CCR, d7 and a1.

rt_io_read:
	movem.l	d4-d5/a0,-(sp)
	move.l	d7,d4
	swap	d4
	andi.w	#$3f,d4			; bank, mirrors folded
	andi.l	#$ffff,d7
	moveq	#0,d6
	cmpi.w	#$6000,d7
	bcc	.dsp
	cmpi.w	#$2140,d7
	bcs.s	.ppu
	cmpi.w	#$2180,d7
	bcs	.apu
	beq	.wmdata
	cmpi.w	#$4210,d7
	beq	.rdnmi
	cmpi.w	#$4211,d7
	beq	.timeup
	cmpi.w	#$4212,d7
	beq	.hvbjoy
	cmpi.w	#$4016,d7
	beq	.joyser
	cmpi.w	#$4017,d7
	beq	.out			; no pad in port 2
.shadow:
	cmpi.w	#$2000,d7
	bcs.s	.out
	lea	rt_io-$2000,a0
	move.b	(a0,d7.l),d6
.out:	movem.l	(sp)+,d4-d5/a0
	rts
.ppu:	cmpi.w	#$2134,d7
	bcs.s	.shadow
	cmpi.w	#$2137,d7
	bcs.s	.mpy
	cmpi.w	#$2139,d7
	beq	.vramlo
	cmpi.w	#$213A,d7
	beq	.vramhi
	cmpi.w	#$213E,d7
	beq.s	.stat77
	cmpi.w	#$213F,d7
	beq.s	.stat78
	bra.s	.shadow
.mpy:	move.l	rt_mpy,d6
	subi.w	#$2134,d7
	lsl.w	#3,d7
	lsr.l	d7,d6
	bra	.out
.stat77:
	moveq	#$01,d6
	bra	.out
.stat78:
	moveq	#$03,d6
	bra	.out
.vramlo:				; reads return the prefetch latch
	move.b	rt_vprefetch,d6
	tst.b	rt_vminc_high
	bne	.out
	bsr	rt_vprefetch_load		; reloaded from the address before
	bsr	rt_vram_step		; the step: after setting the address,
	bra	.out			; the first word comes twice
.vramhi:
	move.b	rt_vprefetch+1,d6
	tst.b	rt_vminc_high
	beq	.out
	bsr	rt_vprefetch_load
	bsr	rt_vram_step
	bra	.out
.apu:	andi.w	#3,d7
	lea	rt_apu_out,a0
	move.b	(a0,d7.w),d6
	bra	.out
.wmdata:
	move.l	rt_wmadd,d4
	andi.l	#$1ffff,d4
	lea	smkx_wram,a0
	move.b	(a0,d4.l),d6
	addq.l	#1,rt_wmadd
	bra	.out
.rdnmi:	moveq	#2,d6
	tst.b	rt_rdnmi
	beq	.out
	ori.b	#$80,d6
	sf	rt_rdnmi
	bra	.out
.timeup:
	tst.b	rt_timeup
	beq	.out
	moveq	#-128,d6
	andi.w	#$ff,d6
	sf	rt_timeup
	bra	.out
.joyser:	moveq	#1,d6			; after auto-read: 1s, i.e. a standard pad
	bra	.out			; sits in port 1 (DKC checks, CODE_80C0F8)
.hvbjoy:
	not.b	rt_hvb_toggle		; alternate V-blank and H-blank together, so
	move.b	rt_hvb_toggle,d6	; waits for either flag set or clear end;
	andi.b	#$c0,d6			; auto-joypad read is never busy
	bra	.out
.dsp:	bra	.out			; $6000-$7FFF in $00-$1F: open bus (no DSP-1)

rt_io_write:
	movem.l	d4-d5/a0,-(sp)
	andi.l	#$ffff,d7
	cmpi.w	#$6000,d7
	bcc	.dsp
	cmpi.w	#$2000,d7
	bcs	.out
	lea	rt_io-$2000,a0
	move.b	d6,(a0,d7.l)
	tst.b	rt_logging		; during the frame's IRQ: log PPU writes
	beq.s	.nolog
	cmpi.w	#$2100,d7
	bcs.s	.nolog
	cmpi.w	#$2200,d7
	bcc.s	.nolog
	cmpi.w	#$2104,d7		; OAM and VRAM data land in their memories
	beq.s	.nolog
	cmpi.w	#$2118,d7
	beq.s	.nolog
	cmpi.w	#$2119,d7
	beq.s	.nolog
	move.w	rt_nirqlog,d4
	cmpi.w	#IRQLOG_MAX,d4
	bcc.s	.nolog
	mulu.w	#6,d4
	lea	rt_irqlog,a0
	adda.w	d4,a0
	move.w	rt_irqline,(a0)+
	move.w	d7,(a0)+
	move.b	d6,(a0)+
	clr.b	(a0)
	addq.w	#1,rt_nirqlog
.nolog:	move.w	d7,d4
	subi.w	#$2100,d4
	cmpi.w	#$100,d4
	bcs.s	.t21
	subi.w	#$2100,d4		; register - $4200
	cmpi.w	#$20,d4
	bcc.s	.out
	add.w	d4,d4
	move.w	(.w42,pc,d4.w),d4
	jmp	(.w42,pc,d4.w)
.t21:	add.w	d4,d4
	move.w	(.w21,pc,d4.w),d4
	jmp	(.w21,pc,d4.w)
.out:	movem.l	(sp)+,d4-d5/a0
	rts

.oamadd:
	moveq	#0,d4
	move.b	rt_io+$103,d4
	andi.w	#1,d4
	lsl.w	#8,d4
	move.b	rt_io+$102,d4
	add.w	d4,d4
	move.w	d4,rt_oamadd
	bra	.out
.oamdata:
	move.w	rt_oamadd,d4
	cmpi.w	#$200,d4
	bcc.s	.oamhigh
	btst	#0,d4
	bne.s	.oamodd
	move.b	d6,rt_oamlatch
	bra.s	.oaminc
.oamodd:
	lea	rt_oam,a0
	move.b	rt_oamlatch,-1(a0,d4.w)
	move.b	d6,(a0,d4.w)
	bra.s	.oaminc
.oamhigh:
	andi.w	#$21f,d4
	lea	rt_oam,a0
	move.b	d6,(a0,d4.w)
.oaminc:
	addq.w	#1,rt_oamadd
	andi.w	#$3ff,rt_oamadd
	bra	.out
.vmain:	move.b	d6,d4
	andi.w	#3,d4
	move.b	(vminc_table,pc,d4.w),d4
	move.w	d4,rt_vminc
	btst	#7,d6
	sne	rt_vminc_high
	bra	.out
.vmaddl:
	move.b	d6,rt_vmadd+1
	bsr	rt_vprefetch_load
	bra	.out
.vmaddh:
	move.b	d6,rt_vmadd
	bsr	rt_vprefetch_load
	bra	.out
.vmdatal:
	moveq	#0,d4
	move.w	rt_vmadd,d4
	add.l	d4,d4			; up to $FFFE: a long index
	lea	rt_vram,a0
	move.b	d6,(a0,d4.l)
	M7DIRTY
	tst.b	rt_vminc_high
	bne	.out
	bsr	rt_vram_step
	bra	.out
.vmdatah:
	moveq	#0,d4
	move.w	rt_vmadd,d4
	add.l	d4,d4
	lea	rt_vram+1,a0
	move.b	d6,(a0,d4.l)
	M7DIRTY
	tst.b	rt_vminc_high
	beq	.out
	bsr	rt_vram_step
	bra	.out
.m7a:	move.b	rt_m7old,d4		; write-twice: new byte on top
	move.b	d6,rt_m7old
	lsl.w	#8,d6
	move.b	d4,d6
	move.w	d6,rt_m7a
	move.w	d6,rt_m7
	bra.s	.mpyres
.m7b:	move.b	rt_m7old,d4
	move.b	d6,rt_m7old
	move.b	d6,rt_m7b
	lsl.w	#8,d6
	move.b	d4,d6
	move.w	d6,rt_m7+2
.mpyres:
	move.w	rt_m7a,d4
	ext.l	d4
	move.b	rt_m7b,d5
	ext.w	d5
	ext.l	d5
	muls.l	d5,d4
	move.l	d4,rt_mpy
	bra	.out
.cgadd:	moveq	#0,d4
	move.b	d6,d4
	add.w	d4,d4
	move.w	d4,rt_cgadd
	sf	rt_cgflip
	bra	.out
.cgdata:
	not.b	rt_cgflip
	beq.s	.cghigh
	move.b	d6,rt_cglatch
	bra	.out
.cghigh:
	move.w	rt_cgadd,d4
	andi.w	#$1fe,d4
	lea	rt_cgram,a0
	move.b	rt_cglatch,(a0,d4.w)
	andi.b	#$7f,d6
	move.b	d6,1(a0,d4.w)
	addq.w	#2,rt_cgadd
	bra	.out
.apu:	addq.l	#1,rt_apuwrites
	andi.w	#3,d7
	bsr	rt_apu_write
	bra	.out
.wmdata:
	move.l	rt_wmadd,d4
	andi.l	#$1ffff,d4
	lea	smkx_wram,a0
	move.b	d6,(a0,d4.l)
	addq.l	#1,rt_wmadd
	bra	.out
.wmaddl:
	move.b	d6,rt_wmadd+3
	bra	.out
.wmaddm:
	move.b	d6,rt_wmadd+2
	bra	.out
.wmaddh:
	andi.b	#1,d6
	move.b	d6,rt_wmadd+1
	bra	.out
.nmitimen:
	move.b	d6,rt_nmitimen
	bra	.out
.wrmpyb:
	moveq	#0,d4
	move.b	rt_io+$2202,d4
	moveq	#0,d5
	move.b	d6,d5
	mulu.w	d5,d4
	lea	rt_io,a0
	move.b	d4,$2216(a0)
	lsr.w	#8,d4
	move.b	d4,$2217(a0)
	bra	.out
.wrdivb:
	lea	rt_io,a0
	moveq	#0,d4
	move.b	$2205(a0),d4
	lsl.w	#8,d4
	move.b	$2204(a0),d4
	moveq	#0,d5
	move.b	d6,d5
	beq.s	.div0
	divu.w	d5,d4
	move.b	d4,$2214(a0)
	lsr.w	#8,d4
	move.b	d4,$2215(a0)
	swap	d4
	move.b	d4,$2216(a0)
	lsr.w	#8,d4
	move.b	d4,$2217(a0)
	bra	.out
.div0:	st	$2214(a0)
	st	$2215(a0)
	move.b	$2204(a0),$2216(a0)
	move.b	$2205(a0),$2217(a0)
	bra	.out
.mdmaen:
	bsr	rt_dma
	bra	.out
.dsp:	bra	.out
; Write-twice scroll registers: HOFS keeps three bits of the previous
; latch, VOFS takes it whole. Stored as rt_bgofs[bg*2 + (0 = H, 1 = V)].
.bgh:	move.w	d7,d4
	subi.w	#$210D,d4
	lea	rt_bgofs,a0
	moveq	#0,d5
	move.b	d6,d5
	lsl.w	#8,d5
	move.b	rt_bgold,d7
	andi.b	#$f8,d7
	or.b	d7,d5
	move.b	(a0,d4.w*2),d7		; old high byte: its low three bits stay
	andi.b	#7,d7
	or.b	d7,d5
	bra.s	.bgset
.bgv:	move.w	d7,d4
	subi.w	#$210D,d4
	lea	rt_bgofs,a0
	moveq	#0,d5
	move.b	d6,d5
	lsl.w	#8,d5
	move.b	rt_bgold,d5
.bgset:	move.w	d5,(a0,d4.w*2)
	move.b	d6,rt_bgold
	cmpi.w	#2,d4			; $210D/$210E are also M7HOFS/M7VOFS,
	bcc	.out			; latched through the Mode 7 byte
	lea	rt_m7ofs,a0
	moveq	#0,d5
	move.b	d6,d5
	lsl.w	#8,d5
	move.b	rt_m7old,d5
	move.w	d5,(a0,d4.w*2)
	move.b	d6,rt_m7old
	bra	.out
; Mode 7 C, D, X, Y (A and B also feed the multiplier, above).
.m7reg:	move.w	d7,d4
	subi.w	#$211B,d4
	lea	rt_m7,a0
	moveq	#0,d5
	move.b	d6,d5
	lsl.w	#8,d5
	move.b	rt_m7old,d5
	move.w	d5,(a0,d4.w*2)
	move.b	d6,rt_m7old
	bra	.out

.w21:
	dc.w	.out-.w21,.out-.w21,.oamadd-.w21,.oamadd-.w21
	dc.w	.oamdata-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.bgh-.w21,.bgv-.w21,.bgh-.w21
	dc.w	.bgv-.w21,.bgh-.w21,.bgv-.w21,.bgh-.w21
	dc.w	.bgv-.w21,.vmain-.w21,.vmaddl-.w21,.vmaddh-.w21
	dc.w	.vmdatal-.w21,.vmdatah-.w21,.out-.w21,.m7a-.w21
	dc.w	.m7b-.w21,.m7reg-.w21,.m7reg-.w21,.m7reg-.w21
	dc.w	.m7reg-.w21,.cgadd-.w21,.cgdata-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.apu-.w21,.apu-.w21,.apu-.w21,.apu-.w21
	dc.w	.apu-.w21,.apu-.w21,.apu-.w21,.apu-.w21
	dc.w	.apu-.w21,.apu-.w21,.apu-.w21,.apu-.w21
	dc.w	.apu-.w21,.apu-.w21,.apu-.w21,.apu-.w21
	dc.w	.apu-.w21,.apu-.w21,.apu-.w21,.apu-.w21
	dc.w	.apu-.w21,.apu-.w21,.apu-.w21,.apu-.w21
	dc.w	.apu-.w21,.apu-.w21,.apu-.w21,.apu-.w21
	dc.w	.apu-.w21,.apu-.w21,.apu-.w21,.apu-.w21
	dc.w	.apu-.w21,.apu-.w21,.apu-.w21,.apu-.w21
	dc.w	.apu-.w21,.apu-.w21,.apu-.w21,.apu-.w21
	dc.w	.apu-.w21,.apu-.w21,.apu-.w21,.apu-.w21
	dc.w	.apu-.w21,.apu-.w21,.apu-.w21,.apu-.w21
	dc.w	.apu-.w21,.apu-.w21,.apu-.w21,.apu-.w21
	dc.w	.apu-.w21,.apu-.w21,.apu-.w21,.apu-.w21
	dc.w	.apu-.w21,.apu-.w21,.apu-.w21,.apu-.w21
	dc.w	.apu-.w21,.apu-.w21,.apu-.w21,.apu-.w21
	dc.w	.wmdata-.w21,.wmaddl-.w21,.wmaddm-.w21,.wmaddh-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
	dc.w	.out-.w21,.out-.w21,.out-.w21,.out-.w21
.w42:
	dc.w	.nmitimen-.w42,.out-.w42,.out-.w42,.wrmpyb-.w42
	dc.w	.out-.w42,.out-.w42,.wrdivb-.w42,.out-.w42
	dc.w	.out-.w42,.out-.w42,.out-.w42,.mdmaen-.w42
	dc.w	.out-.w42,.out-.w42,.out-.w42,.out-.w42
	dc.w	.out-.w42,.out-.w42,.out-.w42,.out-.w42
	dc.w	.out-.w42,.out-.w42,.out-.w42,.out-.w42
	dc.w	.out-.w42,.out-.w42,.out-.w42,.out-.w42
	dc.w	.out-.w42,.out-.w42,.out-.w42,.out-.w42

vminc_table:
	dc.b	1,32,128,128
	even

; APU handshake model (no sound yet). d7.w = port 0-3, d6.b = value.
;   state 0  IPL ready: ports read $AA/$BB until $CC arrives on port 0
;   state 1  IPL transfer: port 0 echoes; an out-of-sequence index ends the
;            block, and with port 1 = 0 it starts the uploaded program
;   state 2  DKC's loader and driver (uploaded to $04B8 by CODE_8AB0E8):
;            every port echoes. Port 0 flips bit 7 per command; bit 0 set
;            is a data byte (CODE_8AB4FF), bit 0 clear starts the program
;            at ports 2/3, which reports ready with port 1 = $FF (the wait
;            at CODE_8AB204). (SMK's model returned to the IPL on $1F.)
rt_apu_write:
	lea	rt_apu_in,a0
	move.b	d6,(a0,d7.w)
	lea	rt_apu_out,a0
	move.b	rt_apu_state,d4
	beq.s	.ipl
	subq.b	#1,d4
	beq.s	.xfer
	move.b	d6,(a0,d7.w)		; driver: echo
	tst.w	d7
	bne.s	.done
	btst	#0,d6
	bne.s	.done
	st	1(a0)			; started: ready
	rts
.ipl:	tst.w	d7
	bne.s	.done
	cmpi.b	#$cc,d6
	bne.s	.done
	move.b	d6,(a0)
	move.b	#1,rt_apu_state
	st	rt_apu_index
	rts
.xfer:	tst.w	d7
	bne.s	.done
	move.b	d6,(a0)			; the IPL echoes port 0
	move.b	rt_apu_index,d4
	addq.b	#1,d4
	cmp.b	d6,d4
	bne.s	.block
	move.b	d6,rt_apu_index		; next data byte
	rts
.block:	st	rt_apu_index		; indices restart at 0 in the next block
	tst.b	rt_apu_in+1
	bne.s	.done
	move.b	#2,rt_apu_state		; port 1 = 0: execute the upload
.done:	rts

; rt_m7_mark: d4.w = first VRAM word, d3.l = bytes; marks every page the
; word copy touches, for the tile-row cache and the DSP programs.
; Preserves all registers.
rt_m7_mark:
	movem.l	d2/d6-d7/a0-a2,-(sp)
	move.w	d4,d6
	move.l	d3,d7
	lsr.l	#1,d7			; words
	beq.s	.x
	lea	rt_m7_dirty,a1
	lea	rt_vram_dirty,a2
	lea	rt_plan_dirty,a0
.pg:	move.w	d6,d2
	lsr.w	#8,d2
	st	(a2,d2.w)
	st	(a1,d2.w)
	st	(a0,d2.w)
	moveq	#0,d2
	move.b	d6,d2
	neg.w	d2
	addi.w	#256,d2			; words to the end of this page
	add.w	d2,d6
	andi.w	#$7fff,d6
	sub.l	d2,d7
	bgt.s	.pg
.x:	movem.l	(sp)+,d2/d6-d7/a0-a2
	rts

; The VRAM read prefetch: the word at rt_vmadd, loaded when the address is
; written and on the read that steps it. Clobbers d4 and a0.
rt_vprefetch_load:
	moveq	#0,d4
	move.w	rt_vmadd,d4
	add.l	d4,d4			; up to $FFFE: a long index
	lea	rt_vram,a0
	move.b	(a0,d4.l),rt_vprefetch
	move.b	1(a0,d4.l),rt_vprefetch+1
	rts

rt_vram_step:
	move.w	rt_vminc,d4
	add.w	d4,rt_vmadd
	andi.w	#$7fff,rt_vmadd
	rts

; General DMA: d6.b = channel mask written to $420B.
; A-bus to B-bus transfers with an incrementing source that stays inside its
; bank and resolves to host memory read straight from a host pointer; VRAM
; word transfers ($2118, mode 1, increment after the high byte by 1) copy
; directly into rt_vram. Everything else takes the per-byte path.
rt_dma:
	movem.l	d0-d7/a0-a4,-(sp)
	moveq	#0,d0			; channel
	move.b	d6,d1
.chan:	btst	d0,d1
	beq	.nextc
	lea	rt_io+$2300,a2		; $4300 - $2000
	move.w	d0,d2
	lsl.w	#4,d2
	adda.w	d2,a2			; a2 = this channel's registers
	moveq	#0,d3
	move.b	6(a2),d3
	lsl.w	#8,d3
	move.b	5(a2),d3
	subq.l	#1,d3
	andi.l	#$ffff,d3
	addq.l	#1,d3			; bytes, 1..65536
	add.l	d3,rt_dmabytes
	moveq	#0,d5
	move.b	4(a2),d5
	swap	d5
	move.b	3(a2),d5
	lsl.w	#8,d5
	move.b	2(a2),d5		; A-bus address
	move.b	(a2),d2
	btst	#7,d2			; B -> A: per byte
	bne	.slow
	suba.l	a1,a1			; source step: 0 when fixed
	btst	#3,d2
	bne.s	.fixsrc
	btst	#4,d2			; decrement: per byte
	bne	.slow
	addq.l	#1,a1
	move.l	d5,d4
	andi.l	#$ffff,d4
	add.l	d3,d4
	cmpi.l	#$10000,d4
	bhi	.slow			; would wrap inside the bank
.fixsrc:
	move.l	a1,-(sp)
	movea.l	d5,a0
	bsr	rt_resolve
	move.l	a1,d4
	movea.l	(sp)+,a1
	tst.l	d4
	beq	.slow			; source is I/O
	movea.l	d4,a3			; a3 = host source, a1 = its step
	move.b	(a2),d2
	andi.w	#7,d2			; transfer mode
	move.b	1(a2),d6		; B-bus register
	cmpi.b	#$18,d6
	beq	.vram
	cmpi.b	#$19,d6
	beq	.vram
	bsr	.onereg			; the rest: one register only (modes 0, 2, 6)
	bne	.generic
	cmpi.b	#$04,d6
	beq	.oam
	cmpi.b	#$22,d6
	beq	.cgram
	cmpi.b	#$80,d6
	beq	.wram
.generic:
	move.l	a1,d4			; per byte through the handlers: incrementing only
	beq	.slow
	bra	.bytes

; VRAM: mode 1 at $2118 writes word pairs; modes 0/2 write one half.
.vram:	lea	rt_vram,a4
	move.w	rt_vmadd,d4
	move.w	d4,-(sp)		; first word, for the Mode 7 pages
	move.l	d3,d7
	subq.l	#1,d7
	cmpi.w	#1,d2
	beq.s	.vpair
	bsr	.onereg
	bne	.vgen			; modes 3-5, 7: per byte
	moveq	#0,d2			; one half: 0 = low ($2118), 1 = high
	btst	#0,d6
	sne	d2
	andi.w	#1,d2
	move.b	rt_vminc_high,d6	; step after this half?
	sne	d6
	andi.b	#1,d6
	cmp.b	d6,d2
	seq	d6			; d6 = step
.vhalf:	moveq	#0,d5
	move.w	d4,d5
	add.l	d5,d5
	add.w	d2,d5
	move.b	(a3),(a4,d5.l)
	adda.l	a1,a3
	tst.b	d6
	beq.s	.vh1
	add.w	rt_vminc,d4
	andi.w	#$7fff,d4
.vh1:	subq.l	#1,d7
	bpl.s	.vhalf
	bra.s	.vend
.vpair:	cmpi.b	#$18,1(a2)
	bne	.vgen
	tst.b	rt_vminc_high
	beq	.vgen
	btst	#0,d3
	bne	.vgen
	lsr.l	#1,d7			; pairs - 1
	move.w	rt_vminc,d2
.vp:	moveq	#0,d5
	move.w	d4,d5
	add.l	d5,d5
	move.b	(a3),(a4,d5.l)
	adda.l	a1,a3
	move.b	(a3),1(a4,d5.l)
	adda.l	a1,a3
	add.w	d2,d4
	andi.w	#$7fff,d4
	subq.l	#1,d7
	bpl.s	.vp
.vend:	move.w	d4,rt_vmadd
	move.w	(sp)+,d7		; mark every Mode 7 page between first and last word
	move.w	d4,d5
	sub.w	d7,d5
	andi.l	#$7fff,d5
	addq.l	#1,d5
	add.l	d5,d5
	movem.l	d3-d4,-(sp)
	move.l	d5,d3
	move.w	d7,d4
	bsr	rt_m7_mark
	movem.l	(sp)+,d3-d4
	bra	.dmadone
.vgen:	addq.l	#2,sp
	bra	.generic
; Z set when transfer mode d2 writes a single register (0, 2, 6).
.onereg:
	cmpi.w	#0,d2
	beq.s	.one
	cmpi.w	#2,d2
	beq.s	.one
	cmpi.w	#6,d2
.one:	rts

; OAM through $2104: the low table takes word pairs through the latch.
.oam:	move.w	rt_oamadd,d4
	lea	rt_oam,a4
	move.l	d3,d7
	subq.l	#1,d7
.ol:	move.b	(a3),d6
	adda.l	a1,a3
	cmpi.w	#$200,d4
	bcc.s	.ohi
	btst	#0,d4
	bne.s	.oodd
	move.b	d6,rt_oamlatch
	bra.s	.oinc
.oodd:	move.b	rt_oamlatch,-1(a4,d4.w)
	move.b	d6,(a4,d4.w)
	bra.s	.oinc
.ohi:	move.w	d4,d2
	andi.w	#$21f,d2
	move.b	d6,(a4,d2.w)
.oinc:	addq.w	#1,d4
	andi.w	#$3ff,d4
	subq.l	#1,d7
	bpl.s	.ol
	move.w	d4,rt_oamadd
	bra	.dmadone

; CGRAM through $2122 (not while an IRQ's writes are logged).
.cgram:	tst.b	rt_logging
	bne	.generic
	move.w	rt_cgadd,d4
	lea	rt_cgram,a4
	move.l	d3,d7
	subq.l	#1,d7
.cl:	move.b	(a3),d6
	adda.l	a1,a3
	not.b	rt_cgflip
	beq.s	.chigh
	move.b	d6,rt_cglatch
	bra.s	.cn
.chigh:	move.w	d4,d2
	andi.w	#$1fe,d2
	move.b	rt_cglatch,(a4,d2.w)
	andi.b	#$7f,d6
	move.b	d6,1(a4,d2.w)
	addq.w	#2,d4
.cn:	subq.l	#1,d7
	bpl.s	.cl
	move.w	d4,rt_cgadd
	bra	.dmadone
; WRAM through $2180 (DKC clears all 128 KiB this way at reset).
.wram:	move.l	rt_wmadd,d4
	lea	smkx_wram,a4
	move.l	d3,d7
	subq.l	#1,d7
.wl:	andi.l	#$1ffff,d4
	move.b	(a3),(a4,d4.l)
	adda.l	a1,a3
	addq.l	#1,d4
	subq.l	#1,d7
	bpl.s	.wl
	andi.l	#$1ffff,d4
	move.l	d4,rt_wmadd
.dmadone:
	move.l	a1,d4			; a fixed source stays where it was
	beq.s	.cleared
	moveq	#0,d5			; the A-bus address again (d5 was scratch)
	move.b	3(a2),d5
	lsl.w	#8,d5
	move.b	2(a2),d5
	bra	.done
.cleared:
	clr.b	5(a2)
	clr.b	6(a2)
	bra	.nextc
.bytes:	; per byte through the register handlers, source from the host pointer
	lea	dma_patterns,a1
	lsl.w	#2,d2
	moveq	#0,d4			; pattern step
	move.l	d3,d7
	subq.l	#1,d7
	move.l	d7,-(sp)
.fb:	move.w	d4,d6
	andi.w	#3,d6
	add.w	d2,d6
	moveq	#0,d7
	move.b	(a1,d6.w),d7
	add.b	1(a2),d7
	addi.w	#$2100,d7
	move.b	(a3)+,d6
	bsr	rt_io_write
	addq.w	#1,d4
	subq.l	#1,(sp)
	bpl.s	.fb
	addq.l	#4,sp
.done:	add.w	d3,d5			; source address advances within its bank
	move.b	d5,2(a2)
	lsr.w	#8,d5
	move.b	d5,3(a2)
	clr.b	5(a2)
	clr.b	6(a2)
	bra	.nextc
.slow:	subq.l	#1,d3
	moveq	#0,d4			; pattern step
.byte:	lea	dma_patterns,a1
	move.b	(a2),d2
	andi.w	#7,d2
	lsl.w	#2,d2
	move.w	d4,d6
	andi.w	#3,d6
	add.w	d6,d2
	moveq	#0,d6
	move.b	(a1,d2.w),d6		; B-bus offset for this step
	moveq	#0,d7
	move.b	1(a2),d7
	add.b	d6,d7
	addi.w	#$2100,d7
	move.l	d7,-(sp)
	movea.l	d5,a0
	btst	#7,(a2)
	bne.s	.btoa
	bsr	rt_rb			; A -> B
	move.l	(sp)+,d7
	ori.l	#$800000,d7
	movem.l	d4/a0,-(sp)
	bsr	rt_io_write
	movem.l	(sp)+,d4/a0
	bra.s	.step
.btoa:	move.l	(sp)+,d7
	ori.l	#$800000,d7
	move.l	a0,-(sp)
	bsr	rt_io_read
	movea.l	(sp)+,a0
	bsr	rt_wb
.step:	btst	#3,(a2)
	bne.s	.fixed
	move.w	d5,d6
	btst	#4,(a2)
	bne.s	.dec
	addq.w	#1,d6
	bra.s	.setad
.dec:	subq.w	#1,d6
.setad:	move.w	d6,d5
	move.b	d6,2(a2)
	lsr.w	#8,d6
	move.b	d6,3(a2)
.fixed:	addq.w	#1,d4
	dbra	d3,.byte
	clr.b	5(a2)
	clr.b	6(a2)
.nextc:	addq.w	#1,d0
	cmpi.w	#8,d0
	bne	.chan
	movem.l	(sp)+,d0-d7/a0-a4
	rts

dma_patterns:				; B-bus offsets per transfer mode, 4 steps
	dc.b	0,0,0,0, 0,1,0,1, 0,0,0,0, 0,0,1,1
	dc.b	0,1,2,3, 0,1,0,1, 0,0,0,0, 0,0,1,1


; ---------------------------------------------------------------------------
; Keyboard as joypad 1. The handler replaces TOS's for the program's
; lifetime: it drains both ACIAs, skips IKBD packets (mouse, joystick,
; clock, status) and keeps the SNES button word in rt_pad, which rt_frame
; latches into $4218/$4219 once per frame. Keys (by Atari scancode, i.e.
; position on a QWERTY layout): arrows = D-pad, X = B, Z = Y, S = A, A = X,
; Q = L, W = R, Return = Start, Space = Select, Esc = quit.

rt_acia:
	movem.l	d0-d1/a0,-(sp)
.kbd:	btst	#0,$fffffc00.w		; keyboard ACIA: byte received
	beq.s	.midi
	move.b	$fffffc02.w,d0
	bsr.s	rt_kbd_byte
	bra.s	.kbd
.midi:	btst	#0,$fffffc04.w		; MIDI ACIA: drop
	beq.s	.done
	move.b	$fffffc06.w,d0
	bra.s	.midi
.done:	bclr	#6,$fffffa11.w		; MFP ISRB: GPIP4 serviced
	movem.l	(sp)+,d0-d1/a0
	rte

rt_kbd_byte:
	tst.b	rt_kbd_skip
	beq.s	.first
	subq.b	#1,rt_kbd_skip
	rts
.first:	cmpi.b	#$f6,d0
	bcs.s	.key
	moveq	#0,d1
	move.b	d0,d1
	subi.w	#$f6,d1
	move.b	(kbd_packet,pc,d1.w),rt_kbd_skip
	rts
.key:	moveq	#0,d1
	move.b	d0,d1
	andi.w	#$7f,d1
	lea	kbd_map,a0
	move.b	(a0,d1.w),d1
	bmi.s	.special
	tst.b	d0
	bmi.s	.up
	move.w	rt_pad,d0
	bset	d1,d0
	move.w	d0,rt_pad
	rts
.up:	move.w	rt_pad,d0
	bclr	d1,d0
	move.w	d0,rt_pad
	rts
.special:
	cmpi.b	#$ff,d1
	beq.s	.x
	tst.b	d0			; $80: Esc pressed
	bmi.s	.x
	st	rt_quit
.x:	rts

; Bytes that follow an IKBD packet header $F6-$FF.
kbd_packet:
	dc.b	7,5,2,2,2,2,6,2,1,1
; Scancode -> bit of rt_pad ($4219 << 8 | $4218), $FF none, $80 quit.
; $4219: B Y Select Start Up Down Left Right = bits 15..8;
; $4218: A X L R = bits 7..4.
kbd_map:
	dc.b	$FF,$80,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF
	dc.b	$05,$04,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$0C,$FF,$06,$07
	dc.b	$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$0E,$0F,$FF,$FF
	dc.b	$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$0D,$FF,$FF,$FF,$FF,$FF,$FF
	dc.b	$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$0B,$FF,$FF,$09,$FF,$08,$FF,$FF
	dc.b	$0A,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF
	dc.b	$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF
	dc.b	$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF
	even

; ---------------------------------------------------------------------------
; NatFeats output.

; The probe runs with the illegal-instruction vector (4) pointing at
; .absent: $7300/$7301 are illegal opcodes on a real 68030 (four bombs), so
; without an emulator rt_nf_stderr stays 0 and rt_print prints nothing.
rt_nf_init:
	move.w	sr,-(sp)
	ori.w	#$0700,sr
	move.l	$10.w,-(sp)
	move.l	#.absent,$10.w
	movea.l	sp,a1			; the handler comes back to this stack
	clr.l	rt_nf_stderr
	pea	nf_name
	bsr.s	nf_id
	addq.l	#4,sp
	move.l	d0,rt_nf_stderr
.done:	move.l	(sp)+,$10.w
	move.w	(sp)+,sr
	tst.l	rt_nf_stderr
	bne.s	.nf
	move.l	#rt_log_buf,rt_log_ptr		; no NatFeats: keep the text for DKCXLOG.TXT
	move.l	#rt_log_end-LOG_RESERVE,rt_log_lim
.nf:	rts
.absent:
	movea.l	a1,sp
	clr.l	rt_nf_stderr
	bra.s	.done

; The NatFeats opcodes use the C calling convention: arguments above the
; return address, so each sits in its own subroutine.
nf_id:	dc.w	$7300			; NF_ID(name) -> d0
	rts
nf_call:
	dc.w	$7301			; NF_CALL(id, args...)
	rts

; a0 = zero-terminated string.
rt_print:
	movem.l	d0-d1/a0-a1,-(sp)
	move.l	rt_nf_stderr,d0
	beq.s	.log
	move.l	a0,-(sp)
	move.l	d0,-(sp)
	bsr.s	nf_call
	addq.l	#8,sp
.none:	movem.l	(sp)+,d0-d1/a0-a1
	rts
.log:	move.l	rt_log_ptr,d0			; 0: not set up (before rt_nf_init)
	beq.s	.none
	move.w	sr,d1				; (IRQs print too)
	ori.w	#$0700,sr
	movea.l	d0,a1
.cp:	cmpa.l	rt_log_lim,a1
	bcc.s	.full
	move.b	(a0)+,d0
	beq.s	.end
	move.b	d0,(a1)+
	bra.s	.cp
.end:	move.l	a1,rt_log_ptr
.full:	move.w	d1,sr
	bra.s	.none

; rt_log_flush: user mode, at exit. Without NatFeats (a real Falcon) the
; text rt_print collected goes to DKCXLOG.TXT in the current directory.
rt_log_flush:
	tst.l	rt_nf_stderr
	bne.s	.x
	move.l	rt_log_ptr,d6
	beq.s	.x
	sub.l	#rt_log_buf,d6
	ble.s	.x
	movem.l	d0-d7/a0-a6,-(sp)
	clr.w	-(sp)
	pea	log_name
	move.w	#$3c,-(sp)		; Fcreate
	trap	#1
	addq.l	#8,sp
	tst.l	d0
	bmi.s	.done
	move.w	d0,d7
	pea	rt_log_buf
	move.l	d6,-(sp)
	move.w	d7,-(sp)
	move.w	#$40,-(sp)		; Fwrite
	trap	#1
	lea	12(sp),sp
	move.w	d7,-(sp)
	move.w	#$3e,-(sp)		; Fclose
	trap	#1
	addq.l	#4,sp
.done:	movem.l	(sp)+,d0-d7/a0-a6
.x:	rts

; d7.l printed as eight hex digits.
rt_print_hex8:
	movem.l	d6-d7/a0,-(sp)
	lea	rt_hexbuf+8,a0
	clr.b	(a0)
	moveq	#7,d6
.dig:	move.b	d7,-(sp)
	andi.b	#$f,(sp)
	cmpi.b	#10,(sp)
	bcs.s	.num
	addi.b	#'A'-10,(sp)
	bra.s	.put
.num:	addi.b	#'0',(sp)
.put:	move.b	(sp)+,-(a0)
	lsr.l	#4,d7
	dbra	d6,.dig
	bsr	rt_print
	movem.l	(sp)+,d6-d7/a0
	rts

	section	text,code
	include	'smkx_video.s'
	include	'smkx_lcput.s'
	include	'smkx_m7dsp.s'

	section	text,code
rt_text_end:

	section	data,data

nf_name:		dc.b	"NF_STDERR",0
msg_reset_returned:	dc.b	"SMKX: reset procedure returned",10,0
msg_frames:		dc.b	"SMKX: frame limit reached",10,0
msg_status:		dc.b	"SMKX: frame ",0
msg_nmi:		dc.b	" nmi ",0
msg_state:		dc.b	" nmi1C:fc ",0
msg_dma:		dc.b	" dma ",0
msg_apu:		dc.b	" apu ",0
msg_dsp:		dc.b	" fh ",0
; APU ports as the S-SMP IPL leaves them at power-on: $AA/$BB signal
; "ready for upload". After the first write each port echoes the last value
; written to it, which is what the upload handshake waits for.
rt_apu_out:		dc.b	$AA,$BB,$00,$00
rt_apu_in:		dc.b	0,0,0,0
rt_apu_state:		dc.b	0
rt_apu_index:		dc.b	0
msg_busy:		dc.b	"SMKX: frame handler still running; ",0
msg_probe:		dc.b	"SMKX probe $",0
msg_at:			dc.b	" at frame ",0
msg_snap:		dc.b	"SMKX: wrote snapshot",10,0
msg_m7on:		dc.b	"SMKX: Mode 7 on the DSP",10,0
msg_m7off:		dc.b	"SMKX: Mode 7 on the CPU",10,0
msg_m7rows:		dc.b	" m7dsp ",0
msg_rtime:		dc.b	" renders/ticks ",0
msg_slash:		dc.b	"/",0
msg_ppu_dsp:		dc.b	" ppudsp ",0
msg_lc:			dc.b	" lc ",0
msg_lcp:		dc.b	" lcp ",0
msg_vid:		dc.b	" vid ",0
msg_prof:		dc.b	"SMKX prof ",0
msg_times:		dc.b	"SMKX time ",0
msg_exc:		dc.b	"SMKX CPU exception, vector $",0
msg_exc_pc:		dc.b	" at text +",0
msg_exc_regs:		dc.b	"SMKX regs d0-d7 a0-a7 ",0
msg_exc_stack:		dc.b	"SMKX stack (text offsets) ",0
msg_space:		dc.b	" ",0
snap_name:		dc.b	"SNAP.BIN",0,0,0,0,0	; or Fnnnnnnn.BIN
log_name:		dc.b	"DKCXLOG.TXT",0
	even
snap_magic:		dc.b	"SMKXSNAP",0,0,0,4
	even
snap_nlog:		dc.w	0
snap_blocks:
	dc.l	12,snap_magic
	dc.l	$10000,rt_vram
	dc.l	512,rt_cgram_start
	dc.l	544,rt_oam
	dc.l	$6000,rt_io_start
	dc.l	16+12+4,rt_bgofs_start
	dc.l	4,rt_nlog32
	dc.l	IRQLOG_MAX*6,rt_irqlog
	dc.l	$20000,smkx_wram
	dc.l	$800,rt_sram
	dc.l	SCREEN_BYTES,rt_screen_copy
	dc.l	4+OAMSEG_MAX*OAMSEG_BYTES,rt_noamseg
	dc.l	0
msg_samples:		dc.b	"SMKX samples",10,0
msg_dump:		dc.b	"SMKX: wrote a dump",10,0
dump_name:		dc.b	"D0000000.BIN",0
dump_magic:		dc.b	"DKCXDUMP"
	even
dump_frame:		dc.l	0
dump_blocks:
	dc.l	8,dump_magic
	dc.l	4,dump_frame
	dc.l	$20000,smkx_wram
	dc.l	$10000,rt_vram
	dc.l	512,rt_cgram
	dc.l	544,rt_oam
	dc.l	$6000,rt_io
	dc.l	$800,rt_sram
	dc.l	0
msg_quit:		dc.b	"SMKX: quit (Esc)",10,0
msg_c2p:		dc.b	" c2p ",0
msg_wram_at:		dc.b	"SMKX: WRAM at $",0
msg_dbr:		dc.b	" dbr ",0
msg_text_at:		dc.b	", text at $",0
msg_nmilog:		dc.b	"SMKX NMI at WAI, $1C = ",0
msg_sp:			dc.b	" ",0
msg_pc:			dc.b	" pc +",0
msg_ppu:		dc.b	" ppu ",0
msg_cg:			dc.b	" cg ",0
msg_io:			dc.b	" io $",0
msg_from:		dc.b	" from +",0
msg_nl:			dc.b	10,0
msg_trap:		dc.b	"SMKX TRAP at 65816 $",0
msg_updtrap:		dc.b	"SMKX DSP-1 program reached an untranslated branch at $",0
msg_unsupported:	dc.b	"SMKX UNSUPPORTED instruction at $",0
msg_noreturn:		dc.b	"SMKX return to a call site with no continuation at $",0
msg_badtable:		dc.b	"SMKX jump-table entry is not code, X = $",0
msg_badexit:		dc.b	"SMKX callee returned in an unexpected context, exit id $",0
msg_at_site:		dc.b	" to the call at 65816 $",0
msg_dispatch:		dc.b	"SMKX pointer dispatch to an untranslated target (ctx<<24|addr) $",0
	even

	include	"smkx_info.s"		; the info screen; the player's ROM, read at startup

	section	bss,bss

rt_nf_stderr:	ds.l	1
rt_log_ptr:	ds.l	1
rt_log_lim:	ds.l	1
rt_log_buf:	ds.b	LOG_BYTES
rt_log_end:
rt_old_ssp:	ds.l	1
rt_old_vbl:	ds.l	1
rt_old_acia:	ds.l	1
rt_clut:	ds.l	256		; Videl palette for the next VBL (smkx_render)
rt_clut_ready:	ds.b	1
rt_vram_dirty:	ds.b	128		; per 256-word VRAM page, for the tile-row cache
	even
rt_pad:		ds.w	1		; SNES buttons, $4219 << 8 | $4218
rt_script_pad:	ds.w	1
rt_script_at:	ds.l	1
rt_quit:	ds.b	1
rt_kbd_skip:	ds.b	1
	even
rt_old_exc:	ds.l	EXC_LAST-EXC_FIRST+1
rt_exc_pc:	ds.l	1
rt_exc_regs:	ds.l	16
rt_exit_code:	ds.w	1
rt_vblcount:	ds.l	1
rt_frames:	ds.l	1
rt_nmi_frames:	ds.l	1		; NMIs that started a frame handler
rt_nmi_logged:	ds.l	1
rt_dump_next:	ds.l	1		; the next game frame to dump
rt_dumps:	ds.l	1
rt_dump_hits:	ds.l	1
rt_sp_base:	ds.l	1
rt_dp:		ds.w	1		; the 65816 D register (TCD), big-endian
rt_nmis:	ds.l	1
rt_irqs:	ds.l	1
rt_dmabytes:	ds.l	1
rt_apuwrites:	ds.l	1
rt_dspwrites:	ds.l	1
rt_fault_arg:	ds.l	1
rt_last_pc:	ds.l	1
rt_stuck:	ds.l	1
rt_nsamples:	ds.l	1
rt_sampidx:	ds.l	1
rt_nprobed:	ds.w	1
rt_probed:	ds.l	64
rt_samples:	ds.l	2*SAMPLES
rt_io_caller:	ds.l	1
rt_io_addr:	ds.l	1
rt_mpy:		ds.l	1
rt_wmadd:	ds.l	1
rt_m7a:		ds.w	1
rt_bgofs:	ds.w	8		; BG1-4 H/V scroll, as the PPU latched them
rt_m7:		ds.w	6		; Mode 7 A, B, C, D, X, Y
rt_m7ofs:	ds.w	2		; Mode 7 H/V scroll
rt_bgofs_start:	ds.w	8		; the same at the start of the frame
rt_m7_start:	ds.w	6
rt_m7ofs_start:	ds.w	2
rt_nlog32:	ds.w	1		; big-endian u32 count for SNAP.BIN (high word 0)
rt_nirqlog:	ds.w	1
rt_irqline:	ds.w	1
rt_logging:	ds.b	1
	even
rt_irqleft:	ds.w	1
	even
rt_irqlog:	ds.b	IRQLOG_MAX*6
rt_io_start:	ds.b	$6000
rt_renders:	ds.l	1
rt_render_ticks:	ds.l	1	; 200 Hz ticks spent in smkx_render
rt_sync:		ds.b	1	; rt_render_sync: wait for the DSP's frame
rt_rstate:	ds.b	1	; render work: 0 none, 1 asked for, 2 started
rt_in_render:	ds.b	1	; the CPU runs the render context
rt_snapdue:	ds.b	1	; a snapshot waits for the render work
rt_snapnow:	ds.b	1	; this sync render writes the snapshot
	even
rt_main_sp:	ds.l	1	; the game, saved in rt_wai
rt_rctx_sp:	ds.l	1	; the render context, saved by the VBL
; rt_print_times prints these seven in this order
rt_n_main:	ds.l	1
rt_t_main:	ds.l	1
rt_t_nmi:	ds.l	1
rt_t_irq:	ds.l	1
rt_t_dsp1:	ds.l	1
rt_t_render:	ds.l	1
rt_t_vbl:	ds.l	1
rt_t0:		ds.l	1
rt_t_start:	ds.l	1
rt_wait_t0:	ds.l	1
rt_wait_vbl0:	ds.l	1
rt_now_last:	ds.l	1
rt_main_t0:	ds.l	1
rt_main_vbl0:	ds.l	1
rt_main_open:	ds.b	1
	even
rt_vmadd:	ds.w	1
rt_vprefetch:	ds.w	1		; VRAM read prefetch: low byte, high byte
rt_vminc:	ds.w	1
rt_cgadd:	ds.w	1
rt_oamadd:	ds.w	1
rt_in_frame:	ds.b	1
rt_nmi_pending:	ds.b	1
rt_nmitimen:	ds.b	1
rt_rdnmi:	ds.b	1
rt_timeup:	ds.b	1
rt_iflag:	ds.b	1
rt_vf:		ds.b	1		; the 65816 V flag ($FF set), outside the CCR
rt_hvb_toggle:	ds.b	1
rt_vminc_high:	ds.b	1
rt_cgflip:	ds.b	1
rt_cglatch:	ds.b	1
rt_oamlatch:	ds.b	1
rt_m7old:	ds.b	1
rt_m7b:		ds.b	1
rt_bgold:	ds.b	1
rt_hexbuf:	ds.b	10
	even
rt_banksys:	ds.b	256
rt_lotab:	ds.l	256
rt_hitab:	ds.l	256
rt_io:		ds.b	$6000		; shadows of $2000-$7FFF
rt_vram:	ds.b	$10000
rt_cgram:	ds.b	512
rt_oam:		ds.b	544
rt_cgram_start:	ds.b	512
rt_noamseg:	ds.l	1
rt_oamseg:	ds.b	OAMSEG_MAX*OAMSEG_BYTES
rt_sram:	ds.b	$800
rt_pages_r:	ds.l	65536		; host address per 256-byte page, 0 = I/O
rt_pages_w:	ds.l	65536
rt_dcache:	ds.l	2*DCACHE_SIZE
rt_trash:	ds.b	256		; writes to ROM land here
	cnop	0,16
smkx_wram:	ds.b	$20000
		ds.b	$10000		; guard for dp,X / abs,X overruns
rt_rstack:	ds.b	32768		; the render context (the VBL's frame runs on it too)
rt_rstack_top:
rt_ustack:	ds.b	4096
rt_ustack_top:
rt_sstack:	ds.b	32768
rt_sstack_top:
