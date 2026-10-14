; SMK Mode 7 on the Falcon DSP, v1: sampling, CGRAM -> palette slot and
; the four-plane c2p, each finished line streamed over the SSI into DMA
; record (handshake mode, SC2 = frame line).
;
; Reference: ppu_m7_pixels() + ppu_planar() in src/snes_ppu.c; M7TEST.TOS
; checks this kernel against them byte for byte. v1 is written for clarity
; (about 34 instruction cycles per pixel); speed comes after the check.
;
; Memory
;   X:$0000-$3FFF  Mode 7 VRAM word i as (tile*64) << 8 | texel, where
;                  tile = VRAM byte 2i and texel = VRAM byte 2i+1
;   Y:$0000-$03FF  colour tables PL0..PL3 (256 each): CGRAM index ->
;                  bit p of its palette slot, at bit 8
;   Y:$0400-$07FF  line buffer: pixel x, plane p at $0400 + 4x + p
;   Y:$0800-$1FFF  coordinate tables built at boot:
;                  TVM[t] = (t>>3)*128, TUM[t] = t>>3   for t = 0..1023
;                  TVF[t] = (t&7)*8,    TUF[t] = t&7    for t = 0..2047
;                  (tile-0 segments reach texel 2047: see m7_plan.c)
;   X:$4000...     none: X is full. Scalars live in the Y area below.
;   P:$0000-$01FF  this program (internal P; booted by Dsp_ExecBoot)
;
; Host commands (host port, 24-bit words):
;   $000001 VRAM    16384 words as above
;   $000002 COLOUR  1024 words: PL0[256], PL1[256], PL2[256], PL3[256]
;   $000003 LINE    segments covering 256 pixels, each: kind, count, then
;     kind 1 INSIDE a1 = v, a0 = u + $800000, then x1, x0 = the 48-bit
;                   step (dv << 24) + du; u and v (1/256 texel) stay in
;                   0..$3FFFF over the segment, so a1:a0 never carries
;     kind 2 TILE0  the same four words with u, v reduced mod 2048
;     kind 3 FILL   one CGRAM index for every pixel
;   After the 256th pixel the line's 64 plane words go out over the SSI:
;   for each 16-pixel group, planes 0-3, most significant bit leftmost.
;   $000004 PING    answers $4D3743 ("M7C")
;   $000005 VRAMSET first word, count, then count words as for VRAM

	include	'ioequ.inc'

PL0	equ	$0000
LINEB	equ	$0400
TVM	equ	$0800
TUM	equ	$0C00
TVF	equ	$1000
TUF	equ	$1800
KIND	equ	$2000		; Y scalars
COUNT	equ	$2001
LEFT	equ	$2002

	org	p:$0000
	jmp	start

	org	p:$0040
start:
	movep	#1,x:m_pbc		; host port on
	movep	#0,x:m_bcr		; no wait states
	movep	#0,x:m_crb		; SSI transmitter off while set up
	movep	#$4000,x:m_cra		; 16-bit words
	movep	#$0140,x:m_pcc		; PC6/SCK, PC8/STD to the SSI
	movep	#$0020,x:m_pcddr	; PC5 GPIO: the frame line (SC2)
	movep	#$1a00,x:m_crb		; TE, network, synchronous, clock in
	movep	#$0020,x:m_pcd		; SC2 high: record may take words
	jsr	build_tables

main:	jsr	get
	move	#>1,x0
	cmp	x0,a
	jeq	cmd_vram
	move	#>2,x0
	cmp	x0,a
	jeq	cmd_colour
	move	#>3,x0
	cmp	x0,a
	jeq	cmd_line
	move	#>4,x0
	cmp	x0,a
	jeq	cmd_ping
	move	#>5,x0
	cmp	x0,a
	jeq	cmd_vramset
	jmp	main

get:	jclr	#m_hrdf,x:m_hsr,get
	movep	x:m_hrx,a
	rts

put:	jclr	#m_htde,x:m_hsr,put
	movep	a,x:m_htx
	rts

cmd_ping:
	move	#>$4d3743,a
	jsr	put
	jmp	main

; ---------------------------------------------------------------------------
build_tables:
	move	#TVM,r0
	move	#TUM,r1
	move	#TVF,r2
	move	#TUF,r3
	clr	a
	move	#>1,x0
	move	#>7,x1
	do	#1024,bt_end
	move	a1,b
	lsr	b
	lsr	b
	lsr	b
	move	b1,y:(r1)+		; TUM
	rep	#7
	lsl	b
	move	b1,y:(r0)+		; TVM
	add	x0,a
bt_end:
	clr	a
	do	#2048,bf_end
	move	a1,b
	and	x1,b
	move	b1,y:(r3)+		; TUF
	lsl	b
	lsl	b
	lsl	b
	move	b1,y:(r2)+		; TVF
	add	x0,a
bf_end:
	rts

; ---------------------------------------------------------------------------
cmd_vramset:
	jsr	get
	move	a1,r0
	jsr	get
	move	a1,x0
	jmp	vr_go
cmd_vram:
	move	#0,r0
	move	#>16384,x0		; DO immediates stop at 4095
vr_go:	do	x0,vr_end
vr_w:	jclr	#m_hrdf,x:m_hsr,vr_w
	movep	x:m_hrx,x:(r0)+
vr_end:
	jmp	main

cmd_colour:
	move	#PL0,r0
	do	#1024,co_end
co_w:	jclr	#m_hrdf,x:m_hsr,co_w
	movep	x:m_hrx,y:(r0)+
co_end:
	jmp	main

; ---------------------------------------------------------------------------
cmd_line:
	move	#LINEB,r7
	move	#>256,a
	move	a1,y:LEFT
seg:	jsr	get
	move	a1,y:KIND
	jsr	get
	move	a1,y:COUNT
	move	y:KIND,a
	move	#>3,x0
	cmp	x0,a
	jeq	seg_fill
	jsr	get
	move	a1,y:TMP_A1
	jsr	get
	move	a1,y:TMP_A0
	jsr	get
	move	a1,x1
	jsr	get
	move	a1,x0
	move	y:TMP_A1,a1
	move	y:TMP_A0,a0
	move	#>$008000,y1		; multiply by this = shift right 8
	move	#TVM,n1
	move	#TUM-$8000,n0		; tu arrives as tu - $8000
	move	#TVF,n4
	move	#TUF-$8000,n6
	move	#>$100,n3		; colour table stride
	move	y:KIND,b
	move	#>2,y0
	cmp	y0,b
	jeq	seg_tile0

	move	y:COUNT,n7
	nop
	do	n7,in_end
	move	a1,y0
	mpy	y0,y1,b		a0,y0	; b1 = tv
	mpy	y0,y1,b		b1,r1	; b1 = tu - $8000; r1 = tv
	move	b1,r0
	move	r1,r4
	move	r0,r6
	move	y:(r1+n1),r2		; TVM[tv]
	move	y:(r0+n0),n2		; TUM[tu]: map index = r2 + n2
	move	y:(r4+n4),n3		; TVF[tv]
	move	y:(r6+n6),n5		; TUF[tu]
	move	x:(r2+n2),y0		; map word
	mpy	y0,y1,b			; b1 = tile*64
	move	b1,r3
	add	x,a			; the next pixel's coordinates
	lua	(r3)+n3,r5		; tile*64 + fine row
	move	#>$ff,y0
	move	x:(r5+n5),b		; texel word
	and	y0,b
	move	b1,r3			; CGRAM index
	move	#>$100,n3
	nop
	move	y:(r3)+n3,y0		; PL0
	move	y0,y:(r7)+
	move	y:(r3)+n3,y0		; PL1
	move	y0,y:(r7)+
	move	y:(r3)+n3,y0		; PL2
	move	y0,y:(r7)+
	move	y:(r3)+n3,y0		; PL3
	move	y0,y:(r7)+
in_end:
	jmp	seg_next

seg_tile0:
	move	y:COUNT,n7
	nop
	do	n7,t0_end
	move	a1,y0
	mpy	y0,y1,b		a0,y0
	mpy	y0,y1,b		b1,r4	; r4 = tv
	move	b1,r6			; tu - $8000
	move	#0,r3
	move	y:(r4+n4),n3		; TVF[tv]
	move	y:(r6+n6),n5		; TUF[tu]
	add	x,a
	lua	(r3)+n3,r5		; tile 0 + fine row
	move	#>$ff,y0
	move	x:(r5+n5),b
	and	y0,b
	move	b1,r3
	move	#>$100,n3
	nop
	move	y:(r3)+n3,y0
	move	y0,y:(r7)+
	move	y:(r3)+n3,y0
	move	y0,y:(r7)+
	move	y:(r3)+n3,y0
	move	y0,y:(r7)+
	move	y:(r3)+n3,y0
	move	y0,y:(r7)+
t0_end:
	jmp	seg_next

seg_fill:
	jsr	get			; CGRAM index
	move	a1,r3
	move	#>$100,n3
	nop
	move	y:(r3)+n3,x0
	move	y:(r3)+n3,x1
	move	y:(r3)+n3,y0
	move	y:(r3)+n3,y1
	move	y:COUNT,n7
	nop
	do	n7,fi_end
	move	x0,y:(r7)+
	move	x1,y:(r7)+
	move	y0,y:(r7)+
	move	y1,y:(r7)+
fi_end:

seg_next:
	move	y:LEFT,a
	move	y:COUNT,x0
	sub	x0,a
	move	a1,y:LEFT
	tst	a
	jgt	seg
	jsr	c2p_line
	jmp	main

; ---------------------------------------------------------------------------
; c2p: A = 2A + B over 16 pixels, B = the plane bit at bit 8, leaves the
; plane word in A1 bits 23-8: the SSI sends bits 23-8.
c2p_line:
	move	#LINEB,r0
	move	#>64,n0			; next group: 16 pixels x 4 planes
	move	#>4,n2			; same plane, next pixel
	do	#16,c2_end
	move	r0,r1			; plane 0 of this group
	do	#4,c2_plane
	clr	a		r1,r2
	nop
	move	y:(r2)+n2,b
	do	#16,c2_px
	addl	b,a		y:(r2)+n2,b
c2_px:
	jsr	ssi_send		; a1 bits 23-8
	move	(r1)+			; next plane
c2_plane:
	move	(r0)+n0
	nop
c2_end:
	rts

; Wait for the transmitter to be empty, then hand it the word in a1.
ssi_send:
	jclr	#m_tde,x:m_sr,ssi_send
	movep	a1,x:m_tx
	rts

TMP_A1	equ	$2003
TMP_A0	equ	$2004

	end
