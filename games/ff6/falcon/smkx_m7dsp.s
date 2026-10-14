; The DSP's hardware side (included by smkx_rt.s): the sound system claimed
; for the route DSP transmit -> DMA record (handshake mode, four tracks,
; 25.175 MHz, as M7TEST and PPUTEST check it) and DMA playback -> DSP
; receive (handshake mode too: the DSP asks for every word; the tile
; renderer takes its commands that way after SSION, src/smkx_ppu.c), the
; two DSP programs, record and play windows. The programs are booted by the runtime itself
; (smkx_dspboot.s), in the VBL handler when a scene needs the other one:
; the Mode 7 kernel (falcon/dsp/m7c2p.asm, driver src/smkx_m7.c) and the
; tile renderer (falcon/dsp/ppu.asm, driver src/smkx_ppu.c). Record
; windows are armed on the crossbar registers directly.

	ifnd	SMKX_M7_DSP
SMKX_M7_DSP	equ	1		; 0: the CPU composes Mode 7 as well
	endif

	section	text,code

xbios	macro
	move.w	#\1,-(sp)
	trap	#14
	endm

; rt_m7_boot: user mode, before Super. Sets rt_m7_route when the sound
; system is ours (the DSP programs are booted later).
rt_m7_boot:
	sf	rt_m7_route
	ifne	SMKX_M7_DSP
	xbios	128			; Locksnd
	addq.l	#2,sp
	cmpi.w	#1,d0
	bne.s	.x
	clr.w	-(sp)
	xbios	136			; Buffoper(0)
	addq.l	#4,sp
	move.w	#1,-(sp)
	xbios	132			; Setmode(STEREO16)
	addq.l	#4,sp
	move.w	#3,-(sp)
	move.w	#3,-(sp)
	xbios	133			; Settracks(4 play, 4 record)
	addq.l	#6,sp
	clr.w	-(sp)
	xbios	134			; Setmontracks(0)
	addq.l	#4,sp
	move.w	#1,-(sp)
	move.w	#1,-(sp)
	xbios	137			; Dsptristate(xmit on, recv on)
	addq.l	#6,sp
	clr.w	-(sp)			; handshake
	move.w	#1,-(sp)		; prescale 1
	clr.w	-(sp)			; 25.175 MHz
	move.w	#1,-(sp)		; to DMA record
	move.w	#1,-(sp)		; from DSP transmit
	xbios	139			; Devconnect
	lea	12(sp),sp
	clr.w	-(sp)			; handshake: the DSP asks for every word
	move.w	#1,-(sp)		; prescale 1
	clr.w	-(sp)			; 25.175 MHz
	move.w	#2,-(sp)		; to DSP receive
	clr.w	-(sp)			; from DMA playback
	xbios	139			; Devconnect
	lea	12(sp),sp
	st	rt_m7_route
	endif
.x:	rts

; rt_m7_stop: supervisor; ends any record window.
rt_m7_stop:
	tst.b	rt_m7_route
	beq.s	.x
	move.b	#$80,$ffff8901.w
	clr.b	$ffff8901.w
.x:	rts

; rt_m7_release: user mode, at exit.
rt_m7_release:
	tst.b	rt_m7_route
	beq.s	.x
	clr.w	-(sp)
	xbios	136			; Buffoper(0)
	addq.l	#4,sp
	clr.w	-(sp)
	clr.w	-(sp)
	xbios	137			; Dsptristate(0, 0)
	addq.l	#6,sp
	clr.w	-(sp)
	clr.w	-(sp)
	xbios	133			; Settracks(0, 0)
	addq.l	#6,sp
	xbios	129			; Unlocksnd
	addq.l	#2,sp
	sf	rt_m7_route
.x:	rts

; void rt_m7_record(void *start, uint32_t bytes): one-shot record window.
; Play and record share $FF8901: a play window's bits are kept.
rt_m7_record:
	move.l	4(sp),d0
	move.l	8(sp),d1
	add.l	d0,d1
	move.l	d2,-(sp)		; (C keeps d2)
	move.b	$ffff8901.w,d2
	andi.b	#$03,d2			; a play window runs on
	ori.b	#$80,d2
	move.b	d2,$ffff8901.w		; record registers, record off
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
	ori.b	#$10,d2
	move.b	d2,$ffff8901.w		; record on, single window
	move.l	(sp)+,d2
	rts

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

; int rt_m7_record_done(void): 1 once the window is full. The data cache
; is cleared then: DMA does not update it.
rt_m7_record_done:
	moveq	#0,d0
	btst	#4,$ffff8901.w
	bne.s	.x
	movec	cacr,d1
	ori.w	#$0800,d1		; CD: clear the data cache
	movec	d1,cacr
	moveq	#1,d0
.x:	rts

	include	"smkx_dspboot.s"

	section	data,data
m7_image:
	incbin	"m7c2p.bin"
m7_image_end:
	even
m7_image_bytes:	dc.l	m7_image_end-m7_image
ppu_image:
	incbin	"ppu.bin"
ppu_image_end:
	even
ppu_image_bytes:	dc.l	ppu_image_end-ppu_image
ppu7_image:				; the same source for Mode 7 frames (M7K)
	incbin	"ppu7.bin"
ppu7_image_end:
	even
ppu7_image_bytes:	dc.l	ppu7_image_end-ppu7_image
ppuw_image:				; and for frames with windows (WIN)
	incbin	"ppuw.bin"
ppuw_image_end:
	even
ppuw_image_bytes:	dc.l	ppuw_image_end-ppuw_image
	ifne	SMKX_LC
ppulc_image:				; the list compositor's group composer (LCB)
	incbin	"ppulc.bin"
ppulc_image_end:
	even
ppulc_image_bytes:	dc.l	ppulc_image_end-ppulc_image
ppulcw_image:				; ... with the window build (LCB, WIN)
	incbin	"ppulcw.bin"
ppulcw_image_end:
	even
ppulcw_image_bytes:	dc.l	ppulcw_image_end-ppulcw_image
	endif
	ifne	SMKX_TC
tc_image:				; the true-colour program (FF6X_TC, src/smkx_tc.c)
	incbin	"tc.bin"
tc_image_end:
	even
tc_image_bytes:	dc.l	tc_image_end-tc_image
tc7_image:				; and its Mode 7 build (TC7)
	incbin	"tc7.bin"
tc7_image_end:
	even
tc7_image_bytes:	dc.l	tc7_image_end-tc7_image
	endif

	section	bss,bss
rt_m7_dirty:	ds.b	128		; per 256-word VRAM page, written since uploaded
rt_plan_dirty:	ds.b	128		; the same, written since the DSP planner looked
rt_m7_route:	ds.b	1
	even
