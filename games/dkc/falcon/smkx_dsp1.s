; DSP-1 model for the translated SMK program (included by smkx_rt.s).
;
; Interface (HiROM, banks $00-$1F): DR at $6000-$6FFF, SR at $7000-$7FFF.
; Protocol, as SMK drives it:
;   - a command byte, then 16-bit parameters written low byte first;
;   - results read as 16-bit words, low byte first;
;   - SR bit 15 (RQM) polled before reading: this model is always ready;
;   - command $0A streams four words per raster line without end;
;   - a byte $80 written while results are pending, or while waiting for a
;     command, returns to waiting for a command (SMK's reset loop writes 128
;     of them; it ends a raster stream with the bytes 00 80 00 80 ...).
;
; Arithmetic. Multiply, triangle and distance follow their definitions;
; sine and cosine come from a generated 4096-entry Q15 table (smkx_tables.s).
; The projection commands ($02, $06, $0A) implement a geometric model, not
; the chip's firmware: an eye at distance Lfe from the focus point F,
; direction n = (-sinZ sinA, sinZ cosA, cosZ) from F to the eye, a screen
; at distance Les, ground z = 0. For a raster line v the ground scale is
; t = Ez / (Les cosZ + v sinZ) and the Mode 7 row is
;   A = t cosA, B = -t sinA / cosZ, C = t sinA, D = t cosA / cosZ (8.8).
; Results can differ from the real DSP-1 in low bits and in conventions the
; firmware defines; an exact version needs the chip's program.

DSP_CMD		equ	0
DSP_PARAMS	equ	1
DSP_OUTPUT	equ	2

	section	text,code

; d7.w = address ($6000-$7FFF); returns d6.b.
dsp1_read:
	cmpi.w	#$7000,d7
	bcs.s	.dr
	move.b	#$80,d6			; SR: RQM set, 8-bit data register idle
	rts
.dr:	cmpi.b	#DSP_OUTPUT,dsp_state
	beq.s	.out
	moveq	#0,d6
	rts
.out:	movem.l	d4/a0,-(sp)
	move.w	dsp_outpos,d4
	lea	dsp_out,a0
	move.b	1(a0,d4.w),d6		; words are stored big-endian: low byte at +1
	btst	#0,d4
	beq.s	.lo
	move.b	-1(a0,d4.w),d6		; odd position: high byte of the word
.lo:	addq.w	#1,d4
	move.w	d4,dsp_outpos
	cmp.w	dsp_outbytes,d4
	bcs.s	.more
	cmpi.b	#$0a,dsp_cmd
	bne.s	.done
	movem.l	d0-d5/a1,-(sp)		; the raster stream continues: next line
	addq.w	#1,dsp_vs
	bsr	dsp1_raster_line
	movem.l	(sp)+,d0-d5/a1
	clr.w	dsp_outpos
	bra.s	.more
.done:	move.b	#DSP_CMD,dsp_state
.more:	movem.l	(sp)+,d4/a0
	rts

; d6.b = byte written to DR.
dsp1_write:
	movem.l	d0-d5/a0-a1,-(sp)
	move.b	dsp_state,d0
	cmpi.b	#DSP_PARAMS,d0
	beq.s	.param
	cmpi.b	#$80,d6
	beq.s	.reset
	cmpi.b	#DSP_OUTPUT,d0
	beq.s	.x			; other writes while results are pending: ignored
	; waiting for a command
	cmpi.b	#$40,d6
	bcc.s	.x			; $40-$FF: no command
	move.b	d6,dsp_cmd
	moveq	#0,d1
	move.b	d6,d1
	lea	dsp_counts,a0
	move.b	(a0,d1.w),d2		; parameter words
	andi.w	#$0f,d2
	add.w	d2,d2
	move.w	d2,dsp_inbytes
	clr.w	dsp_inpos
	move.b	#DSP_PARAMS,dsp_state
	tst.w	d2
	bne.s	.x
	bsr	dsp1_execute
	bra.s	.x
.reset:	move.b	#DSP_CMD,dsp_state
	bra.s	.x
.param:	move.w	dsp_inpos,d1
	lea	dsp_in,a0
	btst	#0,d1
	bne.s	.hi
	move.b	d6,1(a0,d1.w)		; low byte
	bra.s	.stored
.hi:	move.b	d6,-1(a0,d1.w)		; high byte of the same word
.stored:
	addq.w	#1,d1
	move.w	d1,dsp_inpos
	cmp.w	dsp_inbytes,d1
	bcs.s	.x
	bsr	dsp1_execute
.x:	movem.l	(sp)+,d0-d5/a0-a1
	rts

; Run the command in dsp_cmd with its parameters in dsp_in (big-endian
; words); results go to dsp_out and the model switches to DSP_OUTPUT.
dsp1_execute:
	moveq	#0,d1
	move.b	dsp_cmd,d1
	lea	dsp_counts,a0
	moveq	#0,d2
	move.b	(a0,d1.w),d2
	lsr.w	#4,d2			; result words
	add.w	d2,d2
	move.w	d2,dsp_outbytes
	clr.w	dsp_outpos
	lea	dsp_out,a1
	clr.l	(a1)
	clr.l	4(a1)
	clr.l	8(a1)
	lea	dsp_in,a0
	andi.w	#$0f,d1			; commands repeat with bits 4 and 5 set
	add.w	d1,d1
	move.w	(.jt,pc,d1.w),d1
	jsr	(.jt,pc,d1.w)
	move.b	#DSP_OUTPUT,dsp_state
	tst.w	dsp_outbytes
	bne.s	.r
	move.b	#DSP_CMD,dsp_state
.r:	rts
.jt:	dc.w	dsp1_mult-.jt,dsp1_none-.jt,dsp1_param-.jt,dsp1_none-.jt
	dc.w	dsp1_triangle-.jt,dsp1_none-.jt,dsp1_project-.jt,dsp1_none-.jt
	dc.w	dsp1_radius-.jt,dsp1_none-.jt,dsp1_raster-.jt,dsp1_none-.jt
	dc.w	dsp1_none-.jt,dsp1_none-.jt,dsp1_none-.jt,dsp1_none-.jt

dsp1_none:
	rts

; $00/$20: A * B >> 15 ($28 shares the low nibble with $08: see dsp1_radius).
dsp1_mult:
	move.w	(a0),d0
	muls.w	2(a0),d0
	add.l	d0,d0
	swap	d0
	cmpi.b	#$20,dsp_cmd
	bne.s	.w
	addq.w	#1,d0
.w:	move.w	d0,(a1)
	rts

; $04: angle, radius -> R sin(angle), R cos(angle).
dsp1_triangle:
	move.w	(a0),d0
	bsr	dsp1_sincos		; d1 = sin, d2 = cos (Q15)
	move.w	2(a0),d0
	muls.w	d0,d1
	add.l	d1,d1
	swap	d1
	muls.w	d0,d2
	add.l	d2,d2
	swap	d2
	move.w	d1,(a1)
	move.w	d2,2(a1)
	rts

; $08: X, Y, Z -> X^2 + Y^2 + Z^2 (low, high words).
; $28: X, Y, Z -> sqrt(X^2 + Y^2 + Z^2), limited to $7FFF.
dsp1_radius:
	move.w	(a0),d0
	muls.w	d0,d0
	move.w	2(a0),d1
	muls.w	d1,d1
	add.l	d1,d0
	move.w	4(a0),d1
	muls.w	d1,d1
	add.l	d1,d0
	cmpi.b	#$28,dsp_cmd
	beq.s	.dist
	move.w	d0,(a1)
	swap	d0
	move.w	d0,2(a1)
	rts
.dist:	bsr	dsp1_isqrt
	cmpi.l	#$7fff,d0
	bls.s	.ok
	move.w	#$7fff,d0
.ok:	move.w	d0,(a1)
	rts

; d0.l unsigned -> d0.l = floor(sqrt(d0)). Uses d1-d3.
dsp1_isqrt:
	moveq	#0,d1			; result
	move.l	#$40000000,d2		; bit
.shrink:
	cmp.l	d0,d2
	bls.s	.loop
	lsr.l	#2,d2
	bne.s	.shrink
.loop:	tst.l	d2
	beq.s	.done
	move.l	d1,d3
	add.l	d2,d3
	cmp.l	d3,d0
	bcs.s	.no
	sub.l	d3,d0
	lsr.l	#1,d1
	add.l	d2,d1
	bra.s	.next
.no:	lsr.l	#1,d1
.next:	lsr.l	#2,d2
	bra.s	.loop
.done:	move.l	d1,d0
	rts

; d0.w = angle (65536 = full turn) -> d1.w = sin, d2.w = cos (Q15).
dsp1_sincos:
	move.l	a0,-(sp)
	lea	dsp1_sine,a0
	move.w	d0,d1
	lsr.w	#4,d1
	add.w	d1,d1
	move.w	(a0,d1.w),d2		; sin
	addi.w	#1024*2,d1
	andi.w	#4096*2-1,d1
	move.w	(a0,d1.w),d1		; cos
	exg	d1,d2
	move.l	(sp)+,a0
	rts

; Q15 product: d0.w * d1.w >> 15 -> d0.l (sign-extended word).
mulq15	macro
	muls.w	\1,\2
	add.l	\2,\2
	swap	\2
	ext.l	\2
	endm

; $02: Fx, Fy, Fz, Lfe, Les, Aas, Azs -> Vof, Vva, Cx, Cy.
dsp1_param:
	move.w	10(a0),d0		; Aas
	bsr	dsp1_sincos
	move.w	d1,dsp_sa
	move.w	d2,dsp_ca
	move.w	12(a0),d0		; Azs
	bsr	dsp1_sincos
	move.w	d1,dsp_sz
	move.w	d2,dsp_cz
	move.w	8(a0),dsp_les
	; n = (-sZ sA, sZ cA, cZ); E = F + Lfe n
	move.w	dsp_sz,d3
	move.w	dsp_sa,d0
	mulq15	d3,d0
	neg.l	d0
	move.w	6(a0),d1
	mulq15	d1,d0			; Lfe * nx
	move.w	(a0),d4
	ext.l	d4
	add.l	d0,d4
	move.l	d4,dsp_ex
	move.w	dsp_ca,d0
	mulq15	d3,d0
	move.w	6(a0),d1
	mulq15	d1,d0
	move.w	2(a0),d4
	ext.l	d4
	add.l	d0,d4
	move.l	d4,dsp_ey
	move.w	dsp_cz,d0
	move.w	6(a0),d1
	mulq15	d1,d0
	move.w	4(a0),d4
	ext.l	d4
	add.l	d0,d4
	move.l	d4,dsp_ez
	; ground point at the screen centre: G0 = E + Ez tanZ (sA, -cA)
	move.l	dsp_ez,d0
	move.w	dsp_sz,d1
	ext.l	d1
	muls.l	d1,d0			; Ez sZ (Q15)
	move.w	dsp_cz,d1
	ext.l	d1
	bne.s	.cz
	moveq	#1,d1
.cz:	divs.l	d1,d0			; Ez tanZ
	move.l	d0,d5
	move.w	dsp_sa,d1
	ext.l	d1
	muls.l	d5,d1
	asr.l	#8,d1
	asr.l	#7,d1
	add.l	dsp_ex,d1
	move.w	d1,4(a1)		; Cx
	move.w	dsp_ca,d1
	ext.l	d1
	muls.l	d5,d1
	asr.l	#8,d1
	asr.l	#7,d1
	move.l	dsp_ey,d0
	sub.l	d1,d0
	move.w	d0,6(a1)		; Cy
	; horizon raster: Vva = -Les cZ / sZ; Vof = 0
	move.w	dsp_les,d0
	ext.l	d0
	move.w	dsp_cz,d1
	ext.l	d1
	muls.l	d1,d0
	move.w	dsp_sz,d1
	ext.l	d1
	beq.s	.flat
	divs.l	d1,d0
	neg.l	d0
	bsr	dsp1_clamp16
	move.w	d0,2(a1)		; Vva
.flat:	clr.w	(a1)			; Vof
	rts

; d0.l -> d0.w limited to -32768..32767.
dsp1_clamp16:
	cmpi.l	#32767,d0
	ble.s	.lo
	move.l	#32767,d0
	rts
.lo:	cmpi.l	#-32768,d0
	bge.s	.ok
	move.l	#-32768,d0
.ok:	rts

; $0A: Vs -> A, B, C, D for raster Vs, then for Vs+1, ... while read.
dsp1_raster:
	move.w	(a0),dsp_vs
dsp1_raster_line:
	lea	dsp_out,a1
	move.w	#8,dsp_outbytes
	; denominator Les cZ + v sZ (Q15)
	move.w	dsp_les,d0
	ext.l	d0
	move.w	dsp_cz,d1
	ext.l	d1
	muls.l	d1,d0
	move.w	dsp_vs,d1
	ext.l	d1
	move.w	dsp_sz,d2
	ext.l	d2
	muls.l	d2,d1
	add.l	d1,d0
	asr.l	#8,d0
	asr.l	#7,d0
	ble.s	.sky			; at or above the horizon
	; t = Ez / denominator, in 8.8
	move.l	dsp_ez,d1
	asl.l	#8,d1
	divs.l	d0,d1
	move.l	d1,d4			; t (8.8)
	move.w	dsp_ca,d0
	ext.l	d0
	muls.l	d4,d0
	asr.l	#8,d0
	asr.l	#7,d0
	bsr	dsp1_clamp16
	move.w	d0,(a1)			; A = t cosA
	move.w	dsp_sa,d0
	ext.l	d0
	muls.l	d4,d0
	asr.l	#8,d0
	asr.l	#7,d0
	bsr	dsp1_clamp16
	move.w	d0,4(a1)		; C = t sinA
	; B and D carry 1/cosZ
	move.w	dsp_cz,d5
	ext.l	d5
	bne.s	.cz
	moveq	#1,d5
.cz:	move.w	dsp_sa,d0
	ext.l	d0
	muls.l	d4,d0
	divs.l	d5,d0
	neg.l	d0
	bsr	dsp1_clamp16
	move.w	d0,2(a1)		; B = -t sinA / cosZ
	move.w	dsp_ca,d0
	ext.l	d0
	muls.l	d4,d0
	divs.l	d5,d0
	bsr	dsp1_clamp16
	move.w	d0,6(a1)		; D = t cosA / cosZ
	rts
.sky:	clr.l	(a1)
	clr.l	4(a1)
	rts

; $06: X, Y, Z -> H, V, M (screen position; M = Les / depth, 8.8).
dsp1_project:
	move.w	(a0),d0
	ext.l	d0
	sub.l	dsp_ex,d0		; rel x
	move.w	2(a0),d1
	ext.l	d1
	sub.l	dsp_ey,d1		; rel y
	move.w	4(a0),d2
	ext.l	d2
	sub.l	dsp_ez,d2		; rel z
	movem.l	d0-d2,dsp_rel
	; depth w = -(rel . n) = sZ sA x - sZ cA y - cZ z  (Q15)
	move.w	dsp_sz,d3
	move.w	dsp_sa,d4
	mulq15	d3,d4			; sZ sA
	muls.l	d4,d0
	move.w	dsp_ca,d4
	mulq15	d3,d4			; sZ cA
	muls.l	d4,d1
	sub.l	d1,d0
	move.w	dsp_cz,d4
	ext.l	d4
	muls.l	d4,d2
	sub.l	d2,d0
	asr.l	#8,d0
	asr.l	#7,d0			; w
	ble	.behind
	move.l	d0,d5
	; H = Les (rel . r) / w, r = (cA, sA, 0)
	movem.l	dsp_rel,d0-d1
	move.w	dsp_ca,d4
	ext.l	d4
	muls.l	d4,d0
	move.w	dsp_sa,d4
	ext.l	d4
	muls.l	d4,d1
	add.l	d1,d0
	asr.l	#8,d0
	asr.l	#7,d0
	move.w	dsp_les,d4
	ext.l	d4
	muls.l	d4,d0
	divs.l	d5,d0
	bsr	dsp1_clamp16
	move.w	d0,(a1)
	; V = -Les (rel . u) / w, u = (sA cZ, -cA cZ, sZ)
	movem.l	dsp_rel,d0-d2
	move.w	dsp_cz,d3
	move.w	dsp_sa,d4
	mulq15	d3,d4
	muls.l	d4,d0
	move.w	dsp_ca,d4
	mulq15	d3,d4
	muls.l	d4,d1
	sub.l	d1,d0
	move.w	dsp_sz,d4
	ext.l	d4
	muls.l	d4,d2
	add.l	d2,d0
	asr.l	#8,d0
	asr.l	#7,d0
	neg.l	d0
	move.w	dsp_les,d4
	ext.l	d4
	muls.l	d4,d0
	divs.l	d5,d0
	bsr	dsp1_clamp16
	move.w	d0,2(a1)
	; M = Les * 256 / w
	move.w	dsp_les,d0
	ext.l	d0
	asl.l	#8,d0
	divs.l	d5,d0
	bsr	dsp1_clamp16
	move.w	d0,4(a1)
	rts
.behind:
	clr.l	(a1)
	clr.w	4(a1)
	rts

	section	data,data

; Per command $00-$3F: parameter words in the low nibble, result words in
; the high nibble (raster: one line). Unused commands keep their counts so
; the protocol stays in step; their results read as zero.
dsp_counts:
	dc.b	$12,$04,$47,$33,$22,$00,$33,$11	; 00-07
	dc.b	$23,$00,$41,$13,$23,$33,$22,$11	; 08-0F
	dc.b	$22,$04,$47,$33,$36,$00,$33,$00	; 10-17
	dc.b	$14,$00,$41,$13,$36,$33,$22,$01	; 18-1F
	dc.b	$12,$04,$47,$33,$22,$00,$33,$00	; 20-27
	dc.b	$13,$00,$41,$13,$23,$33,$22,$11	; 28-2F
	dc.b	$22,$00,$47,$00,$36,$00,$33,$00	; 30-37
	dc.b	$14,$00,$41,$00,$36,$33,$22,$01	; 38-3F

	section	bss,bss

dsp_state:	ds.b	1
dsp_cmd:	ds.b	1
	even
dsp_inpos:	ds.w	1
dsp_inbytes:	ds.w	1
dsp_outpos:	ds.w	1
dsp_outbytes:	ds.w	1
dsp_vs:		ds.w	1
dsp_sa:		ds.w	1
dsp_ca:		ds.w	1
dsp_sz:		ds.w	1
dsp_cz:		ds.w	1
dsp_les:	ds.w	1
dsp_ex:		ds.l	1
dsp_ey:		ds.l	1
dsp_ez:		ds.l	1
dsp_rel:	ds.l	3
dsp_in:		ds.w	8
dsp_out:	ds.w	8
