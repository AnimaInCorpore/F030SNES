; smkx_info.s: the info screen and the player's files, read at startup.
; The program carries no game content: the ROM (and SMK's DSP-1 dump) is
; read from next to the program and checked against the build's checksum.
; The texts, the file list and the checksums are in info.i
; (tools/port_info.py, included at the top of smkx_rt.s).  The same file in
; every port: change it in all of them.
;
; info_start runs first thing, in user mode, before anything is set up: on a
; missing or wrong file it says so, waits for a key and quits to the desktop.
; Included where the ROM used to be built in; leaves the data section current.

RF_DEST		equ	0		; where the bytes go
RF_BYTES	equ	4		; how many (a multiple of 4)
RF_OFFSET	equ	8		; from this file offset
RF_SIZE		equ	12		; the file's size (+512: a copier header, skipped)
RF_SUM		equ	16		; per long: sum = rol(sum, 1) + long
RF_NAME		equ	20		; the 8.3 name
RF_ALT		equ	24		; 0, or a second name to try (.SMC)
RF_LEN		equ	28

	ifgt	ROM_BYTES-ROM_SIZE
	fail	"the ROM is larger than ROM_SIZE"
	endif

	section	text,code

info_start:
	movem.l	d3-d7/a3-a6,-(sp)
	lea	info_text,a0
	bsr	info_print
	lea	info_files,a5
.file:	tst.l	RF_DEST(a5)
	beq	.loaded
	lea	info_indent,a0
	bsr	info_print
	move.l	RF_NAME(a5),a4
	bsr	info_open
	bpl.s	.open
	move.l	RF_ALT(a5),d0
	beq.s	.missing
	move.l	d0,a4
	bsr	info_open
	bpl.s	.open
.missing:
	move.l	RF_NAME(a5),a0
	bsr	info_print
	lea	info_notfound,a0
	bra	.fail

.open:	move.l	a4,a0			; the name that opened
	bsr	info_print
	move.w	#2,-(sp)
	move.w	d7,-(sp)
	clr.l	-(sp)
	move.w	#$42,-(sp)		; Fseek(0, handle, from the end): the size
	trap	#1
	lea	10(sp),sp
	move.l	RF_OFFSET(a5),d6
	cmp.l	RF_SIZE(a5),d0
	beq.s	.seek
	tst.l	RF_ALT(a5)		; a copier header: ROMs only
	beq.s	.badsize
	sub.l	#512,d0
	cmp.l	RF_SIZE(a5),d0
	bne.s	.badsize
	add.l	#512,d6
.seek:	clr.w	-(sp)
	move.w	d7,-(sp)
	move.l	d6,-(sp)
	move.w	#$42,-(sp)		; Fseek(offset, handle, from the start)
	trap	#1
	lea	10(sp),sp
	cmp.l	d6,d0
	bne.s	.readerr
	move.l	RF_DEST(a5),-(sp)
	move.l	RF_BYTES(a5),-(sp)
	move.w	d7,-(sp)
	move.w	#$3f,-(sp)		; Fread
	trap	#1
	lea	12(sp),sp
	cmp.l	RF_BYTES(a5),d0
	bne.s	.readerr
	bsr	info_close

	move.l	RF_DEST(a5),a0
	move.l	RF_BYTES(a5),d1
	lsr.l	#2,d1
	moveq	#0,d0
.sum:	rol.l	#1,d0
	add.l	(a0)+,d0
	subq.l	#1,d1
	bne.s	.sum
	lea	info_badsum,a0
	cmp.l	RF_SUM(a5),d0
	bne.s	.fail
	lea	info_ok,a0
	bsr	info_print
	lea	RF_LEN(a5),a5
	bra	.file

.badsize:
	bsr	info_close
	lea	info_size,a0
	bra.s	.fail
.readerr:
	bsr	info_close
	lea	info_readerr,a0
.fail:	bsr	info_print
	lea	info_quit,a0
	bsr	info_print
	move.w	#8,-(sp)		; Cnecin
	trap	#1
	addq.l	#2,sp
	clr.w	-(sp)			; Pterm0
	trap	#1

.loaded:
	ifne	INFO_WAIT
	lea	info_wait,a0
	bsr	info_print
	bsr	info_clock
	move.l	d0,d6
.wait:	move.w	#11,-(sp)		; Cconis
	trap	#1
	addq.l	#2,sp
	tst.w	d0
	bne.s	.key
	bsr	info_clock
	sub.l	d6,d0
	cmp.l	#INFO_WAIT*200,d0
	bcs.s	.wait
	bra.s	.go
.key:	move.w	#8,-(sp)		; Cnecin: the key that skipped the wait
	trap	#1
	addq.l	#2,sp
.go:
	endif
	movem.l	(sp)+,d3-d7/a3-a6
	rts

; a0 = text: Cconws.
info_print:
	move.l	a0,-(sp)
	move.w	#9,-(sp)
	trap	#1
	addq.l	#6,sp
	rts

; a4 = name: Fopen for reading.  d7 = handle; N set when it failed.
info_open:
	clr.w	-(sp)
	move.l	a4,-(sp)
	move.w	#$3d,-(sp)
	trap	#1
	addq.l	#8,sp
	move.l	d0,d7
	rts

info_close:
	move.w	d7,-(sp)
	move.w	#$3e,-(sp)		; Fclose
	trap	#1
	addq.l	#4,sp
	rts

; d0 = the 200 Hz system timer ($4BA, read in supervisor mode).
info_clock:
	pea	.read
	move.w	#38,-(sp)		; Supexec
	trap	#14
	addq.l	#6,sp
	move.l	info_ticks,d0
	rts
.read:	move.l	$4ba.w,info_ticks
	rts

	section	data,data
info_indent:	dc.b	"  ",0
info_ok:	dc.b	": ok",13,10,0
info_notfound:	dc.b	": not found",0
info_size:	dc.b	": not the expected size",0
info_readerr:	dc.b	": could not be read",0
info_badsum:	dc.b	": not the expected file",13,10
		dc.b	"  (another game, revision or dump)",0
info_quit:	dc.b	13,10,13,10,"See README.TXT.  Press a key to quit.",0
	even

	section	bss,bss
	cnop	0,4
smkx_rom:	ds.b	ROM_SIZE	; the player's ROM (info_start reads it)
info_ticks:	ds.l	1

	section	data,data
