; DSP renderer, hardware side (included by smkx_rt.s; the driver is
; src/smkx_m7.c). The program (falcon/dsp/ppu.asm) boots with
; Dsp_ExecBoot; its SSI transmit goes to DMA record in handshake mode, four
; tracks, 25.175 MHz, as M7TEST checks it; DMA playback goes to its SSI
; receive, also in handshake mode (the DSP asks for every word): the
; commands of a frame go by DMA once the DSP takes them from the SSI
; (SSION; PPUTEST's SSI pass checks it). Record and play windows are armed
; on the crossbar registers directly: rendering runs inside the VBL
; handler.

	ifnd	SMKX_M7_DSP
SMKX_M7_DSP	equ	1		; 0: the CPU composes Mode 7 as well
	endif

	section	text,code

xbios	macro
	move.w	#\1,-(sp)
	trap	#14
	endm

; rt_m7_boot: user mode, before Super. Sets rt_m7_route when the kernel is
; booted and the sound system is ours.
rt_m7_boot:
	sf	rt_m7_route
	ifne	SMKX_M7_DSP
	move.w	#1,-(sp)		; ability
	move.l	#M7_KERNEL_WORDS,-(sp)
	pea	m7_kernel
	xbios	110			; Dsp_ExecBoot
	lea	12(sp),sp
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

; rt_m7_stop: supervisor; ends any record window and the stream feed.
rt_m7_stop:
	tst.b	rt_m7_route
	beq.s	.x
	move.b	#$80,$ffff8901.w
	clr.b	$ffff8901.w
	bclr	#1,$ffffa200.w		; no host-port interrupts
	tst.l	feed_oldvec
	beq.s	.x
	move.l	feed_oldvec,FEED_VEC*4.w
	clr.l	feed_oldvec
.x:	rts

; The frame stream goes to the DSP from this interrupt: the host port
; asks (HREQ, level 6, vector FEED_VEC from its IVR) whenever it can take
; a word; the handler writes words while it can and stops asking at the
; end. The 68030 meanwhile runs the game. (src/smkx_m7.c)
FEED_VEC	equ	$c0		; vector 192, $300: unused by TOS and the MFP

; void rt_feed_install(void): supervisor, once
	xdef	rt_feed_install,rt_feed_start,rt_feed_more,rt_feed_done
rt_feed_install:
	move.l	FEED_VEC*4.w,feed_oldvec
	move.l	#rt_feed_isr,FEED_VEC*4.w
	move.b	#FEED_VEC,$ffffa203.w	; the host port's interrupt vector
	rts

; void rt_feed_start(const uint32_t *words, uint32_t n)
rt_feed_start:
	move.l	4(sp),d0
	move.l	d0,feed_ptr
	move.l	8(sp),d1
	lsl.l	#2,d1
	add.l	d1,d0
	move.l	d0,feed_end
	bset	#1,$ffffa200.w		; TREQ: interrupt while it can take one
	rts

; void rt_feed_more(const uint32_t *end): the words up to end are ready
; too (the stream is sent while it is built). The end goes first, then the
; request: in any order against the interrupt, at worst it runs once, finds
; nothing and stops asking again.
rt_feed_more:
	move.l	4(sp),feed_end
	bset	#1,$ffffa200.w		; TREQ
	rts

; int rt_feed_done(void): 1 once every word went out
rt_feed_done:
	moveq	#0,d0
	move.l	feed_ptr,d1
	cmp.l	feed_end,d1
	bcs.s	.x
	moveq	#1,d0
.x:	rts

rt_feed_isr:
	move.l	a0,-(sp)
	movea.l	feed_ptr,a0
.lp:	cmpa.l	feed_end,a0
	bcc.s	.end
	btst	#1,$ffffa202.w		; TXDE
	beq.s	.out
	move.l	(a0)+,$ffffa204.w	; ($A204 unused: TXH, TXM, TXL)
	bra.s	.lp
.end:	bclr	#1,$ffffa200.w		; done: stop asking
.out:	move.l	a0,feed_ptr
	movea.l	(sp)+,a0
	rte

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
; window into the DSP's SSI receive. The play registers are selected with
; the record bits kept (a record window is running), then play goes on.
	xdef	rt_ssi_play,rt_ssi_play_done
rt_ssi_play:
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

	section	data,data
m7_kernel:				; the DSP renderer's boot image
	incbin	"ppu.bin"
M7_KERNEL_WORDS	equ	(*-m7_kernel)/3
	even
	ifd	SMKX_LC
; The same image padded to the 512 words the DSP's bootstrap takes, for rt_dsp_boot (smkx_dspboot.s: SMK's program
; booted again after the list compositor's; Dsp_ExecBoot pads it itself at the start), and the compositor's
; programs in boot format (falcon/dsp/ppu_tile.asm: base, window and LCB builds).
m7_kernel_full:
	incbin	"ppu.bin"
	dcb.b	512*3-(*-m7_kernel_full),0
m7_kernel_full_end:
m7_kernel_full_bytes:	dc.l	m7_kernel_full_end-m7_kernel_full
ppu_image:
	incbin	"ppu_tile.bin"
ppu_image_end:
	even
ppu_image_bytes:	dc.l	ppu_image_end-ppu_image
ppuw_image:
	incbin	"ppu_tilew.bin"
ppuw_image_end:
	even
ppuw_image_bytes:	dc.l	ppuw_image_end-ppuw_image
ppulc_image:
	incbin	"ppu_tilelc.bin"
ppulc_image_end:
	even
ppulc_image_bytes:	dc.l	ppulc_image_end-ppulc_image
ppulcw_image:				; ... with the window build (LCB, WIN)
	incbin	"ppu_tilelcw.bin"
ppulcw_image_end:
	even
ppulcw_image_bytes:	dc.l	ppulcw_image_end-ppulcw_image
	endif
; the program beyond the boot image: blocks of 24-bit words (space,
; address, count, words), sent with LOAD by smkx_m7_init
	xdef	ppu_ext,ppu_ext_end
ppu_ext:
	incbin	"ppu.ext"
ppu_ext_end:
	even

	section	bss,bss
	ifd	SMKX_LC
rt_m7_dirty:	ds.b	128		; per 256-word VRAM page, written since the list compositor's DSP program took it
rt_plan_dirty:	ds.b	128		; the same, written since its planner looked
	else
rt_m7_dirty:	ds.b	64		; smkx_m7.h: M7_PAGES
	endif
rt_m7_route:	ds.b	1
	xdef	rt_pitch72
rt_pitch72:	ds.b	1		; screen lines are 72 words (the DSP's samples)
	even
feed_ptr:	ds.l	1
feed_end:	ds.l	1
feed_oldvec:	ds.l	1
	even
