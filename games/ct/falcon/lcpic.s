; LCPIC.TOS: shows one picture (pic0.planar: 224 rows of 128 words, ppu_planar8's layout; pic0.pal:
; 256 Falcon palette longwords) in the 256x224 eight-plane mode of lcplay.s. A test of the Videl
; setup and the palette, apart from the stream. Ends at a key.

	section	text,code
start:
	lea	ustack_top,sp
	move.w	#89,-(sp)		; XBIOS Montype
	trap	#14
	addq.l	#2,sp
	move.w	d0,monitor
	clr.l	-(sp)
	move.w	#$20,-(sp)		; Super(0)
	trap	#1
	addq.l	#6,sp
	move.l	d0,old_ssp
	lea	sstack_top,sp
	tst.w	monitor
	beq.s	quit
	lea	screen_raw,a1
	move.l	a1,d0
	add.l	#255,d0
	andi.l	#$ffffff00,d0
	movea.l	d0,a1
	move.l	d0,screen
	lea	pic,a0
	move.w	#57344/4-1,d1
.c:	move.l	(a0)+,(a1)+
	dbra	d1,.c
	move.l	screen,d0
	bsr	vid_on
	lea	pal,a0
	lea	$ffff9800.w,a1
	move.w	#255,d0
.p:	move.l	(a0)+,(a1)+
	dbra	d0,.p
.k:	move.w	#11,-(sp)		; Cconis
	trap	#1
	addq.l	#2,sp
	tst.w	d0
	beq.s	.k
quit:	move.l	old_ssp,-(sp)
	move.w	#$20,-(sp)
	trap	#1
	addq.l	#6,sp
	clr.w	-(sp)
	trap	#1

vid_on:
	move.b	d0,$ffff820d.w
	lsr.l	#8,d0
	move.b	d0,$ffff8203.w
	lsr.l	#8,d0
	move.b	d0,$ffff8201.w
	cmpi.w	#2,monitor
	beq.s	.vga
	move.l	#$00fe00ab,$ffff8282.w
	move.l	#$0047002c,$ffff8286.w
	move.l	#$004d00d8,$ffff828a.w
	move.l	#$0271021f,$ffff82a2.w
	move.l	#$005f005f,$ffff82a6.w
	move.l	#$021f026b,$ffff82aa.w
	move.w	#$0200,$ffff820a.w
	move.w	#$0181,$ffff82c0.w
	clr.w	$ffff82c2.w
	bra.s	.common
.vga:	move.l	#$00c6006d,$ffff8282.w
	move.l	#$003502aa,$ffff8286.w
	move.l	#$004b0096,$ffff828a.w
	move.l	#$041903cf,$ffff82a2.w
	move.l	#$004d004b,$ffff82a6.w
	move.l	#$03cb0415,$ffff82aa.w
	move.w	#$0200,$ffff820a.w
	move.w	#$0182,$ffff82c0.w
	move.w	#5,$ffff82c2.w
.common:
	clr.w	$ffff8266.w
	move.w	#$0010,$ffff8266.w
	clr.w	$ffff8264.w		; (a word: $8266 holds the plane count)
	clr.w	$ffff820e.w
	move.w	#128,$ffff8210.w
	rts

	section	data,data
	even
pic:	incbin	"pic0.planar"
pal:	incbin	"pic0.pal"

	section	bss,bss
monitor:	ds.w	1
old_ssp:	ds.l	1
screen:		ds.l	1
ustack:		ds.b	1024
ustack_top:
sstack:		ds.b	1024
sstack_top:
screen_raw:	ds.b	57344+512
