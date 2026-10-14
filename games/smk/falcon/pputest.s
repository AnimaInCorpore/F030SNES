; PPUTEST.TOS: glue for src/pputest.c. Boots the DSP PPU (falcon/dsp/ppu.asm) with
; Dsp_ExecBoot (from user mode), routes DSP transmit -> DMA record in
; handshake mode (four tracks, 25.175 MHz, as the scummvm DSP c2p does under
; this Hatari), runs the test in supervisor mode and prints over NatFeats.

	xref	pputest_main
	xdef	pt_print,pt_hex,pt_record,pt_record_done,pt_play,pt_play_done

	section	text,code
start:
	lea	stack_top,sp
	pea	nf_name
	bsr	nf_id
	addq.l	#4,sp
	move.l	d0,nf_stderr
	; Dsp_ExecBoot(image, words, ability) -- must be called from user mode
	move.w	#1,-(sp)
	move.l	#KERNEL_WORDS,-(sp)
	pea	kernel
	move.w	#110,-(sp)
	trap	#14
	lea	12(sp),sp
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
	move.w	#1,-(sp)
	move.w	#1,-(sp)
	xb	137			; Dsptristate(xmit on, recv on)
	addq.l	#6,sp
	clr.w	-(sp)			; handshake
	move.w	#1,-(sp)		; prescale 1
	clr.w	-(sp)			; 25.175 MHz
	move.w	#1,-(sp)		; to DMA record
	move.w	#1,-(sp)		; from DSP transmit
	xb	139			; Devconnect
	lea	12(sp),sp
	clr.w	-(sp)			; handshake: the DSP asks for every word
	move.w	#1,-(sp)		; prescale 1
	clr.w	-(sp)			; 25.175 MHz
	move.w	#2,-(sp)		; to DSP receive
	clr.w	-(sp)			; from DMA playback
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

; void pt_play(const uint16_t *buf, uint32_t bytes): one-shot DMA playback
; window into the DSP's SSI receive, on the registers (Buffoper would stop
; a record window already running): the play registers are selected with
; the record bits kept, then play is switched on.
pt_play:
	move.l	4(sp),d0
	move.l	8(sp),d1
	add.l	d0,d1
	move.l	d2,-(sp)		; (C keeps d2)
	move.b	$ffff8901.w,d2
	andi.b	#$30,d2			; record enable and repeat stay
	move.b	d2,$ffff8901.w		; bit 7 clear: the play registers
	swap	d0
	move.b	d0,$ffff8903.w
	swap	d0
	move.w	d0,-(sp)
	move.b	(sp)+,$ffff8905.w
	move.b	d0,$ffff8907.w
	swap	d1
	move.b	d1,$ffff890f.w
	swap	d1
	move.w	d1,-(sp)
	move.b	(sp)+,$ffff8911.w
	move.b	d1,$ffff8913.w
	ori.b	#1,d2
	move.b	d2,$ffff8901.w		; play on, single window
	move.l	(sp)+,d2
	rts

; int pt_play_done(void): 1 once the play window has gone out
pt_play_done:
	moveq	#0,d0
	btst	#0,$ffff8901.w
	bne.s	.x
	moveq	#1,d0
.x:	rts

; int pt_record(uint16_t *buf, uint32_t bytes): one-shot DMA record window.
; Called in supervisor mode; XBIOS is reentrant enough for these calls.
pt_record:
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

; int pt_record_done(void): record stopped at the end of its window.
pt_record_done:
	moveq	#0,d0
	btst	#4,$ffff8901.w		; record enable clears at the end
	bne.s	.x
	moveq	#1,d0
.x:	rts

; void pt_print(const char *s)
pt_print:
	move.l	4(sp),a0
print:	movem.l	d0-d1/a0-a1,-(sp)
	move.l	a0,-(sp)
	move.l	nf_stderr,-(sp)
	bsr.s	nf_call
	addq.l	#8,sp
	movem.l	(sp)+,d0-d1/a0-a1
	rts

; void pt_hex(uint32_t v)
pt_hex:					; keeps d2 (callee-saved in C)
	move.l	d2,-(sp)
	move.l	8(sp),d1
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
	incbin	"ppu.bin"
KERNEL_WORDS	equ	(*-kernel)/3
	even
	ifd	PT_SNAP
	xdef	pt_snap,pt_snap_end
pt_snap:
	incbin	"PTSNAP.BIN"		; a runtime snapshot: the bench frame
pt_snap_end:
	even
	endif
; the program beyond the boot image: blocks of 24-bit words (space,
; address, count, words), sent with LOAD (tools/smkx/dsp_asm.py)
	xdef	ppu_ext,ppu_ext_end
ppu_ext:
	incbin	"ppu.ext"
ppu_ext_end:
	even

	section	bss,bss
nf_stderr:	ds.l	1
old_ssp:	ds.l	1
rec_end:	ds.l	1
result:		ds.w	1
hexbuf:		ds.b	12
		ds.b	8192
stack_top:
