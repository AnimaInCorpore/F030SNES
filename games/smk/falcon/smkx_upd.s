; DSP-1 by its own program: the uPD77C25 firmware (assets/dsp1.bin),
; translated to 68030 code by tools/smkx/upd2m68k.py (upd_code.s).
; Included by smkx_rt.s when the firmware is present (SMKX_DSP1_LLE).
;
; The chip's program runs synchronously: every host access to DR or SR first
; runs it from where it last stopped until it polls RQM again (a JRQM on
; itself), then performs the access with the chip's handshake. SMK always
; polls SR before reading results, so running to the next poll gives the
; results the real chip would have ready at that point.
;
; Host-side semantics match tools/smkx/upd7725.py (UPD.host_*), which the
; UPDTEST harness checks against this code.

	xref	upd_code_table
	xdef	upd_yield,upd_trap
	include	'upd_equ.i'

UPD_RQM		equ	15
UPD_DRS		equ	12
UPD_DRC		equ	10

; UPD_NATIVE: SMK's race commands (multiply, parameter, triangle, project,
; distance) run natively (src/dsp1_native.c): at the command poll the
; command byte starts a native command, its parameter words are collected
; with the chip's handshake, dsp1n_exec runs once and the results are served
; the same way. Then the firmware is left at its command poll as $000-$004
; leave it (B $C0, SR $8400, DR $0080): its other registers are dead there.
; tools/smkx/dsp1_native.py proves the C and this protocol on SMK's traffic.
	ifnd	UPD_NATIVE
UPD_NATIVE	equ	0
	endif

	section	text,code

; SMKX_DSP1_LOG: every host access as one word in dsp1_log, dumped to
; DSP1LOG.BIN at the frame limit (tools/smkx/dsp1_stream.py replays it):
; bit 15 = write, bit 14 = SR (reads only), bits 7-0 = the byte.
DSP1_LOG_MAX	equ	1048576

	ifd	SMKX_DSP1_LOG
dsp1_log_put:				; d6.b = byte, d5.w = flags; keeps all
	move.l	a0,-(sp)
	move.l	d4,-(sp)
	move.l	dsp1_log_n,d4
	cmpi.l	#DSP1_LOG_MAX,d4
	bcc.s	.full
	addq.l	#1,dsp1_log_n
	add.l	d4,d4
	lea	dsp1_log,a0
	move.w	d5,(a0,d4.l)
	move.b	d6,1(a0,d4.l)
.full:	move.l	(sp)+,d4
	move.l	(sp)+,a0
	rts
	endif

; d7.w = address ($6000-$7FFF); returns d6.b (DR byte or SR high byte).
dsp1_read:
	ifd	SMKX_DSP1_LOG
	bsr.s	dsp1_read_raw
	move.w	ccr,-(sp)
	move.l	d5,-(sp)
	move.w	#$0000,d5
	cmpi.w	#$7000,d7
	bcs.s	.lg
	move.w	#$4000,d5
.lg:	bsr	dsp1_log_put
	move.l	(sp)+,d5
	move.w	(sp)+,ccr
	rts
dsp1_read_raw:
	endif
	ifne	UPD_NATIVE
	tst.b	dsp1n_mode
	bne	dsp1n_read
	endif
	bsr	upd_run_timed
	cmpi.w	#$7000,d7
	bcs.s	.dr
	move.b	upd_state+upd_sr,d6	; SR: the host sees the high byte
	rts
.dr:	move.l	d4,-(sp)
	move.w	upd_state+upd_sr,d4
	btst	#UPD_DRC,d4
	bne.s	.r8
	btst	#UPD_DRS,d4
	bne.s	.r16hi
	bset	#UPD_DRS,d4		; 16-bit: low byte first
	move.w	d4,upd_state+upd_sr
	move.b	upd_state+upd_dr+1,d6
	move.l	(sp)+,d4
	rts
.r16hi:	andi.w	#$6FFF,d4		; clear RQM and DRS
	move.w	d4,upd_state+upd_sr
	move.b	upd_state+upd_dr,d6
	move.l	(sp)+,d4
	rts
.r8:	bclr	#UPD_RQM,d4
	move.w	d4,upd_state+upd_sr
	move.b	upd_state+upd_dr+1,d6
	move.l	(sp)+,d4
	rts

; d6.b = byte written to DR ($6000-$6FFF).
dsp1_write:
	ifd	SMKX_DSP1_LOG
	move.w	ccr,-(sp)
	move.l	d5,-(sp)
	move.w	#$8000,d5
	bsr	dsp1_log_put
	move.l	(sp)+,d5
	move.w	(sp)+,ccr
	endif
	ifne	UPD_NATIVE
	tst.b	dsp1n_mode
	bne	dsp1n_write
	endif
	bsr	upd_run_timed
	ifne	UPD_NATIVE
	cmpi.w	#$004,upd_state+upd_pc	; the command poll, 8-bit transfers:
	bne.s	.fw			; this byte is a command
	btst	#UPD_DRC-8,upd_state+upd_sr
	beq.s	.fw
	cmpi.b	#$40,d6
	bcc.s	.fw
	moveq	#0,d4
	move.b	d6,d4
	move.b	dsp1n_tab(pc,d4.w),d4
	bmi.s	.fw			; not native: the firmware runs it
	ext.w	d4
	move.w	d4,dsp1n_need
	moveq	#0,d4
	move.b	d6,d4
	move.w	d4,dsp1n_cmd
	clr.w	dsp1n_n
	sf	dsp1n_hi
	move.w	#$8000,dsp1n_sr		; RQM: it waits for parameters (SR = 0)
	move.b	#1,dsp1n_mode
	rts
.fw:
	endif
	move.l	d4,-(sp)
	move.w	upd_state+upd_sr,d4
	btst	#UPD_DRC,d4
	bne.s	.w8
	btst	#UPD_DRS,d4
	bne.s	.w16hi
	bset	#UPD_DRS,d4		; 16-bit: low byte first
	move.w	d4,upd_state+upd_sr
	move.b	d6,upd_state+upd_dr+1
	move.l	(sp)+,d4
	rts
.w16hi:	andi.w	#$6FFF,d4		; clear RQM and DRS
	move.w	d4,upd_state+upd_sr
	move.b	d6,upd_state+upd_dr
	move.l	(sp)+,d4
	rts
.w8:	bclr	#UPD_RQM,d4
	move.w	d4,upd_state+upd_sr
	move.b	d6,upd_state+upd_dr+1
	move.l	(sp)+,d4
	rts

; Native hot paths (src/dsp1_hle.c), called by the translated firmware at
; the loop heads tools/smkx/upd2m68k.py hooks. The state block (a3) has the
; layout of the C UpdState once A, B, K, L, DP and RP are stored in it.
; Returns d7 = the program counter to continue at, or 0 to run the firmware
; (upd_hle_on clear). The translated code keeps no other state in registers
; across the hook: d4 and d7 are scratch, a0/a1/a3 are preserved here.
	ifnd	UPD_HLE
UPD_HLE		equ	1		; 0: the firmware runs every loop
	endif
; One glue per region of tools/smkx/upd2c.py (dsp1_region_<name>).
UPD_GLUE	macro
	xref	dsp1_region_\1
	xdef	upd_hle_\1_glue
upd_hle_\1_glue:
	moveq	#0,d7
	tst.b	upd_hle_on
	beq	.x\@
	move.w	d0,upd_a_saved(a3)
	move.w	d1,upd_b_saved(a3)
	move.w	d2,upd_k_saved(a3)
	move.w	d3,upd_l_saved(a3)
	move.w	d5,upd_dp_saved(a3)
	move.w	d6,upd_rp_saved(a3)
	movem.l	a0-a1,-(sp)
	addq.l	#1,rt_n_hle
	ifne	SMKX_DIAG
	move.l	rt_t_vbl,-(sp)		; VBL time that passes meanwhile is not ours
	jsr	rt_now
	move.l	d0,-(sp)
	endif
	; C may use a5/a6 as scratch, and the VBL runs the translated NMI and
	; IRQs on the interrupted registers (a5 = WRAM, a6 = the 65816 stack):
	; no VBL while C runs. IPL 4 still lets MFP timer C through.
	move.w	sr,-(sp)
	move.w	#$2400,sr
	move.l	a1,-(sp)		; data ROM
	move.l	a0,-(sp)		; data RAM
	move.l	a3,-(sp)		; state block
	jsr	dsp1_region_\1
	lea	12(sp),sp
	move.w	(sp)+,sr
	move.l	d0,d7
	ifne	SMKX_DIAG
	jsr	rt_now
	sub.l	(sp)+,d0
	sub.l	rt_t_vbl,d0
	add.l	(sp)+,d0
	add.l	d0,rt_t_hle
	endif
	movem.l	(sp)+,a0-a1
	move.w	upd_a_saved(a3),d0
	move.w	upd_b_saved(a3),d1
	move.w	upd_k_saved(a3),d2
	move.w	upd_l_saved(a3),d3
	moveq	#0,d5
	move.w	upd_dp_saved(a3),d5
	moveq	#0,d6
	move.w	upd_rp_saved(a3),d6
.x\@:	rts
	endm

	xdef	upd_hle_on
	UPD_GLUE	raster
	UPD_GLUE	project

	ifne	UPD_NATIVE
; (Not between upd_run_timed and upd_run: a release build falls through
; from one into the other.)
	xref	dsp1n_exec
; Parameter words of the native commands $00-$3F (-1: the firmware);
; src/dsp1_native.c dsp1n_params has the same list.
dsp1n_tab:
	dc.b	2,-1,7,-1,2,-1,3,-1, -1,-1,-1,-1,-1,-1,-1,-1
	dc.b	-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1
	dc.b	-1,-1,-1,-1,-1,-1,-1,-1, 3,-1,-1,-1,-1,-1,-1,-1
	dc.b	-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1
	even

; A byte to DR while a native command collects its parameters (d6.b).
; May clobber d4-d7/a0, like the firmware path (rt_wdsp keeps them).
dsp1n_write:
	cmpi.b	#1,dsp1n_mode
	bne	.x			; (results pending: not SMK's)
	tst.b	dsp1n_hi
	bne.s	.hi
	st	dsp1n_hi		; 16-bit: low byte first
	move.b	d6,dsp1n_lo
	bset	#UPD_DRS-8,dsp1n_sr
	rts
.hi:	sf	dsp1n_hi
	bclr	#UPD_DRS-8,dsp1n_sr
	move.w	dsp1n_n,d4
	add.w	d4,d4
	lea	dsp1n_in,a0
	move.b	d6,(a0,d4.w)
	move.b	dsp1n_lo,1(a0,d4.w)
	addq.w	#1,dsp1n_n
	move.w	dsp1n_n,d4
	cmp.w	dsp1n_need,d4
	bne.s	.x
	bra.s	dsp1n_run
.x:	rts

; d6.w = a whole parameter word, while a native command collects them and
; no byte of a word is pending (rt_wdspw). May clobber d4/a0.
	xdef	dsp1n_word
dsp1n_word:
	move.w	dsp1n_n,d4
	add.w	d4,d4
	lea	dsp1n_in,a0
	move.w	d6,(a0,d4.w)
	addq.w	#1,dsp1n_n
	move.w	dsp1n_n,d4
	cmp.w	dsp1n_need,d4
	beq.s	dsp1n_run
	rts

; d6.w = the next result word, low byte first as rt_rdspw reads it, while
; results are served and no byte of a word is pending. May clobber d4/a0.
	xdef	dsp1n_rdword
dsp1n_rdword:
	move.w	dsp1n_n,d4
	add.w	d4,d4
	lea	dsp1n_out,a0
	move.w	(a0,d4.w),d6
	addq.w	#1,dsp1n_n
	move.w	dsp1n_n,d4
	cmp.w	dsp1n_nout,d4
	beq	dsp1n_done
	rts

dsp1n_run:
	; all parameters in: run it. C may use a5/a6 as scratch, and the VBL
	; runs the translated NMI and IRQs on the interrupted registers: no VBL
	; while C runs (as in UPD_GLUE).
	movem.l	d0-d1/a1,-(sp)
	move.w	sr,-(sp)
	move.w	#$2400,sr
	pea	upd_rom
	pea	upd_ram
	pea	dsp1n_out
	pea	dsp1n_in
	moveq	#0,d0
	move.w	dsp1n_cmd,d0
	move.l	d0,-(sp)
	jsr	dsp1n_exec
	lea	20(sp),sp
	move.w	d0,dsp1n_nout
	move.w	(sp)+,sr
	movem.l	(sp)+,d0-d1/a1
	addq.l	#1,dsp1n_runs
	clr.w	dsp1n_n
	move.w	#$8000,dsp1n_sr		; RQM: the first result is ready
	move.b	#2,dsp1n_mode
	rts

; A read while a native command runs (d7.w = address): SR's high byte, or
; the results' bytes, low first. After the last one the firmware is left
; at its command poll.
dsp1n_read:
	cmpi.w	#$7000,d7
	bcs.s	.dr
	move.b	dsp1n_sr,d6
	rts
.dr:	cmpi.b	#2,dsp1n_mode
	beq.s	.res
	moveq	#0,d6			; (DR while parameters are due: not SMK's)
	rts
.res:	move.w	dsp1n_n,d4
	add.w	d4,d4
	lea	dsp1n_out,a0
	tst.b	dsp1n_hi
	bne.s	.hi
	st	dsp1n_hi
	bset	#UPD_DRS-8,dsp1n_sr
	move.b	1(a0,d4.w),d6
	rts
.hi:	sf	dsp1n_hi
	bclr	#UPD_DRS-8,dsp1n_sr
	move.b	(a0,d4.w),d6
	addq.w	#1,dsp1n_n
	move.w	dsp1n_n,d4
	cmp.w	dsp1n_nout,d4
	beq.s	dsp1n_done
	rts
dsp1n_done:
	sf	dsp1n_mode		; done: as $000-$004 leave it
	move.w	#$004,upd_state+upd_pc
	move.w	#$8400,upd_state+upd_sr
	move.w	#$0080,upd_state+upd_dr
	move.w	#$00C0,upd_state+upd_b_saved
	st	upd_parked
	move.b	#$80,upd_parkrqm
.x:	rts
	endif

; upd_run, timed into rt_t_dsp1 (1/38400 s, rt_now). Preserves every
; register and the CCR.
upd_run_timed:
	ifne	SMKX_DIAG
	move.w	ccr,-(sp)
	move.l	d0,-(sp)
	move.l	rt_t_vbl,-(sp)		; VBL time that passes meanwhile is not ours
	jsr	rt_now
	move.l	d0,-(sp)
	bsr	upd_run
	jsr	rt_now
	sub.l	(sp)+,d0
	sub.l	rt_t_vbl,d0
	add.l	(sp)+,d0
	add.l	d0,rt_t_dsp1
	move.l	(sp)+,d0
	move.w	(sp)+,ccr
	rts
	endif				; release: straight into upd_run

; Run the chip's program from its resume point to its next host poll.
; Preserves every register and the CCR.
;
; The program always stops at a JRQM/JNRQM on itself, which waits while RQM
; keeps the value it had then (upd_parkrqm). As long as the host has not
; changed RQM since (status reads and the first byte of a 16-bit transfer
; do not), running it would stop again at once with nothing changed: skip.
upd_run:
	move.w	ccr,-(sp)
	tst.b	upd_parked
	beq.s	.enter
	move.l	d4,-(sp)
	move.b	upd_state+upd_sr,d4
	andi.b	#$80,d4			; RQM
	cmp.b	upd_parkrqm,d4
	movem.l	(sp)+,d4		; movem leaves the CCR alone
	bne.s	.enter			; RQM changed: the program runs on
	move.w	(sp)+,ccr
	rts
.enter:	movem.l	d0-d7/a0-a6,-(sp)
	lea	upd_state,a3
	lea	upd_ram,a0
	lea	upd_rom,a1
	move.w	upd_a_saved(a3),d0
	move.w	upd_b_saved(a3),d1
	move.w	upd_k_saved(a3),d2
	move.w	upd_l_saved(a3),d3
	moveq	#0,d5
	move.w	upd_dp_saved(a3),d5
	moveq	#0,d6
	move.w	upd_rp_saved(a3),d6
	moveq	#0,d7
	move.w	upd_pc(a3),d7
	addq.l	#1,upd_runs
	jmp	([upd_code_table,d7.w*4])

; The translated program lands here at a host poll, with upd_pc set.
upd_yield:
	move.b	upd_sr(a3),d7		; parked while RQM keeps this value
	andi.b	#$80,d7
	move.b	d7,upd_parkrqm
	st	upd_parked
	move.w	d0,upd_a_saved(a3)
	move.w	d1,upd_b_saved(a3)
	move.w	d2,upd_k_saved(a3)
	move.w	d3,upd_l_saved(a3)
	move.w	d5,upd_dp_saved(a3)
	move.w	d6,upd_rp_saved(a3)
	movem.l	(sp)+,d0-d7/a0-a6
	move.w	(sp)+,ccr
	rts

; A branch condition the translator does not model (serial ports).
upd_trap:
	sf	upd_parked
	move.w	upd_pc(a3),d7
	andi.l	#$ffff,d7
	move.l	d7,-(sp)
	jmp	rt_upd_trap

	section	data,data
	cnop	0,2
upd_rom:
	incbin	"dsp1.bin",6144,2048	; data ROM, 1024 big-endian words

	section	bss,bss
	cnop	0,4
upd_state:	ds.b	upd_state_size
upd_ram:	ds.w	256
upd_runs:	ds.l	1
upd_parked:	ds.b	1		; $FF: stopped at a host poll (upd_yield)
upd_parkrqm:	ds.b	1		; RQM ($80 or 0) that keeps it there
	ifne	UPD_NATIVE
dsp1n_mode:	ds.b	1		; 0 the firmware, 1 parameters, 2 results
dsp1n_hi:	ds.b	1		; the next byte is a word's high byte
dsp1n_lo:	ds.b	1
	even
dsp1n_cmd:	ds.w	1
dsp1n_need:	ds.w	1		; parameter words
dsp1n_n:	ds.w	1		; words so far
dsp1n_nout:	ds.w	1		; result words
dsp1n_sr:	ds.w	1		; SR as the host sees it
dsp1n_in:	ds.w	8
dsp1n_out:	ds.w	8
dsp1n_runs:	ds.l	1
	endif
	even
	xdef	upd_runs

	ifd	SMKX_DSP1_LOG
	section	bss,bss
	xdef	dsp1_log,dsp1_log_n
dsp1_log_n:	ds.l	1
dsp1_log:	ds.w	DSP1_LOG_MAX
	section	text,code
	endif

	section	data,data
upd_hle_on:	dc.b	-UPD_HLE		; $FF when on
	even
	section	text,code
