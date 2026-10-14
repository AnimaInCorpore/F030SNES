; LCPLAY.TOS: plays a recorded list-compositor stream (build/host/*.lcp, written by
; `lc_model hsim|hreal` with LC_REC=file, checked by `lc_model play`) on the Falcon:
; DATA (what the DSP would send: source rows, clean ring and layer ring groups) and COPY
; (the 030's restores) into a memory laid out as the model's, BLIT as one blitter run each
; (HOP 2, OP 3, endmasks, skew, FXSR/NFSR), and each frame ends with the Videl pointed at the
; page's window: start group, HSCROLL, the line offset that keeps the ring's row stride.
; 256x224, eight planes, as the runtime (smkx_video.s). Double buffered: the next frame is
; drawn to the page that is not shown; the start address is written and the flip awaited
; before the next frame starts.
;
;   CHECK=1: after every frame the page window is summed (rotate left 1 then add each word,
;   as the recorder does) and compared with the stream's checksum; the result is printed
;   over NatFeats and the program ends after one pass. CHECK=0 (default): loops until a key.
;
;   TIMING=1 (F030SMW's addition, taken over): PASSES passes over the stream without the vertical-blank
;   wait, then the total 200 Hz ticks. SKIPBLIT/SKIPCOPY/SKIPDATA=1 read past that op kind, so
;   that running with one skipped and subtracting gives what it cost (F030SMAS analysis/list-compositor-smas.md; the d6 scratch register is its fix of F030SMW 9348783, which used d5, the memory base).
;
; From F030CT's falcon/lcplay.s (fc09a13); the stream is build/lc/stream.lcp (Makefile: check-lc-play).
; No timing claim about a Falcon: Hatari's blitter and Videl are not a Falcon's.

	ifnd	CHECK
CHECK	equ	0
	endif
	ifnd	NOHS
NOHS	equ	0			; 1: ignore HSCROLL (diagnosis)
	endif
	ifnd	COPYSCR
COPYSCR	equ	0			; 1: show a contiguous copy of the window (diagnosis)
	endif
	ifnd	LOOP
LOOP	equ	1			; 0: one pass, then end (for recording)
	endif
	ifnd	TIMING
TIMING	equ	0			; 1: PASSES passes, no VBL wait, total 200 Hz ticks printed
	endif
	ifnd	PASSES
PASSES	equ	5
	endif
	ifnd	SKIPBLIT
SKIPBLIT	equ	0		; 1: BLIT records are read past, no blitter
	endif
	ifnd	SKIPCOPY
SKIPCOPY	equ	0
	endif
	ifnd	SKIPDATA
SKIPDATA	equ	0
	endif
MEMSZ	equ	2884640			; the stream's memsz (the model's mem), checked below
ROWB	equ	512			; bytes of a ring row: 32 groups of 16

	section	text,code

start:
	lea	ustack_top,sp
	pea	nf_name
	bsr	nf_id
	addq.l	#4,sp
	move.l	d0,nf_stderr
	move.w	#89,-(sp)		; XBIOS Montype
	trap	#14
	addq.l	#2,sp
	move.w	d0,monitor
	move.w	#2,-(sp)		; XBIOS Physbase
	trap	#14
	addq.l	#2,sp
	move.l	d0,old_phys
	clr.l	-(sp)
	move.w	#$20,-(sp)		; Super(0)
	trap	#1
	addq.l	#6,sp
	move.l	d0,old_ssp
	lea	sstack_top,sp
	lea	msg_start,a0
	bsr	print
	tst.w	monitor
	beq	quit			; monochrome: nothing to show
	lea	stream,a0
	cmpi.l	#$4c435031,(a0)		; 'LCP1'
	bne	badstream
	cmpi.l	#MEMSZ,8(a0)
	bne	badstream
	move.l	4(a0),nframes
	; data cache off (the blitter writes behind it), instruction cache on and cleared
	move.l	#$0801,d0
	dc.l	$4e7b0002		; movec d0,cacr
	lea	mem_raw,a0
	move.l	a0,d0
	add.l	#255,d0
	andi.l	#$ffffff00,d0
	move.l	d0,mem_base
	bsr	vid_save
	bsr	vid_on
	clr.l	bad_frames
	move.l	#-1,first_bad

pass:	bsr	clear_mem
	move.l	$462.w,vbl_start
	move.l	$462.w,vbl_prev
	clr.l	overruns
	move.l	$4ba.w,tstart
	lea	stream+32,a5
	move.l	mem_base,d5
	movea.l	d5,a6
	move.l	nframes,d7
	clr.l	frameno

opl:	move.w	(a5)+,d0
	beq	endframe
	subq.w	#1,d0
	beq.s	op_data
	subq.w	#1,d0
	beq.s	op_copy
	subq.w	#1,d0
	beq.s	op_blit

; PAL: 256 longwords, loaded when the frame is shown
	addq.l	#2,a5
	move.l	a5,pal_ptr
	lea	1024(a5),a5
	st	pal_pending
	bra.s	opl

; DATA: dst (mem offset), n bytes, n bytes of data
op_data:
	move.l	(a5)+,d1
	move.w	(a5)+,d2
	ifne	SKIPDATA
	moveq	#0,d3
	move.w	d2,d3
	adda.l	d3,a5
	bra	opl
	endif
	lea	(a6,d1.l),a0
	lsr.w	#1,d2
	subq.w	#1,d2
.dl:	move.w	(a5)+,(a0)+
	dbra	d2,.dl
	bra.s	opl

; COPY: dst, src (mem offsets), n bytes (a multiple of 16)
op_copy:
	move.l	(a5)+,d1
	move.l	(a5)+,d2
	move.w	(a5)+,d3
	ifne	SKIPCOPY
	bra	opl
	endif
	lea	(a6,d1.l),a0
	lea	(a6,d2.l),a1
	lsr.w	#4,d3
	subq.w	#1,d3
.cl:	movem.l	(a1)+,d0-d2/d4
	movem.l	d0-d2/d4,(a0)
	lea	16(a0),a0
	dbra	d3,.cl
	bra	opl

; BLIT: src, dst, xcount, ycount, sxi, syi, dxi, dyi, em1, em2, em3, fxsr, nfsr, skew, pad
op_blit:
	ifne	SKIPBLIT
	lea	30(a5),a5
	bra	opl
	endif
	lea	$ffff8a00.w,a0
	move.l	(a5)+,d1
	add.l	d5,d1
	move.l	(a5)+,d2
	add.l	d5,d2
	move.w	(a5)+,d3
	move.w	(a5)+,d4
	move.w	(a5)+,$20(a0)		; source x increment
	move.w	(a5)+,$22(a0)		; source y increment
	move.w	(a5)+,$2e(a0)		; destination x increment
	move.w	(a5)+,$30(a0)		; destination y increment
	move.w	(a5)+,$28(a0)		; endmask 1
	move.w	(a5)+,$2a(a0)		; endmask 2
	move.w	(a5)+,$2c(a0)		; endmask 3
	move.l	d1,$24(a0)		; source address
	move.l	d2,$32(a0)		; destination address
	move.w	d3,$36(a0)		; x count
	move.w	d4,$38(a0)		; y count
	moveq	#0,d3
	tst.b	(a5)+			; FXSR
	beq.s	.nf
	bset	#7,d3
.nf:	tst.b	(a5)+			; NFSR
	beq.s	.ns
	bset	#6,d3
.ns:	or.b	(a5)+,d3		; skew
	addq.l	#1,a5
	move.b	#2,$3a(a0)		; HOP: source
	move.b	#3,$3b(a0)		; OP: D = S
	move.b	d3,$3d(a0)		; skew, FXSR, NFSR
	move.b	#$c0,$3c(a0)		; start, hog
.bw:	tst.b	$3c(a0)
	bmi.s	.bw
	bra	opl

; END of frame: page, window address (mem offset), HSCROLL, groups fetched, checksums
endframe:
	addq.l	#2,a5			; page
	move.l	(a5)+,d1
	move.w	(a5)+,d2
	move.w	(a5)+,d3
	move.l	(a5)+,crc_window
	addq.l	#8,a5			; the exact renderer's and the whole memory's checksums
	add.l	d5,d1
	ifne	CHECK
	bsr	check_window
	endif
	ifne	COPYSCR
	movea.l	d1,a0
	lea	screen_raw,a1
	move.l	a1,d4
	add.l	#255,d4
	andi.l	#$ffffff00,d4
	movea.l	d4,a1
	move.l	d4,d1
	move.w	#223,d6
.cr:	movea.l	a0,a2
	moveq	#15,d4
.cg:	move.l	(a2)+,(a1)+
	move.l	(a2)+,(a1)+
	move.l	(a2)+,(a1)+
	move.l	(a2)+,(a1)+
	dbra	d4,.cg
	lea	ROWB(a0),a0
	dbra	d6,.cr
	endif
	move.l	$462.w,d0
	cmp.l	vbl_prev,d0
	beq.s	.inT
	addq.l	#1,overruns
.inT:	move.l	d1,d4
	move.l	d4,d6			; high and mid first: writing them clears the low byte (Hatari's Videl).
	lsr.l	#8,d6			; d6, not d5: d5 is the memory base all stream ops add (F030SMW's 9348783 used d5
	lsr.l	#8,d6			; here, which broke every frame after the first)
	move.b	d6,$ffff8201.w
	move.l	d4,d6
	lsr.l	#8,d6
	move.b	d6,$ffff8203.w
	move.b	d4,$ffff820d.w		; the low byte last
	ifne	NOHS
	moveq	#0,d2
	moveq	#16,d3
	endif
	move.b	d2,$ffff8265.w		; HSCROLL
	moveq	#32,d4
	sub.w	d3,d4
	lsl.w	#3,d4			; words to the next ring row after the line's fetch
	ifne	COPYSCR
	moveq	#0,d4
	endif
	move.w	d4,$ffff820e.w
	ifeq	TIMING
.vb:	cmp.l	$462.w,d0		; shown from the next vertical blank on
	beq.s	.vb
	endif
	move.l	$462.w,vbl_prev
	tst.b	pal_pending
	beq.s	.np
	bsr	load_pal
.np:	addq.l	#1,frameno
	subq.l	#1,d7
	bne	opl
	ifne	TIMING
	move.l	$4ba.w,d0
	sub.l	tstart,d0
	add.l	d0,ttotal
	addq.l	#1,tpasses
	cmpi.l	#PASSES,tpasses
	blo	pass
	lea	msg_ticks,a0
	bsr	print
	move.l	ttotal,d0
	bsr	print_dec
	lea	msg_nl,a0
	bsr	print
	lea	msg_passes,a0
	bsr	print
	move.l	tpasses,d0
	bsr	print_dec
	lea	msg_nl,a0
	bsr	print
	move.l	nframes,d0
	bsr	print_dec
	lea	msg_nl,a0
	bsr	print
	bra	done
	endif
	ifne	CHECK
	bra.s	done
	else
	ifne	LOOP
	bsr	key_pressed
	beq	pass
	endif
	bra.s	done
	endif

done:
	lea	msg_vbl,a0
	bsr	print
	move.l	$462.w,d0
	sub.l	vbl_start,d0
	bsr	print_dec
	lea	msg_over,a0
	bsr	print
	move.l	overruns,d0
	bsr	print_dec
	lea	msg_nl,a0
	bsr	print
	move.l	frameno,d0
	bsr	print_dec
	lea	msg_nl,a0
	bsr	print
	ifne	CHECK
	lea	msg_done,a0
	bsr	print
	move.l	frameno,d0
	bsr	print_dec
	lea	msg_bad,a0
	bsr	print
	move.l	bad_frames,d0
	bsr	print_dec
	lea	msg_first,a0
	bsr	print
	move.l	first_bad,d0
	bsr	print_dec
	lea	msg_nl,a0
	bsr	print
	endif
	bsr	vid_restore
quit:	move.l	old_ssp,-(sp)
	move.w	#$20,-(sp)
	trap	#1
	addq.l	#6,sp
	clr.w	-(sp)
	trap	#1

badstream:
	lea	msg_bad_stream,a0
	bsr	print
	bra.s	quit

	ifne	CHECK
; the page window's checksum against the stream's: d1 = the window's address, d3 = groups
check_window:
	movem.l	d0-d6/a0-a1,-(sp)
	movea.l	d1,a0
	moveq	#0,d0
	moveq	#0,d2
	move.w	#223,d6
.r:	movea.l	a0,a1
	move.w	d3,d5
	subq.w	#1,d5
.g:	moveq	#7,d4
.w:	move.w	(a1)+,d2
	rol.l	#1,d0
	add.l	d2,d0
	dbra	d4,.w
	dbra	d5,.g
	lea	ROWB(a0),a0
	dbra	d6,.r
	cmp.l	crc_window,d0
	beq.s	.ok
	addq.l	#1,bad_frames
	tst.l	first_bad
	bpl.s	.ok
	move.l	frameno,first_bad
.ok:	movem.l	(sp)+,d0-d6/a0-a1
	rts
	endif

; the stream's palette longwords to the Videl
load_pal:
	movea.l	pal_ptr,a0
	lea	$ffff9800.w,a1
	move.w	#255,d0
.p:	move.l	(a0)+,(a1)+
	dbra	d0,.p
	sf	pal_pending
	rts

; Z set when no key is waiting (GEMDOS Cconis)
key_pressed:
	move.w	#11,-(sp)
	trap	#1
	addq.l	#2,sp
	tst.w	d0
	beq.s	.none
	move.w	#8,-(sp)		; Cnecin: take the key
	trap	#1
	addq.l	#2,sp
	moveq	#1,d0
.none:	rts

clear_mem:
	movea.l	mem_base,a0
	move.l	#MEMSZ/16,d0
	moveq	#0,d1
	moveq	#0,d2
	moveq	#0,d3
	moveq	#0,d4
.c:	movem.l	d1-d4,(a0)
	lea	16(a0),a0
	subq.l	#1,d0
	bne.s	.c
	rts

; ---------------------------------------------------------------------------
; Videl: 256x224, eight planes (the timings of smkx_video.s); the window registers
; are set per frame.

vid_save:
	move.b	$ffff8260.w,sv_shift
	move.w	$ffff8266.w,sv_spshift
	move.w	$ffff820a.w,sv_sync
	move.l	$ffff820e.w,sv_offset
	move.l	$ffff8264.w,sv_hscroll
	movem.l	$ffff8282.w,d0-d3
	movem.l	d0-d3,sv_h
	movem.l	$ffff82a2.w,d0-d3
	movem.l	d0-d3,sv_v
	move.l	$ffff82c0.w,sv_control
	lea	sv_palette,a0
	lea	$ffff9800.w,a1
	moveq	#15,d1
.p:	move.l	(a1)+,(a0)+
	dbra	d1,.p
	rts

vid_restore:
	lea	sv_palette,a0
	lea	$ffff9800.w,a1
	moveq	#15,d1
.p:	move.l	(a0)+,(a1)+
	dbra	d1,.p
	movem.l	sv_h,d0-d3
	movem.l	d0-d3,$ffff8282.w
	movem.l	sv_v,d0-d3
	movem.l	d0-d3,$ffff82a2.w
	move.l	sv_control,$ffff82c0.w
	move.l	sv_hscroll,$ffff8264.w
	move.l	sv_offset,$ffff820e.w
	clr.w	$ffff8266.w
	move.w	sv_spshift,$ffff8266.w
	move.b	sv_shift,$ffff8260.w
	move.w	sv_sync,$ffff820a.w
	move.l	old_phys,d0
	bra	vid_base

vid_on:
	move.l	mem_base,d0
	bsr	vid_base
	cmpi.w	#2,monitor
	beq.s	.vga
	move.l	#$00fe00ab,$ffff8282.w	; RGB/TV, 50 Hz: HHT, HBB
	move.l	#$0047002c,$ffff8286.w	; HBE, HDB
	move.l	#$004d00d8,$ffff828a.w	; HDE, HSS
	move.l	#$0271021f,$ffff82a2.w	; VFT, VBB
	move.l	#$005f005f,$ffff82a6.w	; VBE, VDB
	move.l	#$021f026b,$ffff82aa.w	; VDE, VSS
	move.w	#$0200,$ffff820a.w
	move.w	#$0181,$ffff82c0.w
	clr.w	$ffff82c2.w
	bra.s	.common
.vga:	move.l	#$00c6006d,$ffff8282.w	; VGA, doubled lines
	move.l	#$003502aa,$ffff8286.w
	move.l	#$004b0096,$ffff828a.w
	move.l	#$041903cf,$ffff82a2.w
	move.l	#$004d004b,$ffff82a6.w
	move.l	#$03cb0415,$ffff82aa.w
	move.w	#$0200,$ffff820a.w
	move.w	#$0182,$ffff82c0.w
	move.w	#5,$ffff82c2.w
.common:
	clr.w	$ffff8266.w
	move.w	#$0010,$ffff8266.w	; eight planes, 256 colours
	clr.w	$ffff8264.w		; (a word: $8266 holds the plane count)
	move.w	#128,$ffff8210.w	; 256 pixels of eight planes: 16 groups
	move.w	#128,$ffff820e.w
	lea	$ffff9800.w,a1
	move.w	#255,d1
.pal:	clr.l	(a1)+
	dbra	d1,.pal
	rts

vid_base:
	move.l	d0,d1			; high and mid first: writing them clears the low byte
	lsr.l	#8,d1
	lsr.l	#8,d1
	move.b	d1,$ffff8201.w
	move.l	d0,d1
	lsr.l	#8,d1
	move.b	d1,$ffff8203.w
	move.b	d0,$ffff820d.w		; the low byte last
	rts

; ---------------------------------------------------------------------------
; NatFeats output

nf_id:	dc.w	$7300			; NF_ID(name) -> d0
	rts
nf_call:
	dc.w	$7301			; NF_CALL(id, args...)
	rts

; a0 = zero-terminated string
print:
	movem.l	d0-d1/a0-a1,-(sp)
	move.l	nf_stderr,d0
	beq.s	.none
	move.l	a0,-(sp)
	move.l	d0,-(sp)
	bsr.s	nf_call
	addq.l	#8,sp
.none:	movem.l	(sp)+,d0-d1/a0-a1
	rts

; d0.l in decimal
print_dec:
	movem.l	d0-d3/a0,-(sp)
	lea	decbuf+11,a0
	clr.b	(a0)
	moveq	#0,d3
	tst.l	d0
	bpl.s	.pos
	moveq	#1,d3			; -1 (none)
	neg.l	d0
.pos:	move.l	d0,d1
.d:	divu.w	#10,d1
	move.l	d1,d2
	swap	d2
	add.b	#'0',d2
	move.b	d2,-(a0)
	andi.l	#$ffff,d1
	bne.s	.d
	tst.b	d3
	beq.s	.np
	move.b	#'-',-(a0)
.np:	bsr	print
	movem.l	(sp)+,d0-d3/a0
	rts

	section	data,data
nf_name:	dc.b	"NF_STDERR",0
msg_start:	dc.b	"LCPLAY: start",10,0
msg_done:	dc.b	"LCPLAY: frames ",0
msg_bad:	dc.b	" window checksum mismatches ",0
msg_first:	dc.b	" first bad frame ",0
msg_nl:		dc.b	10,0
msg_ticks:	dc.b	"LCPLAY: ticks(200Hz) total ",0
msg_passes:	dc.b	"LCPLAY: passes ",0
msg_vbl:	dc.b	"LCPLAY: vertical blanks used ",0
msg_over:	dc.b	", frames that overran a blank ",0
msg_bad_stream:	dc.b	"LCPLAY: not the stream this player was built for",10,0
	even
stream:		incbin	"stream.lcp"
	even

	section	bss,bss
nf_stderr:	ds.l	1
tstart:		ds.l	1
ttotal:		ds.l	1
tpasses:	ds.l	1
old_ssp:	ds.l	1
old_phys:	ds.l	1
monitor:	ds.w	1
nframes:	ds.l	1
frameno:	ds.l	1
bad_frames:	ds.l	1
vbl_start:	ds.l	1
vbl_prev:	ds.l	1
overruns:	ds.l	1
first_bad:	ds.l	1
crc_window:	ds.l	1
pal_ptr:	ds.l	1
pal_pending:	ds.b	1
	even
mem_base:	ds.l	1
sv_shift:	ds.b	1
	even
sv_spshift:	ds.w	1
sv_sync:	ds.w	1
sv_offset:	ds.l	1
sv_hscroll:	ds.l	1
sv_h:		ds.l	4
sv_v:		ds.l	4
sv_control:	ds.l	1
sv_palette:	ds.l	16
decbuf:		ds.b	12
	even
ustack:		ds.b	1024
ustack_top:
sstack:		ds.b	4096
sstack_top:
screen_raw:	ds.b	57344+512
mem_raw:	ds.b	MEMSZ+512
