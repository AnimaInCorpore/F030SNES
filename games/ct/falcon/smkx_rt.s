; SMKX runtime: the Falcon side of the translated Chrono Trigger program
; (from the F030SMK port, hence the names).
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
	xref	smkx_video_init,smkx_render,smkx_pal_used,smkx_pal_next,smkx_snap_planes
	xref	smkx_input
	xref	smkx_apu_init,smkx_apu_read,smkx_apu_write,smkx_apu_in,smkx_apu_out,smkx_apu_frame,smkx_apu_steps,smkx_apu_pc
	xref	smkx_apu_dump,smkx_lc_dump
	xdef	rt_now,rt_cprint,rt_cprint_hex,rt_frames
	xref	smkx_m7_init,smkx_m7_on,smkx_m7_rows,smkx_m7_pages,smkx_m7_timeouts
	xref	smkx_ppu_stat
	xdef	rt_m7_dirty,rt_plan_dirty,rt_m7_record,rt_m7_record_done
	xdef	rt_dsp_boot,m7_image,m7_image_bytes,ppu_image,ppu_image_bytes,ppu7_image,ppu7_image_bytes
	xdef	ppuw_image,ppuw_image_bytes
	ifne	SMKX_LC
	xdef	ppulc_image,ppulc_image_bytes,ppulcw_image,ppulcw_image_bytes
	xref	smkx_lc_active,smkx_lc_window,smkx_lc_stat,lc_prof,smkx_vid_stat
	xdef	smkx_vid_set_window,smkx_lc_cpu,smkx_lc_blit,smkx_lc_blit_busy,smkx_lc_dcache_clear
	endif
	ifne	SMKX_TC
	xdef	tc_image,tc_image_bytes,tc7_image,tc7_image_bytes
	endif
	xdef	rt_vid_planes,rt_m7_route,rt_snapdue
	xdef	smkx_oklab
	xdef	rt_vram,rt_vram_r,rt_cgram,rt_oam,rt_sram,rt_io_start,rt_irqlog,rt_nirqlog
	xdef	rt_cgram_start,rt_noamseg,rt_oamseg
	xdef	rt_bgofs_start,rt_m7_start,rt_m7ofs_start,rt_back,rt_screen

	xdef	rt_rb,rt_rw,rt_wb,rt_ww,rt_iorb,rt_iorw,rt_iowb,rt_ioww
	xdef	rt_pages_r,rt_pages_w,rt_nmitimen,rt_logging,rt_r2180,rt_w2180,rt_dbr,rt_lotab,rt_hitab
	xdef	rt_setdbr,rt_bit8,rt_bit16,rt_mvn,rt_mvp,rt_sei,rt_cli,rt_vf
	xdef	rt_dispatch,rt_probe,rt_trap,rt_unsupported,rt_badtable,rt_badexit,rt_noreturn,rt_tableoob
	xdef	rt_setsp,rt_ramjml,rt_plpwidth,rt_dreg,rt_ramcode,rt_ovlcheck,rt_lastp,rt_ovlmatch
	xdef	smkx_wram,smkx_rom

	ifnd	SMKX_FRAMES
SMKX_FRAMES	equ	0		; quit after this many frames (0 = never)
	endif

ROM_SIZE	equ	$400000	; Chrono Trigger: HiROM, 4 MiB
SRAM_SIZE	equ	$2000	; 8 KiB battery RAM at $20-$3F:$6000-$7FFF
INTERRUPT_DISPATCH	equ	8	; tools/smkx/analyze.py
SAMPLES		equ	20000
IRQLOG_MAX	equ	256
DCACHE_SLOTS	equ	1024		; rt_dispatch's cache: (key, handler) per slot
IRQ_CHAIN_MAX	equ	8		; V-IRQs per frame
OAMSEG_MAX	equ	IRQ_CHAIN_MAX+1	; snes_ppu.h: PPU_OAMSEG_MAX
OAMSEG_BYTES	equ	4+544
PAL_SNAP_BYTES	equ	96+48+256+4+2	; palette.h
	ifnd	SMKX_DIAG
SMKX_DIAG	equ	1	; 0: release build without per-call diagnostics
	endif
	ifnd	SMKX_SYNC
SMKX_SYNC	equ	1	; the game's frame (IRQs, NMI) only once its main loop
	endif			; waits for it, as on the SNES (0: every VBL)
SYNC_MAX	equ	8	; VBLs without a wait before it comes anyway
	ifnd	SMKX_RENDER
SMKX_RENDER	equ	1	; 0: no rendering at all (timing and reference runs); otherwise the VBL asks for a composition
	endif			; every frame and smkx_render_poll (smkx_video.c) throttles it: the work runs in the game's idle time
	ifnd	SMKX_SNAP_EVERY
SMKX_SNAP_EVERY	equ	0	; also write Fnnnnnnn.BIN every Nth frame (0 = off)
	endif
	ifnd	SMKX_SNAP_FROM
SMKX_SNAP_FROM	equ	0	; ... from this frame on (a window of consecutive frames)
	endif
	ifnd	SMKX_HOLD
SMKX_HOLD	equ	0	; 1: at the frame limit write the snapshot and stay (no exit): the picture stays up
	endif
	ifnd	SMKX_LC
SMKX_LC	equ	0	; the list compositor (src/smkx_lc.c; CTX_LC=1, eight-plane builds only)
	endif
	ifnd	CT_LZ
CT_LZ	equ	1	; Gfx_DecompressEntry natively (falcon/ct_lz.s; 0: translated)
	endif
	ifnd	CT_LZ_CHECK
CT_LZ_CHECK	equ	0	; 1: native and translated both, WRAM compared (ct_lz.s)
	endif
LOG_BYTES	equ	196608		; rt_print's text without NatFeats (CTXLOG.TXT)
LOG_RESERVE	equ	8192		; ... of which the last bytes are for a crash report
VBL_VECTOR	equ	$70
ACIA_VECTOR	equ	$118		; MFP GPIP4: keyboard and MIDI ACIAs
TIMERC_VECTOR	equ	$114		; MFP timer C, 200 Hz under TOS

; NOTE_CALLER: the translated call site of a generic memory access, for the
; status line and the profile (rt_io_caller); SMKX_DIAG=0 leaves it out.
NOTE_CALLER	macro
	ifne	SMKX_DIAG
	bsr	rt_note_caller
	endif
	endm

; M7DIRTY: after a VRAM byte write at byte offset d4.l, mark its 256-word
; page as written since the render copy (rt_vram_sync). Clobbers d4, a0.
M7DIRTY	macro
	lsr.w	#8,d4
	lsr.w	#1,d4
	lea	rt_vr_dirty,a0
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
	move.l	TIMERC_VECTOR.w,rt_old_tc	; saved here: rt_exit can run from
	move.l	VBL_VECTOR.w,rt_old_vbl		; any point below, a crash included
	move.l	ACIA_VECTOR.w,rt_old_acia
	bsr	rt_catch_exceptions
	lea	msg_start,a0
	bsr	rt_print

	bsr	rt_build_banks
	bsr	rt_build_pages
	bsr	rt_init_stacks
	jsr	smkx_video_init		; C: palette state
	jsr	smkx_apu_init		; C: the SPC700, waiting in its boot protocol
	bsr	rt_video_on
	lea	msg_m7off,a0
	tst.b	rt_m7_route
	beq.s	.m7msg
	lea	msg_m7on,a0
.m7msg:	bsr	rt_print

	move.w	#$2700,sr
	move.l	#rt_tc_sample,TIMERC_VECTOR.w	; 200 Hz PC samples in every context
	move.l	#rt_vbl,VBL_VECTOR.w
	move.l	#rt_acia,ACIA_VECTOR.w
	move.b	#1,rt_io+$2016		; $4016 bit 0: a pad in port 1
	move.l	#smkx_input,rt_script_at
	move.w	#$2300,sr

	; SNES reset state: emulation mode, S = $01FF, DBR = 0, registers 0.
	moveq	#0,d0
	moveq	#0,d1
	moveq	#0,d2
	lea	smkx_wram,a5
	lea	($1FF,a5),a6
	movea.l	a5,a2			; D = 0
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
	move.l	rt_old_tc,TIMERC_VECTOR.w
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

; ---------------------------------------------------------------------------
; Stack regions. The translated code keeps its return addresses on the 68k
; stack, a shadow of the 65816 JSR/JSL frames. CT's menu runs its screens as
; cooperative tasks, each on its own 65816 stack: the scheduler switches S
; with TCS and a task resumes with PLD/PLP/PLY/PLX/RTS on its own stack. So
; every stack region below gets its own 68k stack, and TCS/TXS (rt_setsp)
; switches a7 with S: a region keeps the a7 it had when S left it, and S set
; to a region's top (an empty 65816 stack) starts its 68k stack afresh.
;
; Each row: lowest S, highest S, empty-stack S (or -1), 68k stack bytes.
REGION_ROW	equ	8
rt_regions:
	dc.w	$0000,$06FF,$06FF,$4000	; 0: the engines' stack ($06FF), reset's $01FF
	dc.w	$0700,$09FF,$09FF,$2000	; 1: menu scheduler
	dc.w	$0A00,$0A3F,$0A3F,$1000	; 2-9: menu task stacks, 64 bytes each
	dc.w	$0A40,$0A7F,$0A7F,$1000
	dc.w	$0A80,$0ABF,$0ABF,$1000
	dc.w	$0AC0,$0AFF,$0AFF,$1000
	dc.w	$0B00,$0B3F,$0B3F,$1000
	dc.w	$0B40,$0B7F,$0B7F,$1000
	dc.w	$0B80,$0BBF,$0BBF,$1000
	dc.w	$0BC0,$0BFF,$0BFF,$1000
	dc.w	$1D00,$1DFF,$1DFD,$2000	; 10: the menu's temporary stack (C2841D)
	dc.w	$0000,$FFFF,-1,$4000	; 11: anything else
REGIONS		equ	12
REGION_STACKS	equ	$4000+$2000+8*$1000+$2000+$4000

; Fresh stack tops, one per region.
rt_init_stacks:
	lea	rt_regions,a0
	lea	rt_region_top,a1
	lea	rt_region_stacks,a2
	moveq	#REGIONS-1,d0
.r:	moveq	#0,d1
	move.w	6(a0),d1
	adda.l	d1,a2
	move.l	a2,(a1)+
	addq.l	#REGION_ROW,a0
	dbra	d0,.r
	clr.w	rt_cur_region
	rts

; rt_setsp: d7.l = the new S. Sets a6 and switches the 68k stack between
; regions (above). Preserves every register but d7, and the CCR. Inside the
; VBL handler (translated NMI and IRQs) only a6 changes.
rt_setsp:
	move.w	sr,-(sp)		; interrupts off: the translated NMI runs TXS
	ori.w	#$0700,sr		; too, and these globals must survive it
	move.w	(sp)+,rt_sp_ccr		; (the whole SR)
	move.l	(sp)+,rt_sp_ret
	lea	(0,a5,d7.l),a6
	tst.b	rt_in_frame
	bne	.done
	movem.l	d0-d1/a0-a1,rt_sp_save
	lea	rt_regions,a0
	moveq	#0,d0
.find:	cmp.w	(a0),d7
	bcs	.next
	cmp.w	2(a0),d7
	bls	.found
.next:	addq.l	#REGION_ROW,a0
	addq.w	#1,d0
	cmpi.w	#REGIONS-1,d0
	bne	.find
.found:	move.w	rt_cur_region,d1
	movem.l	d2/a2,-(sp)		; history: from<<24|to<<16|S, a7 before, site
	move.w	rt_shist_at,d2
	addq.w	#1,rt_shist_at
	andi.w	#15,d2
	mulu.w	#12,d2
	lea	rt_shist,a2
	adda.w	d2,a2
	move.b	d1,(a2)+
	move.b	d0,(a2)+
	move.w	d7,(a2)+
	move.l	sp,(a2)
	addq.l	#8,(a2)+
	move.l	rt_sp_ret,(a2)
	movem.l	(sp)+,d2/a2
	cmp.w	d1,d0
	beq	.same
	lea	rt_region_sp,a1
	move.l	sp,(a1,d1.w*4)		; the region S leaves keeps its a7
	move.w	d0,rt_cur_region
	cmp.w	4(a0),d7
	beq	.fresh
	move.l	(a1,d0.w*4),d1
	beq	.fresh			; never entered: a fresh stack
	movea.l	d1,sp
	bra	.out
.same:	cmp.w	4(a0),d7
	bne	.out
.fresh:	lea	rt_region_top,a1
	movea.l	(a1,d0.w*4),sp
.out:	movem.l	rt_sp_save,d0-d1/a0-a1
.done:	move.l	rt_sp_ret,-(sp)
	move.w	rt_sp_ccr,sr
	rts

; rt_ramjml: a0.w = $0500 or $0504, where an engine stored `JML handler`
; (or an RTI) for the NMI or IRQ. Returns a0 = the translated handler.
; Preserves every other register and the CCR.
rt_ramjml:
	move.w	ccr,-(sp)
	movem.l	d6-d7,-(sp)
	move.l	a0,d7
	andi.l	#$ffff,d7
	lea	smkx_wram,a0
	adda.l	d7,a0
	cmpi.b	#$5c,(a0)
	beq.s	.jml
	cmpi.b	#$40,(a0)
	bne.s	.bad
	lea	rt_rti_stub,a0
	bra.s	.done
.jml:	moveq	#0,d7
	move.b	3(a0),d7
	swap	d7
	move.b	2(a0),d6
	lsl.w	#8,d6
	move.b	1(a0),d6
	move.w	d6,d7
	movea.l	d7,a0
	move.w	#INTERRUPT_DISPATCH,d6
	bsr	rt_dispatch
.done:	movem.l	(sp)+,d6-d7
	move.w	(sp)+,ccr
	rts
.bad:	moveq	#0,d7
	move.b	(a0),d7
	move.l	d7,rt_fault_arg
	lea	msg_ramjml,a0
	bra	rt_fatal_arg

; rt_ramcode: run 65816 code the game generated in WRAM, from a0 (24-bit)
; to its RTS/RTL, which pulls the caller's return frame. d6.w: bit 0 = M
; 8-bit, bit 1 = X 8-bit, as the translated caller had them. CT builds a
; Mode 7 copy routine at $7E3000 (C309E9): runs of `LDA #$hh00 : TCD`,
; `LDA $abs : STA $dp`, ended by RTS. Only the opcodes seen so far are
; interpreted; any other stops with its opcode and address. Keeps d1, d2
; and the other registers the translated code relies on; A, D and S
; change as the code says, the flags are left as they were.
rt_ramcode:
	move.w	ccr,-(sp)
	movem.l	d4-d7/a0-a1,-(sp)
	move.w	d6,d4			; widths
	move.l	a0,d7			; host pointer of the WRAM code
	andi.l	#$1ffff,d7
	swap	d7
	cmpi.w	#1,d7			; bank $7F, or $7E / a system bank
	beq	.hi
	swap	d7
	andi.l	#$ffff,d7
	bra	.host
.hi:	swap	d7
.host:	lea	smkx_wram,a1
	adda.l	d7,a1
.op:	moveq	#0,d7
	move.b	(a1)+,d7
	cmpi.b	#$ad,d7
	beq	.lda_abs
	cmpi.b	#$85,d7
	beq	.sta_dp
	cmpi.b	#$a9,d7
	beq	.lda_imm
	cmpi.b	#$5b,d7
	beq	.tcd
	cmpi.b	#$60,d7
	beq	.rts
	cmpi.b	#$6b,d7
	beq	.rtl
	cmpi.b	#$54,d7
	beq	.mvn
	move.l	a1,d6			; unsupported: report opcode << 24 | address
	subq.l	#1,d6
	sub.l	#smkx_wram,d6
	ror.l	#8,d7
	or.l	d6,d7
	move.l	d7,rt_fault_arg
	lea	msg_ramcode,a0
	bra	rt_fatal_arg
.lda_abs:
	moveq	#0,d6
	move.b	1(a1),d6
	lsl.w	#8,d6
	move.b	(a1),d6
	addq.l	#2,a1
	or.l	d3,d6			; DBR:abs
	movea.l	d6,a0
	btst	#0,d4
	beq	.lda16
	bsr	rt_rb
	move.b	d6,d0
	bra	.op
.lda16:	bsr	rt_rw
	move.w	d6,d0
	bra	.op
.sta_dp:
	moveq	#0,d6
	move.b	(a1)+,d6
	move.l	a2,d7
	sub.l	a5,d7
	add.w	d7,d6			; D + dp, bank 0
	andi.l	#$ffff,d6
	movea.l	d6,a0
	btst	#0,d4
	beq	.sta16
	move.b	d0,d6
	bsr	rt_wb
	bra	.op
.sta16:	move.w	d0,d6
	bsr	rt_ww
	bra	.op
.lda_imm:
	btst	#0,d4
	beq	.imm16
	move.b	(a1)+,d0
	bra	.op
.imm16:	move.b	1(a1),d0
	lsl.w	#8,d0
	move.b	(a1),d0
	addq.l	#2,a1
	bra	.op
.tcd:	moveq	#0,d7
	move.w	d0,d7
	move.w	d7,rt_dreg
	lea	(0,a5,d7.l),a2
	bra	.op
.mvn:	moveq	#0,d7			; MVN dst,src: operands dst, src
	move.b	(a1)+,d7
	swap	d7
	moveq	#0,d6
	move.b	(a1)+,d6
	swap	d6
	bsr	rt_mvn			; also sets the data bank
	bra	.op
.rts:	addq.l	#2,a6
	bra	.done
.rtl:	addq.l	#3,a6
.done:	movem.l	(sp)+,d4-d7/a0-a1
	move.w	(sp)+,ccr
	rts

; rt_ovlcheck: a0 = 24-bit address in a WRAM program the translator knew
; (analyze.WRAM_OVERLAYS), a1 = the 8 bytes it translated there. Stops
; when WRAM holds something else. Preserves every register and the CCR.
rt_ovlcheck:
	move.w	ccr,-(sp)
	movem.l	d7/a0,-(sp)
	move.l	a0,d7
	andi.l	#$1ffff,d7
	lea	smkx_wram,a0
	adda.l	d7,a0
	moveq	#7,d7
.cmp:	cmpm.b	(a0)+,(a1)+
	bne.s	.bad
	dbra	d7,.cmp
	movem.l	(sp)+,d7/a0
	move.w	(sp)+,ccr
	rts
.bad:	move.l	4(sp),rt_fault_arg
	lea	msg_ovlcheck,a0
	bra	rt_fatal_arg

; rt_ovlmatch: as rt_ovlcheck, but d7.l = 0 when WRAM holds the bytes and
; 1 when not (then rt_ramcode runs what is there). Keeps the rest and the CCR.
rt_ovlmatch:
	move.w	ccr,-(sp)
	move.l	a0,-(sp)
	move.l	a0,d7
	andi.l	#$1ffff,d7
	lea	smkx_wram,a0
	adda.l	d7,a0
	moveq	#7,d7
.cmp:	cmpm.b	(a0)+,(a1)+
	bne.s	.no
	dbra	d7,.cmp
	moveq	#0,d7
	bra.s	.x
.no:	moveq	#1,d7
.x:	movea.l	(sp)+,a0
	move.w	(sp)+,ccr
	rts

; An RTI stored in the trampoline: return from the interrupt at once.
rt_rti_stub:
	addq.l	#4,a6
	rts

; A PLP whose pulled M/X bits no analysed successor covers.
rt_plpwidth:
	move.l	d7,rt_fault_arg
	lea	msg_plpwidth,a0
	bra	rt_fatal_arg

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
	bsr	rt_sample
	addq.l	#1,rt_vblcount
	tst.b	rt_in_frame
	bne	.busy
	st	rt_in_frame
	movem.l	d0-d7/a0-a6,-(sp)
	move.l	sp,rt_vbl_sp		; the frame runs on its own stack: the
	tst.b	rt_in_render		; interrupted one may be a small task stack
	beq.s	.game
	movea.l	rt_main_sp,a6		; over the render context: the NMI needs
	movea.l	14*4(a6),a6		; the game's 65816 S (its saved a6)
.game:	lea	rt_vstack_top,sp
	bsr	rt_now
	move.l	d0,-(sp)
	move.w	#$2300,sr		; let the MFP (keyboard, timers) run
	bsr	rt_frame
	move.w	#$2700,sr
	bsr	rt_now
	sub.l	(sp)+,d0
	add.l	d0,rt_t_vbl
	movea.l	rt_vbl_sp,sp
	; Who runs next: the game, always after a frame over the render
	; context; the render work when it waited RWAIT frames for the game
	; to idle (a scene without an idle mark), or at once while the DSP
	; has no picture to draw: every VBL the pending plan or send waits
	; for an idle mark is a VBL the DSP idles (the pictures are DSP
	; bound; the game keeps its frame, the VBL over the render context
	; returns to it).
	tst.b	rt_in_render
	bne.s	.suspend
	tst.b	rt_rstate
	beq.s	.ret
	tst.b	smkx_render_waiting	; it only waits for the DSP
	bne.s	.ret
	tst.l	smkx_inflight
	beq.s	.now
	addq.w	#1,rt_rwait
	cmpi.w	#RWAIT,rt_rwait
	bcs.s	.ret
.now:	move.l	sp,rt_main_sp		; the game as interrupted: back by RTE
	st	rt_main_kind
	sf	rt_in_frame
	bsr	rt_chain_tos
	bra	rt_to_render
.suspend:
	move.l	sp,rt_rctx_sp		; the render context as interrupted
	sf	rt_in_render
	sf	rt_in_frame
	bsr	rt_chain_tos
	bra	rt_to_main
.ret:	movem.l	(sp)+,d0-d7/a0-a6
	sf	rt_in_frame
.chain:	move.l	rt_old_vbl,-(sp)
	rts
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
; Render work in the game's idle time. Composing a frame (planning it for
; the DSP renderer, or the Mode 7 and CPU paths) takes the 68030 10 ms to
; some 400 ms; done in the VBL before the NMI it cost game frames. The VBL
; now only asks for it (rt_rstate 1) and the work runs in its own context
; (rt_render_thread, stack rt_rstack): started or resumed where the game
; waits for the next frame (the "idle" marks, tools/smkx/marks.txt), or by
; the VBL when the game has not idled for RWAIT frames. A VBL over the
; render context runs its frame and returns to the game. While work is
; pending the frame's inputs stay as they were asked for (no frame-start
; copy, no IRQ log, no OAM segments); snapshots and the frame limit wait.

RWAIT	equ	3
	xref	smkx_render_waiting,smkx_inflight,smkx_render_phase

; rt_mark_idle: from translated code, all registers and the CCR kept.
rt_mark_idle:
	st	rt_idled		; the main loop waits for the frame (SMKX_SYNC)
	move.w	sr,-(sp)
	ori.w	#$0700,sr
	tst.b	rt_rstate
	beq.s	.no
	tst.b	rt_in_frame		; an NMI or IRQ running the same code
	bne.s	.no
	movem.l	d0-d7/a0-a6,-(sp)
	move.l	sp,rt_main_sp		; the game: back by MOVE SR, RTS
	sf	rt_main_kind
	bra.s	rt_to_render
.no:	move.w	(sp)+,sr
	rts

; At IPL 7, the game saved at rt_main_sp: start or resume the work.
rt_to_render:
	clr.w	rt_rwait
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
	movea.l	rt_rctx_sp,sp
	movem.l	(sp)+,d0-d7/a0-a6
	rte

; At IPL 7: back into the game.
rt_to_main:
	movea.l	rt_main_sp,sp
	tst.b	rt_main_kind
	bne.s	.rte
	movem.l	(sp)+,d0-d7/a0-a6
	move.w	(sp)+,sr
	rts
.rte:	movem.l	(sp)+,d0-d7/a0-a6
	rte

rt_render_thread:
	move.l	rt_back,-(sp)
	jsr	smkx_render_start	; C: 1 when the frame is in rt_back
	addq.l	#4,sp
	move.w	#$2700,sr
	tst.l	d0
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

; Show rt_back: the plane count and palette the frame was composed for
; (C: smkx_render_present), then the Videl base.
rt_flip:
	movem.l	d0-d1/a0-a1,-(sp)
	jsr	smkx_render_present
	movem.l	(sp)+,d0-d1/a0-a1
	addq.l	#1,rt_renders
	move.l	d0,-(sp)
	move.l	rt_back,d0
	move.l	rt_screen,rt_back
	move.l	d0,rt_screen
	bsr	vid_set_base
	ifne	SMKX_LC
	clr.b	$ffff8265.w		; no HSCROLL and no line offset for a plain screen
	clr.w	$ffff820e.w		; (the list compositor's ring window sets them)
	endif
	move.l	(sp)+,d0
	rts

; void rt_flip_c(void): rt_flip for the render context in C (smkx_video.c
; take_done), interrupts off, all registers kept.
	xdef	rt_flip_c
rt_flip_c:
	movem.l	d0-d7/a0-a6,-(sp)
	bsr	rt_flip
	movem.l	(sp)+,d0-d7/a0-a6
	rts

; In every rendered frame: the DSP renderer's frame shown when it is
; there; the next composition asked for when no work is pending and the
; DSP will want it soon (C: smkx_render_poll times it).
rt_render_poll:
	tst.l	rt_screen
	beq.s	.x
	movem.l	d0-d7/a0-a6,-(sp)
	moveq	#0,d0
	tst.b	rt_rstate
	seq	d0
	andi.l	#1,d0
	move.l	d0,-(sp)		; idle: no work pending
	jsr	smkx_render_poll	; bit 0: present, bit 1: ask
	addq.l	#4,sp
	move.l	d0,d7
	btst	#0,d7
	beq.s	.noflip
	bsr	rt_flip
.noflip:
	btst	#1,d7
	beq.s	.done
	bsr	rt_vram_sync		; its VRAM too
	move.b	#1,rt_rstate		; this frame's inputs, from now kept
	clr.w	rt_rwait
.done:	movem.l	(sp)+,d0-d7/a0-a6
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
.full:	lea	rt_dsp_ticks,a0		; the DSP's state at this tick:
	moveq	#0,d7			; 0 recording: a picture on its way
	btst	#4,$ffff8901.w
	bne.s	.cnt
	moveq	#1,d7			; 1 done, not seen yet (the VBL's poll)
	tst.l	smkx_inflight
	bne.s	.cnt
	move.b	rt_rstate,d7		; idle, the next picture not sent:
	cmpi.b	#2,d7
	beq.s	.rc
	addq.l	#2,d7			; 2 not asked for, 3 asked (no idle yet)
	bra.s	.cnt
.rc:	moveq	#0,d7			; the render context has it:
	move.b	smkx_render_phase,d7	; 4-13: phase 1-5 (smkx_video.c),
	beq.s	.oth			; odd: the game has the CPU
	subq.l	#1,d7
	add.l	d7,d7
	addq.l	#4,d7
	tst.b	rt_in_frame
	bne.s	.susp
	tst.b	rt_in_render
	bne.s	.cnt
.susp:	addq.l	#1,d7
	bra.s	.cnt
.oth:	moveq	#14,d7			; 14 outside compose_frame
.cnt:	lsl.l	#2,d7
	addq.l	#1,(a0,d7.l)
	movem.l	(sp)+,d7/a0
	move.l	rt_old_tc,-(sp)
	rts

rt_dump_samples:
	lea	msg_dspticks,a0
	bsr	rt_print
	lea	rt_dsp_ticks,a1
	moveq	#14,d5
.dt:	move.l	(a1)+,d7
	bsr	rt_print_hex8
	lea	msg_slash,a0
	bsr	rt_print
	dbra	d5,.dt
	lea	msg_nl,a0
	bsr	rt_print
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
	addq.l	#1,rt_frames
	clr.w	rt_vline		; the beam at the top again (SLHV/OPVCT)
	tst.b	rt_quit			; Esc: leave cleanly
	beq.s	.run
	lea	msg_quit,a0
	bsr	rt_print
	bsr	rt_status
	moveq	#0,d7
	bra	rt_exit
.run:	movea.l	rt_script_at,a0		; scripted input (CTX_INPUT): frame, pad
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
	movem.l	d0-d1/a0-a1,-(sp)
	jsr	smkx_apu_frame		; the music advances with the clock
	movem.l	(sp)+,d0-d1/a0-a1
	ifne	SMKX_SYNC
	; The game's frame only once its main loop waits for it (an idle mark
	; or a V-blank poll since the last one): NMI-driven counters, fades
	; and scrolls then keep step with the main loop, as on the SNES, where
	; CT runs one NMI per pass. Else a lag VBL: no IRQs, no NMI (the
	; renderer goes on). After SYNC_MAX of them it comes anyway (a wait
	; without a mark; rt_forced counts those).
	sf	rt_gameframe
	tst.b	rt_nmitimen
	bpl.s	.gf
	tst.b	rt_idled
	bne.s	.gf
	addq.w	#1,rt_lag
	cmpi.w	#SYNC_MAX,rt_lag
	bcs	.noirq
	addq.l	#1,rt_forced
.gf:	st	rt_gameframe
	sf	rt_idled
	clr.w	rt_lag
	endif
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
	tst.b	rt_rstate
	sne	rt_logging		; logged for the renderer unless its inputs are kept
	not.b	rt_logging
	bsr	rt_now
	move.l	d0,rt_t0
	bsr	rt_entry_regs
	subq.l	#4,a6
	move.b	rt_vf,-(sp)		; the interrupted code's 65816 V (RTI)
	jsr	smkx_irq
	move.b	(sp)+,rt_vf
	bsr	rt_now
	sub.l	rt_t0,d0
	add.l	d0,rt_t_irq
	sf	rt_logging
	tst.b	rt_rstate
	bne.s	.noseg
	bsr	rt_oam_segment
.noseg:	subq.w	#1,rt_irqleft
	bpl	.irq
.noirq:
	move.l	#SMKX_FRAMES,d7
	beq.s	.render
	cmp.l	rt_frames,d7
	bhi.s	.render
	tst.b	rt_rstate		; render work pending: stop after it
	bne.s	.render
	ifne	SMKX_SNAP_EVERY
	st	rt_snapnow		; the last frame as F*.BIN too
	endif
	bsr	rt_render_sync
	lea	snap_name,a0
	move.l	#'SNAP',(a0)+
	move.l	#'.BIN',(a0)+
	clr.b	(a0)
	lea	msg_frames,a0
	bsr	rt_print
	bsr	rt_status
	bsr	rt_dump_samples
	movem.l	d0-d1/a0-a1,-(sp)
	jsr	smkx_apu_dump
	ifne	SMKX_LC
	jsr	smkx_lc_dump
	endif
	movem.l	(sp)+,d0-d1/a0-a1
	bsr	rt_write_snapshot
	ifne	SMKX_HOLD
	lea	msg_hold,a0		; the picture stays up for a screenshot (scripts/lc_shot.py)
	bsr	rt_print
.hold:	stop	#$2700
	bra.s	.hold
	endif
	moveq	#0,d7
	bra	rt_exit
.render:
	ifeq	SMKX_RENDER
	bra.s	.nmi			; no rendering
	endif
	ifne	SMKX_SNAP_EVERY
	move.l	rt_frames,d7
	divu.w	#SMKX_SNAP_EVERY,d7
	swap	d7
	tst.w	d7
	bne.s	.nosnap
	ifne	SMKX_SNAP_FROM
	cmp.l	#SMKX_SNAP_FROM,rt_frames
	blo.s	.nosnap
	endif
	st	rt_snapdue
.nosnap:
	endif
	ifne	SMKX_SNAP_EVERY
	tst.b	rt_snapdue		; a snapshot: this frame composed at once,
	beq.s	.async			; when no render work is pending
	tst.b	rt_rstate
	bne.s	.async
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
	ifne	SMKX_SYNC
	tst.b	rt_gameframe		; a lag VBL: not yet
	beq.s	.nonmi
	endif
	addq.l	#1,rt_nmis
	bsr	rt_now
	move.l	d0,rt_t0
	bsr	rt_entry_regs
	subq.l	#4,a6
	move.b	rt_vf,-(sp)		; the interrupted code's 65816 V (RTI)
	jsr	smkx_nmi
	move.b	(sp)+,rt_vf
	; The V-blank flag stands for the main loop again: on the SNES a poll
	; (LDA $4210 : BPL) sees it set before the NMI is taken, although the
	; NMI handler reads it too (CT's field NMI does, first thing; the
	; location change at $C03B71 waited on it for minutes).
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
; planes are then the same frame's).
rt_render_sync:
	st	rt_sync
rt_render:
	tst.l	rt_screen
	beq	.no
	movem.l	d0-d7/a0-a6,-(sp)
	tst.b	rt_rstate		; no work pending: this frame's VRAM
	bne.s	.vr
	bsr	rt_vram_sync
.vr:
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
	move.l	d0,d7
	bsr	rt_now
	sub.l	(sp)+,d0
	add.l	d0,rt_t_render
	move.l	$4ba.w,d0
	sub.l	(sp)+,d0
	add.l	d0,rt_render_ticks
	tst.l	d7
	beq	.noflip
	ifd	SMKX_PROFILE
	bsr	rt_print_prof
	endif
	ifne	SMKX_LC
	cmpi.l	#2,d7			; 2: the list compositor's ring window is on the screen (its palette is set)
	beq.s	.snapc
	endif
	bsr	rt_flip			; counts it in rt_renders
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
.noflip:
	movem.l	(sp)+,d0-d7/a0-a6
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
	xdef	rt_mark_main_start,rt_mark_main_end,rt_mark_idle,rt_mark_vwait
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

; BattleSys_WaitScanline ($CD3B82) polls the latched V counter until it
; reaches the line at DP $45 (the HUD split, $D3). With one line per latch
; (rt_io_read .slhv) that took ~200 passes of five register reads, some
; 1,000 runtime calls per wait. At its latch ($CD3B9A) the counter moves
; to the line before the target: the first latch ends the wait in the
; state the loop ends in. a2 = the 65816 direct page.
rt_mark_vwait:
	move.w	sr,-(sp)
	move.l	d0,-(sp)
	moveq	#0,d0
	move.b	$45(a2),d0
	subq.w	#1,d0			; the next latch gives the target
	bmi.s	.x
	cmp.w	rt_vline,d0
	bls.s	.x
	move.w	d0,rt_vline
.x:	move.l	(sp)+,d0
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

; "SMKX time" frames, then cumulative: main-loop frames, main-loop time
; (without interrupts), NMI, IRQ, DSP-1 (inside both), rendering, whole
; VBL handler.
rt_print_times:
	movem.l	d5-d7/a0-a1,-(sp)
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
	lea	msg_vbls,a0
	bsr	rt_print
	move.l	rt_vblcount,d7		; every VBL, handled or lost
	bsr	rt_print_hex8
	lea	msg_nmi,a0
	bsr	rt_print
	move.l	rt_nmis,d7
	bsr	rt_print_hex8
	lea	msg_slash,a0		; of them without a wait (SMKX_SYNC)
	bsr	rt_print
	move.l	rt_forced,d7
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
	jsr	smkx_apu_steps		; SPC700 instructions (bit 31: stopped)
	move.l	d0,d7
	bsr	rt_print_hex8
	lea	msg_slash,a0
	bsr	rt_print
	jsr	smkx_apu_pc		; its PC << 16 | where it stopped
	move.l	d0,d7
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
	bsr	rt_print		; then plan/upload/send time (1/38400 s),
					; the last refusal's sizes, frames by SSI,
					; frames with the colour math palette,
					; frames with the sub screen, frames
					; in bands (true colour)
	lea	smkx_ppu_stat,a1	; src/smkx_ppu.h (PPU_ST_N)
	moveq	#22,d6			; PPU_ST_N - 1
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
	ifne	SMKX_LC
	lea	msg_lc,a0		; list compositor (src/smkx_lc.c): frames shown, planned, cold, groups, copies,
	bsr	rt_print		; fallbacks, plan ticks, walk ticks, list/copy/window ticks (1/38400 s), ...
	lea	smkx_lc_stat,a1
	moveq	#21,d6
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
	lea	msg_fio,a0
	bsr	rt_print
	move.l	rt_fio_addr,d7
	bsr	rt_print_hex8
	lea	msg_from,a0
	bsr	rt_print
	move.l	rt_fio_caller,d7
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
	ifne	SMKX_LC
	jsr	smkx_lc_active		; the ring window: the picture as a plain screen
	tst.l	d0
	beq.s	.plain
	pea	rt_screen_copy
	jsr	smkx_lc_window
	addq.l	#4,sp
	bra.s	.noscr
.plain:
	endif
	move.l	rt_screen,d0
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

; void rt_dump(const char *name, const void *buf, long len): a file of len bytes (debug builds: the check's pictures).
	xdef	rt_dump
rt_dump:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a0		; name
	move.l	52(sp),a1		; buffer
	move.l	56(sp),d6		; length
	clr.w	-(sp)
	move.l	a0,-(sp)
	move.w	#$3c,-(sp)		; Fcreate
	trap	#1
	addq.l	#8,sp
	tst.l	d0
	bmi.s	.x
	move.w	d0,d7
	move.l	52(sp),-(sp)		; (the buffer, again: a1 does not survive)
	move.l	d6,-(sp)
	move.w	d7,-(sp)
	move.w	#$40,-(sp)		; Fwrite
	trap	#1
	lea	12(sp),sp
	move.w	d7,-(sp)
	move.w	#$3e,-(sp)		; Fclose
	trap	#1
	addq.l	#4,sp
.x:	movem.l	(sp)+,d2-d7/a2-a6
	rts

; rt_entry_regs: before the translated NMI or IRQ runs on the interrupted
; registers, rebuild what it relies on from the machine state: X and Y
; with zero upper words, D (a2) and the data bank (d3, a3, a4). The VBL may
; have interrupted a runtime helper that uses these registers as scratch
; (rt_dma does), and the handlers start with D and the bank unknown.
rt_entry_regs:
	lea	smkx_wram,a5		; constant, but C glue could hold anything
	andi.l	#$ffff,d1
	andi.l	#$ffff,d2
	moveq	#0,d7
	move.w	rt_dreg,d7
	lea	(0,a5,d7.l),a2
	move.b	rt_dbr,d7
	bra	rt_setdbr

; ---------------------------------------------------------------------------
; Data bank. d7.b = bank; sets d3 = bank << 16, a3 = b:$8000, a4 = b:$0000.
; Preserves every other register and the CCR.

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
	move.l	a0,d7			; $6000-$7FFF: open bus in $00-$1F, SRAM in $20-$3F
	swap	d7
	andi.w	#$3f,d7
	cmpi.w	#$20,d7
	bcs.s	.io
	move.l	a0,d7
	andi.l	#SRAM_SIZE-1,d7
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
	cmpi.w	#$2180,d7		; WMDATA straight away: CT's sprite builders
	beq.s	.wm			; ($C30D0E) read their lists through it
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
	beq.s	.unm
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
.unm:	move.l	a0,d7			; $217F in a system bank: the WRAM
	cmpi.w	#$217f,d7		; port trick (rt_iorw's .trick)
	bne.s	.slow
	swap	d7
	btst	#6,d7
	bne.s	.slow
	move.l	rt_wmadd,d7
	andi.l	#$1ffff,d7
	lea	smkx_wram,a1
	moveq	#0,d6
	move.b	(a1,d7.l),d6
	addq.l	#1,rt_wmadd
	lsl.w	#8,d6
	movea.l	smkx_apu_out,a1
	move.b	3(a1),d6
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
	beq.s	.unm
	move.l	a0,d7
	andi.l	#$ff,d7
	cmpi.w	#$ff,d7
	beq.s	.slow			; the high byte lies on the next page
	ror.w	#8,d6
	move.w	d6,(a1,d7.l)
	movem.l	(sp)+,d6-d7/a1
	move.w	(sp)+,ccr
	rts
.unm:	move.l	a0,d7			; $217F in a system bank: the WRAM
	cmpi.w	#$217f,d7		; port trick (rt_iorw's .trick)
	bne.s	.slow
	swap	d7
	btst	#6,d7
	bne.s	.slow
	movea.l	smkx_apu_in,a1
	move.b	d6,3(a1)
	move.l	rt_wmadd,d7
	andi.l	#$1ffff,d7
	lea	smkx_wram,a1
	lsr.w	#8,d6
	move.b	d6,(a1,d7.l)
	addq.l	#1,rt_wmadd
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
	move.l	a0,rt_fio_addr		; diagnostics: the last fixed-register read
	move.l	2(sp),rt_fio_caller
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

; Direct entries for the WRAM data port ($2180), byte accesses: rt_io_write's
; and rt_io_read's .wmdata without the register dispatch. d6 = value (write) or
; result (read); every other register and the CCR are kept.
rt_w2180:
	move.w	ccr,-(sp)
	movem.l	d7/a1,-(sp)
	move.b	d6,rt_io+$180		; the shadow, as rt_io_write stores it first
	move.l	rt_wmadd,d7
	andi.l	#$1ffff,d7
	lea	smkx_wram,a1
	move.b	d6,(a1,d7.l)
	addq.l	#1,rt_wmadd
	movem.l	(sp)+,d7/a1
	move.w	(sp)+,ccr
	rts

rt_r2180:
	move.w	ccr,-(sp)
	movem.l	d7/a1,-(sp)
	move.l	rt_wmadd,d7
	andi.l	#$1ffff,d7
	lea	smkx_wram,a1
	moveq	#0,d6
	move.b	(a1,d7.l),d6
	addq.l	#1,rt_wmadd
	movem.l	(sp)+,d7/a1
	move.w	(sp)+,ccr
	rts

rt_iorw:
	move.w	ccr,-(sp)
	movem.l	d5/d7,-(sp)
	move.l	a0,d7
	cmpi.w	#$217f,d7
	beq.s	.trick
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
; The WRAM port trick: a 16-bit access at $217F pairs APU port 3 (the
; mirror at $217F) with WMDATA ($2180). CT's battle window effects
; (BattleFx_QuadWindowEmit*, $D1FA29 on) write each scanline's edges with
; STA $217F (the fraction to the APU, the integer to WRAM) and skip bytes
; with LDA $217F, up to four per line. The port half is a side effect, not
; a poll: it passes without running the SPC700 (port 3's byte in
; smkx_apu_in/out); the WMDATA half goes straight to WRAM. rt_rw and rt_ww
; do the same for the access through the data bank.
.trick:	move.l	a1,-(sp)
	move.l	rt_wmadd,d7
	andi.l	#$1ffff,d7
	lea	smkx_wram,a1
	moveq	#0,d6
	move.b	(a1,d7.l),d6
	addq.l	#1,rt_wmadd
	lsl.w	#8,d6
	movea.l	smkx_apu_out,a1
	move.b	3(a1),d6
	move.l	(sp)+,a1
	movem.l	(sp)+,d5/d7
	move.w	(sp)+,ccr
	rts

rt_ioww:
	move.w	ccr,-(sp)
	movem.l	d6-d7,-(sp)
	move.l	a0,d7
	cmpi.w	#$217f,d7
	beq.s	.trick
	bsr	rt_io_write		; low byte first
	move.l	(sp),d6
	lsr.w	#8,d6
	move.l	a0,d7
	addq.l	#1,d7
	bsr	rt_io_write
	movem.l	(sp)+,d6-d7
	move.w	(sp)+,ccr
	rts
.trick:	move.l	a1,-(sp)		; rt_iorw's .trick, written
	movea.l	smkx_apu_in,a1
	move.b	d6,3(a1)
	move.l	rt_wmadd,d7
	andi.l	#$1ffff,d7
	lea	smkx_wram,a1
	lsr.w	#8,d6
	move.b	d6,(a1,d7.l)
	addq.l	#1,rt_wmadd
	move.l	(sp)+,a1
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
; data bank at the destination bank. Where both pages are mapped (rt_pages_r
; and rt_pages_w), a run up to the next page boundary of either is copied
; directly, byte by byte in the same order (a fill through an overlapping
; move stays one); otherwise a byte goes through rt_rb/rt_wb (I/O).

rt_mvn:
	move.w	ccr,-(sp)
	movem.l	d4-d5/a1,-(sp)
	move.l	d6,d4
	move.l	d7,d5
.loop:	move.l	d4,d7			; source page: bank << 8 | X >> 8
	move.w	d1,d7
	lsr.l	#8,d7
	andi.l	#$ffff,d7
	movea.l	(rt_pages_r,d7.l*4),a0
	cmpa.w	#0,a0
	beq.s	.byte
	move.l	d5,d7
	move.w	d2,d7
	lsr.l	#8,d7
	andi.l	#$ffff,d7
	movea.l	(rt_pages_w,d7.l*4),a1
	cmpa.w	#0,a1
	beq.s	.byte
	moveq	#0,d7
	move.b	d1,d7
	adda.l	d7,a0
	moveq	#0,d6
	move.b	d2,d6
	adda.l	d6,a1
	cmp.w	d6,d7			; the run: to the nearer page end
	bcc.s	.hi
	move.w	d6,d7
.hi:	neg.w	d7
	addi.w	#256,d7
	moveq	#0,d6			; at most C+1
	move.w	d0,d6
	addq.l	#1,d6
	cmp.l	d7,d6
	bcc.s	.run
	move.l	d6,d7
.run:	add.w	d7,d1
	add.w	d7,d2
	sub.w	d7,d0
	subq.w	#1,d7
.cp:	move.b	(a0)+,(a1)+
	dbra	d7,.cp
	cmpi.w	#$ffff,d0
	bne.s	.loop
	bra.s	.end
.byte:	move.l	d4,d7
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
	bne	.loop
.end:	move.l	d5,d7
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
	lsr.l	#8,d7
	andi.l	#$ffff,d7
	movea.l	(rt_pages_r,d7.l*4),a0
	cmpa.w	#0,a0
	beq.s	.byte
	move.l	d5,d7
	move.w	d2,d7
	lsr.l	#8,d7
	andi.l	#$ffff,d7
	movea.l	(rt_pages_w,d7.l*4),a1
	cmpa.w	#0,a1
	beq.s	.byte
	moveq	#0,d7			; downwards: to the nearer page start
	move.b	d1,d7
	moveq	#0,d6
	move.b	d2,d6
	lea	1(a0,d7.l),a0
	lea	1(a1,d6.l),a1
	cmp.w	d6,d7
	bls.s	.lo
	move.w	d6,d7
.lo:	addq.w	#1,d7
	moveq	#0,d6
	move.w	d0,d6
	addq.l	#1,d6
	cmp.l	d7,d6
	bcc.s	.run
	move.l	d6,d7
.run:	sub.w	d7,d1
	sub.w	d7,d2
	sub.w	d7,d0
	subq.w	#1,d7
.cp:	move.b	-(a0),-(a1)
	dbra	d7,.cp
	cmpi.w	#$ffff,d0
	bne.s	.loop
	bra.s	.end
.byte:	move.l	d4,d7
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
	bne	.loop
.end:	move.l	d5,d7
	swap	d7
	bsr	rt_setdbr
	movem.l	(sp)+,d4-d5/a1
	move.w	(sp)+,ccr
	rts

; The multiplier and divider from translated code (tools/smkx/emit.py
; writes the operands to rt_io itself): d6 = the byte (or word at $4202)
; written. As rt_io_write's .wrmpyb/.wrdivb; all registers and the CCR
; are preserved.
	xdef	rt_io,rt_wrmpyb,rt_wrmpyw,rt_wrdivb,rt_rw217f,rt_ww217f

; The multiplier and divider through an unknown data bank (d3 = DBR << 16),
; from translated code: in a system bank the register (as above), else
; rt_rb/rt_rw/rt_wb/rt_ww. d6 = the value; a0 is scratch; the X flag kept.
	xdef	rt_drb4214,rt_drb4215,rt_drb4216,rt_drb4217,rt_drw4214,rt_drw4216,rt_dwb4202,rt_dwb4204,rt_dwb4205,rt_dwb4203,rt_dwb4206,rt_dww4202
rt_drb4214:
	btst	#22,d3
	bne.s	.g
	move.b	rt_io+$2214,d6
	rts
.g:	lea	$4214.w,a0
	lea	(0,a0,d3.l),a0
	bra	rt_rb
rt_drb4215:
	btst	#22,d3
	bne.s	.g
	move.b	rt_io+$2215,d6
	rts
.g:	lea	$4215.w,a0
	lea	(0,a0,d3.l),a0
	bra	rt_rb
rt_drb4216:
	btst	#22,d3
	bne.s	.g
	move.b	rt_io+$2216,d6
	rts
.g:	lea	$4216.w,a0
	lea	(0,a0,d3.l),a0
	bra	rt_rb
rt_drb4217:
	btst	#22,d3
	bne.s	.g
	move.b	rt_io+$2217,d6
	rts
.g:	lea	$4217.w,a0
	lea	(0,a0,d3.l),a0
	bra	rt_rb
rt_drw4214:
	btst	#22,d3
	bne.s	.g
	move.w	rt_io+$2214,d6
	ror.w	#8,d6
	rts
.g:	lea	$4214.w,a0
	lea	(0,a0,d3.l),a0
	bra	rt_rw
rt_drw4216:
	btst	#22,d3
	bne.s	.g
	move.w	rt_io+$2216,d6
	ror.w	#8,d6
	rts
.g:	lea	$4216.w,a0
	lea	(0,a0,d3.l),a0
	bra	rt_rw
rt_dwb4202:
	btst	#22,d3
	bne.s	.g
	move.b	d6,rt_io+$2202
	rts
.g:	lea	$4202.w,a0
	lea	(0,a0,d3.l),a0
	bra	rt_wb
rt_dwb4204:
	btst	#22,d3
	bne.s	.g
	move.b	d6,rt_io+$2204
	rts
.g:	lea	$4204.w,a0
	lea	(0,a0,d3.l),a0
	bra	rt_wb
rt_dwb4205:
	btst	#22,d3
	bne.s	.g
	move.b	d6,rt_io+$2205
	rts
.g:	lea	$4205.w,a0
	lea	(0,a0,d3.l),a0
	bra	rt_wb
rt_dwb4203:
	btst	#22,d3
	bne.s	.g
	bra	rt_wrmpyb
.g:	lea	$4203.w,a0
	lea	(0,a0,d3.l),a0
	bra	rt_wb
rt_dwb4206:
	btst	#22,d3
	bne.s	.g
	bra	rt_wrdivb
.g:	lea	$4206.w,a0
	lea	(0,a0,d3.l),a0
	bra	rt_wb
rt_dww4202:
	btst	#22,d3
	bne.s	.g
	bra	rt_wrmpyw
.g:	lea	$4202.w,a0
	lea	(0,a0,d3.l),a0
	bra	rt_ww

; 16-bit LDA/STA $217F through an unknown data bank (d3 = DBR << 16), from
; translated code: in a system bank the WRAM port trick (see rt_iorw),
; otherwise rt_rw/rt_ww. d6 = the word; a0 is scratch; every other
; register and the X flag (the 65816 carry) are kept (no ADD, shift or
; CCR write touches X here).
rt_rw217f:
	btst	#22,d3
	bne.s	.gen
	move.l	a1,-(sp)
	move.l	d7,-(sp)
	move.l	rt_wmadd,d7
	andi.l	#$1ffff,d7
	lea	smkx_wram,a1
	moveq	#0,d6
	move.b	(a1,d7.l),d6
	movea.l	rt_wmadd,a1
	addq.l	#1,a1
	move.l	a1,rt_wmadd
	ror.w	#8,d6
	movea.l	smkx_apu_out,a1
	move.b	3(a1),d6
	move.l	(sp)+,d7
	movea.l	(sp)+,a1
	rts
.gen:	lea	$217f.w,a0
	lea	(0,a0,d3.l),a0
	bra	rt_rw
rt_ww217f:
	btst	#22,d3
	bne.s	.gen
	move.l	a1,-(sp)
	move.l	d7,-(sp)
	movea.l	smkx_apu_in,a1
	move.b	d6,3(a1)
	move.l	rt_wmadd,d7
	andi.l	#$1ffff,d7
	lea	smkx_wram,a1
	ror.w	#8,d6
	move.b	d6,(a1,d7.l)
	ror.w	#8,d6
	movea.l	rt_wmadd,a1
	addq.l	#1,a1
	move.l	a1,rt_wmadd
	move.l	(sp)+,d7
	movea.l	(sp)+,a1
	rts
.gen:	lea	$217f.w,a0
	lea	(0,a0,d3.l),a0
	bra	rt_ww
rt_wrmpyw:
	move.b	d6,rt_io+$2202
	move.w	d6,-(sp)
	lsr.w	#8,d6
	bsr.s	rt_wrmpyb
	move.w	(sp)+,d6
	rts
rt_wrmpyb:
	move.w	ccr,-(sp)
	movem.l	d5-d6,-(sp)
	move.b	d6,rt_io+$2203
	andi.w	#$ff,d6
	moveq	#0,d5
	move.b	rt_io+$2202,d5
	mulu.w	d5,d6
	move.b	d6,rt_io+$2216
	lsr.w	#8,d6
	move.b	d6,rt_io+$2217
	movem.l	(sp)+,d5-d6
	move.w	(sp)+,ccr
	rts
rt_wrdivb:
	move.w	ccr,-(sp)
	movem.l	d4-d6/a0,-(sp)
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
.x:	movem.l	(sp)+,d4-d6/a0
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
	; A direct-mapped cache in front of the search (a pointer dispatch runs
	; thousands of times a second, always to the same few handlers). A slot
	; is (key, handler), written with one movem: the VBL runs translated code
	; that dispatches too, and an interrupt never splits an instruction.
	move.l	d7,d6
	lsr.l	#8,d6
	eor.l	d7,d6
	andi.w	#DCACHE_SLOTS-1,d6
	lsl.w	#3,d6			; slot offset
	lea	rt_dcache,a1
	cmp.l	(a1,d6.w),d7
	bne.s	.search0
	movea.l	4(a1,d6.w),a0
	ifne	SMKX_DIAG
	bsr	.history
	endif
	movem.l	(sp)+,d4-d5/d7/a1
	move.w	(sp)+,ccr
	rts
.search0:
	moveq	#0,d4			; lo
	move.l	d6,-(sp)		; the slot to fill
	lea	smkx_dispatch,a1
	move.l	#smkx_dispatch_count,d5	; hi (exclusive)
.search:
	cmp.l	d4,d5
	bls.s	.missp
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
	move.l	(sp)+,d4		; fill the cache slot
	lea	rt_dcache,a1
	movem.l	d7/a0,(a1,d4.w)
	ifne	SMKX_DIAG
	bsr	.history
	endif
	movem.l	(sp)+,d4-d5/d7/a1
	move.w	(sp)+,ccr
	rts
	ifne	SMKX_DIAG
.history:				; target, site, region (the trap report's last dispatches)
	move.w	rt_dhist_at,d4
	addq.w	#1,rt_dhist_at
	andi.w	#15,d4
	mulu.w	#12,d4
	lea	rt_dhist,a1
	move.l	d7,(a1,d4.w)
	move.l	22(sp),4(a1,d4.w)	; (the return address of the bsr is on top)
	move.w	rt_cur_region,8(a1,d4.w)
	move.l	a6,d5
	sub.l	a5,d5
	move.w	d5,10(a1,d4.w)
	rts
	endif
.missp:	addq.l	#4,sp
.miss:	move.l	d7,rt_fault_arg
	move.l	18(sp),rt_io_caller	; the translated dispatch site
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
	; the 68k stack above the site (return addresses as text offsets) and
	; the 65816 stack (S and its next 24 bytes), for the report
	lea	msg_stk68,a0
	bsr	rt_print
	lea	4(sp),a1
	moveq	#11,d5
.s68:	move.l	(a1)+,d7
	sub.l	#start,d7
	bsr	rt_print_hex8
	lea	msg_space,a0
	bsr	rt_print
	dbra	d5,.s68
	lea	msg_stk816,a0
	bsr	rt_print
	move.l	a6,d7
	sub.l	#smkx_wram,d7
	bsr	rt_print_hex8
	lea	msg_space,a0
	bsr	rt_print
	lea	1(a6),a1
	moveq	#5,d5
.s816:	move.b	(a1)+,d7
	lsl.l	#8,d7
	move.b	(a1)+,d7
	lsl.l	#8,d7
	move.b	(a1)+,d7
	lsl.l	#8,d7
	move.b	(a1)+,d7
	bsr	rt_print_hex8
	lea	msg_space,a0
	bsr	rt_print
	dbra	d5,.s816
	lea	msg_nl,a0
	bsr	rt_print
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
	bsr	rt_status
	bsr	rt_print_dhist
	moveq	#3,d7
	bra	rt_exit

; The last 16 pointer dispatches, oldest first: key (ctx << 24 | target),
; translated site (text offset), 68k stack region << 16 | 65816 S.
rt_print_dhist:
	movem.l	d5-d7/a0-a1,-(sp)
	lea	msg_shist,a0		; the stack switches first
	bsr	rt_print
	moveq	#15,d5
	move.w	rt_shist_at,d6
.s1:	andi.w	#15,d6
	move.w	d6,d7
	mulu.w	#12,d7
	lea	rt_shist,a1
	adda.w	d7,a1
	move.l	(a1)+,d7
	bsr	rt_print_hex8
	lea	msg_space,a0
	bsr	rt_print
	move.l	(a1)+,d7
	bsr	rt_print_hex8
	lea	msg_space,a0
	bsr	rt_print
	move.l	(a1)+,d7
	sub.l	#start,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print
	addq.w	#1,d6
	dbra	d5,.s1
	lea	msg_dhist,a0
	bsr	rt_print
	moveq	#15,d5
	move.w	rt_dhist_at,d6
.one:	andi.w	#15,d6
	move.w	d6,d7
	mulu.w	#12,d7
	lea	rt_dhist,a1
	adda.w	d7,a1
	move.l	(a1)+,d7
	bsr	rt_print_hex8
	lea	msg_space,a0
	bsr	rt_print
	move.l	(a1)+,d7
	sub.l	#start,d7
	bsr	rt_print_hex8
	lea	msg_space,a0
	bsr	rt_print
	move.l	(a1)+,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print
	addq.w	#1,d6
	dbra	d5,.one
	movem.l	(sp)+,d5-d7/a0-a1
	rts

rt_fatal_arg:
	move.w	#$2700,sr
	move.l	#rt_log_end,rt_log_lim
	bsr	rt_print
	move.l	rt_fault_arg,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print
	bsr	rt_status
	bsr	rt_print_dhist
	lea	snap_name,a0		; TRAP.BIN: the machine state (WRAM!) at the stop
	move.l	#'TRAP',(a0)+
	move.l	#'.BIN',(a0)+
	clr.b	(a0)
	bsr	rt_write_snapshot
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
	cmpi.w	#$4016,d7		; joypad port 1: a standard pad is present
	beq	.pad1			; (F030DKC: a shadow byte is lost at the strobe)
	cmpi.w	#$4017,d7
	beq	.out
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
; 262), counted from line 0 at each frame (rt_frame), so a wait for a line
; (CT's BattleSys_WaitScanline: latch, OPVCT >= A) ends after that many
; polls instead of never, and CT's field engine, which runs object scripts
; until the beam reaches a line budget (Field_ReadHVLatch, $0169), gets
; one line per script slice: every queued script runs each frame.
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
.apu:	andi.l	#3,d7			; the SPC700 (C: src/smkx_apu.c)
	movem.l	d0-d1/a1,-(sp)
	move.l	d7,-(sp)
	jsr	smkx_apu_read
	addq.l	#4,sp
	move.l	d0,d6
	movem.l	(sp)+,d0-d1/a1
	bra	.out
.wmdata:
	move.l	rt_wmadd,d4
	andi.l	#$1ffff,d4
	lea	smkx_wram,a0
	move.b	(a0,d4.l),d6
	addq.l	#1,rt_wmadd
	bra	.out
.pad1:	moveq	#1,d6
	bra	.out
.rdnmi:	moveq	#2,d6
	st	rt_idled		; a poll for the V-blank: waiting too
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
.dsp:	moveq	#0,d6			; no coprocessor: open bus
	bra	.out

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

; COLDATA: bits 5-7 pick the fixed colour's red, green and blue, bits 0-4
; their value; the three are kept at $21F0-$21F2 (no register there), so
; the frame start copies them with the other shadows.
.coldata:
	moveq	#$1f,d4
	and.b	d6,d4
	btst	#5,d6
	beq.s	.cg
	move.b	d4,rt_io+$1f0
.cg:	btst	#6,d6
	beq.s	.cb
	move.b	d4,rt_io+$1f1
.cb:	btst	#7,d6
	beq	.out
	move.b	d4,rt_io+$1f2
	bra	.out

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
	andi.l	#3,d7
	movem.l	d0-d1/a1,-(sp)
	moveq	#0,d4
	move.b	d6,d4
	move.l	d4,-(sp)
	move.l	d7,-(sp)
	jsr	smkx_apu_write
	addq.l	#8,sp
	movem.l	(sp)+,d0-d1/a1
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
.dsp:	addq.l	#1,rt_dspwrites		; no coprocessor
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
	dc.w	.out-.w21,.out-.w21,.coldata-.w21,.out-.w21
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



; rt_m7_mark: d4.w = first VRAM word, d3.l = bytes; marks every page the
; word copy touches as written since the render copy. Preserves all
; registers.
rt_m7_mark:
	movem.l	d2/d6-d7/a0,-(sp)
	move.w	d4,d6
	move.l	d3,d7
	lsr.l	#1,d7			; words
	beq.s	.x
	lea	rt_vr_dirty,a0
.pg:	move.w	d6,d2
	lsr.w	#8,d2
	st	(a0,d2.w)
	moveq	#0,d2
	move.b	d6,d2
	neg.w	d2
	addi.w	#256,d2			; words to the end of this page
	add.w	d2,d6
	andi.w	#$7fff,d6
	sub.l	d2,d7
	bgt.s	.pg
.x:	movem.l	(sp)+,d2/d6-d7/a0
	rts

; rt_vram_sync: the render copy of VRAM (rt_vram_r) takes the pages the
; game wrote since the last sync, and they count as changed for the DSP
; uploads and the planner (rt_m7_dirty, rt_plan_dirty). Called where a
; frame's inputs are fixed (the frame's registers, OAM and CGRAM are
; copied at its start): the render work, spanning VBLs whose NMIs stream
; new sprite graphics, then reads VRAM of the same frame. Preserves all
; registers.
rt_vram_sync:
	movem.l	d0-d5/a0-a4,-(sp)
	lea	rt_vr_dirty,a2
	lea	rt_m7_dirty,a3
	lea	rt_plan_dirty,a4
	moveq	#0,d1			; page
.pg:	tst.b	(a2,d1.w)
	beq.s	.next
	sf	(a2,d1.w)
	move.l	d1,d2
	lsl.l	#8,d2
	add.l	d2,d2			; 512 bytes a page
	lea	rt_vram,a0
	adda.l	d2,a0
	lea	rt_vram_r,a1
	adda.l	d2,a1
	moveq	#0,d3			; what differs
	moveq	#512/16-1,d0
.cp:	move.l	(a0)+,d4
	move.l	(a1),d5
	eor.l	d4,d5
	or.l	d5,d3
	move.l	d4,(a1)+
	move.l	(a0)+,d4
	move.l	(a1),d5
	eor.l	d4,d5
	or.l	d5,d3
	move.l	d4,(a1)+
	move.l	(a0)+,d4
	move.l	(a1),d5
	eor.l	d4,d5
	or.l	d5,d3
	move.l	d4,(a1)+
	move.l	(a0)+,d4
	move.l	(a1),d5
	eor.l	d4,d5
	or.l	d5,d3
	move.l	d4,(a1)+
	dbra	d0,.cp
	tst.l	d3
	beq.s	.next			; written with what it held: no conversion, no plan is stale
	st	(a3,d1.w)
	st	(a4,d1.w)
.next:	addq.w	#1,d1
	cmpi.w	#128,d1
	bcs.s	.pg
	movem.l	(sp)+,d0-d5/a0-a4
	rts

rt_vram_step:
	move.w	rt_vminc,d4
	add.w	d4,rt_vmadd
	andi.w	#$7fff,rt_vmadd
	rts

; General DMA: d6.b = channel mask written to $420B.
; A-bus to B-bus transfers from host memory (an incrementing source that
; stays inside its bank, or a fixed one) copy directly: VRAM (pairs or one
; half, any increment), OAM, CGRAM and WRAM ($2180), with the ports' own
; address, latch and wrap rules (from F030SMK). Everything else takes the
; per-byte path through the register handlers.
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
	; The usual upload: word increment 1, incrementing source, no wrap at
	; $8000: the words land in order, a straight copy of long words.
	cmpa.w	#1,a1
	bne.s	.vp
	cmpi.w	#1,d2
	bne.s	.vp
	move.l	d7,d5
	addq.l	#1,d5			; pairs
	moveq	#0,d6
	move.w	d4,d6
	add.l	d5,d6
	cmpi.l	#$8000,d6
	bhi.s	.vp
	moveq	#0,d6
	move.w	d4,d6
	add.l	d6,d6
	lea	(a4,d6.l),a0
	add.w	d5,d4
	andi.w	#$7fff,d4
	move.l	d5,d6
	lsr.l	#1,d6			; long words
	bra.s	.vfl
.vfc:	move.l	(a3)+,(a0)+
.vfl:	subq.l	#1,d6
	bpl.s	.vfc
	btst	#0,d5
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
	bmi.s	.cbq
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
	bne.s	.bafb			; (CT's title fills its OAM buffer with $E0
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
; Keyboard as joypad 1 (from F030SMW). The handler replaces TOS's for the
; program's lifetime: it drains both ACIAs, skips IKBD packets (mouse,
; joystick, clock, status) and keeps the SNES button word in rt_pad, which
; rt_frame latches into $4218/$4219 once per frame. Keys (by Atari
; scancode, i.e. position on a QWERTY layout): arrows = D-pad, X = B,
; Z = Y, S = A, A = X, Q = L, W = R, Return = Start, Space = Select,
; Esc = quit.

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
	move.l	#rt_log_buf,rt_log_ptr		; no NatFeats: keep the text for CTXLOG.TXT
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
; text rt_print collected goes to CTXLOG.TXT in the current directory.
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

; C callable: rt_cprint(const char *), rt_cprint_hex(uint32_t).
rt_cprint:
	move.l	a0,-(sp)
	movea.l	8(sp),a0
	bsr	rt_print
	movea.l	(sp)+,a0
	rts
rt_cprint_hex:
	move.l	d7,-(sp)
	move.l	8(sp),d7
	bsr	rt_print_hex8
	move.l	(sp)+,d7
	rts

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
	ifne	SMKX_LC
	include	'smkx_lcput.s'
	endif
	include	'smkx_m7dsp.s'
	include	'ct_lz.s'

	section	text,code
rt_text_end:

	section	data,data

nf_name:		dc.b	"NF_STDERR",0
msg_reset_returned:	dc.b	"SMKX: reset procedure returned",10,0
msg_hold:		dc.b	"SMKX: holding the picture",10,0
msg_frames:		dc.b	"SMKX: frame limit reached",10,0
msg_quit:		dc.b	"SMKX: Esc pressed, leaving",10,0
msg_status:		dc.b	"SMKX: frame ",0
msg_nmi:		dc.b	" nmi ",0
msg_vbls:		dc.b	" vbl ",0
msg_state:		dc.b	" state ",0
msg_dma:		dc.b	" dma ",0
msg_apu:		dc.b	" apu ",0
msg_dsp:		dc.b	" spc ",0
msg_busy:		dc.b	"SMKX: frame handler still running; ",0
msg_probe:		dc.b	"SMKX probe $",0
msg_at:			dc.b	" at frame ",0
msg_snap:		dc.b	"SMKX: wrote snapshot",10,0
msg_m7on:		dc.b	"SMKX: rendering on the DSP (Mode 7 kernel, tile renderer)",10,0
msg_m7off:		dc.b	"SMKX: rendering on the CPU",10,0
msg_m7rows:		dc.b	" m7dsp ",0
msg_ppu_dsp:		dc.b	" ppudsp ",0
msg_rtime:		dc.b	" renders/ticks ",0
	ifne	SMKX_LC
msg_lc:			dc.b	" lc ",0
msg_vid:		dc.b	" vid ",0
msg_lcp:		dc.b	" lcprof ",0
	endif
msg_slash:		dc.b	"/",0
msg_prof:		dc.b	"SMKX prof ",0
msg_times:		dc.b	"SMKX time ",0
msg_exc:		dc.b	"SMKX CPU exception, vector $",0
msg_exc_pc:		dc.b	" at text +",0
msg_exc_regs:		dc.b	"SMKX regs d0-d7 a0-a7 ",0
msg_exc_stack:		dc.b	"SMKX stack (text offsets) ",0
msg_space:		dc.b	" ",0
snap_name:		dc.b	"SNAP.BIN",0,0,0,0,0	; or Fnnnnnnn.BIN
log_name:		dc.b	"CTXLOG.TXT",0
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
	dc.l	SRAM_SIZE,rt_sram
	dc.l	4,smkx_snap_planes
	dc.l	SCREEN_BYTES,rt_screen_copy
	dc.l	PAL_SNAP_BYTES,smkx_pal_used
	dc.l	PAL_SNAP_BYTES,smkx_pal_next
	dc.l	4+OAMSEG_MAX*OAMSEG_BYTES,rt_noamseg
	dc.l	0
msg_samples:		dc.b	"SMKX samples",10,0
msg_tsamples:		dc.b	"SMKX tsamples",10,0
msg_dspticks:		dc.b	"SMKX dsp ticks (drawing/done unseen/idle: not asked/asked/render phases 1-5 run,held/other) ",0
msg_sp:			dc.b	" ",0
msg_pc:			dc.b	" pc +",0
msg_ppu:		dc.b	" ppu ",0
msg_cg:			dc.b	" cg ",0
msg_io:			dc.b	" io $",0
msg_from:		dc.b	" from +",0
msg_fio:		dc.b	" reg $",0
msg_nl:			dc.b	10,0
msg_trap:		dc.b	"SMKX TRAP at 65816 $",0
msg_updtrap:		dc.b	"SMKX DSP-1 program reached an untranslated branch at $",0
msg_unsupported:	dc.b	"SMKX UNSUPPORTED instruction at $",0
msg_noreturn:		dc.b	"SMKX return to a call site with no continuation at $",0
msg_badtable:		dc.b	"SMKX jump-table entry is not code, X = $",0
msg_badexit:		dc.b	"SMKX callee returned in an unexpected context, exit id $",0
msg_tableoob:		dc.b	"SMKX jump table index past the translated table: X $",0
msg_stk68:		dc.b	10,"SMKX 68k stack (text offsets): ",0
msg_stk816:		dc.b	10,"SMKX 65816 S and stack bytes: ",0
msg_at_site:		dc.b	" to the call at 65816 $",0
msg_dispatch:		dc.b	"SMKX pointer dispatch to an untranslated target (ctx<<24|addr) $",0
msg_ramjml:		dc.b	"SMKX WRAM trampoline holds neither JML nor RTI: opcode $",0
msg_ovlcheck:		dc.b	"SMKX WRAM program changed since translation, entry $",0
msg_dhist:		dc.b	"SMKX last dispatches (key, site, region<<16|S):",10,0
msg_shist:		dc.b	"SMKX last stack switches (from<<24|to<<16|S, a7, site):",10,0
msg_ramcode:		dc.b	"SMKX WRAM code: unsupported opcode << 24 | WRAM address $",0
msg_plpwidth:		dc.b	"SMKX PLP pulled widths no successor covers, P = $",0
	even

	include	"smkx_info.s"		; the info screen; the player's ROM, read at startup
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
rt_old_acia:	ds.l	1
rt_script_at:	ds.l	1
rt_pad:		ds.w	1		; SNES buttons, $4219 << 8 | $4218
rt_script_pad:	ds.w	1
rt_quit:	ds.b	1
rt_kbd_skip:	ds.b	1
	even
rt_old_exc:	ds.l	EXC_LAST-EXC_FIRST+1
rt_exc_pc:	ds.l	1
rt_exc_regs:	ds.l	16
rt_exit_code:	ds.w	1
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
rt_dsp_ticks:	ds.l	15	; per timer tick (rt_tc_sample)
rt_old_tc:	ds.l	1
rt_nprobed:	ds.w	1
rt_probed:	ds.l	64
rt_samples:	ds.l	2*SAMPLES
rt_pages_r:	ds.l	65536		; per 256-byte page: host address, 0 = I/O
rt_pages_w:	ds.l	65536		; the same for writes; ROM pages go to rt_trash
rt_trash:	ds.b	256
rt_tsamples:	ds.l	2*SAMPLES
rt_io_caller:	ds.l	1
rt_io_addr:	ds.l	1
rt_fio_addr:	ds.l	1
rt_fio_caller:	ds.l	1
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
rt_main_kind:	ds.b	1	; the game saved by rt_mark_idle (0) or a VBL (-1)
rt_snapdue:	ds.b	1	; a snapshot waits for the render work
rt_snapnow:	ds.b	1	; this sync render writes the snapshot
	even
rt_rwait:	ds.w	1	; frames the asked-for work waited for the game to idle
rt_main_sp:	ds.l	1
rt_rctx_sp:	ds.l	1
			even
; rt_print_times prints these seven in this order
rt_n_main:	ds.l	1
rt_t_main:	ds.l	1
rt_t_nmi:	ds.l	1
rt_t_irq:	ds.l	1
rt_t_dsp1:	ds.l	1
rt_t_render:	ds.l	1
rt_t_vbl:	ds.l	1
rt_t0:		ds.l	1
rt_now_last:	ds.l	1
rt_main_t0:	ds.l	1
rt_main_vbl0:	ds.l	1
rt_main_open:	ds.b	1
	even
rt_vmadd:	ds.w	1
rt_vminc:	ds.w	1
rt_cgadd:	ds.w	1
rt_oamadd:	ds.w	1
rt_vline:	ds.w	1		; the modelled scanline (SLHV latches)
rt_vlatch:	ds.w	1
rt_in_frame:	ds.b	1
rt_nmitimen:	ds.b	1
rt_rdnmi:	ds.b	1
rt_idled:	ds.b	1	; the main loop waited since the last game frame
rt_gameframe:	ds.b	1	; this VBL runs the game's IRQs and NMI
	even
rt_lag:		ds.w	1	; VBLs since the last game frame
rt_forced:	ds.l	1	; game frames that came without a wait
rt_timeup:	ds.b	1
rt_iflag:	ds.b	1
rt_vf:		ds.b	1		; the 65816 V flag ($FF set), outside the CCR
rt_hvb_toggle:	ds.b	1
rt_opvct_ff:	ds.b	1
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
rt_vram_r:	ds.b	$10000	; the renderer's copy (rt_vram_sync)
rt_vr_dirty:	ds.b	128	; pages written since the last sync
rt_cgram:	ds.b	512
rt_oam:		ds.b	544
rt_cgram_start:	ds.b	512
rt_noamseg:	ds.l	1
rt_oamseg:	ds.b	OAMSEG_MAX*OAMSEG_BYTES
rt_sram:	ds.b	SRAM_SIZE
	cnop	0,16
smkx_wram:	ds.b	$20000
		ds.b	$10000		; guard for dp,X / abs,X overruns
rt_region_stacks:	ds.b	REGION_STACKS
rt_region_top:	ds.l	REGIONS
rt_region_sp:	ds.l	REGIONS
rt_cur_region:	ds.w	1
rt_dhist_at:	ds.w	1
rt_shist_at:	ds.w	1
rt_shist:	ds.b	16*12
rt_dhist:	ds.b	16*12
rt_dcache:	ds.l	2*DCACHE_SLOTS
rt_dreg:	ds.w	1		; D, as TCD and PLD set it
rt_dbr:		ds.b	1		; the data bank, as rt_setdbr set it
rt_lastp:	ds.b	1		; P as the last PLP from below a frame pulled it
	even
rt_sp_ccr:	ds.w	1
rt_sp_ret:	ds.l	1
rt_sp_save:	ds.l	4
rt_vbl_sp:	ds.l	1
rt_vstack:	ds.b	32768
rt_vstack_top:
rt_rstack:	ds.b	16384
rt_rstack_top:
rt_ustack:	ds.b	4096
rt_ustack_top:
rt_sstack:	ds.b	32768
rt_sstack_top:
