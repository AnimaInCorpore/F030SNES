; void rt_ssi_play(const uint16_t *start, uint32_t bytes): one-shot play
; window into the DSP's SSI receive (bytes: whole frames of four stereo
; 16-bit tracks, 16 bytes). The play registers are selected with the
; record bits kept (a record window may be armed), then play goes on.
	xdef	rt_ssi_play,rt_ssi_play_done
rt_ssi_play:
	move.l	4(sp),d0
	move.l	8(sp),d1
	add.l	d0,d1
	move.l	d2,-(sp)
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

; int rt_ssi_play_done(void): 1 once the play window has gone out
rt_ssi_play_done:
	moveq	#0,d0
	btst	#0,$ffff8901.w
	bne.s	.x
	moveq	#1,d0
.x:	rts
