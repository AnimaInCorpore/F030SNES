; PPUTEST.TOS: glue for src/pputest.c. Boots the DSP renderer
; (falcon/dsp/ppu.asm) with the runtime's own loader (smkx_dspboot.s: a
; reset through the PSG and the bootstrap, no XBIOS), routes DSP
; transmit -> DMA record in handshake mode (four tracks, 25.175 MHz, as for
; M7TEST), runs the test in supervisor mode and prints over NatFeats. The
; frames come from build/dsp_snap -pack (ppu_tests.bin).

	xref	pputest_main
	xdef	m7t_print,m7t_hex,m7t_record,m7t_record_done,ppu_tests
	xdef	pt_boot

	section	text,code
start:
	lea	stack_top,sp
	pea	nf_name
	bsr	nf_id
	addq.l	#4,sp
	move.l	d0,nf_stderr
	bsr	sound_claim
	tst.w	d0
	bne.s	.noroute
	clr.l	-(sp)
	move.w	#$20,-(sp)		; Super(0)
	trap	#1
	addq.l	#6,sp
	move.l	d0,old_ssp
	jsr	pputest_main
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

; void pt_boot(int kernel): the renderer, booted (supervisor): ppu.bin
; (DSP_KERNEL_TILE), its Mode 7 build ppu7.bin (1) or its window build
; ppuw.bin (2).
pt_boot:
	move.l	4(sp),d0
	cmpi.l	#1,d0
	beq.s	.m7
	cmpi.l	#2,d0
	beq.s	.w
	move.l	#KERNEL_END-kernel,-(sp)
	pea	kernel
	bra.s	.go
.m7:	move.l	#KERNEL7_END-kernel7,-(sp)
	pea	kernel7
	bra.s	.go
.w:	move.l	#KERNELW_END-kernelw,-(sp)
	pea	kernelw
.go:	bsr	rt_dsp_boot
	addq.l	#8,sp
	rts

	include	"smkx_dspboot.s"
	section	text,code

; XBIOS sound calls ($80 Locksnd ... $8B Devconnect)
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
	clr.w	-(sp)
	move.w	#1,-(sp)
	xb	137			; Dsptristate(xmit on, recv off)
	addq.l	#6,sp
	clr.w	-(sp)			; handshake
	move.w	#1,-(sp)		; prescale 1
	clr.w	-(sp)			; 25.175 MHz
	move.w	#1,-(sp)		; to DMA record
	move.w	#1,-(sp)		; from DSP transmit
	xb	139			; Devconnect
	lea	12(sp),sp
	moveq	#0,d0
	rts
.fail:	moveq	#1,d0
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

; int m7t_record(uint16_t *buf, uint32_t bytes): one-shot DMA record window.
; Called in supervisor mode; XBIOS is reentrant enough for these calls.
m7t_record:
	move.l	4(sp),d0
	move.l	8(sp),d1
	add.l	d0,d1
	move.l	d1,rec_end
	move.l	d1,-(sp)		; end
	move.l	d0,-(sp)		; start
	move.w	#1,-(sp)		; record buffer
	xb	131			; Setbuffer
	lea	12(sp),sp
	move.w	#4,-(sp)		; RECORD_ENABLE
	xb	136			; Buffoper
	addq.l	#4,sp
	moveq	#0,d0
	rts

; int m7t_record_done(void): record stopped at the end of its window.
m7t_record_done:
	moveq	#0,d0
	btst	#4,$ffff8901.w		; record enable clears at the end
	bne.s	.x
	moveq	#1,d0
.x:	rts

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
	move.l	d2,-(sp)		; callee-saved in the C ABI
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
msg_noroute:	dc.b	"PPUTEST: sound system busy, no DSP -> DMA route",10,0
	even
kernel:
	incbin	"ppu_tile.bin"
KERNEL_END:
	even
kernel7:
	incbin	"ppu_tile7.bin"
KERNEL7_END:
	even
kernelw:
	incbin	"ppu_tilew.bin"
KERNELW_END:
	even
ppu_tests:
	incbin	"tile_tests.bin"
	even

	section	bss,bss
nf_stderr:	ds.l	1
old_ssp:	ds.l	1
rec_end:	ds.l	1
result:		ds.w	1
hexbuf:		ds.b	12
		ds.b	8192
stack_top:
