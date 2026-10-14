; SSITEST.TOS: the scaffold for CPU -> DSP data by DMA, without the CPU
; writing a word: DMA playback -> crossbar -> DSP SSI receive. The DSP side
; is falcon/dsp/ssirx.asm (a receive interrupt fills a ring).
;
; Route: Devconnect(DMAPLAY -> DSPREC, 25.175 MHz, prescale 1),
; Dsptristate(xmit off, recv on), 16-bit stereo, 4 tracks: 8 words a
; frame, 49,170 frames/s, at most 393,000 words/s (786 KB/s).
;
; Both crossbar modes, each with two passes:
;   mode 0  no handshake: a word every slot (the DSP must keep up)
;   mode 1  handshake: the DSP asks for every word (ssirx.asm)
; Pass 1: the DSP idles; every word is read back and compared.
; Pass 2: the DSP runs a busy loop (drawing, in the real thing) while the
; buffer plays; then count and checksum are compared.
; Results over NatFeats: "SSITEST ..." lines.

N	equ	4096			; words per pass (< the ring's 8192)

	section	text,code
start:
	lea	stack_top,sp
	pea	nf_name
	bsr	nf_id
	addq.l	#4,sp
	move.l	d0,nf_stderr
	move.w	#1,-(sp)		; Dsp_ExecBoot(image, words, ability)
	move.l	#KERNEL_WORDS,-(sp)
	pea	kernel
	move.w	#110,-(sp)
	trap	#14
	lea	12(sp),sp
	bsr	sound_claim
	tst.w	d0
	bne	.noroute
	clr.l	-(sp)
	move.w	#$20,-(sp)		; Super(0)
	trap	#1
	addq.l	#6,sp
	move.l	d0,old_ssp
	bsr	test
	move.w	d0,result
	move.l	old_ssp,-(sp)
	move.w	#$20,-(sp)
	trap	#1
	addq.l	#6,sp
	bsr	sound_release
	move.w	result,-(sp)
	move.w	#$4c,-(sp)
	trap	#1
.noroute:
	lea	msg_noroute,a0
	bsr	print
	move.w	#2,-(sp)
	move.w	#$4c,-(sp)
	trap	#1

xb	macro
	move.w	#\1,-(sp)
	trap	#14
	endm

sound_claim:
	xb	128			; Locksnd
	addq.l	#2,sp
	cmpi.w	#1,d0
	bne	.fail
	clr.w	-(sp)
	xb	136			; Buffoper(0)
	addq.l	#4,sp
	move.w	#1,-(sp)
	xb	132			; Setmode(STEREO16)
	addq.l	#4,sp
	move.w	#3,-(sp)
	move.w	#3,-(sp)
	xb	133			; Settracks(4 play, 4 record)
	addq.l	#6,sp
	clr.w	-(sp)
	xb	134			; Setmontracks(0)
	addq.l	#4,sp
	move.w	#1,-(sp)		; receive on
	clr.w	-(sp)			; transmit off
	xb	137			; Dsptristate
	addq.l	#6,sp
	moveq	#0,d0
	rts
.fail:	moveq	#1,d0
	rts

; route: DMA playback -> DSP receive, handshake when mode = 1
route:	moveq	#1,d0
	sub.l	mode,d0			; protocol: 0 handshake, 1 none
	move.w	d0,-(sp)
	move.w	#1,-(sp)		; prescale 1
	clr.w	-(sp)			; 25.175 MHz
	move.w	#2,-(sp)		; to DSP receive
	clr.w	-(sp)			; from DMA playback
	xb	139			; Devconnect
	lea	12(sp),sp
	rts

sound_release:
	clr.w	-(sp)
	xb	136			; Buffoper(0)
	addq.l	#4,sp
	clr.w	-(sp)
	clr.w	-(sp)
	xb	137			; Dsptristate(0, 0)
	addq.l	#6,sp
	clr.w	-(sp)
	clr.w	-(sp)
	xb	133			; Settracks(0, 0)
	addq.l	#6,sp
	xb	129			; Unlocksnd
	addq.l	#2,sp
	rts

; ---------------------------------------------------------------------------
; the test (supervisor); d0 = 0 when both passes are exact

test:	moveq	#1,d0			; PING
	bsr	hput
	bsr	hget
	bmi	.dead
	move.l	d0,d7
	lea	msg_ping,a0
	bsr	print
	move.l	d7,d1
	bsr	hex
	bsr	nl
	cmpi.l	#$535352,d7
	bne	.dead
	; the pattern: w(i) = i * 40503 + 12345, and its sum and XOR as the
	; ring holds them (w << 8, 24 bits)
	lea	buf,a0
	move.l	#12345,d1
	moveq	#0,d5			; sum
	moveq	#0,d6			; xor
	move.w	#N-1,d2
.fill:	move.w	d1,(a0)+
	moveq	#0,d3
	move.w	d1,d3
	lsl.l	#8,d3
	add.l	d3,d5
	eor.l	d3,d6
	add.l	#40503,d1
	dbra	d2,.fill
	andi.l	#$FFFFFF,d5
	move.l	d5,want_sum
	move.l	d6,want_xor
	clr.l	mode
	clr.l	loss0
.mode:	moveq	#0,d4			; this mode's errors
	bsr	route
	lea	msg_mode,a0
	bsr	print
	move.l	mode,d1
	bsr	hex
	bsr	nl

	; pass 1: DSP idle, every word compared
	bsr	arm
	bsr	play
	bsr	wait_play
	bsr	count
	lea	msg_p1,a0
	bsr	report
	moveq	#5,d0			; READ N
	bsr	hput
	move.l	#N,d0
	bsr	hput
	lea	buf,a1
	moveq	#0,d6			; mismatches
	moveq	#-1,d7			; first
	move.w	#N-1,d2
	moveq	#0,d3			; index
.rd:	bsr	hget
	bmi	.dead
	lsr.l	#8,d0
	cmp.w	(a1)+,d0
	beq.s	.ok
	addq.l	#1,d6
	tst.l	d7
	bpl.s	.ok
	move.l	d3,d7
.ok:	addq.l	#1,d3
	dbra	d2,.rd
	lea	msg_mis,a0
	bsr	print
	move.l	d6,d1
	bsr	hex
	lea	msg_first,a0
	bsr	print
	move.l	d7,d1
	bsr	hex
	bsr	nl
	add.l	d6,d4
	tst.l	d7			; the words around the first mismatch
	bmi.s	.nodump
	bsr	dump
.nodump:
	cmpi.l	#N,got
	beq.s	.p2
	addq.l	#1,d4

	; pass 2: the DSP busy (about 3 x the transfer) while the ring fills
.p2:	bsr	arm
	bsr	play
	moveq	#7,d0			; BUSY
	bsr	hput
	move.l	#20000,d0		; x 16 cycles: ~20 ms at 16 MIPS
	bsr	hput
	bsr	hget			; the position after the busy loop
	bmi	.dead
	move.l	d0,busy_pos
	bsr	wait_play
	bsr	count
	lea	msg_p2,a0
	bsr	report
	lea	msg_busy,a0
	bsr	print
	move.l	busy_pos,d1
	bsr	hex
	bsr	nl
	moveq	#6,d0			; SUM N
	bsr	hput
	move.l	#N,d0
	bsr	hput
	bsr	hget
	bmi	.dead
	move.l	d0,d6
	bsr	hget
	bmi	.dead
	move.l	d0,d7
	lea	msg_sum,a0
	bsr	print
	move.l	d6,d1
	bsr	hex
	lea	msg_xor,a0
	bsr	print
	move.l	d7,d1
	bsr	hex
	lea	msg_want,a0
	bsr	print
	move.l	want_sum,d1
	bsr	hex
	lea	msg_sp,a0
	bsr	print
	move.l	want_xor,d1
	bsr	hex
	bsr	nl
	cmp.l	want_sum,d6
	beq.s	.s1
	addq.l	#1,d4
.s1:	cmp.l	want_xor,d7
	beq.s	.s2
	addq.l	#1,d4
.s2:	cmpi.l	#N,got
	beq.s	.s3
	addq.l	#1,d4
.s3:	moveq	#4,d0			; STOP
	bsr	hput
	bsr	hget
	tst.l	mode
	bne.s	.m1
	move.l	d4,loss0		; free-running: reported, not failed
	addq.l	#1,mode
	bra	.mode
.m1:	lea	msg_loss0,a0
	bsr	print
	move.l	loss0,d1
	bsr	hex
	bsr	nl
	lea	msg_result,a0		; the handshake mode decides
	bsr	print
	move.l	d4,d1
	bsr	hex
	bsr	nl
	move.l	d4,d0
	rts
.dead:	lea	msg_dead,a0
	bsr	print
	moveq	#3,d0
	rts

; dump: ring words d7 - 4 .. d7 + 11 against the pattern ("i got want")
dump:	movem.l	d0-d7/a0-a1,-(sp)
	move.l	d7,d5
	subq.l	#4,d5
	bpl.s	.d0
	moveq	#0,d5
.d0:	moveq	#5,d0			; READ first + 16 (from the ring start)
	bsr	hput
	move.l	d5,d0
	add.l	#16,d0
	bsr	hput
	moveq	#0,d3
.skip:	cmp.l	d5,d3
	bcc.s	.show
	bsr	hget
	addq.l	#1,d3
	bra.s	.skip
.show:	moveq	#15,d2
.sh:	bsr	hget
	lsr.l	#8,d0
	move.l	d0,d6
	lea	msg_dump,a0
	bsr	print
	move.l	d3,d1
	bsr	hex
	lea	msg_sp,a0
	bsr	print
	move.l	d6,d1
	bsr	hex
	lea	msg_sp,a0
	bsr	print
	move.l	d3,d1			; the pattern's word d3
	mulu.l	#40503,d1
	add.l	#12345,d1
	andi.l	#$FFFF,d1
	bsr	hex
	bsr	nl
	addq.l	#1,d3
	dbra	d2,.sh
	movem.l	(sp)+,d0-d7/a0-a1
	rts

; ARM the DSP's receiver in the current mode
arm:	moveq	#2,d0
	bsr	hput
	move.l	mode,d0
	bsr	hput
	bsr	hget
	rts

; one play window over buf (single shot); t0 for the report
play:	move.l	$4ba.w,t0
	pea	buf+N*2			; end
	pea	buf			; start
	clr.w	-(sp)			; play buffer
	xb	131			; Setbuffer
	lea	12(sp),sp
	move.w	#1,-(sp)		; PLAY_ENABLE
	xb	136			; Buffoper
	addq.l	#4,sp
	rts

; until the window has played, then two 200 Hz ticks for the last frames
wait_play:
	move.l	#20000000,d1
.w:	btst	#0,$ffff8901.w		; play enable clears at the end
	beq.s	.end
	subq.l	#1,d1
	bne.s	.w
.end:	move.l	$4ba.w,d0
	sub.l	t0,d0
	move.l	d0,ticks
	move.l	$4ba.w,d1
	addq.l	#2,d1
.t:	cmp.l	$4ba.w,d1
	bhi.s	.t
	rts

; COUNT -> got, roe
count:	moveq	#3,d0
	bsr	hput
	bsr	hget
	move.l	d0,got
	bsr	hget
	move.l	d0,roe
	rts

; "<a0> words N received got roe roe ticks ticks"
report:	bsr	print
	lea	msg_words,a0
	bsr	print
	move.l	#N,d1
	bsr	hex
	lea	msg_recv,a0
	bsr	print
	move.l	got,d1
	bsr	hex
	lea	msg_roe,a0
	bsr	print
	move.l	roe,d1
	bsr	hex
	lea	msg_ticks,a0
	bsr	print
	move.l	ticks,d1
	bsr	hex
	bsr	nl
	rts

; host port: hput d0; hget -> d0 (N flag set on a timeout)
hput:	move.l	#2000000,d1
.w:	btst	#1,$ffffa202.w		; TXDE
	bne.s	.go
	subq.l	#1,d1
	bne.s	.w
	moveq	#-1,d0
	rts
.go:	move.l	d0,$ffffa204.w
	moveq	#0,d1
	rts
hget:	move.l	#2000000,d1
.w:	btst	#0,$ffffa202.w		; RXDF
	bne.s	.go
	subq.l	#1,d1
	bne.s	.w
	moveq	#-1,d0
	rts
.go:	move.l	$ffffa204.w,d0
	andi.l	#$FFFFFF,d0
	rts

; ---------------------------------------------------------------------------
print:	movem.l	d0-d2/a0-a1,-(sp)
	move.l	a0,-(sp)
	move.l	nf_stderr,-(sp)
	bsr.s	nf_call
	addq.l	#8,sp
	movem.l	(sp)+,d0-d2/a0-a1
	rts
nl:	lea	msg_nl,a0
	bra.s	print
hex:	movem.l	d0-d2/a0,-(sp)		; d1
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
	bsr.s	print
	movem.l	(sp)+,d0-d2/a0
	rts
nf_id:	dc.w	$7300
	rts
nf_call:
	dc.w	$7301
	rts

	section	data,data
nf_name:	dc.b	"NF_STDERR",0
msg_noroute:	dc.b	"SSITEST: sound system busy, no DMA -> DSP route",10,0
msg_ping:	dc.b	"SSITEST ping ",0
msg_p1:		dc.b	"SSITEST pass 1 (DSP idle)",0
msg_p2:		dc.b	"SSITEST pass 2 (DSP busy)",0
msg_words:	dc.b	" words ",0
msg_recv:	dc.b	" received ",0
msg_roe:	dc.b	" overrun ",0
msg_ticks:	dc.b	" ticks ",0
msg_mis:	dc.b	"SSITEST pass 1 mismatches ",0
msg_first:	dc.b	" first ",0
msg_busy:	dc.b	"SSITEST pass 2 ring position when the busy loop ended ",0
msg_sum:	dc.b	"SSITEST pass 2 sum ",0
msg_xor:	dc.b	" xor ",0
msg_want:	dc.b	" expected ",0
msg_sp:		dc.b	" ",0
msg_result:	dc.b	"SSITEST errors (handshake) ",0
msg_loss0:	dc.b	"SSITEST no-handshake errors (information: Hatari delivers late crossbar words back to back) ",0
msg_dead:	dc.b	"SSITEST: the DSP does not answer",10,0
msg_nl:		dc.b	10,0
msg_mode:	dc.b	"SSITEST mode (0 no handshake, 1 handshake) ",0
msg_dump:	dc.b	"SSITEST dump ",0
	even
kernel:
	incbin	"ssirx.bin"
KERNEL_WORDS	equ	(*-kernel)/3
	even

	section	bss,bss
nf_stderr:	ds.l	1
old_ssp:	ds.l	1
t0:		ds.l	1
ticks:		ds.l	1
got:		ds.l	1
roe:		ds.l	1
busy_pos:	ds.l	1
mode:		ds.l	1
loss0:		ds.l	1
want_sum:	ds.l	1
want_xor:	ds.l	1
result:		ds.w	1
hexbuf:		ds.b	12
		even
buf:		ds.w	N		; the words played (ST-RAM: DMA reads it)
		ds.b	8192
stack_top:
