; Videl output for the translated program (included by smkx_rt.s).
; 256x224 in four or eight interleaved bitplanes (rt_vid_planes): four for
; the Mode 7 path with the adaptive 16-colour palette (src/palette.c),
; eight for the DSP renderer, whose pixels are CGRAM indices; smkx_render
; loads the palette. With SMKX_TC (SCIVX_TC=1) 256x224 in true colour
; (RGB565) instead, always: the true-colour renderer (src/smkx_tc.c) and
; the CPU's fallback both write it; the timing is Snow Bros' (F030Arcade,
; tested on a real Falcon: analysis/ct.md). RGB/TV and VGA timings as in the bring-up viewer
; (falcon/viewer.s). A monochrome monitor leaves the desktop untouched and
; rendering off.

	ifne	SMKX_TC
SCREEN_BYTES	equ	114688		; 256 x 224, RGB565
	else
SCREEN_BYTES	equ	57344		; 256 x 224, eight planes
	endif

	section	text,code

rt_video_on:
	movem.l	d0-d7/a0-a2,-(sp)
	move.w	#89,-(sp)		; XBIOS Montype
	trap	#14
	addq.l	#2,sp
	tst.w	d0
	beq	.mono
	move.w	d0,vid_monitor
	move.w	#2,-(sp)		; XBIOS Physbase
	trap	#14
	addq.l	#2,sp
	move.l	d0,vid_old_phys
	bsr	vid_save
	lea	rt_screen_raw,a0
	move.l	a0,d0
	add.l	#255,d0
	andi.l	#$ffffff00,d0
	move.l	d0,rt_screen
	add.l	#SCREEN_BYTES,d0	; a multiple of 256: still aligned
	move.l	d0,rt_back
	movea.l	rt_screen,a0
	move.w	#2*SCREEN_BYTES/4-1,d1
.clr:	clr.l	(a0)+
	dbra	d1,.clr
	move.l	rt_screen,d0
	bsr	vid_set_base
	ifne	SMKX_TC
	cmpi.w	#2,vid_monitor
	beq.s	.tvga
	move.l	#$00c70076,$ffff8282.w	; RGB/TV, 50 Hz, true colour: HHT, HBB
	move.l	#$003f001e,$ffff8286.w	; HBE, HDB
	move.l	#$007600ab,$ffff828a.w	; HDE, HSS
	move.l	#$0271021f,$ffff82a2.w	; VFT, VBB
	move.l	#$005f005f,$ffff82a6.w	; VBE, VDB
	move.l	#$021f026b,$ffff82aa.w	; VDE, VSS
	move.w	#$0200,$ffff820a.w
	move.w	#$0185,$ffff82c0.w	; 25.175 MHz
	clr.w	$ffff8266.w		; (Snow Bros' order: the shift mode, then
	move.w	#$0100,$ffff8266.w	; VCO)
	clr.w	$ffff82c2.w		; 4 cycles a pixel
	bra.s	.tcommon
.tvga:	move.l	#$00fc00a9,$ffff8282.w	; VGA, doubled lines
	move.l	#$002502f3,$ffff8286.w
	move.l	#$00a800c0,$ffff828a.w
	move.l	#$041903cf,$ffff82a2.w
	move.l	#$004d004b,$ffff82a6.w
	move.l	#$03cb0415,$ffff82aa.w
	move.w	#$0200,$ffff820a.w
	move.w	#$0182,$ffff82c0.w
	clr.w	$ffff8266.w
	move.w	#$0100,$ffff8266.w
	move.w	#5,$ffff82c2.w		; doubled lines
.tcommon:
	move.w	#16,vid_planes
	clr.w	$ffff8264.w		; (a word: $8266 holds true colour)
	clr.w	$ffff820e.w
	move.w	#256,$ffff8210.w	; 512 bytes per line
	clr.l	$ffff9800.w		; the border: palette entry 0, black
	st	vid_active
	bra	.mono
	endif
	cmpi.w	#2,vid_monitor
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
	clr.w	$ffff8266.w		; four-plane, 16 colours
	move.w	#4,vid_planes
	clr.l	$ffff8264.w
	clr.w	$ffff820e.w
	move.w	#64,$ffff8210.w		; 128 bytes per line
	; black until the first composed frame loads the adaptive palette
	lea	$ffff9800.w,a1
	moveq	#15,d1
.pal:	clr.l	(a1)+
	dbra	d1,.pal
	st	vid_active
.mono:	movem.l	(sp)+,d0-d7/a0-a2
	rts

; void rt_vid_planes(int planes): 4 or 8. The Videl takes the new mode at
; once; the frame on screen is in the other one until the next flip.
rt_vid_planes:
	ifne	SMKX_TC
	rts				; true colour throughout
	endif
	tst.b	vid_active
	beq.s	.x
	move.l	4(sp),d0
	cmp.w	vid_planes,d0
	beq.s	.x
	move.w	d0,vid_planes
	cmpi.w	#8,d0
	beq.s	.eight
	clr.w	$ffff8266.w		; four planes
	move.w	#64,$ffff8210.w
	rts
.eight:	move.w	#$0010,$ffff8266.w	; eight planes, 256 colours
	move.w	#128,$ffff8210.w
.x:	rts

rt_video_off:
	tst.b	vid_active
	beq.s	.x
	movem.l	d0-d3/a0-a1,-(sp)
	lea	vid_save_palette,a0
	lea	$ffff9800.w,a1
	moveq	#15,d1
.pal:	move.l	(a0)+,(a1)+
	dbra	d1,.pal
	movem.l	vid_save_h,d0-d3
	movem.l	d0-d3,$ffff8282.w
	movem.l	vid_save_v,d0-d3
	movem.l	d0-d3,$ffff82a2.w
	move.l	vid_save_control,$ffff82c0.w
	move.l	vid_save_hscroll,$ffff8264.w
	move.l	vid_save_offset,$ffff820e.w
	clr.w	$ffff8266.w
	move.w	vid_save_spshift,$ffff8266.w
	move.b	vid_save_st_shift,$ffff8260.w
	move.w	vid_save_sync,$ffff820a.w
	move.l	vid_old_phys,d0
	bsr.s	vid_set_base
	sf	vid_active
	movem.l	(sp)+,d0-d3/a0-a1
.x:	rts

vid_save:
	move.b	$ffff8260.w,vid_save_st_shift
	move.w	$ffff8266.w,vid_save_spshift
	move.w	$ffff820a.w,vid_save_sync
	move.l	$ffff820e.w,vid_save_offset
	move.l	$ffff8264.w,vid_save_hscroll
	movem.l	$ffff8282.w,d0-d3
	movem.l	d0-d3,vid_save_h
	movem.l	$ffff82a2.w,d0-d3
	movem.l	d0-d3,vid_save_v
	move.l	$ffff82c0.w,vid_save_control
	lea	$ffff9800.w,a0
	lea	vid_save_palette,a1
	moveq	#15,d1
.pal:	move.l	(a0)+,(a1)+
	dbra	d1,.pal
	rts

vid_set_base:
	move.l	d1,-(sp)
	move.l	d0,d1			; the high and mid bytes first: writing them clears the low byte (Hatari's Videl)
	lsr.l	#8,d1
	lsr.l	#8,d1
	move.b	d1,$ffff8201.w
	move.l	d0,d1
	lsr.l	#8,d1
	move.b	d1,$ffff8203.w
	move.b	d0,$ffff820d.w
	move.l	(sp)+,d1
	rts

	ifne	SMKX_LC
; void smkx_vid_set_window(uint32_t addr, int hscroll, int lineoff): the list compositor's ring window (C ABI):
; the address of its first group, HSCROLL, the words to skip at the end of a line. Eight planes only.
smkx_vid_set_window:
	move.l	4(sp),d0
	bsr.s	vid_set_base
	move.b	11(sp),$ffff8265.w
	move.w	14(sp),$ffff820e.w
	rts
	endif

	section	bss,bss
rt_screen:		ds.l	1	; shown
rt_back:		ds.l	1	; composed next
vid_old_phys:		ds.l	1
vid_monitor:		ds.w	1
vid_planes:		ds.w	1
vid_save_spshift:	ds.w	1
vid_save_sync:		ds.w	1
vid_save_offset:	ds.l	1
vid_save_hscroll:	ds.l	1
vid_save_h:		ds.l	4
vid_save_v:		ds.l	4
vid_save_control:	ds.l	1
vid_save_palette:	ds.l	16
vid_active:		ds.b	1
vid_save_st_shift:	ds.b	1
	even
rt_screen_copy:		ds.b	SCREEN_BYTES	; the last composed frame, for SNAP.BIN
rt_screen_raw:		ds.b	2*SCREEN_BYTES+256
