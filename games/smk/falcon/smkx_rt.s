; SMKX runtime: the Falcon side of the translated Super Mario Kart program.
;
; The translated code (build/smkx/smk_code.s) runs in supervisor mode with
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
	xref	dsp1_sine
	xref	smkx_input
	xref	smkx_video_init,smkx_render,smkx_render_poll,smkx_pal_used,smkx_pal_next,smkx_clut
	xref	smkx_m7_init,smkx_m7_on,smkx_m7_rows,smkx_m7_pages,smkx_m7_timeouts,smkx_m7_ssi
	xref	smkx_t_pal,smkx_t_smp,smkx_t_up,smkx_t_stream,smkx_thr_stat
	xdef	rt_m7_dirty,rt_m7_record,rt_m7_record_done,rt_vram_dirty
	xdef	rt_stuck,rt_vblcount,rt_frames
	xdef	smkx_oklab
	xdef	rt_vram,rt_cgram,rt_oam,rt_sram,rt_io_start,rt_irqlog,rt_nirqlog
	xdef	rt_cgram_start,rt_noamseg,rt_oamseg
	xdef	rt_bgofs_start,rt_m7_start,rt_m7ofs_start

	xdef	rt_rb,rt_rw,rt_wb,rt_ww,rt_iorb,rt_iorw,rt_iowb,rt_ioww
	xdef	rt_setdbr,rt_bit8,rt_bit16,rt_mvn,rt_mvp,rt_sei,rt_cli,rt_vf
	xdef	rt_w4203,rt_w4206,rt_w2122,rt_wapu0,rt_wapu1,rt_wapu2,rt_wapu3
	xdef	rt_pages_r
	xdef	rt_wdsp,rt_wdspw,rt_rdsp,rt_rdsp_sr,rt_rdspw,rt_rdsp_srw,rt_io,rt_apu_out
	xdef	rt_r4211,rt_r4212,rt_w4200,rt_w420b
	xdef	rt_dispatch,rt_probe,rt_trap,rt_unsupported,rt_badtable,rt_badexit,rt_noreturn,rt_tableoob
	xdef	smkx_wram,smkx_rom,smkx_romw
	ifd	SMKX_LC
	xref	smkx_clut8,smkx_lc_active,smkx_lc_window,smkx_lc_stat,smkx_ppu_stat,lc_prof,smkx_tile_stat
	xdef	rt_plan_dirty,rt_want8,rt_clut8_due,smkx_vid_set_window
	xdef	smkx_lc_cpu,smkx_lc_blit,smkx_lc_blit_busy,smkx_lc_dcache_clear
	xdef	rt_dsp_boot,ppu_image,ppu_image_bytes,ppuw_image,ppuw_image_bytes,ppulc_image,ppulc_image_bytes,ppulcw_image,ppulcw_image_bytes
	xdef	m7_kernel_full,m7_kernel_full_bytes
	endif

	ifnd	SMKX_FRAMES
SMKX_FRAMES	equ	0		; quit after this many frames (0 = never)
	endif

ROM_SIZE	equ	$80000
SAMPLES		equ	20000
IRQLOG_MAX	equ	256
IRQ_CHAIN_MAX	equ	8		; V-IRQs per frame
DCACHE_SIZE	equ	1024		; dispatch cache slots
OAMSEG_MAX	equ	IRQ_CHAIN_MAX+1	; snes_ppu.h: PPU_OAMSEG_MAX
OAMSEG_BYTES	equ	4+544
PAL_SNAP_BYTES	equ	96+48+256+4+2	; palette.h
	ifnd	SMKX_STATUS_EVERY
SMKX_STATUS_EVERY	equ	50	; frames between status lines
	endif
	ifnd	SMKX_RENDER
SMKX_RENDER	equ	1	; 0: no rendering at all (timing and reference runs); otherwise smkx_render is called every
				; frame and throttles itself (src/smkx_video.c)
	endif
	ifnd	SMKX_DIAG
SMKX_DIAG	equ	1	; 0: release build without per-call diagnostics
	endif
	ifnd	SMKX_SNAP_EVERY
SMKX_SNAP_EVERY	equ	0	; also write Fnnnnnnn.BIN every Nth frame (0 = off)
	endif
	ifnd	SMKX_PACE
SMKX_PACE	equ	1	; 1: no SNES frame while SMK's main loop is busy
	endif
	ifnd	SMKX_SUMS
SMKX_SUMS	equ	0	; test: log a WRAM sum before every NMI (tools/ref65816.c)
	endif
LOG_BYTES	equ	196608		; rt_print's text without NatFeats (SMKXLOG.TXT)
LOG_RESERVE	equ	8192		; ... of which the last bytes are for a crash report
VBL_VECTOR	equ	$70
TIMERC_VECTOR	equ	$114		; MFP timer C, 200 Hz under TOS
ACIA_VECTOR	equ	$118		; MFP GPIP4: keyboard and MIDI ACIAs

; NOTE_CALLER: remember the translated call site for status lines and the
; profile (diagnostics builds only).
NOTE_CALLER	macro
	ifne	SMKX_DIAG
	bsr	rt_note_caller
	endif
	endm

; M7DIRTY: after a VRAM byte write at byte offset d4.l, mark its 256-word
; page for the fast composer's tile-row cache (all 128 pages) and for the
; DSP Mode 7 kernel (words 0-$3FFF). Clobbers d4 and a0.
	ifd	SMKX_LC
; (SMKX_LC: all 128 pages for the list compositor's DSP program too: written since the DSP took the page
; (rt_m7_dirty), written since its planner looked (rt_plan_dirty))
M7DIRTY	macro
	lsr.w	#8,d4
	lsr.w	#1,d4
	lea	rt_vram_dirty,a0
	st	(a0,d4.w)
	lea	rt_m7_dirty,a0
	st	(a0,d4.w)
	lea	rt_plan_dirty,a0
	st	(a0,d4.w)
	endm
	else
M7DIRTY	macro
	lsr.w	#8,d4
	lsr.w	#1,d4
	lea	rt_vram_dirty,a0
	st	(a0,d4.w)
	cmpi.w	#64,d4
	bcc.s	.m7d\@
	lea	rt_m7_dirty,a0
	st	(a0,d4.w)
.m7d\@:
	endm
	endif


	section	text,code

; ---------------------------------------------------------------------------
; Startup

start:
	lea	rt_ustack_top,sp
	bsr	rt_m7_boot		; XBIOS DSP and sound calls: user mode
	clr.l	-(sp)
	move.w	#$20,-(sp)		; Super(0)
	trap	#1
	addq.l	#6,sp
	move.l	d0,rt_old_ssp
	lea	rt_sstack_top,sp

	bsr	rt_nf_init
	move.l	TIMERC_VECTOR.w,rt_old_tc	; saved here: rt_exit can run from
	move.l	VBL_VECTOR.w,rt_old_vbl		; any point below, a crash included
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

	bsr	rt_build_words
	bsr	rt_build_banks
	bsr	rt_build_pages
	jsr	smkx_video_init		; C: palette state
	bsr	rt_video_on
	lea	msg_m7off,a0
	tst.b	rt_m7_route
	beq.s	.m7msg
	jsr	smkx_m7_init
	tst.l	d0
	beq.s	.m7msg
	lea	msg_m7on,a0
	tst.b	smkx_m7_ssi
	beq.s	.m7msg
	lea	msg_m7ssi,a0
.m7msg:	bsr	rt_print

	move.w	#$2700,sr
	move.l	#rt_tc_sample,TIMERC_VECTOR.w	; 200 Hz PC samples in every context
	ifne	SMKX_PACE
	st	rt_main_busy		; no frame before the main loop first waits
	endif
	move.l	#rt_vbl,VBL_VECTOR.w
	move.l	#rt_acia,ACIA_VECTOR.w
	move.l	#smkx_input,rt_script_at
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
	jsr	smkx_reset
	lea	msg_reset_returned,a0
	bsr	rt_print
	moveq	#2,d7
	bra	rt_exit

; rt_exit: d7.w = TOS return code.
rt_exit:
	move.w	#$2700,sr
	move.l	rt_old_vbl,VBL_VECTOR.w
	move.l	rt_old_tc,TIMERC_VECTOR.w
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

; The big-endian word view: romw[i] = rom[i+1] << 8 | rom[i].
rt_build_words:
	lea	smkx_rom,a0
	lea	smkx_romw,a1
	move.l	#ROM_SIZE-2,d7
.loop:	move.b	1(a0),(a1)+
	move.b	(a0)+,(a1)+
	subq.l	#1,d7
	bpl.s	.loop
	move.b	smkx_rom,(a1)+		; the last word wraps to the start
	move.b	(a0),(a1)+
	rts

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

; SMK's IRQs upload the OAM buffers every frame without asking whether the
; main loop has finished them (only the NMI checks $44). On the SNES the
; main loop always finishes within the frame; here it takes several VBLs,
; and a frame in between showed half-built sprite tables (karts cut down
; to one 8x8 quarter, or missing). With SMKX_PACE a VBL that finds the main
; loop busy leaves the frame due, and the loop's wait for the NMI
; ($80805C, rt_mark_main_wait) runs it: every SNES frame sees a finished
; main loop, as on the console. A busy main loop gets no frame however long
; it runs (a VBL then only shows a finished picture): an earlier fallback
; after PACE_MAX VBLs ran IRQs and NMI in the middle of long intro passes,
; at points that depended on speed, and the game state with them (found
; with tools/ref65816.c). Frames also wait for the main loop's first wait:
; rt_main_busy is set from the start. SMK waits for the NMI nowhere else.
rt_vbl:
	move.l	2(sp),rt_last_pc		; the interrupted PC, for rt_status
	ifd	SMKX_LC
	movem.l	d0/a0-a1,-(sp)		; the Videl's plane count follows rt_want8 (smkx_video.s); with
	bsr	rt_vid_mode		; eight planes its palette is CGRAM (smkx_clut8), not the 16 colours
	tst.b	vid_eight
	beq.s	.c4
	tst.b	rt_clut8_due
	beq.s	.c8x
	sf	rt_clut8_due
	lea	smkx_clut8,a0
	lea	$ffff9800.w,a1
	move.w	#256-1,d0
.c8:	move.l	(a0)+,(a1)+
	dbra	d0,.c8
.c8x:	sf	rt_clut_due
	movem.l	(sp)+,d0/a0-a1
	bra.s	.clut
.c4:	movem.l	(sp)+,d0/a0-a1
	endif
	tst.b	rt_clut_due		; the shown frame's colours: the base
	beq.s	.clut			; set with them takes effect now
	sf	rt_clut_due
	movem.l	d0/a0-a1,-(sp)
	lea	smkx_clut,a0
	lea	$ffff9800.w,a1
	moveq	#16-1,d0
.cl:	move.l	(a0)+,(a1)+
	dbra	d0,.cl
	movem.l	(sp)+,d0/a0-a1
.clut:	bsr	rt_sample
	addq.l	#1,rt_vblcount
	tst.b	rt_in_frame
	bne.s	.busy
	ifne	SMKX_PACE
	tst.b	rt_main_busy
	beq.s	.run
	st	rt_frame_due		; the main loop's wait runs the frame
	bsr	rt_present
	bra.s	.chain
	endif
.run:	bsr	rt_vbl_frame
.chain:	move.l	rt_old_vbl,-(sp)
	rts
.busy:	addq.l	#1,rt_stuck		; a frame handler is still running
	ifne	SMKX_PACE
	st	rt_frame_due		; still one frame per VBL: the next one is
	endif				; due at the main loop's wait (not lost)
	move.l	rt_stuck,-(sp)
	andi.l	#127,(sp)+
	bne.s	.chain
	move.l	a0,-(sp)
	lea	msg_busy,a0
	bsr	rt_print
	move.l	(sp)+,a0
	bsr	rt_status
	bra.s	.chain

; SMKX_PACE: a VBL while the main loop is busy shows the DSP's frame as
; soon as it is in (smkx_render_poll finishes it: samples, palette, CLUT);
; the next one starts at the main loop's wait. Called with the VBL masked.
rt_present:
	tst.l	rt_screen
	beq	.x
	st	rt_in_frame
	movem.l	d0-d7/a0-a6,-(sp)
	bsr	rt_now
	move.l	d0,-(sp)
	move.w	#$2300,sr		; let the MFP (keyboard, timers) run
	jsr	smkx_render_poll
	move.w	#$2700,sr
	move.l	d0,d1
	bsr	rt_now
	sub.l	(sp)+,d0
	add.l	d0,rt_t_render
	add.l	d0,rt_t_vbl
	ifd	SMKX_LC
	cmpi.l	#4,d1			; the list compositor's picture went up
	bne.s	.n4
	addq.l	#1,rt_renders
	bra.s	.n
.n4:
	endif
	cmpi.l	#1,d1
	bne.s	.n
	addq.l	#1,rt_renders
	move.l	rt_back,d0
	move.l	rt_screen,rt_back
	move.l	d0,rt_screen
	bsr	vid_set_base
	ifd	SMKX_LC
	sf	rt_want8		; SMK's own frame: four planes again
	endif
	st	rt_clut_due
.n:	movem.l	(sp)+,d0-d7/a0-a6
	sf	rt_in_frame
.x:	rts

; One SNES frame (IRQs, render, NMI), from the VBL or the main loop's wait.
; Called with the VBL masked; returns so.
rt_vbl_frame:
	st	rt_in_frame
	clr.b	rt_frame_due
	movem.l	d0-d7/a0-a6,-(sp)
	bsr	rt_now
	tst.b	rt_waiting		; the main loop waited for this frame
	beq.s	.nw
	sf	rt_waiting
	move.l	d0,d1
	sub.l	rt_wait_t0,d1
	add.l	d1,rt_t_wait
.nw:	move.l	d0,-(sp)
	move.w	#$2300,sr		; let the MFP (keyboard, timers) run
	bsr	rt_frame
	move.w	#$2700,sr
	bsr	rt_now
	sub.l	(sp)+,d0
	add.l	d0,rt_t_vbl
	movem.l	(sp)+,d0-d7/a0-a6
	ifne	SMKX_PACE
	st	rt_main_busy		; the NMI set $44: the main loop runs a pass
	endif				; now (a VBL before $808060 found it idle)
	sf	rt_in_frame
	rts

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

; Timer C (IPL 6) interrupts the VBL handler too, so these samples cover
; the translated NMI and IRQs and rendering, not only the main loop. Ring of
; the latest SAMPLES (PC, last runtime call site); chains to TOS's handler.
rt_tc_sample:
	movem.l	d7/a0,-(sp)
	move.l	rt_tsampidx,d7
	addq.l	#1,rt_tsampidx
	cmpi.l	#SAMPLES-1,d7
	bcs.s	.in
	clr.l	rt_tsampidx
.in:	lsl.l	#3,d7
	lea	rt_tsamples,a0
	adda.l	d7,a0
	move.l	8+2(sp),d7		; interrupted PC
	sub.l	#start,d7
	tst.b	rt_in_frame
	beq.s	.main
	bset	#31,d7			; inside the VBL handler (NMI, IRQs, render)
.main:	move.l	d7,(a0)+
	move.l	rt_io_caller,d7
	sub.l	#start,d7
	move.l	d7,(a0)
	cmpi.l	#SAMPLES,rt_ntsamples
	bcc.s	.full
	addq.l	#1,rt_ntsamples
.full:	movem.l	(sp)+,d7/a0
	move.l	rt_old_tc,-(sp)
	rts

rt_dump_samples:
	lea	msg_tsamples,a0
	bsr	rt_print
	lea	rt_tsamples,a1
	move.l	rt_ntsamples,d5
	bsr.s	.list
	lea	msg_samples,a0
	bsr	rt_print
	lea	rt_samples,a1
	move.l	rt_nsamples,d5
.list:	bra.s	.next
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
	ifd	SMKX_HOLD
	tst.b	rt_hold			; held at the frame limit: the picture stays on the Videl (screenshots)
	bne.s	.held
	endif
	addq.l	#1,rt_frames
	clr.w	rt_vline		; the beam at the top again (SLHV/OPVCT; F030CT)
	tst.b	rt_quit			; Esc: leave cleanly
	beq.s	.run
	lea	msg_quit,a0
	bsr	rt_print
	bsr	rt_status
	moveq	#0,d7
	bra	rt_exit
	ifd	SMKX_HOLD
.held:	rts
	endif
.run:	movea.l	rt_script_at,a0		; scripted input (SMKX_INPUT): frame, pad
	move.l	(a0),d7
	bmi.s	.pad
	cmp.l	rt_frames,d7
	bhi.s	.pad
	move.w	6(a0),rt_script_pad
	addq.l	#8,rt_script_at
.pad:	move.w	rt_pad,d7		; auto-joypad read: $4218/$4219 latch here
	or.w	rt_script_pad,d7
	move.b	d7,rt_io+$2218
	lsr.w	#8,d7
	move.b	d7,rt_io+$2219
	ifd	SMKX_WATCH
	bsr	rt_watch
	endif
	move.l	rt_frames,d7
	divu.w	#SMKX_STATUS_EVERY,d7	; status lines (and the timing line)
	swap	d7
	tst.w	d7
	bne.s	.nolog
	bsr	rt_status
.nolog:
	bsr	rt_frame_start
	; V-count IRQs, when enabled and unmasked, run before the NMI, in line
	; order: a handler that sets VTIME to a later line gets called again
	; there (SMK chains its screen splits). Their PPU writes are logged
	; with the line they start at.
	clr.w	rt_irqline
	moveq	#IRQ_CHAIN_MAX-1,d7
	move.w	d7,rt_irqleft
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
	bsr	rt_entry_regs
	move.b	rt_iflag,-(sp)		; RTI restores the interrupted code's I flag
	st	rt_iflag		; taking the interrupt sets it (from F030SMW2)
	move.b	rt_vf,-(sp)		; and its V (the 68k SR comes back by rte)
	subq.l	#4,a6
	jsr	smkx_irq
	move.b	(sp)+,rt_vf
	move.b	(sp)+,rt_iflag
	bsr	rt_now
	sub.l	rt_t0,d0
	add.l	d0,rt_t_irq
	sf	rt_logging
	bsr	rt_oam_segment
	subq.w	#1,rt_irqleft
	bpl	.irq
.noirq:
	ifne	SMKX_SUMS
	tst.b	rt_nmitimen		; test switch: WRAM sum before each NMI,
	bpl.s	.nosum			; as tools/ref65816.c prints it
	bsr	rt_wram_sum
.nosum:
	endif
	move.l	#SMKX_FRAMES,d7
	beq.s	.render
	cmp.l	rt_frames,d7
	bhi.s	.render
	st	rt_final		; the last frame: drawn at once
	bsr	rt_render
	lea	snap_name,a0
	move.l	#'SNAP',(a0)+
	move.l	#'.BIN',(a0)+
	clr.b	(a0)
	lea	msg_frames,a0
	bsr	rt_print
	bsr	rt_status
	bsr	rt_dump_samples
	ifd	SMKX_IOSTATS
	bsr	rt_print_iostat
	endif
	bsr	rt_write_snapshot
	ifd	SMKX_DSP1_LOG
	bsr	rt_write_dsp1log
	endif
	ifd	SMKX_HOLD
	lea	msg_hold,a0
	bsr	rt_print
	st	rt_hold
	rts
	endif
	moveq	#0,d7
	bra	rt_exit
.render:
	ifd	SMKX_RENDER_FROM
	cmpi.l	#SMKX_RENDER_FROM,rt_frames	; no renders before this frame
	bcs.s	.nmi
	endif
	ifne	SMKX_RENDER
	bsr	rt_render
	endif
.nmi:	st	rt_rdnmi
	tst.b	rt_nmitimen
	bpl.s	.nonmi
	addq.l	#1,rt_nmis
	bsr	rt_now
	move.l	d0,rt_t0
	bsr	rt_entry_regs
	move.b	rt_iflag,-(sp)		; RTI restores the interrupted code's I flag
	st	rt_iflag		; taking the interrupt sets it (from F030SMW2)
	move.b	rt_vf,-(sp)		; and its V (the 68k SR comes back by rte)
	subq.l	#4,a6
	jsr	smkx_nmi
	move.b	(sp)+,rt_vf
	move.b	(sp)+,rt_iflag
	; The V-blank flag stands for the main loop again: on the SNES a poll
	; (LDA $4210 : BPL) sees it set before the NMI is taken, although the
	; NMI handler reads it too (from F030CT: its location change waited on
	; it for minutes).
	st	rt_rdnmi
	bsr	rt_now
	sub.l	rt_t0,d0
	add.l	d0,rt_t_nmi
.nonmi:
	rts

; The state the frame starts from: the register shadows and the latched
; scroll and Mode 7 values as the previous NMI left them.
rt_frame_start:
	movem.l	d0/a0-a1,-(sp)
	; only what the renderer reads: PPU $2100-$21FF, $4200-$421F and the
	; DMA/HDMA channels $4300-$437F (the rest of rt_io_start is not used)
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

; SNAP.BIN at a trap or CPU exception, once: WRAM, VRAM, registers as they
; were (the planes are the last composed frame). GEMDOS is safe: the game
; code never calls TOS.
rt_crash_snapshot:
	tas	rt_crash_snapped
	bne.s	.x
	move.w	#$2300,sr		; GEMDOS may need the MFP
	lea	snap_name,a0
	move.l	#'SNAP',(a0)+
	move.l	#'.BIN',(a0)+
	clr.b	(a0)
	bsr	rt_write_snapshot
	move.w	#$2700,sr
.x:	rts

	ifd	SMKX_WATCH
; SMKX_WATCH = a WRAM address ($7E0000-$7FFFFF): every VBL, report when the
; word there changed, with the frame, the interrupted PC (text offset) and
; the last dispatches.
rt_watch:
	movem.l	d4-d7/a0-a1,-(sp)
	lea	smkx_wram+(SMKX_WATCH-$7E0000),a0
	move.w	(a0),d6
	cmp.w	rt_watch_old,d6
	beq	.x
	move.w	d6,rt_watch_old
	lea	msg_watch,a0
	bsr	rt_print
	moveq	#0,d7
	move.w	d6,d7
	bsr	rt_print_hex8
	lea	msg_at,a0
	bsr	rt_print
	move.l	rt_frames,d7
	bsr	rt_print_hex8
	lea	msg_watch_pc,a0
	bsr	rt_print
	move.l	rt_last_pc,d7
	sub.l	#start,d7
	bsr	rt_print_hex8
	lea	msg_dhist,a0
	bsr	rt_print
	moveq	#7,d5
	move.l	rt_dhist_i,d4
.dh:	addq.l	#4,d4
	andi.l	#31,d4
	lea	rt_dhist,a1
	move.l	(a1,d4.l),d7
	bsr	rt_print_hex8
	lea	msg_space,a0
	bsr	rt_print
	dbra	d5,.dh
	lea	msg_nl,a0
	bsr	rt_print
.x:	movem.l	(sp)+,d4-d7/a0-a1
	rts
	endif

; rt_cglog: an IRQ's CGRAM data byte d6 into the IRQ log, as rt_io_write
; logs it. Preserves all registers.
rt_cglog:
	movem.l	d4/a0,-(sp)
	move.w	rt_nirqlog,d4
	cmpi.w	#IRQLOG_MAX,d4
	bcc.s	.x
	mulu.w	#6,d4
	lea	rt_irqlog,a0
	adda.w	d4,a0
	move.w	rt_irqline,(a0)+
	move.w	#$2122,(a0)+
	move.b	d6,(a0)+
	clr.b	(a0)
	addq.w	#1,rt_nirqlog
.x:	movem.l	(sp)+,d4/a0
	rts

	ifne	SMKX_SUMS
; rt_wram_sum: "SMKX sum NMIS SUM FRAMES", the sum of WRAM as big-endian longs
; without the stack page $1F00-$1FFF (the runtime writes no interrupt frames
; or return addresses there). From F030SMW.
rt_wram_sum:
	movem.l	d0/d6-d7/a0,-(sp)
	lea	smkx_wram,a0
	moveq	#0,d6
	move.w	#$1F00/4-1,d0
.lo:	add.l	(a0)+,d6
	dbra	d0,.lo
	lea	smkx_wram+$2000,a0
	move.l	#($20000-$2000)/4-1,d0
.hi:	add.l	(a0)+,d6
	subq.l	#1,d0
	bpl.s	.hi
	lea	msg_sum,a0
	bsr	rt_print
	move.l	rt_nmis,d7
	bsr	rt_print_hex8
	lea	msg_sp,a0
	bsr	rt_print
	move.l	d6,d7
	bsr	rt_print_hex8
	lea	msg_sp,a0
	bsr	rt_print
	move.l	rt_frames,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print
	movem.l	(sp)+,d0/d6-d7/a0
	rts
	endif

; rt_oam_segment: OAM as it is now, used from rt_irqline on.
rt_oam_segment:
	movem.l	d0/a0-a1,-(sp)
	move.l	rt_noamseg,d0
	beq.s	.new			; the frame's first segment always
	tst.b	rt_oam_dirty
	beq.s	.x			; unchanged: the last segment stays in use
.new:	sf	rt_oam_dirty
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

; Compose a frame into the back buffer (C: smkx_render), then show it:
; the Videl takes the new base at its next frame. With the DSP the frame
; is drawn while the game goes on: smkx_render answers 2 until it is in.
; Snapshot frames and the last one are drawn at once (sync), so that the
; snapshot's inputs are the frame's. When the DSP's frame is in (3), it is
; shown and the next one starts in the same call.
rt_render:
	tst.l	rt_screen
	beq	.no
	movem.l	d0-d7/a0-a6,-(sp)
.again:	moveq	#0,d2			; sync?
	tst.b	rt_final
	sne	d2
	ifne	SMKX_SNAP_EVERY
	move.l	rt_frames,d7
	divu.w	#SMKX_SNAP_EVERY,d7
	swap	d7
	tst.w	d7
	bne.s	.nsync
	moveq	#1,d2
.nsync:
	endif
	andi.l	#1,d2
	move.l	$4ba.w,-(sp)		; _hz_200: timer C runs above the VBL
	bsr	rt_now
	move.l	d0,-(sp)
	move.l	d2,-(sp)
	move.l	rt_back,-(sp)
	jsr	smkx_render
	addq.l	#8,sp
	move.l	d0,d1			; 0: forced blank, nothing composed
	bsr	rt_now
	sub.l	(sp)+,d0
	add.l	d0,rt_t_render
	move.l	$4ba.w,d0
	sub.l	(sp)+,d0
	add.l	d0,rt_render_ticks
	tst.l	d1
	bne.s	.drawn
	st	rt_clut_due		; forced blank: the black colours
	ifd	SMKX_LC
	sf	rt_want8
	endif
	bra	.blank
.drawn:	cmpi.l	#2,d1			; 2: the DSP is drawing it
	beq	.blank
	ifd	SMKX_LC
	cmpi.l	#5,d1			; 5: the list compositor has nothing new
	beq	.blank
	cmpi.l	#4,d1			; 4: its ring window is on the Videl already
	bne.s	.old
	addq.l	#1,rt_renders
	bra	.snap
.old:	sf	rt_want8		; SMK's own frame: four planes
	endif
	addq.l	#1,rt_renders
	ifd	SMKX_PROFILE
	bsr	rt_print_prof
	endif
	move.l	rt_back,d0
	move.l	rt_screen,rt_back
	move.l	d0,rt_screen
	bsr	vid_set_base
	st	rt_clut_due
	cmpi.l	#3,d1			; the DSP's frame: now start the next
	beq	.again
.snap:
	ifne	SMKX_SNAP_EVERY
	move.l	rt_frames,d7
	divu.w	#SMKX_SNAP_EVERY,d7
	swap	d7
	tst.w	d7
	bne.s	.nosnap
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

	ifd	SMKX_PROFILE
	xref	smkx_prof
; One line per render: frame, then smkx_prof[0..22] (src/smkx_video.c).
rt_print_prof:
	movem.l	d5-d7/a0-a1,-(sp)
	lea	msg_prof,a0
	bsr	rt_print
	move.l	rt_frames,d7
	bsr	rt_print_hex8
	lea	smkx_prof,a1
	moveq	#22,d5
.one:	lea	msg_space,a0
	bsr	rt_print
	move.l	(a1)+,d7
	bsr	rt_print_hex8
	dbra	d5,.one
	lea	msg_nl,a0
	bsr	rt_print
	movem.l	(sp)+,d5-d7/a0-a1
	rts
	endif

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
	xdef	rt_mark_main_start,rt_mark_main_end,rt_mark_main_wait
rt_mark_main_start:
	move.w	sr,-(sp)
	move.w	#$2700,sr
	st	rt_main_busy		; SMKX_PACE: no frames until the wait
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

; The main loop waits for the NMI ($80805C, every pass of the spin): the
; frame a VBL left due runs now (SMKX_PACE, see rt_vbl).
rt_mark_main_wait:
	move.w	sr,-(sp)
	move.w	#$2700,sr
	sf	rt_main_busy
	tst.b	rt_waiting		; the first pass of the spin: the wait
	bne.s	.w			; starts (rt_vbl_frame ends it)
	st	rt_waiting
	move.l	d0,-(sp)
	bsr	rt_now
	move.l	d0,rt_wait_t0
	move.l	(sp)+,d0
.w:	tst.b	rt_frame_due
	beq.s	.x
	tst.b	rt_in_frame
	bne.s	.x
	bsr	rt_vbl_frame
.x:	move.w	(sp)+,sr
	rts

; "SMKX time" frames, then cumulative: main-loop frames, main-loop time
; (without interrupts), NMI, IRQ, DSP-1 (inside both), rendering, whole
; VBL handler, native raster lines and time, firmware runs, VBLs.
rt_print_times:
	movem.l	d5-d7/a0-a1,-(sp)
	lea	msg_times,a0
	bsr	rt_print
	move.l	rt_frames,d7
	bsr	rt_print_hex8
	move.l	upd_runs,rt_n_upd
	lea	rt_n_main,a1
	moveq	#9,d5
.one:	lea	msg_space,a0
	bsr	rt_print
	move.l	(a1)+,d7
	bsr	rt_print_hex8
	dbra	d5,.one
	lea	msg_space,a0		; then the VBLs so far (with SMKX_PACE a
	bsr	rt_print		; frame is a main-loop pass, not a VBL)
	move.l	rt_vblcount,d7
	bsr	rt_print_hex8
	lea	msg_space,a0		; then the main loop's time waiting for
	bsr	rt_print		; the frame (1/38400 s)
	move.l	rt_t_wait,d7
	bsr	rt_print_hex8
	lea	msg_space,a0		; then the renderer's 68030 time: the
	bsr	rt_print		; palette, its samples, the uploads and
	move.l	smkx_t_pal,d7		; the stream (src/smkx_video.c,
	bsr	rt_print_hex8		; src/smkx_m7.c)
	lea	msg_space,a0
	bsr	rt_print
	move.l	smkx_t_smp,d7
	bsr	rt_print_hex8
	lea	msg_space,a0
	bsr	rt_print
	move.l	smkx_t_up,d7
	bsr	rt_print_hex8
	lea	msg_space,a0
	bsr	rt_print
	move.l	smkx_t_stream,d7
	bsr	rt_print_hex8
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
	moveq	#0,d7
	move.w	$36(a0),d7		; the main loop's state index ($36)
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
	move.l	rt_dspwrites,d7
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
	lea	msg_rtime,a0
	bsr	rt_print
	move.l	rt_renders,d7
	bsr	rt_print_hex8
	lea	msg_slash,a0
	bsr	rt_print
	move.l	rt_render_ticks,d7
	bsr	rt_print_hex8
	ifd	SMKX_VBLSTAT
	lea	msg_vbl,a0		; emulated time: VBLs (the vertical blanks that ran) and _hz_200
	bsr	rt_print
	move.l	rt_vblcount,d7
	bsr	rt_print_hex8
	lea	msg_slash,a0
	bsr	rt_print
	move.l	$4ba.w,d7
	bsr	rt_print_hex8
	lea	msg_thr,a0		; the render throttle: share << 16 | base lost << 8 | lost (last window)
	bsr	rt_print
	move.l	smkx_thr_stat,d7
	bsr	rt_print_hex8
	endif
	ifd	SMKX_LC
	lea	msg_tile,a0		; list compositor: entered/left/refused/boot failures/blank/probes, then
	bsr	rt_print		; frames shown/planned/cold/groups/copies/fallbacks
	lea	smkx_tile_stat,a1
	moveq	#8,d6
	bsr	rt_print_longs
	lea	msg_lc,a0
	bsr	rt_print
	lea	smkx_lc_stat,a1
	moveq	#6,d6
	bsr	rt_print_longs
	endif
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
	ifd	SMKX_DSP1_LOG
; DSP1LOG.BIN: dsp1_log's words (falcon/smkx_upd.s), for dsp1_stream.py.
rt_write_dsp1log:
	movem.l	d0-d7/a0-a6,-(sp)
	clr.w	-(sp)
	pea	dsp1log_name
	move.w	#$3c,-(sp)		; Fcreate
	trap	#1
	addq.l	#8,sp
	tst.l	d0
	bmi.s	.x
	move.w	d0,d7
	pea	dsp1_log
	move.l	dsp1_log_n,d0
	add.l	d0,d0
	move.l	d0,-(sp)
	move.w	d7,-(sp)
	move.w	#$40,-(sp)		; Fwrite
	trap	#1
	lea	12(sp),sp
	move.w	d7,-(sp)
	move.w	#$3e,-(sp)		; Fclose
	trap	#1
	addq.l	#4,sp
	lea	msg_dsp1log,a0
	bsr	rt_print
.x:	movem.l	(sp)+,d0-d7/a0-a6
	rts
	endif

	ifd	SMKX_LC
; rt_print_longs: a1 = longs, d6 = how many; hex, separated by slashes
rt_print_longs:
	movem.l	d6-d7/a0-a1,-(sp)
	subq.w	#1,d6
.pl:	move.l	(a1)+,d7
	movem.l	d6/a1,-(sp)
	bsr	rt_print_hex8
	movem.l	(sp)+,d6/a1
	subq.w	#1,d6
	bmi.s	.pe
	lea	msg_slash,a0
	movem.l	d6/a1,-(sp)
	bsr	rt_print
	movem.l	(sp)+,d6/a1
	bra.s	.pl
.pe:	movem.l	(sp)+,d6-d7/a0-a1
	rts
	endif

rt_write_snapshot:
	movem.l	d0-d7/a0-a6,-(sp)
	ifd	SMKX_LC
	sf	snap_is8
	jsr	smkx_lc_active		; the ring window shown: the picture as a plain eight-plane screen
	tst.l	d0
	beq.s	.plain
	pea	rt_screen_copy
	jsr	smkx_lc_window
	addq.l	#4,sp
	st	snap_is8
	bra.s	.noscr
.plain:
	endif
	move.l	rt_screen,d0
	beq.s	.noscr
	movea.l	d0,a0
	lea	rt_screen_copy,a1
	tst.b	rt_pitch72
	bne.s	.p72
	move.w	#28672/4-1,d0
.cp:	move.l	(a0)+,(a1)+
	dbra	d0,.cp
	bra.s	.noscr
.p72:	move.w	#224-1,d1		; 128 of every 144 bytes (the samples stay)
.ln:	moveq	#128/4-1,d0
.lc:	move.l	(a0)+,(a1)+
	dbra	d0,.lc
	lea	16(a0),a0
	dbra	d1,.ln
.noscr:	clr.w	-(sp)
	pea	snap_name
	move.w	#$3c,-(sp)		; Fcreate
	trap	#1
	addq.l	#8,sp
	tst.l	d0
	bmi	.fail
	move.w	d0,d7
	lea	snap_blocks,a3
	ifd	SMKX_LC
	tst.b	snap_is8
	beq.s	.blk
	lea	snap_blocks8,a3		; version 4 (src/tile/snes_ppu.h): eight planes
	endif
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

; rt_entry_regs: before the translated NMI or IRQ runs on the interrupted
; registers, rebuild what it relies on from the machine state: X and Y with
; zero upper words (the translated code indexes with d1.l and d2.l), a5 and
; the data bank (d3, a3, a4). The VBL may have interrupted a runtime helper
; that uses these registers as scratch (rt_dma does), and the handlers start
; with the bank unknown. (From F030CT.)
rt_entry_regs:
	lea	smkx_wram,a5		; constant, but C glue could hold anything
	andi.l	#$ffff,d1
	andi.l	#$ffff,d2
	moveq	#0,d7
	move.b	rt_dbr,d7
	bra	rt_setdbr

rt_setdbr:
	move.w	ccr,-(sp)
	move.b	d7,rt_dbr		; first: rt_entry_regs rebuilds from it
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

; Generic accesses go through page tables built from rt_resolve's rules:
; one entry per 256-byte page of the 24-bit address space, the host address
; of the page (0 = I/O). Writes use their own table: ROM pages point at a
; scratch page (writes to ROM are dropped).
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
	cmpi.w	#$2180,d7		; WMDATA straight away (from F030CT: its
	beq.s	.wm			; sprite builders read their lists through it)
	bsr	rt_io_read
	bra.s	.done
.wm:	move.l	rt_wmadd,d7		; as rt_io_read's .wmdata
	andi.l	#$1ffff,d7
	lea	smkx_wram,a1
	moveq	#0,d6
	move.b	(a1,d7.l),d6
	addq.l	#1,rt_wmadd
	bra.s	.done

rt_rw:
	NOTE_CALLER
	move.w	ccr,-(sp)		; both bytes on one mapped page: one lookup,
	movem.l	d7/a1,-(sp)		; one move and the byte swap
	move.l	a0,d7
	lsr.l	#8,d7
	andi.l	#$ffff,d7
	movea.l	(rt_pages_r,d7.l*4),a1
	cmpa.w	#0,a1
	beq.s	.slow
	move.l	a0,d7
	andi.l	#$ff,d7
	cmpi.w	#$ff,d7
	beq.s	.slow			; the high byte lies on the next page
	moveq	#0,d6
	move.w	(a1,d7.l),d6
	ror.w	#8,d6
	movem.l	(sp)+,d7/a1
	move.w	(sp)+,ccr
	rts
.slow:	movem.l	(sp)+,d7/a1
	move.w	(sp)+,ccr
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
	move.w	ccr,-(sp)		; both bytes on one mapped page: one move
	movem.l	d6-d7/a1,-(sp)
	move.l	a0,d7
	lsr.l	#8,d7
	andi.l	#$ffff,d7
	movea.l	(rt_pages_w,d7.l*4),a1
	cmpa.w	#0,a1
	beq.s	.slow
	move.l	a0,d7
	andi.l	#$ff,d7
	cmpi.w	#$ff,d7
	beq.s	.slow			; the high byte lies on the next page
	ror.w	#8,d6
	move.w	d6,(a1,d7.l)
	movem.l	(sp)+,d6-d7/a1
	move.w	(sp)+,ccr
	rts
.slow:	movem.l	(sp)+,d6-d7/a1
	move.w	(sp)+,ccr
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

; ---------------------------------------------------------------------------
; Direct entries for registers the translated code names at translation
; time (tools/smkx/emit.py, `direct_io`): the effects of rt_iowb/rt_iorb
; through rt_io_write/rt_io_read without their range checks and dispatch.
; Writes take the byte in d6, reads return it in d6; every other register
; and the CCR (the carry lives in X) are preserved. Writes that an IRQ
; must log fall back to the generic path.

rt_w4203:				; WRMPYB: the product into RDMPY
	move.w	ccr,-(sp)
	movem.l	d4-d5,-(sp)
	move.b	d6,rt_io+$2203
	moveq	#0,d4
	move.b	rt_io+$2202,d4
	moveq	#0,d5
	move.b	d6,d5
	mulu.w	d5,d4
	move.b	d4,rt_io+$2216
	lsr.w	#8,d4
	move.b	d4,rt_io+$2217
	movem.l	(sp)+,d4-d5
	move.w	(sp)+,ccr
	rts

rt_w4206:				; WRDIVB: quotient and remainder
	move.w	ccr,-(sp)
	movem.l	d4-d5/a0,-(sp)
	lea	rt_io,a0
	move.b	d6,$2206(a0)
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
	bra.s	.x
.div0:	st	$2214(a0)
	st	$2215(a0)
	move.b	$2204(a0),$2216(a0)
	move.b	$2205(a0),$2217(a0)
.x:	movem.l	(sp)+,d4-d5/a0
	move.w	(sp)+,ccr
	rts

rt_w2122:				; CGDATA: latch, then the colour word
	tst.b	rt_logging
	bne.s	.log
	move.w	ccr,-(sp)
	movem.l	d4/a0,-(sp)
	move.b	d6,rt_io+$122
	not.b	rt_cgflip
	beq.s	.hi
	move.b	d6,rt_cglatch
	bra.s	.x
.hi:	move.w	rt_cgadd,d4
	andi.w	#$1fe,d4
	lea	rt_cgram,a0
	move.b	rt_cglatch,(a0,d4.w)
	move.b	d6,1(a0,d4.w)
	andi.b	#$7f,1(a0,d4.w)
	addq.w	#2,rt_cgadd
.x:	movem.l	(sp)+,d4/a0
	move.w	(sp)+,ccr
	rts
.log:	movea.w	#$2122,a0		; during the frame's IRQ: logged
	bra	rt_iowb

; APU ports 0-3 (writes): the echo model in rt_apu_write.
APUW	macro
rt_wapu\1:
	tst.b	rt_logging
	bne.s	.log\@
	move.w	ccr,-(sp)
	movem.l	d4-d7/a0-a1,-(sp)
	move.b	d6,rt_io+$140+\1
	addq.l	#1,rt_apuwrites
	moveq	#\1,d7
	bsr	rt_apu_write
	movem.l	(sp)+,d4-d7/a0-a1
	move.w	(sp)+,ccr
	rts
.log\@:	movea.w	#$2140+\1,a0
	bra	rt_iowb
	endm
	APUW	0
	APUW	1
	APUW	2
	APUW	3

rt_r4211:				; TIMEUP: bit 7 once per V-count IRQ, cleared by reading
	move.w	ccr,-(sp)
	moveq	#0,d6
	tst.b	rt_timeup
	beq.s	.x
	move.b	#$80,d6
	sf	rt_timeup
.x:	move.w	(sp)+,ccr
	rts

rt_r4212:				; HVBJOY: V-blank and H-blank alternate
	move.w	ccr,-(sp)
	not.b	rt_hvb_toggle
	move.b	rt_hvb_toggle,d6
	andi.b	#$c0,d6
	move.w	(sp)+,ccr
	rts

rt_w4200:				; NMITIMEN
	move.b	d6,rt_io+$2200
	move.b	d6,rt_nmitimen
	rts

rt_w420b:				; MDMAEN: the channels' transfers now
	move.w	ccr,-(sp)
	move.b	d6,rt_io+$220B
	bsr	rt_dma
	move.w	(sp)+,ccr
	rts

rt_wdsp:				; DSP-1 data register
	move.w	ccr,-(sp)
	movem.l	d4-d7/a0,-(sp)
	addq.l	#1,rt_dspwrites
	move.w	#$6000,d7
	bsr	dsp1_write
	movem.l	(sp)+,d4-d7/a0
	move.w	(sp)+,ccr
	rts

; 16-bit write of the data register (low byte first). A native DSP-1
; command (UPD_NATIVE) takes the word at once; else two byte writes.
rt_wdspw:
	move.w	ccr,-(sp)
	movem.l	d4-d7/a0,-(sp)
	addq.l	#2,rt_dspwrites
	ifd	SMKX_DSP1_LLE
	ifne	UPD_NATIVE
	ifnd	SMKX_DSP1_LOG
	cmpi.b	#1,dsp1n_mode
	bne.s	.bytes
	tst.b	dsp1n_hi
	bne.s	.bytes
	bsr	dsp1n_word
	bra.s	.x
	endif
	endif
	endif
.bytes:	move.w	d6,-(sp)
	move.w	#$6000,d7
	bsr	dsp1_write		; the low byte
	move.w	(sp),d6
	lsr.w	#8,d6
	move.w	#$6001,d7
	bsr	dsp1_write		; the high byte
	addq.l	#2,sp
.x:	movem.l	(sp)+,d4-d7/a0
	move.w	(sp)+,ccr
	rts

rt_rdsp:				; DSP-1 data register
	move.w	ccr,-(sp)
	movem.l	d4-d5/d7/a0,-(sp)
	move.w	#$6000,d7
	bsr	dsp1_read
	movem.l	(sp)+,d4-d5/d7/a0
	move.w	(sp)+,ccr
	rts

rt_rdsp_sr:				; DSP-1 status register (its high byte)
	move.w	ccr,-(sp)
	movem.l	d4-d5/d7/a0,-(sp)
	move.w	#$7000,d7
	bsr	dsp1_read
	movem.l	(sp)+,d4-d5/d7/a0
	move.w	(sp)+,ccr
	rts

rt_rdspw:				; 16-bit read of the data register: low byte first
	move.w	ccr,-(sp)
	movem.l	d4-d5/d7/a0,-(sp)
	ifd	SMKX_DSP1_LLE
	ifne	UPD_NATIVE
	ifnd	SMKX_DSP1_LOG
	cmpi.b	#2,dsp1n_mode		; a native command's result: the word
	bne.s	.bytes
	tst.b	dsp1n_hi
	bne.s	.bytes
	bsr	dsp1n_rdword
	bra.s	.x
	endif
	endif
	endif
.bytes:	move.w	#$6000,d7
	bsr	dsp1_read
	move.b	d6,-(sp)
	move.w	#$6001,d7
	bsr	dsp1_read
	lsl.w	#8,d6
	move.b	(sp)+,d6
.x:	movem.l	(sp)+,d4-d5/d7/a0
	move.w	(sp)+,ccr
	rts

rt_rdsp_srw:				; 16-bit read of the status register
	move.w	ccr,-(sp)
	ifd	SMKX_DSP1_LLE
	ifne	UPD_NATIVE
	ifnd	SMKX_DSP1_LOG
	tst.b	dsp1n_mode		; a native command: SR as the host sees it
	beq.s	.fw			; (both bytes read SR's high byte)
	move.b	dsp1n_sr,d6
	lsl.w	#8,d6
	move.b	dsp1n_sr,d6
	move.w	(sp)+,ccr
	rts
.fw:
	endif
	endif
	endif
	movem.l	d4-d5/d7/a0,-(sp)
	move.w	#$7000,d7
	bsr	dsp1_read
	move.b	d6,-(sp)
	move.w	#$7001,d7
	bsr	dsp1_read
	lsl.w	#8,d6
	move.b	(sp)+,d6
	movem.l	(sp)+,d4-d5/d7/a0
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
	bfins	d6,d7{0:8}		; key: context << 24 | 24-bit address
	ifne	SMKX_DIAG
	move.l	rt_dhist_i,d4		; ring of the last 8 keys, for trap reports
	addq.l	#4,d4
	andi.l	#31,d4
	move.l	d4,rt_dhist_i
	lea	rt_dhist,a1
	move.l	d7,(a1,d4.l)
	endif
	; direct-mapped cache in front of the binary search; a slot's key and
	; handler move with one movem each, so an interrupting dispatch (the
	; VBL runs translated NMI and IRQ code) never sees half a slot
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
	bra	rt_fatal_sp
rt_unsupported:
	lea	msg_unsupported,a0
	bra	rt_fatal_sp
rt_noreturn:
	lea	msg_noreturn,a0
	bra	rt_fatal_sp
rt_tableoob:				; (sp) = the 65816 site, d1 = X past the table
	move.l	d1,rt_fault_arg
	lea	msg_tableoob,a0
	bsr	rt_print
	move.l	rt_fault_arg,d7
	bsr	rt_print_hex8
	lea	msg_at_site,a0
	move.l	(sp),rt_fault_arg
	bra	rt_fatal_arg
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
	lea	msg_dhist,a0		; the last dispatches, oldest first
	bsr	rt_print
	moveq	#7,d5
	move.l	rt_dhist_i,d4
.dh:	addq.l	#4,d4
	andi.l	#31,d4
	lea	rt_dhist,a1
	move.l	(a1,d4.l),d7
	bsr	rt_print_hex8
	lea	msg_space,a0
	bsr	rt_print
	dbra	d5,.dh
	lea	msg_at_site,a0
	move.l	rt_io_caller,rt_fault_arg
	bra	rt_fatal_arg
rt_fatal_sp:
	move.l	(sp),rt_fault_arg
	bra	rt_fatal_arg		; (it used to fall into rt_catch_exceptions)
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
	lea	msg_dhist,a0		; the last dispatches, oldest first
	bsr	rt_print
	moveq	#7,d5
	move.l	rt_dhist_i,d4
.dh:	addq.l	#4,d4
	andi.l	#31,d4
	lea	rt_dhist,a1
	move.l	(a1,d4.l),d7
	bsr	rt_print_hex8
	lea	msg_space,a0
	bsr	rt_print
	dbra	d5,.dh
	lea	msg_nl,a0
	bsr	rt_print
	bsr	rt_status
	bsr	rt_crash_snapshot
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
	bsr	rt_crash_snapshot
	moveq	#1,d7
	bra	rt_exit

; ---------------------------------------------------------------------------
; SNES I/O. d7 = 24-bit address; d6.b = value (write) or result (read).
; Callers (rt_rb/rt_wb) have saved the CCR, d7 and a1.

rt_io_read:
	movem.l	d4-d5/a0,-(sp)
	ifd	SMKX_IOSTATS
	move.l	d7,d4			; count per register: writes low, reads | $8000
	andi.l	#$7fff,d4
	ori.w	#$8000,d4
	lsl.l	#2,d4
	lea	rt_iostat,a0
	addq.l	#1,(a0,d4.l)
	endif
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
	cmpi.w	#$2137,d7
	beq.s	.slhv
	cmpi.w	#$213D,d7
	beq.s	.opvct
	cmpi.w	#$213E,d7
	beq.s	.stat77
	cmpi.w	#$213F,d7
	beq.s	.stat78
	bra.s	.shadow
; No beam here: every latch through SLHV is one scanline later (wrapping at
; 262), so a wait for a line (latch, OPVCT >= A) ends after that many polls
; instead of never (from F030CT: its BattleSys_WaitScanline hung).
.slhv:	move.w	rt_vline,d4
	addq.w	#1,d4
	cmpi.w	#262,d4
	bcs.s	.slhv1
	moveq	#0,d4
.slhv1:	move.w	d4,rt_vline
	move.w	d4,rt_vlatch
	bra	.shadow
.opvct:	not.b	rt_opvct_ff		; low byte first, then bit 8
	beq.s	.opvhi
	move.b	rt_vlatch+1,d6
	bra	.out
.opvhi:	move.b	rt_vlatch,d6
	andi.b	#1,d6
	bra	.out
.mpy:	move.l	rt_mpy,d6
	subi.w	#$2134,d7
	lsl.w	#3,d7
	lsr.l	d7,d6
	bra	.out
.stat77:
	moveq	#$01,d6
	bra	.out
.stat78:
	sf	rt_opvct_ff		; reading STAT78 resets the OPHCT/OPVCT flip-flops
	moveq	#$03,d6
	bra	.out
.vramlo:
	moveq	#0,d4
	move.w	rt_vmadd,d4
	add.l	d4,d4			; up to $FFFE: a long index
	lea	rt_vram,a0
	move.b	(a0,d4.l),d6
	tst.b	rt_vminc_high
	bne	.out
	bsr	rt_vram_step
	bra	.out
.vramhi:
	moveq	#0,d4
	move.w	rt_vmadd,d4
	add.l	d4,d4
	lea	rt_vram+1,a0
	move.b	(a0,d4.l),d6
	tst.b	rt_vminc_high
	beq	.out
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
.hvbjoy:
	not.b	rt_hvb_toggle		; alternate V-blank and H-blank together, so
	move.b	rt_hvb_toggle,d6	; waits for either flag set or clear end;
	andi.b	#$c0,d6			; auto-joypad read is never busy
	bra	.out
.dsp:	bsr	dsp1_read
	bra	.out

rt_io_write:
	movem.l	d4-d5/a0,-(sp)
	andi.l	#$ffff,d7
	ifd	SMKX_IOSTATS
	move.l	d7,d4			; count per register: writes low, reads | $8000
	andi.l	#$7fff,d4
	
	lsl.l	#2,d4
	lea	rt_iostat,a0
	addq.l	#1,(a0,d4.l)
	endif
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
	st	rt_oam_dirty
	lea	rt_oam,a0
	move.b	rt_oamlatch,-1(a0,d4.w)
	move.b	d6,(a0,d4.w)
	bra.s	.oaminc
.oamhigh:
	st	rt_oam_dirty
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
	bra	.out
.vmaddh:
	move.b	d6,rt_vmadd
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
.dsp:	addq.l	#1,rt_dspwrites
	bsr	dsp1_write
	bra	.out
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
;   state 2  driver: every port echoes; $1F on port 0 is SMK's request to
;            return to the IPL for another upload (CODE_81F4FD's callers)
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
	cmpi.b	#$1f,d6
	bne.s	.done
	clr.b	rt_apu_state		; back to the IPL
	move.l	#$aabb0000,(a0)
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

; rt_m7_mark: d4.w = first VRAM word, d3.l = bytes; marks every Mode 7
; page the word copy touches. Preserves all registers.
rt_m7_mark:
	movem.l	d2/d6-d7/a1,-(sp)
	move.w	d4,d6
	move.l	d3,d7
	lsr.l	#1,d7			; words
	beq.s	.x
	lea	rt_m7_dirty,a1
.pg:	move.w	d6,d2
	lsr.w	#8,d2
	st	(rt_vram_dirty,d2.w)	; every page: the tile-row cache
	ifd	SMKX_LC
	st	(a1,d2.w)		; the list compositor's DSP program: every page
	st	(rt_plan_dirty,d2.w)
	else
	cmpi.w	#$4000,d6
	bcc.s	.skip
	move.w	d6,d2
	lsr.w	#8,d2
	st	(a1,d2.w)
	endif
.skip:	moveq	#0,d2
	move.b	d6,d2
	neg.w	d2
	addi.w	#256,d2			; words to the end of this page
	add.w	d2,d6
	andi.w	#$7fff,d6
	sub.l	d2,d7
	bgt.s	.pg
.x:	movem.l	(sp)+,d2/d6-d7/a1
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
	bmi	.mpydma			; B -> A
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
	beq.s	.vram
	cmpi.b	#$19,d6
	beq.s	.vram
	cmpi.b	#$80,d6
	bne	.bytes
	bsr	.onereg
	beq	.wram
	bra	.bytes

; VRAM (from games/smw/F030DKC): mode 1 at $2118 writes word pairs, modes
; 0/2/6 one half; any increment, fixed or incrementing source. The Mode 7
; pages between the first and the last word are marked.
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
	bra	.vend
.vpair:	cmpi.b	#$18,1(a2)
	bne	.vgen
	tst.b	rt_vminc_high
	beq	.vgen
	btst	#0,d3
	bne	.vgen
	lsr.l	#1,d7			; pairs - 1
	move.w	rt_vminc,d2
	cmpa.w	#1,a1			; source and VRAM both step by one: a copy
	bne	.vp
	cmpi.w	#1,d2
	bne	.vp
	move.l	d7,d6
	addq.l	#1,d6			; pairs
	moveq	#0,d5
	move.w	d4,d5
	add.l	d6,d5
	cmpi.l	#$8000,d5
	bhi	.vp			; it would wrap: per pair
	moveq	#0,d5
	move.w	d4,d5
	add.l	d5,d5
	lea	(a4,d5.l),a0
	add.w	d6,d4			; the address after the last pair
	andi.w	#$7fff,d4
	move.l	d6,d5
	lsr.l	#1,d5			; long words
	bra.s	.vcl1
.vcl:	move.l	(a3)+,(a0)+
.vcl1:	subq.l	#1,d5
	bpl.s	.vcl
	btst	#0,d6
	beq	.vend
	move.w	(a3)+,(a0)+
	bra	.vend
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
	bra.s	.dmadone
.vgen:	addq.l	#2,sp
	bra	.bytes
; Z set when transfer mode d2 writes a single register (0, 2, 6).
.onereg:
	cmpi.w	#0,d2
	beq.s	.one
	cmpi.w	#2,d2
	beq.s	.one
	cmpi.w	#6,d2
.one:	rts

; WRAM through $2180 (from F030DKC).
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
; The paths above may take d5 as scratch; a fixed source stays where it was.
.dmadone:
	move.l	a1,d4
	beq.s	.cleared
	moveq	#0,d5			; the A-bus address again
	move.b	3(a2),d5
	lsl.w	#8,d5
	move.b	2(a2),d5
	bra	.done
.cleared:
	clr.b	5(a2)
	clr.b	6(a2)
	bra	.nextc
.bytes:	; OAM and CGRAM uploads (mode 0 to $2104 / $2122) copy directly, with
	; the ports' own address, latch and wrap rules (rt_io_write .oamdata,
	; .cgdata). During an IRQ the CGRAM bytes are logged as the byte path
	; logs them (rt_cglog).
	move.l	a1,d4			; the byte paths below: incrementing source only
	beq	.slow
	bsr	.onereg			; modes 0, 2, 6 write one register: for OAM
	bne	.generic		; and CGRAM they are the same transfer
	cmpi.b	#$04,1(a2)
	beq	.oamdma
	cmpi.b	#$22,1(a2)
	bne	.generic
	movem.l	d0-d1,-(sp)		; channel number and mask of the loop
	lea	rt_cgram,a4
	move.w	rt_cgadd,d4
	move.b	rt_cgflip,d0
	move.b	rt_cglatch,d1
	move.l	d3,d7
	subq.l	#1,d7
	tst.b	rt_logging
	bne.s	.cb
	tst.b	d0
	bne.s	.cb			; a low byte is latched already
	addq.l	#1,d7			; bytes
.cbp:	subq.l	#2,d7			; no IRQ logs them: pairs as colour words
	bmi.s	.cbq			; (from F030CT)
	move.w	d4,d2
	andi.w	#$1fe,d2
	move.b	(a3)+,d1
	move.b	d1,(a4,d2.w)
	move.b	(a3)+,d6
	andi.b	#$7f,d6
	move.b	d6,1(a4,d2.w)
	addq.w	#2,d4
	bra.s	.cbp
.cbq:	addq.l	#1,d7			; -1: done, 0: one low byte left
	bmi.s	.cbd
.cb:	move.b	(a3)+,d6
	tst.b	rt_logging
	beq.s	.cbl
	bsr	rt_cglog
.cbl:	not.b	d0
	beq.s	.cbh
	move.b	d6,d1
	bra.s	.cbn
.cbh:	move.w	d4,d2
	andi.w	#$1fe,d2
	move.b	d1,(a4,d2.w)
	andi.b	#$7f,d6
	move.b	d6,1(a4,d2.w)
	addq.w	#2,d4
.cbn:	subq.l	#1,d7
	bpl.s	.cb
.cbd:	move.w	d4,rt_cgadd
	move.b	d0,rt_cgflip
	move.b	d1,rt_cglatch
	move.b	-1(a3),rt_io+$122	; the port's shadow: the last byte
	movem.l	(sp)+,d0-d1
	bra	.done
.oamdma:
	movem.l	d0-d1,-(sp)		; channel number and mask of the loop
	st	rt_oam_dirty
	lea	rt_oam,a4
	move.w	rt_oamadd,d4
	move.b	rt_oamlatch,d0
	move.l	d3,d7
	; From an even address in the low table, pairs land as a plain copy:
	; move them in one go, then the rest byte by byte.
	btst	#0,d4
	bne.s	.obrest
	move.w	#$200,d1
	sub.w	d4,d1			; bytes left in the low table
	ext.l	d1
	cmp.l	d7,d1
	bls.s	.oblen
	move.l	d7,d1
.oblen:	andi.l	#$fffffffe,d1		; whole pairs
	beq.s	.obrest
	sub.l	d1,d7
	lea	(a4,d4.w),a0
	add.w	d1,d4
	move.b	-2(a3,d1.l),d0		; the latch holds the last pair's first byte
	lsr.l	#1,d1
	subq.l	#1,d1
.obw:	move.b	(a3)+,(a0)+
	move.b	(a3)+,(a0)+
	dbra	d1,.obw
	andi.w	#$3ff,d4
.obrest:
	subq.l	#1,d7
	bmi.s	.obend
.ob:	move.b	(a3)+,d6
	cmpi.w	#$200,d4
	bcc.s	.obh
	btst	#0,d4
	bne.s	.obo
	move.b	d6,d0
	bra.s	.obi
.obo:	move.b	d0,-1(a4,d4.w)
	move.b	d6,(a4,d4.w)
	bra.s	.obi
.obh:	move.w	d4,d1
	andi.w	#$21f,d1
	move.b	d6,(a4,d1.w)
.obi:	addq.w	#1,d4
	andi.w	#$3ff,d4
	subq.l	#1,d7
	bpl.s	.ob
.obend:	move.w	d4,rt_oamadd
	move.b	d0,rt_oamlatch
	move.b	-1(a3),rt_io+$104	; the port's shadow: the last byte
	movem.l	(sp)+,d0-d1
	bra	.done
.generic:	; per byte through the register handlers, source from the host pointer
	ifd	SMKX_IOSTATS
	movem.l	d4/a0,-(sp)		; bytes per B-bus register and path
	moveq	#0,d4
	move.b	1(a2),d4
	ori.w	#$100,d4
	lsl.l	#2,d4
	lea	rt_dmastat,a0
	add.l	d3,(a0,d4.l)
	movem.l	(sp)+,d4/a0
	endif
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
.mpydma: ; B -> A from the multiplier's product ($2134-$2136, as the Mode 7
	; "multiply by DMA" trick reads it) into WRAM: reading the product
	; changes nothing, so its bytes are read once per pattern step and
	; filled in. Everything else B -> A goes per byte.
	btst	#4,d2
	bne	.slow			; decrementing destination
	moveq	#0,d6
	move.b	1(a2),d6
	movea.l	d6,a3			; B-bus register
	andi.w	#7,d2
	lsl.w	#2,d2
	lea	dma_patterns,a4
	adda.w	d2,a4			; the mode's four B-bus offsets
	moveq	#0,d4			; their bytes, first step in the top byte
	moveq	#3,d2
.bap:	move.l	a3,d7
	add.b	(a4)+,d7
	cmpi.b	#$34,d7
	bcs	.slow
	cmpi.b	#$36,d7
	bhi	.slow
	andi.w	#$ff,d7
	addi.w	#$2100,d7
	bsr	rt_io_read
	lsl.l	#8,d4
	move.b	d6,d4
	dbra	d2,.bap
	suba.l	a4,a4			; destination step: 0 when fixed
	btst	#3,(a2)
	bne.s	.bafix
	addq.l	#1,a4
	move.l	d5,d7
	andi.l	#$ffff,d7
	add.l	d3,d7
	cmpi.l	#$10000,d7
	bhi	.slow			; would wrap inside the bank
.bafix:	movea.l	d5,a0
	bsr	rt_resolve
	cmpa.l	#smkx_wram,a1
	bcs	.slow			; not WRAM (I/O, ROM, SRAM)
	move.l	a1,d7
	add.l	d3,d7
	cmpi.l	#smkx_wram+$20000,d7
	bhi	.slow
	move.l	d3,d7
	cmpa.w	#1,a4			; one byte repeated into consecutive bytes
	bne.s	.bafb			; (F030CT's title fills its OAM buffer with $E0
	move.l	d4,d2			; that way: M7A = 1, M7B = $E0, 512 bytes):
	rol.l	#8,d2			; long words
	cmp.l	d4,d2
	bne.s	.bafb
.bafe:	move.l	a1,d2			; bytes up to a long boundary
	andi.w	#3,d2
	beq.s	.bafl
	move.b	d4,(a1)+
	subq.l	#1,d7
	bne.s	.bafe
	bra.s	.bafx
.bafl:	move.l	d7,d2
	lsr.l	#2,d2
	beq.s	.baft
.bafll:	move.l	d4,(a1)+
	subq.l	#1,d2
	bne.s	.bafll
.baft:	andi.w	#3,d7
	beq.s	.bafx
.baftb:	move.b	d4,(a1)+
	subq.w	#1,d7
	bne.s	.baftb
	bra.s	.bafx
.bafb:	subq.l	#1,d7
.baf:	rol.l	#8,d4
	move.b	d4,(a1)
	adda.l	a4,a1
	subq.l	#1,d7
	bpl.s	.baf
.bafx:
	move.l	a4,d4
	beq	.cleared
	bra	.done
.slow:
	ifd	SMKX_IOSTATS
	movem.l	d4/a0,-(sp)		; bytes per B-bus register and path
	moveq	#0,d4
	move.b	1(a2),d4
	ori.w	#$200,d4
	lsl.l	#2,d4
	lea	rt_dmastat,a0
	add.l	d3,(a0,d4.l)
	movem.l	(sp)+,d4/a0
	endif
	subq.l	#1,d3
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
; Keyboard as joypad 1 (from F030SMW). The handler replaces TOS's for the
; program's lifetime: it drains both ACIAs, skips IKBD packets (mouse,
; joystick, clock, status) and keeps the SNES button word in rt_pad, which
; rt_frame latches into $4218/$4219 once per frame. Keys (by Atari scancode,
; i.e. position on a QWERTY layout): arrows = D-pad, X = B, Z = Y, S = A,
; A = X, Q = L, W = R, Return = Start, Space = Select, Esc = quit.

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
	move.l	#rt_log_buf,rt_log_ptr		; no NatFeats: keep the text for SMKXLOG.TXT
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
; text rt_print collected goes to SMKXLOG.TXT in the current directory.
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

	ifd	SMKX_DSP1_LLE
	include	'smkx_upd.s'		; the DSP-1's own program, translated
	else
	include	'smkx_dsp1.s'		; geometric model, when no firmware is present
	endif

	section	text,code
	include	'smkx_video.s'
	ifd	SMKX_LC
	include	'smkx_dspboot.s'	; the DSP booted without XBIOS: SMK's program and the compositor's
	include	'smkx_lcput.s'		; the compositor's walker and block copies
	endif
	include	'smkx_m7dsp.s'

	section	text,code
rt_text_end:

	section	data,data

nf_name:		dc.b	"NF_STDERR",0
msg_start:		dc.b	"SMKX: translated SMK starting",10,0
msg_reset_returned:	dc.b	"SMKX: reset procedure returned",10,0
msg_quit:	dc.b	"SMKX: Esc pressed, leaving",10,0
msg_frames:		dc.b	"SMKX: frame limit reached",10,0
	ifd	SMKX_IOSTATS
msg_iostat:		dc.b	"SMKX io ",0
msg_dmastat:		dc.b	"SMKX dma ",0
	even
; Registers with at least 1000 accesses: "SMKX io 0000aaaa count", reads
; with bit 15 set in the address.
	section	text,code
rt_print_iostat:
	movem.l	d5-d7/a0-a1,-(sp)
	lea	rt_dmastat,a1		; "SMKX dma 0000psrr bytes": path 1 generic, 2 slow
	moveq	#0,d5
.dma:	move.l	(a1)+,d6
	beq.s	.dnext
	lea	msg_dmastat,a0
	bsr	rt_print
	move.l	d5,d7
	bsr	rt_print_hex8
	lea	msg_space,a0
	bsr	rt_print
	move.l	d6,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print
.dnext:	addq.l	#1,d5
	cmpi.l	#$300,d5
	bne.s	.dma
	lea	rt_iostat,a1
	moveq	#0,d5
.one:	move.l	(a1)+,d6
	cmpi.l	#1000,d6
	bcs.s	.next
	lea	msg_iostat,a0
	bsr	rt_print
	move.l	d5,d7
	bsr	rt_print_hex8
	lea	msg_space,a0
	bsr	rt_print
	move.l	d6,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print
.next:	addq.l	#1,d5
	cmpi.l	#$10000,d5
	bne.s	.one
	movem.l	(sp)+,d5-d7/a0-a1
	rts
	section	bss,bss
rt_iostat:		ds.l	$10000
rt_dmastat:		ds.l	$300
	section	data,data
	endif
msg_status:		dc.b	"SMKX: frame ",0
msg_nmi:		dc.b	" nmi ",0
msg_state:		dc.b	" state ",0
msg_dma:		dc.b	" dma ",0
msg_apu:		dc.b	" apu ",0
msg_dsp:		dc.b	" dsp1 ",0
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
msg_m7ssi:		dc.b	"SMKX: Mode 7 on the DSP, its commands by DMA playback",10,0
msg_m7off:		dc.b	"SMKX: Mode 7 on the CPU",10,0
msg_m7rows:		dc.b	" m7dsp ",0
msg_rtime:		dc.b	" renders/ticks ",0
msg_slash:		dc.b	"/",0
msg_prof:		dc.b	"SMKX prof ",0
msg_times:		dc.b	"SMKX time ",0
msg_wram_at:		dc.b	"SMKX: WRAM at $",0
msg_text_at:		dc.b	", text at $",0
msg_tsamples:		dc.b	"SMKX tsamples",10,0
dsp1log_name:		dc.b	"DSP1LOG.BIN",0
msg_dsp1log:		dc.b	"SMKX: wrote DSP1LOG.BIN",10,0
msg_exc:		dc.b	"SMKX CPU exception, vector $",0
msg_exc_pc:		dc.b	" at text +",0
msg_dhist:		dc.b	"; last dispatches (ctx<<24|addr) ",0
msg_watch:		dc.b	"SMKX watch: word now $",0
msg_watch_pc:		dc.b	", interrupted pc +",0
msg_exc_regs:		dc.b	"SMKX regs d0-d7 a0-a7 ",0
msg_exc_stack:		dc.b	"SMKX stack (text offsets) ",0
msg_space:		dc.b	" ",0
snap_name:		dc.b	"SNAP.BIN",0,0,0,0,0	; or Fnnnnnnn.BIN
log_name:		dc.b	"SMKXLOG.TXT",0
	even
snap_magic:		dc.b	"SMKXSNAP",0,0,0,3
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
	dc.l	28672,rt_screen_copy
	dc.l	PAL_SNAP_BYTES,smkx_pal_used
	dc.l	PAL_SNAP_BYTES,smkx_pal_next
	dc.l	4+OAMSEG_MAX*OAMSEG_BYTES,rt_noamseg
	dc.l	0
	ifd	SMKX_LC
; The same for a picture of the list compositor (eight planes of CGRAM indices): the layout of src/tile/snes_ppu.h
; (version 4, F030CT's): SRAM padded to 8 KiB, a planes word, the 57,344-byte frame, the two palette blocks
; (empty: no adaptive palette here).
snap_magic8:		dc.b	"SMKXSNAP",0,0,0,4
snap_planes8:		dc.l	8
snap_blocks8:
	dc.l	12,snap_magic8
	dc.l	$10000,rt_vram
	dc.l	512,rt_cgram_start
	dc.l	544,rt_oam
	dc.l	$6000,rt_io_start
	dc.l	16+12+4,rt_bgofs_start
	dc.l	4,rt_nlog32
	dc.l	IRQLOG_MAX*6,rt_irqlog
	dc.l	$20000,smkx_wram
	dc.l	$800,rt_sram
	dc.l	8192-$800,snap_zero
	dc.l	4,snap_planes8
	dc.l	57344,rt_screen_copy
	dc.l	2*PAL_SNAP_BYTES,snap_zero
	dc.l	4+OAMSEG_MAX*OAMSEG_BYTES,rt_noamseg
	dc.l	0
	endif
	ifd	SMKX_LC
msg_tile:		dc.b	" tile ",0
msg_lc:			dc.b	" lc ",0
	endif
	ifd	SMKX_VBLSTAT
msg_vbl:		dc.b	" vbl ",0
msg_thr:		dc.b	" thr ",0
	endif
	ifd	SMKX_HOLD
msg_hold:		dc.b	"SMKX: holding the last picture",10,0
	endif
msg_samples:		dc.b	"SMKX samples",10,0
msg_sp:			dc.b	" ",0
msg_sum:		dc.b	"SMKX sum ",0
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
msg_tableoob:		dc.b	"SMKX jump table index past the translated table: X $",0
msg_at_site:		dc.b	" to the call at 65816 $",0
msg_dispatch:		dc.b	"SMKX pointer dispatch to an untranslated target (ctx<<24|addr) $",0
	even

smkx_rom:
	incbin	"smk.sfc"
	even
smkx_oklab:				; BGR555 -> OKLab (tools/smkx/oklab.py)
	incbin	"oklab555.bin"

	section	bss,bss

rt_nf_stderr:	ds.l	1
rt_log_ptr:	ds.l	1
rt_log_lim:	ds.l	1
rt_log_buf:	ds.b	LOG_BYTES
rt_log_end:
rt_old_ssp:	ds.l	1
rt_old_vbl:	ds.l	1
rt_old_exc:	ds.l	EXC_LAST-EXC_FIRST+1
rt_exc_pc:	ds.l	1
rt_exc_regs:	ds.l	16
rt_exit_code:	ds.w	1
rt_old_acia:	ds.l	1
rt_pad:		ds.w	1		; SNES buttons, $4219 << 8 | $4218
rt_script_pad:	ds.w	1
rt_script_at:	ds.l	1
rt_quit:	ds.b	1
rt_kbd_skip:	ds.b	1
rt_vblcount:	ds.l	1
rt_frames:	ds.l	1
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
rt_ntsamples:	ds.l	1
rt_tsampidx:	ds.l	1
rt_old_tc:	ds.l	1
rt_nprobed:	ds.w	1
rt_probed:	ds.l	64
rt_tsamples:	ds.l	2*SAMPLES
rt_samples:	ds.l	2*SAMPLES
rt_io_caller:	ds.l	1
rt_io_addr:	ds.l	1
rt_mpy:		ds.l	1
rt_wmadd:	ds.l	1
rt_vline:	ds.w	1		; the modelled scanline (SLHV latches)
rt_vlatch:	ds.w	1
rt_m7a:		ds.w	1
rt_bgofs:	ds.w	8		; BG1-4 H/V scroll, as the PPU latched them
rt_m7:		ds.w	6		; Mode 7 A, B, C, D, X, Y
rt_m7ofs:	ds.w	2		; Mode 7 H/V scroll
rt_bgofs_start:	ds.w	8		; the same at the start of the frame
rt_m7_start:	ds.w	6
rt_m7ofs_start:	ds.w	2
	ifd	SMKX_HOLD
rt_hold:	ds.b	1		; the frame limit was reached: no more frames
	endif
	ifd	SMKX_LC
snap_is8:	ds.b	1		; the snapshot is the compositor's eight-plane picture
	even
snap_zero:	ds.b	6144		; (never written: padding of the version 4 layout)
	endif
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
rt_final:	ds.b	1		; rendering the last frame (sync)
	even
rt_render_ticks:	ds.l	1	; 200 Hz ticks spent in smkx_render
; rt_print_times prints these ten in this order
rt_t_wait:	ds.l	1		; time the main loop spent waiting
rt_wait_t0:	ds.l	1
rt_n_main:	ds.l	1
rt_t_main:	ds.l	1
rt_t_nmi:	ds.l	1
rt_t_irq:	ds.l	1
rt_t_dsp1:	ds.l	1
rt_t_render:	ds.l	1
rt_t_vbl:	ds.l	1
rt_n_hle:	ds.l	1		; native DSP-1 raster lines
rt_t_hle:	ds.l	1		; their time
rt_n_upd:	ds.l	1		; firmware runs (host accesses)
rt_t0:		ds.l	1
rt_now_last:	ds.l	1
rt_main_t0:	ds.l	1
rt_main_vbl0:	ds.l	1
rt_main_open:	ds.b	1
	even
rt_watch_old:	ds.w	1
rt_crash_snapped:	ds.b	1
	even
rt_dhist:	ds.l	8
rt_dhist_i:	ds.l	1
	even
rt_pages_r:	ds.l	65536
rt_pages_w:	ds.l	65536
rt_dcache:	ds.l	2*DCACHE_SIZE
rt_trash:	ds.b	256
rt_oam_dirty:	ds.b	1
	even
rt_vmadd:	ds.w	1
rt_vminc:	ds.w	1
rt_cgadd:	ds.w	1
rt_oamadd:	ds.w	1
rt_in_frame:	ds.b	1
rt_main_busy:	ds.b	1		; SMKX_PACE: main_start until the wait
rt_waiting:	ds.b	1		; the main loop spins at its NMI wait
rt_frame_due:	ds.b	1		; a VBL came while the main loop was busy
rt_clut_due:	ds.b	1		; smkx_clut goes to the Videl at the next VBL
rt_nmitimen:	ds.b	1
rt_rdnmi:	ds.b	1
rt_opvct_ff:	ds.b	1		; OPVCT: the next read gives bit 8
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
rt_dbr:		ds.b	1		; the data bank, as rt_setdbr set it
rt_vram_dirty:	ds.b	128		; per 256-word VRAM page: src/ppu_fast.c drops its rows
	even
rt_io:		ds.b	$6000		; shadows of $2000-$7FFF
rt_vram:	ds.b	$10000
rt_cgram:	ds.b	512
rt_oam:		ds.b	544
rt_cgram_start:	ds.b	512
rt_noamseg:	ds.l	1
rt_oamseg:	ds.b	OAMSEG_MAX*OAMSEG_BYTES
rt_sram:	ds.b	$800
	cnop	0,16
smkx_wram:	ds.b	$20000
		ds.b	$10000		; guard for dp,X / abs,X overruns
smkx_romw:	ds.w	ROM_SIZE
rt_ustack:	ds.b	4096
rt_ustack_top:
rt_sstack:	ds.b	32768
rt_sstack_top:
