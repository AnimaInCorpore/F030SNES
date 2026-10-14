; Chrono Trigger's LZ decompressor, Gfx_DecompressEntry ($C30557-$C308B2),
; as 68030 code (included by smkx_rt.s). The translation calls
; rt_native_lz at $C30557 (tools/smkx/natives.txt) with two arguments:
;
;	pea	<stub>			; the context's RTL ($C308B2)
;	pea	<bits>.w		; M, X and D of the caller's P, as PHP sees them
;	jsr	rt_native_lz
;
; Handled, it leaves every effect the 65816 routine leaves and continues at
; the stub; declined, it changes nothing and returns to the translated
; routine. Every step cites the listing (third_party/ct_disassembly,
; bank_C3.asm). The four 65816 variants differ only in the offset width
; (12 bits: $C305E5 and $C306A8; 11 bits: $C3076B and $C3082D) and the MVN
; bank ($7E: $C305E5 and $C3076B; $7F: $C306A8 and $C3082D).
;
; Declined, so the translation runs it:
; - a source bank whose 256 pages and the next bank's first page are not one
;   linear host block (rt_pages_r): I/O, or a mapping that is not linear;
; - output starting in bank $7E at or below $0316, where it could
;   overwrite the routine's own direct page ($0300-$0316), which the 65816
;   reads back from memory and this code keeps in registers.
;
; Registers inside: d1 X, d2 Y, d4 the segment end ($09) (upper words 0),
; d3 the WRAM port address (rt_wmadd), d5 $0B, d6 $0D, a1 the source bank's
; host address, a2 the MVN bank's host address, a4 the length shift
; (lz_mask the offset mask), a5 WRAM, a6 the 65816 S.

	section	text,code
	xref	rt_ccr2p
	xdef	rt_native_lz

LZ_D0	equ	0			; the saved registers (movem d0-d7/a1-a6)
LZ_D1	equ	4
LZ_D2	equ	8
LZ_D3	equ	12
LZ_CCR	equ	56			; the caller's CCR
LZ_BITS	equ	62			; the long pea <bits>.w pushed; its low byte at +3

; one byte through WMDATA ($2180): as rt_io_write's .wmdata
LZPUT	macro
	move.l	d3,d7
	andi.l	#$1ffff,d7
	move.b	\1,(a5,d7.l)
	addq.l	#1,d3
	endm

rt_native_lz:
	move.w	ccr,-(sp)
	movem.l	d0-d7/a1-a6,-(sp)
	ifeq	CT_LZ
	bra	.decline
	endif
	; the source bank ($0302: PHA/PLB at $C30578) mapped linearly?
	moveq	#0,d7
	move.b	$302(a5),d7
	lea	lz_lin,a0
	move.b	(a0,d7.w),d6
	bne.s	.known
	bsr	lz_check
	move.b	(a0,d7.w),d6
.known:	cmpi.b	#1,d6
	bne	.decline
	move.l	d7,d6
	lsl.l	#8,d6
	movea.l	(rt_pages_r,d6.l*4),a1	; host DB:$0000
	; output that could reach the direct page: the translation's case
	btst	#0,$305(a5)
	bne.s	.go
	move.b	$304(a5),d6
	lsl.w	#8,d6
	move.b	$303(a5),d6
	cmpi.w	#$0316,d6
	bls	.decline
.go:
	ifne	CT_LZ_CHECK
	bsr	lzc_save		; the state before, to undo the native run
	endif
	; $C30557-$C3055E: PHB, PHD, PHP, (REP #$30) PHA, PHX, PHY: the bytes
	; below S stay in WRAM
	move.l	LZ_D3(sp),d7
	swap	d7
	move.b	d7,(a6)			; PHB: DB
	move.l	a2,d7
	sub.l	a5,d7
	move.b	d7,-2(a6)		; PHD: D high at S, low below
	lsr.w	#8,d7
	move.b	d7,-1(a6)
	move.w	LZ_CCR(sp),d7		; PHP: as the translation's PHP (emit.py)
	move.b	(rt_ccr2p,d7.w),d6
	or.b	LZ_BITS+3(sp),d6
	andi.b	#$BF,d6
	move.b	rt_vf,d7
	andi.b	#$40,d7
	or.b	d7,d6
	move.b	d6,-3(a6)
	ifne	CT_LZ_CHECK
	move.l	a6,d7			; where P went, for rt_mark_lzchk
	subq.l	#3,d7
	sub.l	a5,d7
	move.l	d7,lzc_poff
	endif
	move.w	LZ_D0+2(sp),d7		; PHA: C
	move.b	d7,-5(a6)
	lsr.w	#8,d7
	move.b	d7,-4(a6)
	move.w	LZ_D1+2(sp),d7		; PHX
	move.b	d7,-7(a6)
	lsr.w	#8,d7
	move.b	d7,-6(a6)
	move.w	LZ_D2+2(sp),d7		; PHY
	move.b	d7,-9(a6)
	lsr.w	#8,d7
	move.b	d7,-8(a6)
	; $C3055F-$C3056B: D = $0300; X = [$00] + 2; $09 = X + the word at [$00]
	moveq	#0,d1
	move.b	$301(a5),d1
	lsl.w	#8,d1
	move.b	$300(a5),d1
	moveq	#0,d7
	move.b	1(a1,d1.l),d7
	lsl.w	#8,d7
	move.b	(a1,d1.l),d7
	addq.w	#2,d1			; INC A, INC A, TAX
	moveq	#0,d4
	move.w	d1,d4
	add.w	d7,d4			; CLC, ADC [$00]
	; $C3056D-$C30573: Y = $03; WMADDL/WMADDH ($2181/2) = Y
	moveq	#0,d2
	move.b	$304(a5),d2
	lsl.w	#8,d2
	move.b	$303(a5),d2
	move.w	d2,rt_wmadd+2
	clr.b	$310(a5)		; $C30576: STZ $10
	; $C30582/$C3058E: WMADDH = $05 (the runtime keeps bit 0); its bit 0
	; picks the $7F variants (LSR A, BCC)
	move.b	$305(a5),d7
	andi.b	#1,d7
	move.b	d7,rt_wmadd+1
	lea	(a5),a2			; MVN $7E,$7E
	beq.s	.b7e
	adda.l	#$10000,a2		; MVN $7F,$7F
.b7e:	move.l	rt_wmadd,d3
	; $C3057C-$C30580: the first tail byte, (DB:$09) AND #$C0: zero picks
	; the 12-bit variants
	move.w	#$0fff,lz_mask		; AND.W #$0FFF, LSR A x4 ($C30624)
	movea.w	#4,a4
	move.b	(a1,d4.l),d7
	andi.b	#$c0,d7
	beq.s	.w12
	move.w	#$07ff,lz_mask		; AND.W #$07FF, LSR A x3 ($C307A9)
	movea.w	#3,a4
.w12:	move.b	$30d(a5),d6		; $0D as it is, if no flag byte comes
.eight:	moveq	#8,d5			; $C305E5: LDA #$08, STA $0B

; $C305E9: CPX $09, BEQ: the segment's end
.top:	cmp.w	d4,d1
	beq	.seg
	move.b	(a1,d1.l),d0		; LDA $0000,X
	beq.s	.lit8			; BEQ: eight literals
	addq.w	#1,d1			; INX
	move.b	d0,d6
	lsr.b	#1,d6			; LSR A, STA $0D
	bcs.s	.ref			; BCS: a back-reference
.lit:	LZPUT	(a1,d1.l)		; LDA $0000,X, STA $2180
	addq.w	#1,d2			; INY
	addq.w	#1,d1			; INX
; $C30601: DEC $0B, BEQ (next flag byte); LSR $0D, BCS
.bit:	subq.b	#1,d5
	beq.s	.eight
	lsr.b	#1,d6
	bcc.s	.lit

; $C30614-$C30642: a back-reference, two bytes: length (b1 >> 4 or 3) + 2
; in $0F, offset b1:b0 AND the mask; MVN copies $0F + 1 bytes from Y -
; offset to Y in the output bank; WMADDL/WMADDH = Y; X = the old X + 2
.ref:	moveq	#0,d0
	move.b	1(a1,d1.l),d0
	move.w	a4,d7
	lsr.b	d7,d0
	addq.b	#2,d0			; INC A, INC A
	move.b	d0,$30f(a5)		; STA $0F
	moveq	#0,d7
	move.b	1(a1,d1.l),d7
	lsl.w	#8,d7
	move.b	(a1,d1.l),d7
	and.w	lz_mask,d7		; AND.W #mask
	neg.w	d7
	add.w	d2,d7			; TYA, SEC, SBC $15
	move.b	d1,$315(a5)		; STX $15
	move.w	d1,-(sp)
	lsr.w	#8,d1
	move.b	d1,$316(a5)
	moveq	#0,d1
	move.w	d7,d1			; TAX: the source
	move.b	$302(a5),-10(a6)	; PHB: DB (= $0302) below the six pushes
	; MVN: A = $0F:$10 ($10 = 0): A + 1 bytes, X and Y wrap in the bank
.mvn:	move.b	(a2,d1.l),(a2,d2.l)
	addq.w	#1,d1
	addq.w	#1,d2
	dbra	d0,.mvn
	move.w	d2,d3			; TYA, STA $2181 (16 bits)
	move.w	(sp)+,d1		; LDX $15, INX, INX
	addq.w	#2,d1
	bra.s	.bit

; $C3059D-$C305E3: a flag byte of 0: eight literals, X += 9, Y += 8 (the
; ADC's carry from X + 9 goes into Y: REP #$21 clears it only once)
.lit8:	LZPUT	1(a1,d1.l)
	LZPUT	2(a1,d1.l)
	LZPUT	3(a1,d1.l)
	LZPUT	4(a1,d1.l)
	LZPUT	5(a1,d1.l)
	LZPUT	6(a1,d1.l)
	LZPUT	7(a1,d1.l)
	LZPUT	8(a1,d1.l)
	moveq	#0,d7
	addi.w	#9,d1
	addx.w	d7,d2			; the carry of X + 9
	addq.w	#8,d2
	bra	.top

; $C30644-$C3065B: the tail byte AND #$3F: 0 ends; else $0B = it, $09 =
; the next word + $00 (REP #$21: carry clear), X += 3
.seg:	move.b	(a1,d1.l),d7
	andi.b	#$3f,d7
	beq.s	.done
	move.b	d7,d5
	moveq	#0,d7
	move.b	2(a1,d1.l),d7
	lsl.w	#8,d7
	move.b	1(a1,d1.l),d7
	move.b	$301(a5),d0
	lsl.w	#8,d0
	move.b	$300(a5),d0
	add.w	d0,d7
	move.w	d7,d4
	addq.w	#3,d1
	bra	.top

; $C308A4-$C308B2: $06 = Y - $03; PLY, PLX, PLA, PLP, PLD, PLB, RTL (the
; stub). The direct page as the 65816 leaves it: $09, $0B, $0D.
.done:	move.b	$304(a5),d0
	lsl.w	#8,d0
	move.b	$303(a5),d0
	move.w	d2,d7
	sub.w	d0,d7
	move.b	d7,$306(a5)
	lsr.w	#8,d7
	move.b	d7,$307(a5)
	move.b	d4,$309(a5)
	move.w	d4,d7
	lsr.w	#8,d7
	move.b	d7,$30a(a5)
	move.b	d5,$30b(a5)
	move.b	d6,$30d(a5)
	move.l	d3,rt_wmadd
	addq.l	#1,lz_calls
	ifne	CT_LZ_CHECK
	bsr	lzc_keep		; keep the result, undo it: the translation
	bra.s	.decline2		; runs, rt_mark_lzchk compares at its RTL
	endif
	movem.l	(sp)+,d0-d7/a1-a6
	move.w	(sp)+,ccr
	addq.l	#8,sp			; our return address and the bits
	movea.l	(sp)+,a0		; the stub: RTL
	jmp	(a0)

.decline:
	addq.l	#1,lz_declined
.decline2:
	movem.l	(sp)+,d0-d7/a1-a6
	move.w	(sp)+,ccr
	movea.l	(sp)+,a0
	addq.l	#8,sp			; the bits and the stub
	jmp	(a0)

; lz_lin[bank] = 1 when bank's 256 pages and the next bank's page 0 are one
; linear host block in rt_pages_r, else 2. d7 = bank; keeps the registers.
lz_check:
	movem.l	d0-d2/a0-a1,-(sp)
	lea	lz_lin,a0
	move.b	#2,(a0,d7.w)
	cmpi.w	#$ff,d7
	beq	.x
	move.l	d7,d0
	lsl.l	#8,d0
	lea	(rt_pages_r,d0.l*4),a1
	move.l	(a1),d1			; the bank's host base
	beq	.x
	move.w	#256,d0			; pages 0-255 and the next bank's page 0
.pg:	cmp.l	(a1)+,d1
	bne.s	.x
	addi.l	#256,d1
	dbra	d0,.pg
	move.b	#1,(a0,d7.w)
.x:	movem.l	(sp)+,d0-d2/a0-a1
	rts

; Check builds (CT_LZ_CHECK): lzc_save keeps WRAM and the port address
; before the native run, lzc_keep keeps the result and restores the state
; before; rt_mark_lzchk (marks.txt: $C308B2, the translated routine's
; RTL) compares the translation's result with the native one.
	ifne	CT_LZ_CHECK
lzc_save:
	move.w	sr,lzc_sr		; no NMI or IRQ inside the native pass: its
	ori.w	#$0700,sr		; result is the routine's alone (lzc_keep)
	movem.l	d0/a0-a1,-(sp)
	lea	smkx_wram,a0
	lea	lzc_a,a1
	move.w	#$20000/16-1,d0
.c:	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	move.l	(a0)+,(a1)+
	dbra	d0,.c
	move.l	rt_wmadd,lzc_wa
	move.l	rt_vblcount,lzc_vbl0
	move.l	rt_nmis,lzc_nmi0
	movem.l	(sp)+,d0/a0-a1
	rts
lzc_keep:
	movem.l	d0/a0-a2,-(sp)
	lea	smkx_wram,a0
	lea	lzc_b,a1
	lea	lzc_a,a2
	move.w	#$20000/4-1,d0
.c:	move.l	(a0),(a1)+
	move.l	(a2)+,(a0)+
	dbra	d0,.c
	move.l	rt_wmadd,lzc_wb
	move.l	lzc_wa,rt_wmadd
	move.l	rt_nmis,d0		; the game's interrupts so far
	add.l	rt_irqs,d0
	move.l	d0,lzc_ints
	move.l	rt_vblcount,lzc_vbl
	move.l	rt_nmis,lzc_nmi1
	move.l	$300(a5),lzc_par	; $0300-$0303: source, bank, dest low
	move.b	$304(a5),lzc_par+4
	move.b	$305(a5),lzc_par+5
	move.b	rt_in_frame,lzc_par+6
	st	lzc_pending
	movem.l	(sp)+,d0/a0-a2
	move.w	lzc_sr,sr
	rts
	endif

	xdef	rt_mark_lzchk
rt_mark_lzchk:
	ifne	CT_LZ_CHECK
	move.w	sr,-(sp)
	tst.b	lzc_pending
	beq	.x
	sf	lzc_pending
	movem.l	d0-d2/d7/a0-a1,-(sp)
	; an NMI or IRQ during the translated pass (a forced game frame,
	; SYNC_MAX lag VBLs, or an IRQ) changed WRAM besides the routine: such a
	; call cannot be compared (lzc_intr counts them)
	move.l	rt_nmis,d7
	add.l	rt_irqs,d7
	cmp.l	lzc_ints,d7
	beq	.cmp
	addq.l	#1,lzc_intr
	bra	.ok
.cmp:	addq.l	#1,lzc_checked
	moveq	#0,d2			; differing bytes in this call
	lea	smkx_wram,a0
	lea	lzc_b,a1
	move.l	#$20000-1,d0
.c:	cmpm.b	(a0)+,(a1)+
	bne	.bad
.n:	subq.l	#1,d0
	bpl	.c
	move.l	rt_wmadd,d7
	cmp.l	lzc_wb,d7
	beq	.pok
	addq.l	#1,d2
	lea	msg_lzport,a0		; the port address: translated, native
	bsr	rt_print
	bsr	rt_print_hex8
	lea	msg_sp,a0
	bsr	rt_print
	move.l	lzc_wb,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print
.pok:	tst.l	d2
	beq	.ok
	addq.l	#1,lzc_bad
	bra	.ok
; a differing byte: its WRAM offset, the translated value, the native one.
; The pushed P ($C30559 PHP) may differ in C alone: the translation keeps
; C dead there (its PHD at $C30558 takes X from its own SUB.L; the PLP's C
; is not read after the RTL), so it pushes whatever the host X flag held,
; where this code pushes the caller's C (lzc_pflags counts these).
.bad:	move.l	a0,d7
	subq.l	#1,d7
	subi.l	#smkx_wram,d7
	cmp.l	lzc_poff,d7
	bne	.real
	move.b	-1(a0),d7
	move.b	-1(a1),d1
	eor.b	d1,d7
	andi.b	#$fe,d7			; every bit but C must agree
	bne	.real
	addq.l	#1,lzc_pflags
	bra	.n
.real:	addq.l	#1,d2
	cmpi.l	#1,d2
	bne	.nohd
	movem.l	a0-a1,-(sp)		; the call: VBLs during the translated pass,
	lea	msg_lzhead,a0		; the parameters $0300-$0305, rt_in_frame
	bsr	rt_print
	move.l	rt_vblcount,d7
	sub.l	lzc_vbl,d7
	bsr	rt_print_hex8
	lea	msg_sp,a0
	bsr	rt_print
	move.l	lzc_par,d7
	bsr	rt_print_hex8
	lea	msg_sp,a0
	bsr	rt_print
	move.l	lzc_par+4,d7
	bsr	rt_print_hex8
	lea	msg_sp,a0
	bsr	rt_print
	move.l	lzc_vbl,d7		; VBLs and NMIs during the native pass
	sub.l	lzc_vbl0,d7
	bsr	rt_print_hex8
	lea	msg_sp,a0
	bsr	rt_print
	move.l	lzc_nmi1,d7
	sub.l	lzc_nmi0,d7
	bsr	rt_print_hex8
	lea	msg_sp,a0
	bsr	rt_print
	move.l	rt_nmis,d7		; NMIs during the translated pass
	sub.l	lzc_nmi1,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print
	movem.l	(sp)+,a0-a1
.nohd:	cmpi.l	#6,d2
	bhi	.n
	movem.l	a0-a1,-(sp)
	move.l	a0,d7
	subq.l	#1,d7
	subi.l	#smkx_wram,d7
	lea	msg_lzbad,a0
	bsr	rt_print
	bsr	rt_print_hex8
	lea	msg_sp,a0
	bsr	rt_print
	movea.l	(sp),a0
	moveq	#0,d7
	move.b	-1(a0),d7
	bsr	rt_print_hex8
	lea	msg_sp,a0
	bsr	rt_print
	movea.l	4(sp),a1
	moveq	#0,d7
	move.b	-1(a1),d7
	bsr	rt_print_hex8
	lea	msg_lzcall,a0
	bsr	rt_print
	move.l	lzc_checked,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print
	movem.l	(sp)+,a0-a1
	bra	.n
.ok:	move.l	lzc_checked,d7		; a summary every 32 calls
	add.l	lzc_intr,d7
	andi.w	#31,d7
	bne	.q
	lea	msg_lzsum,a0
	bsr	rt_print
	move.l	lzc_checked,d7
	bsr	rt_print_hex8
	lea	msg_lzsum2,a0
	bsr	rt_print
	move.l	lzc_bad,d7
	bsr	rt_print_hex8
	lea	msg_lzsum3,a0
	bsr	rt_print
	move.l	lzc_pflags,d7
	bsr	rt_print_hex8
	lea	msg_lzsum4,a0
	bsr	rt_print
	move.l	lzc_intr,d7
	bsr	rt_print_hex8
	lea	msg_nl,a0
	bsr	rt_print
.q:	movem.l	(sp)+,d0-d2/d7/a0-a1
.x:	move.w	(sp)+,sr
	endif
	rts

	ifne	CT_LZ_CHECK
	section	data,data
msg_lzbad:	dc.b	"SMKX lz diff +",0
msg_lzcall:	dc.b	" in call $",0
msg_lzport:	dc.b	"SMKX lz port differs ",0
msg_lzsum:	dc.b	"SMKX lz checked $",0
msg_lzsum2:	dc.b	" differing $",0
msg_lzsum3:	dc.b	" dead P flags only $",0
msg_lzsum4:	dc.b	" interrupted $",0
msg_lzhead:	dc.b	"SMKX lz differing call: VBLs, $0300.., in_frame ",0
	even
	endif

	section	bss,bss
lz_lin:		ds.b	256
	even
lz_mask:	ds.w	1
	xdef	lz_calls,lz_declined
lz_calls:	ds.l	1
lz_declined:	ds.l	1
	ifne	CT_LZ_CHECK
	xdef	lzc_checked,lzc_bad
lzc_pending:	ds.b	1
	even
lzc_checked:	ds.l	1
lzc_bad:	ds.l	1
lzc_shown:	ds.l	1
lzc_poff:	ds.l	1
lzc_pflags:	ds.l	1
lzc_ints:	ds.l	1
lzc_intr:	ds.l	1
lzc_vbl:	ds.l	1
lzc_vbl0:	ds.l	1
lzc_sr:		ds.w	1
	even
lzc_nmi0:	ds.l	1
lzc_nmi1:	ds.l	1
lzc_par:	ds.l	2
lzc_wa:		ds.l	1
lzc_wb:		ds.l	1
lzc_a:		ds.b	$20000
lzc_b:		ds.b	$20000
	endif
	section	text,code
