; Booting a DSP program without XBIOS (included by smkx_rt.s and
; pputest.s): supervisor mode, safe inside the VBL handler.
;
; The reset line is bit 4 of the PSG's port A (Hatari: psg.c resets the DSP
; when it is written as 1); after the reset the DSP's bootstrap takes 512
; words over the host port into P:$0000-$01FF and starts there. An image
; from tools/smkx/dsp_asm.py --boot is those 512 words, then blocks for the
; program's own stage-2 loader, all of it sent in order: the host port's
; transmit-empty flag paces both.

	section	text,code

; void rt_dsp_boot(const uint8_t *image, uint32_t bytes)
rt_dsp_boot:
	move.l	#2000000,d0		; a DMA play window into the DSP's SSI
.pw:	btst	#0,$ffff8901.w		; (rt_ssi_play) ends first: the DSP may
	beq.s	.pd			; still be reading its padding
	subq.l	#1,d0
	bne.s	.pw
	bclr	#0,$ffff8901.w
.pd:	move.l	4(sp),a0
	move.l	8(sp),d1
	move.l	d2,-(sp)
	move.w	sr,-(sp)
	ori.w	#$0700,sr
	move.b	#14,$ffff8800.w		; port A
	move.b	$ffff8800.w,d0
	bset	#4,d0
	move.b	d0,$ffff8802.w		; reset
	moveq	#63,d2
.hold:	dbra	d2,.hold
	bclr	#4,d0
	move.b	d0,$ffff8802.w
	move.w	(sp)+,sr
	divu.l	#3,d1
	subq.l	#1,d1
	bmi.s	.x
.w:	btst	#1,$ffffa202.w		; TXDE
	beq.s	.w
	move.b	(a0)+,$ffffa205.w
	move.b	(a0)+,$ffffa206.w
	move.b	(a0)+,$ffffa207.w
	subq.l	#1,d1
	bpl.s	.w
.x:	move.l	(sp)+,d2
	rts
