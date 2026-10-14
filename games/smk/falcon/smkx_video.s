; Videl output for the translated SMK program (included by smkx_rt.s).
; 256x224, four interleaved bitplanes; the 16 colours come from the
; adaptive palette (src/palette.c), loaded by smkx_render. RGB/TV and VGA timings as in the
; bring-up viewer (falcon/viewer.s). A monochrome monitor leaves the desktop
; untouched and rendering off.

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
	add.l	#32256,d0		; 224 lines of 72 words: a multiple of 256
	move.l	d0,rt_back
	movea.l	rt_screen,a0
	move.w	#2*32256/4-1,d1
.clr:	clr.l	(a0)+
	dbra	d1,.clr
	move.l	rt_screen,d0
	bsr	vid_set_base
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

rt_video_off:
	tst.b	vid_active
	beq	.x
	movem.l	d0-d3/a0-a1,-(sp)
	ifd	SMKX_LC
	sf	rt_want8		; eight planes (the list compositor): the four-plane line length and no HSCROLL
	move.w	#64,$ffff8210.w
	clr.b	$ffff8265.w
	clr.b	vid_eight
	endif
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

	ifd	SMKX_LC
; d0 = the screen's address. The high and mid bytes first: writing them clears the low byte (Hatari's Videl); the
; list compositor's ring windows are not 256-aligned.
vid_set_base:
	move.l	d0,d1
	lsr.l	#8,d1
	lsr.l	#8,d1
	move.b	d1,$ffff8201.w
	move.l	d0,d1
	lsr.l	#8,d1
	move.b	d1,$ffff8203.w
	move.b	d0,$ffff820d.w
	rts

; void smkx_vid_set_window(uint32_t addr, int hscroll, int lineoff): the list compositor's ring window (C ABI): the
; address of its first group, HSCROLL, the words to skip at the end of a line.
smkx_vid_set_window:
	st	rt_want8		; (the VBL switches the Videl to eight planes with the first ring window)
	move.l	4(sp),d0
	bsr.s	vid_set_base
	move.b	11(sp),$ffff8265.w
	move.w	14(sp),$ffff820e.w
	rts

; The Videl's mode follows rt_want8 (the VBL calls this): four planes with SMK's 16 colours, or eight planes
; (the list compositor: 256 colours = CGRAM). The Videl fetches an eight-plane line 16 units later than a four-plane
; one: HDB and HDE move with the mode (hdb/hde offsets 128/bpp in Hatari's videl.c, as on the hardware; F030SMW's
; smkx_video.s has the eight-plane values). Clobbers d0.
rt_vid_mode:
	move.b	rt_want8,d0
	cmp.b	vid_eight,d0
	beq	.x
	tst.b	vid_active
	beq	.x
	move.b	d0,vid_eight
	tst.b	d0
	bne.s	.eight
	clr.w	$ffff8266.w		; four planes
	move.w	#64,$ffff8210.w
	clr.b	$ffff8265.w		; no HSCROLL and no line offset for a plain screen
	clr.w	$ffff820e.w
	cmpi.w	#2,vid_monitor
	beq.s	.v4
	move.l	#$0047002c,$ffff8286.w	; RGB/TV: HBE, HDB
	move.l	#$004d00d8,$ffff828a.w	; HDE, HSS
	rts
.v4:	move.l	#$003502aa,$ffff8286.w
	move.l	#$004b0096,$ffff828a.w
	rts
.eight:	move.w	#$0010,$ffff8266.w	; eight planes, 256 colours
	move.w	#128,$ffff8210.w
	cmpi.w	#2,vid_monitor
	beq.s	.v8
	move.l	#$0047003c,$ffff8286.w	; HDB and HDE 16 units later
	move.l	#$005d00d8,$ffff828a.w
	rts
.v8:	move.l	#$003502ba,$ffff8286.w
	move.l	#$005b0096,$ffff828a.w
.x:	rts
	else
vid_set_base:
	move.b	d0,$ffff820d.w
	lsr.l	#8,d0
	move.b	d0,$ffff8203.w
	lsr.l	#8,d0
	move.b	d0,$ffff8201.w
	rts
	endif

	section	bss,bss
rt_screen:		ds.l	1	; shown
rt_back:		ds.l	1	; composed next
	ifd	SMKX_LC
rt_want8:		ds.b	1	; the Videl should run eight planes (the list compositor)
rt_clut8_due:		ds.b	1	; smkx_clut8 goes to the Videl at the next VBL
vid_eight:		ds.b	1	; the Videl does
	even
	endif
vid_old_phys:		ds.l	1
vid_monitor:		ds.w	1
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
	ifd	SMKX_LC
rt_screen_copy:		ds.b	57344	; the last composed frame, for SNAP.BIN (eight planes at most)
	else
rt_screen_copy:		ds.b	28672	; the last composed frame, for SNAP.BIN
	endif
rt_screen_raw:		ds.b	2*32256+256
