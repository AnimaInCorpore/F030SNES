; PLANBENCH.TOS: glue for src/planbench.c: NatFeats output, a stack, the frames and the ROM.

	xref	tctest_main
	xdef	m7t_print,m7t_hex,pb_frames,pb_rom

	section	text,code
start:
	lea	stack_top,sp
	pea	nf_name
	bsr	nf_id
	addq.l	#4,sp
	move.l	d0,nf_stderr
	clr.l	-(sp)
	move.w	#$20,-(sp)		; Super(0)
	trap	#1
	addq.l	#6,sp
	move.l	d0,old_ssp
	jsr	tctest_main
	move.l	old_ssp,-(sp)
	move.w	#$20,-(sp)
	trap	#1
	addq.l	#6,sp
	clr.w	-(sp)
	move.w	#$4c,-(sp)
	trap	#1

; void m7t_print(const char *s)
m7t_print:
	move.l	4(sp),a0
print:	movem.l	d0-d1/a0-a1,-(sp)
	move.l	a0,-(sp)
	move.l	nf_stderr,-(sp)
	bsr.s	nf_call
	addq.l	#8,sp
	movem.l	(sp)+,d0-d1/a0-a1
	rts

; void m7t_hex(uint32_t v)
m7t_hex:
	move.l	4(sp),d1
	move.l	d2,-(sp)
	lea	hexbuf+8,a0
	clr.b	(a0)
	moveq	#7,d0
.dig:	move.b	d1,d2
	andi.b	#$f,d2
	addi.b	#'0',d2
	cmpi.b	#'9',d2
	bls.s	.put
	addq.b	#7,d2
.put:	move.b	d2,-(a0)
	lsr.l	#4,d1
	dbra	d0,.dig
	move.l	(sp)+,d2
	bra.s	print

nf_id:	dc.w	$7300
	rts
nf_call:
	dc.w	$7301
	rts

	section	data,data
nf_name:	dc.b	"NF_STDERR",0
	even
pb_frames:
	incbin	"planbench.bin"
	even
pb_rom:
	incbin	"sciv.sfc"
	even
	section	bss,bss
nf_stderr:	ds.l	1
old_ssp:	ds.l	1
hexbuf:		ds.b	12
		ds.b	8192
stack_top:
