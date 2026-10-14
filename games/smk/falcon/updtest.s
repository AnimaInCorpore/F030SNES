; UPDTEST.TOS: the translated DSP-1 program against the Python interpreter.
; Replays build/smkx/updtest_ops.i (write DR byte / read DR byte / read SR)
; and prints every byte read over NatFeats as "UPD xx xx ...", 16 per line.
; tools/smkx/updtest.py check compares them with updtest_expect.txt.

	section	text,code
start:
	lea	stack_top,sp
	clr.l	-(sp)
	move.w	#$20,-(sp)		; Super(0)
	trap	#1
	addq.l	#6,sp
	pea	nf_name
	bsr	nf_id
	addq.l	#4,sp
	move.l	d0,nf_stderr
	lea	msg_start,a0
	bsr	print
	lea	updtest_ops,a2
	move.l	#UPDTEST_OPS-1,d5
	moveq	#0,d4			; bytes on the current line
.op:	move.b	(a2)+,d3
	move.b	(a2)+,d6
	cmpi.b	#1,d3
	bne.s	.rd
	move.w	#$6000,d7
	bsr	dsp1_write
	bra.s	.next
.rd:	move.w	#$6000,d7
	cmpi.b	#2,d3
	beq.s	.r
	move.w	#$7000,d7
.r:	bsr	dsp1_read
	bsr	put_byte
.next:	subq.l	#1,d5
	bpl.s	.op
	tst.w	d4
	beq.s	.done
	lea	msg_nl,a0
	bsr	print
.done:	lea	msg_end,a0
	bsr	print
	move.l	upd_runs,d7
	bsr	print_hex8
	lea	msg_nl,a0
	bsr	print
	clr.w	-(sp)
	trap	#1			; Pterm0

; d6.b -> "xx " on the current line; 16 per line, prefixed "UPD ".
put_byte:
	tst.w	d4
	bne.s	.cont
	lea	msg_upd,a0
	bsr	print
.cont:	lea	hexbuf,a0
	move.b	d6,d7
	lsr.b	#4,d7
	bsr	.digit
	move.b	d7,(a0)+
	move.b	d6,d7
	bsr	.digit
	move.b	d7,(a0)+
	move.b	#' ',(a0)+
	clr.b	(a0)
	lea	hexbuf,a0
	bsr	print
	addq.w	#1,d4
	cmpi.w	#16,d4
	bne.s	.x
	moveq	#0,d4
	lea	msg_nl,a0
	bsr	print
.x:	rts
.digit:	andi.b	#$f,d7
	cmpi.b	#10,d7
	bcs.s	.num
	addi.b	#'A'-10,d7
	rts
.num:	addi.b	#'0',d7
	rts

print_hex8:
	movem.l	d6-d7/a0,-(sp)
	lea	hexbuf+8,a0
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
	bsr	print
	movem.l	(sp)+,d6-d7/a0
	rts

rt_upd_trap:
	lea	msg_trap,a0
	bsr	print
	move.l	(sp)+,d7
	bsr	print_hex8
	lea	msg_nl,a0
	bsr	print
	move.w	#1,-(sp)
	move.w	#$4c,-(sp)
	trap	#1

nf_id:	dc.w	$7300
	rts
nf_call:
	dc.w	$7301
	rts
print:	movem.l	d0-d1/a0-a1,-(sp)
	move.l	a0,-(sp)
	move.l	nf_stderr,-(sp)
	bsr.s	nf_call
	addq.l	#8,sp
	movem.l	(sp)+,d0-d1/a0-a1
	rts

SMKX_DIAG	equ	1
	include	'smkx_upd.s'

; No clock in the test harness: upd_run_timed's hooks.
rt_now:	moveq	#0,d0
	rts
	section	bss,bss
rt_t_dsp1:	ds.l	1
rt_t_vbl:	ds.l	1
rt_n_hle:	ds.l	1
rt_t_hle:	ds.l	1
	section	text,code

	section	data,data
nf_name:	dc.b	"NF_STDERR",0
msg_start:	dc.b	"UPDTEST start",10,0
msg_upd:	dc.b	"UPD ",0
msg_nl:		dc.b	10,0
msg_end:	dc.b	"UPDTEST end, program runs $",0
msg_trap:	dc.b	"UPDTEST: untranslated DSP-1 branch at $",0
	even
	include	'updtest_ops.i'
	even

	section	bss,bss
nf_stderr:	ds.l	1
hexbuf:		ds.b	12
		ds.b	4096
stack_top:
