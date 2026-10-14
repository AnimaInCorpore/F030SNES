; TCBENCH: the core of a chunky true-colour renderer, timed. Not a
; renderer: synthetic VRAM and descriptors, three BG layers (4, 4 and 2
; bits a pixel) of 33 tiles a row, 224 rows, then each pixel's colour as
; RGB565 through the ring and the SSI into DMA record, as ppu.asm sends its
; plane words. It measures what a chunky kernel would cost against the
; planar one (analysis/ct.md, "True colour").
;
; Per tile row: the two VRAM words split into the four plane bytes; four
; L: (X:Y) table lookups, one per plane, give all eight pixels at once as
; 6-bit fields (pixels 0-3 in the X word, 4-7 in the Y word) and add up to
; the eight colour indices; each index is taken out with the multiplier
; and turned into its key (depth << 8 | CGRAM index, 0 when transparent)
; through a table per depth and palette; the key goes into the row's line
; where it is greater (the front layer). The row is then looked up in the
; RGB565 table and queued.
;
; Host commands (24-bit words): $000004 PING (answers $54434230 >> 8:
; "TCB"); $000030 BENCH n, flags: n rows (bit 0 of flags: send them over
; the SSI, else only compute); answers 1 when done.
;
; Memory (as ppu.asm: Y:a = P:a for a < $4000, X:a = P:$4000 + a):
;   P:$0000-$0FFF  this program
;   Y:$1000-$11FF  RING;  Y:$1200-$12FF  PAL (RGB565, bits 23-8)
;   L:$1400-$17FF  T0-T3: per plane and byte its pixels' bits (X: 0-3, Y: 4-7)
;   Y:$2000-$3FFF  VRAM (synthetic)
;   Y:$1800-$191F  ZL, the row's line of keys (256 + margin)
;   X:$0400-$04FF  DESC: per layer 33 x (tile row address, key table)
;   X:$1800-$1DFF  KT: per depth (12) and palette (8) 16 keys

	include	'ioequ.inc'

TIE	equ	14
RING	equ	$1000
PAL	equ	$1200
TB	equ	$1400
VRAM	equ	$2000
ZL	equ	$1800
DESC	equ	$0400
KT	equ	$1800

; X internal variables
TXSTART	equ	$00
ROW	equ	$01
NROWS	equ	$02
FLAGS	equ	$03
DPTR	equ	$04
LAYN	equ	$05
SEED	equ	$06
DBUF	equ	$10		; Y: the tile row's eight indices

	org	p:$0000
	jmp	start

	org	p:$0010			; SSI transmit data: tx_isr
	jsr	tx_isr
	jsr	tx_isr

	org	p:$0040
start:
	jmp	start2

; As ppu.asm: the next ring word unless sent already (bit 0), then off.
tx_isr:
	jset	#0,y:(r6),_stop
	movep	y:(r6),x:m_tx
	bset	#0,y:(r6)+
	rti
_stop:	bclr	#TIE,x:m_crb
	rti

start2:
	movep	#1,x:m_pbc
ld_blk:	jsr	ld_get
	move	#>3,x0
	cmp	x0,a
	jeq	init
	move	a1,x1			; space
	jsr	ld_get
	move	a1,r0			; address
	jsr	ld_get
	move	a1,x0			; count
	move	x1,a
	tst	a
	jeq	_p
	move	#>1,y0
	cmp	y0,a
	jeq	_x
	do	x0,_ye
	jsr	ld_get
	move	a1,y:(r0)+
_ye:	jmp	ld_blk
_x:	do	x0,_xe
	jsr	ld_get
	move	a1,x:(r0)+
_xe:	jmp	ld_blk
_p:	do	x0,_pe
	jsr	ld_get
	movem	a1,p:(r0)+
_pe:	jmp	ld_blk

ld_get:	jclr	#m_hrdf,x:m_hsr,ld_get
	movep	x:m_hrx,a
	rts

; ---------------------------------------------------------------------------
; One tile row of a layer into ZL. r0 -> its first VRAM word (planes 0/1;
; +8 planes 2/3), r4 -> ZL at its x, x:DPTR the next descriptor word (the
; key table), r1 = T0, r2 = T1, r3 = T2, r5 = T3, n0 = 8,
; y0 = $008000, x1 = $FF.
tile4:
	move	y:(r0),x0		; p0 | p1 << 8
	mpy	x0,y0,a			; a1 = p1
	move	x0,b
	and	x1,b			; b1 = p0
	move	a1,n2
	move	b1,n1
	move	y:(r0+n0),x0		; p2 | p3 << 8
	mpy	x0,y0,a			; a1 = p3
	move	x0,b
	and	x1,b			; b1 = p2
	move	a1,n5
	move	b1,n3
	move	l:(r1+n1),a		; plane 0: pixels 0-3 (X), 4-7 (Y)
	move	l:(r2+n2),b		; plane 1
	add	b,a
	move	l:(r3+n3),b		; plane 2
	add	b,a
	move	l:(r5+n5),b		; plane 3
	add	b,a
	jmp	fields

; The same for two bits a pixel (planes 0/1 only).
tile2:
	move	y:(r0),x0
	mpy	x0,y0,a
	move	x0,b
	and	x1,b
	move	a1,n2
	move	b1,n1
	nop
	move	l:(r1+n1),a
	move	l:(r2+n2),b
	add	b,a

; a = eight 6-bit fields: pixels 0-3 in A1 (bits 23-18, 17-12, 11-6,
; 5-0), 4-7 in A0. Per pixel: mpy by 2^5 leaves its field in B1 and the
; rest, shifted up, in B0 (one constant, no mask), the line word (Y) loads
; in parallel, the rest goes back to x0 in the N register's gap; then its
; key (the key table in r5, from x:DPTR) and the max into ZL (Y).
PIX	macro
	mpy	x0,y0,b		y:(r4),a
	move	b1,n5
	move	b0,x0
	move	x:(r5+n5),b
	cmp	a,b
	tgt	b,a
	move	a,y:(r4)+
	endm

fields:
	move	a1,x0			; pixels 0-3
	move	a0,x1			; pixels 4-7
	move	x:DPTR,r0		; the key table
	move	#>$20,y0
	move	x:(r0)+,r5
	move	r0,x:DPTR
	PIX
	PIX
	PIX
	PIX
	move	x1,x0
	PIX
	PIX
	PIX
	PIX
	move	#TB+$300,r5		; T3 again
	move	#>$008000,y0
	move	#>$ff,x1
	rts

; ---------------------------------------------------------------------------
init:
	andi	#$fb,omr
	movep	#1,x:m_pbc
	movep	#0,x:m_bcr
	movep	#0,x:m_crb
	movep	#$4000,x:m_cra		; 16-bit words
	movep	#$01C0,x:m_pcc
	movep	#$0030,x:m_pcddr
	movep	#$1a00,x:m_crb		; TE, network, synchronous
	movep	#$0020,x:m_pcd		; SC2 high: record may take words
	movep	#$3000,x:m_ipr
	move	#$ffff,m0
	move	#$ffff,m1
	move	#$ffff,m2
	move	#$ffff,m3
	move	#$ffff,m4
	move	#$ffff,m5
	move	#$1ff,m6
	move	#$1ff,m7
	move	#RING,r6
	move	#RING,r7
	; The tables only need to exist: the hot path has no branch on the
	; data (the depth test is a conditional transfer), so their contents
	; do not change the time. T0-T3 (L:) and KT get simple patterns.
	move	#TB,r0
	move	#>$041041,a
	move	#>$000001,x0
	do	#$400,_tb
	move	a1,x:(r0)
	move	a1,y:(r0)+
	add	x0,a
_tb:
	move	#KT,r0
	clr	a
	move	a1,x:(r0)+
	move	#>$000101,a
	do	#$5ff,_kt
	move	a1,x:(r0)+
	add	x0,a
_kt:
	; PAL: a colour per index (bits 23-8), bit 0 clear
	move	#PAL,r0
	move	#>$012300,a
	move	#>$4b00,x0
	do	#256,_pl
	add	x0,a
	move	#>$ffff00,x1
	and	x1,a
	move	a1,y:(r0)+
_pl:
	; VRAM: a pattern (the data only fills the tiles)
	move	#VRAM,r0
	move	#>$1234,a
	move	#>$9e37,x0
	move	#>$ffff,x1
	move	#>$2000,y0
	do	y0,_vr
	add	x0,a
	and	x1,a
	move	a1,y:(r0)+
_vr:
	; DESC: per layer and column a tile row address and a key table
	move	#DESC,r0
	move	#>VRAM,a
	move	#>KT,b
	do	#99,_ds
	move	a1,x:(r0)+
	move	b1,x:(r0)+
	move	#>37,x0
	add	x0,a
	move	#>$1fff,x0
	and	x0,a
	move	#>VRAM,x0
	or	x0,a
	move	#>16*5,x0
	add	x0,b
	move	#>$5ff,x0
	and	x0,b
	move	#>KT,x0
	or	x0,b
_ds:
	andi	#$fc,mr			; interrupts on

main:	jsr	get
	move	#>4,x0
	cmp	x0,a
	jeq	cmd_ping
	move	#>$30,x0
	cmp	x0,a
	jeq	cmd_bench
	jmp	main

get:	jclr	#m_hrdf,x:m_hsr,get
	movep	x:m_hrx,a
	rts

put:	jclr	#m_htde,x:m_hsr,put
	movep	a,x:m_htx
	rts

cmd_ping:
	move	#>$544342,a
	jsr	put
	jmp	main

cmd_bench:
	jsr	get
	move	a1,x:NROWS
	jsr	get
	move	a1,x:FLAGS
	clr	a
	move	a1,x:ROW
	move	a1,x:TXSTART
	ori	#$03,mr			; the ring: all marked sent
	bclr	#TIE,x:m_crb
	move	#RING,r6
	move	#RING,r7
	move	#>1,a
	rep	#512
	move	a,y:(r7)+
	andi	#$fc,mr
_row:	jclr	#0,x:FLAGS,_nr1
	jsr	room
_nr1:	jsr	bench_row
	jclr	#0,x:FLAGS,_nr2
	jsr	gate
_nr2:	move	x:ROW,a
	move	#>1,x0
	add	x0,a
	move	a1,x:ROW
	move	x:NROWS,x0
	cmp	x0,a
	jlt	_row
	jclr	#0,x:FLAGS,_done
_dr:	jsr	gate
	jsr	fill
	tst	a
	jne	_dr
	bclr	#TIE,x:m_crb
_done:	move	#>1,a
	jsr	put
	jmp	main

; One row: ZL cleared (the backdrop's key, 0), three layers, the output.
bench_row:
	clr	a
	move	#ZL,r4
	rep	#288
	move	a,y:(r4)+
	move	#TB,r1
	move	#TB+$100,r2
	move	#TB+$200,r3
	move	#TB+$300,r5
	move	#8,n0
	move	#>$008000,y0
	move	#>$ff,x1
	move	#DESC,r0
	move	r0,x:DPTR
	move	#ZL,r4
	do	#33,_l1			; layer 1, 4 bits
	move	x:DPTR,r0
	nop
	move	x:(r0)+,x0
	move	r0,x:DPTR
	move	x0,r0
	nop
	jsr	tile4
	nop
_l1:
	move	#ZL,r4
	do	#33,_l2			; layer 2, 4 bits
	move	x:DPTR,r0
	nop
	move	x:(r0)+,x0
	move	r0,x:DPTR
	move	x0,r0
	nop
	jsr	tile4
	nop
_l2:
	move	#ZL,r4
	do	#33,_l3			; layer 3, 2 bits
	move	x:DPTR,r0
	nop
	move	x:(r0)+,x0
	move	r0,x:DPTR
	move	x0,r0
	nop
	jsr	tile2
	nop
_l3:
	jclr	#0,x:FLAGS,_c		; the colours: queued, or only looked up
	move	#ZL,r4
	move	#PAL,r5
	move	#>$ff,x1
	do	#256,_o
	move	y:(r4)+,a
	and	x1,a
	move	a1,n5
	nop
	move	y:(r5+n5),x0
	move	x0,y:(r7)+
_o:
	rts
_c:	move	#ZL,r4
	move	#PAL,r5
	move	#>$ff,x1
	do	#256,_o2
	move	y:(r4)+,a
	and	x1,a
	move	a1,n5
	nop
	move	y:(r5+n5),x0
	move	x0,y:$10
_o2:
	rts

fill:	move	r7,a
	move	r6,x0
	sub	x0,a
	move	#>$1ff,x0
	and	x0,a
	move	a1,x0
	move	x0,a
	rts

gate:	jsr	fill
	tst	a
	jeq	_x
	jsr	tx_start
	bset	#TIE,x:m_crb
_x:	rts

tx_start:
	jset	#0,x:TXSTART,_x
	jsr	fill
	tst	a
	jeq	_x
	bset	#0,x:TXSTART
_w:	jclr	#m_tde,x:m_sr,_w
	movep	y:(r6),x:m_tx
	bset	#0,y:(r6)+
_x:	rts

; Room for a row: at most 256 words queued.
room:	jsr	gate
	jsr	fill
	move	#>256,x0
	cmp	x0,a
	jgt	room
	rts

	end
