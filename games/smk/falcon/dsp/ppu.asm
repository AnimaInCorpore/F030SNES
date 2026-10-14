; SMK's PPU on the Falcon DSP: the whole VRAM lives here and the DSP will
; compose every line (analysis/translator.md, "The DSP renderer").
; Milestone a: VRAM storage, the two-stage program load, read-back.
; PPUTEST.TOS (src/pputest.c) checks it word for word.
;
; Memory
;   X:$0000-$3FFF  VRAM words $0000-$3FFF as lo << 14 | hi (lo = VRAM byte
;                  2i, hi = byte 2i+1) in bits 21-14 and 7-0: the Mode 7
;                  format, tile * 64 << 8 | texel. Bits 13-8 and 23-22 (the
;                  "lanes") keep VRAM words $4000-$5FFF: word $4000 + j in
;                  X:2j and X:2j+1 (xl_read has the bit order)
;   Y:$0000-$03FF  GT0..GT3: the 2-bpp pixel tables (see GT)
;   Y:$0400-$19AD  VRAM words $6000-$7FFF, each kept (like $4000-$5FFF)
;                  with bits 7-4 and 11-8 exchanged (swap): $6AAA-$7FFF
;                  ("fast") in bits 15-0 of Y:$0400 on, one word each;
;                  $6000-$6AA9 ("slow") as two bytes in bits 23-16 of
;                  Y:$0400 + 2m and + 2m + 1
;   Y:$19B0-$26AF  Mode 7 tables TVM TVF TUM, V7
;   Y:$26B0-$2DDA  program, second part (EXT2)
;   Y:$2CA0-$2D9F  HIST: samples per CGRAM index this frame (sent at END)
;   Y:$2DA0-$2DC8  FRAME's register shadow (SH0, SHN fields)
;   Y:$2DDB-$2DEB  the line's samples (SMP, SMPP)
;   Y:$2DBF-$2EAA  the overlays' images: internal P from rn_cells to vw
;                  holds the BG cell code or the Mode 7 sampling loops
;   Y:$2EAB-$2EFF  scalars, the line's paint order and layer descriptors
;   Y:$2F00-$32FF  colour tables PL0..PL3: CGRAM index -> bit p of its
;                  palette slot, at bit 8 (as in the Mode 7 kernel)
;   Y:$3540-$357F  T0V (in LB2, which Mode 7 lines leave free): tile 0's
;                  64 texels through V7, for TILE0 segments (T0OK)
;   Y:$3300-$373F  layer buffers LB0..LB3, 272 words each: pixel x at
;                  LBn + 8 + x (8 words of margin both sides for the cells
;                  that stick out); negative = transparent, else rank << 16
;                  | PL0 + CGRAM index, rank = the (layer, priority) pair's
;                  place in the paint order (1 = backmost): the frontmost
;                  pixel is the largest value, and its low 16 bits address
;                  its colour in PL0
;   Y:$3740-$3751  compose: the 16 winners of a group (PL0 addresses)
;   Y:$3752-$37A3  the line's layer descriptors, base table, sprite list
;   Y:$37A4-$38E3  row caches (RC)
;   Y:$38E4-$3D89  = P:$38E4-$3D89 (on the Falcon P:$0000-$3FFF is Y and
;                  P:$4000-$7FFF is X, Hatari dsp_cpu.c): program beyond the
;                  boot image, loaded with LOAD
;   Y:$3D8A-$3DEF  the line's slivers (SLV)
;   Y:$3DF0-$3EFF  the sprites' layer buffer (LBS)
;   Y:$3F00-$3FFF  the frame's sprites (OBJL)
;   P:$0000-$01FF  the boot image (Dsp_ExecBoot)
;
; Host commands (host port, 24-bit words):
;   4 PING     answers $505055 ("PPU")
;   5 VRAMSET  first VRAM word ($0000-$7FFF), count (1-$8000), then count
;              words: 16 bits, low byte = VRAM byte 2i
;   6 LOAD     space (0 P, 1 X, 2 Y), address, count (at least 1), words
;   7 VRAMGET  first VRAM word, count (at least 1): answers count words
;   8 PEEK     P address (+ $10000: Y address): answers the word there
;   2 COLOUR   1024 words: PL0[256], PL1[256], PL2[256], PL3[256]
;   3 SSION    commands from the SSI from now on (DMA playback, see get_x);
;              answers 1 over the host port
;   9 SSIOFF   (over the SSI) commands from the host port again
;  10 OPTIONS  bit 0: draw lines but do not send them (timing tests),
;              bit 3: leave the sprite slivers in LBS (diagnostics)
;  15 FRAME    a frame as a stream of words op << 16 | payload: 0 END;
;              1 LINE (payload = lines to draw with the state as it is);
;              2 SET (payload = shadow field, SH_*), then the value (the
;              host's resolved Regs field); 3 OAM, then an OAMSET's words.
;              Every line is derived here from the shadow (derive_bg,
;              derive_m7); its 64 plane words go out over the SSI (for
;              each 16-pixel group, planes 0-3); each group's first pixel
;              counts in HIST, which goes out after the last line (256
;              words: the samples per CGRAM index) and is cleared
;              (src/ppu_dsp.c emits the stream)
;  12 OAMSET   n (0-128), then n sprites of two words, in OAM order,
;              those wholly off the screen left out: (X + 64) << 15 | H
;              << 8 | Y, name | attr << 8 | table << 16 | (W / 8 - 1) <<
;              19; table = (the name base + gap) >> 12 & 7

	opt	mex			; macro expansions in the listing
	include	'ioequ.inc'

VPK	equ	$4000		; first VRAM word of the upper half
XLN	equ	$2000		; VPK .. VPK + XLN - 1: in the X words' spare
				; bits (the "lanes", see the memory map)
SLOWN	equ	$0AAA		; then SLOWN "slow" words (two bytes in Y),
FAST0	equ	VPK+XLN+SLOWN	; the rest ($6AAA-$7FFF) fast: one per Y word
PKB	equ	$0400		; Y: the slow and fast words
TVM	equ	$19B0		; Mode 7 tables, 1024 words each, indexed by the
TVF	equ	TVM+$400	; texel row tv / column tu: TVM = (tv >> 3) * 128,
TUM	equ	TVF+$400	; TVF = (tv & 7) * 8, TUM = tu >> 3 (tu & 7 comes
				; from an AND: TUM is a multiple of 8)
V7	equ	TUM+$400	; 256: CGRAM index -> BG1's layer value (0: TRN)
EXT2	equ	V7+$100		; program: the second part (to M7IMG)
SMP	equ	$2DDB		; the line's 16 samples (each group's first winner)
SMPP	equ	SMP+16		;   where the next goes
SH0	equ	$2DA0		; FRAME: the register shadow (SHN fields)
HIST	equ	$2CA0		; 256: the frame's samples per CGRAM index
M7IMG	equ	$2DF5		; the Mode 7 overlay's image (64 words)
BGIMG	equ	$2E35		; the BG overlay's image, saved at the first line
GT	equ	$0000		; GT0..GT3, 256 words each: 2-bpp pixel k (0 =
				; leftmost) of a four-pixel group, index = the
				; plane 1 nibble << 4 | the plane 0 nibble (as the
				; swapped words keep them); the colour (1-3), or
				; TRN when 0 (transparent)
TRN	equ	$800000		; transparent: negative, whatever base is added
; Y scalars ($2EAB-$2EFF)
G_TXM	equ	$2EAB		; tile column mask: 31 or 63
VGEN	equ	$2EAC		; VRAM generation: VRAMSET and LOAD count it up
SRCB	equ	$2EAD		; the drawn layer's row cache
SR4	equ	$2EAE		; ren: the caller's r4
STX	equ	$2EAF		; tile column of the cell
SE	equ	$2EB0		; rn_d4: the cell's base
SROW	equ	$2EB1		; character row word
SLB	equ	$2EB2		; the layer's buffer
SPL	equ	$2EB3		; planes 0/1 while planes 2/3 are fetched
T0OK	equ	$2EB4		; T0V holds tile 0 through this line's V7 (FRAME
				; clears it, so do BG lines and a new V7)
G_ROW	equ	$2EB8		; the layer being drawn: its LDESC words
G_WIDE	equ	$2EB9
G_HOFS	equ	$2EBA
G_CHR	equ	$2EBB
G_FY	equ	$2EBC
G_BPP	equ	$2EBD
G_PAL	equ	$2EBE
ORDN	equ	$2EC0		; paint-order entries
RANK	equ	$2EC1		; 12 words: (layer * 2 + priority) -> rank << 16;
				; OBJ is layer 4, priorities 0-3
CN	equ	$2ECD		; layers drawn this line (sprites included)
CLIST	equ	$2ECE		; 5: their buffers
SENDF	equ	$2ED3		; compose sends the line over the SSI
KEY	equ	$2ED4		; 9: the row cache key being checked
G_LAYER	equ	$2EDD		; layer being drawn
OPTS	equ	$2EDE		; bit 0: lines are not sent (timing tests)
BENCHN	equ	$2EDF		; BENCH: repetitions left
K0	equ	$2EE0		; fy: the row in the character
K1	equ	$2EE1		; 7 - fy: the same, v-flipped
CMUL	equ	$2EE2		; 2^(shift - 1): chr << 3 (2 bpp) or << 4 (4 bpp)
GTOK	equ	$2EE3		; the pixel tables GT are built
SP5	equ	$2EE4		; compose: the fifth buffer's pointer
OBJN	equ	$2EE5		; sprites in OBJL
OCNT	equ	$2EE6		; sprites on this line (at most 32)
OSL	equ	$2EE7		; slivers drawn (at most 34)
OX	equ	$2EE8		; the sprite being drawn: x
ODY	equ	$2EE9		;   its row (after v-flip)
OW2	equ	$2EEA		;   its third word
OBASE	equ	$2EEB		;   rank << 16 | PL0 + CGRAM base
ORB	equ	$2EEC		;   its character row's VRAM word, column 0
OCOL	equ	$2EED		;   the column
OSX	equ	$2EEE		;   the sliver's x
OSN	equ	$2EEF		; slivers to clear after composing
V7KEY	equ	$2EF0		; the rank V7 was built for
MSEG	equ	$2EF1		; Mode 7: segments left
MCNT	equ	$2EF2		;   the segment's pixels
MU	equ	$2EF3		;   u, v, du, dv as sent
MV	equ	$2EF4
MDU	equ	$2EF5
MDV	equ	$2EF6
MKIND	equ	$2EF7
OVL	equ	$2EF8		; the overlay in internal P: 0 BG, 1 Mode 7
MNSEG	equ	$2EF9		; Mode 7: the line's segments
FROW	equ	$2EFA		; FRAME: the row being drawn
DBG	equ	$2EFB		; derive: the layer
DTM	equ	$2EFC		;   the enabled layers
DSC	equ	$2EFD		;   BGnSC
DFY	equ	$2EFE		;   the row in the character
; FRAME's register shadow: resolved values as the host's Regs has them
SH_INI	equ	SH0		; INIDISP
SH_MODE	equ	SH0+1		; BGMODE
SH_SC	equ	SH0+2		; 4: BG1SC..BG4SC
SH_NBA	equ	SH0+6		; 2: BG12NBA, BG34NBA
SH_OFS	equ	SH0+8		; 8: BGnHOFS, BGnVOFS (bg * 2 + v)
SH_M7S	equ	SH0+16		; M7SEL
SH_M7	equ	SH0+17		; 6: M7A..M7D, M7X, M7Y (sign-extended)
SH_M7O	equ	SH0+23		; 2: M7HOFS, M7VOFS
SH_TM	equ	SH0+25		; TM
SH_WIN	equ	SH0+26		; 15: the windows, three spans per layer (BG1-4,
				; OBJ), x << 12 | len, len 0 for none
				; (src/ppu_dsp.c, ppu_win_spans)
SHN	equ	41
LDESC	equ	$3752		; 34: 4 x 8 (EN ROW WIDE HOFS CHR FY BPP PAL),
OBJEN	equ	LDESC+32	; OBJ on this line,
OROW	equ	LDESC+33	; the screen row (for the sprites' rows)
PL0	equ	$2F00
LB0	equ	$3300
LBSIZE	equ	272
MV7	equ	LB0+3*LBSIZE+200	; derive_m7's temporaries (LB3: Mode 7
				; lines leave it free): see derive_m7
T0V	equ	LB0+2*LBSIZE+32	; 64-aligned, in LB2
SEGB	equ	LB0+LBSIZE	; Mode 7: the line's segments, six words each (in LB1,
				; which Mode 7 lines leave free)
IB	equ	$3740		; 18 words: a group's winners (+2 read ahead)
BASET	equ	$3774		; 16: (priority << 3 | palette) -> base
SLIST	equ	$3784		; 32: the sprites on this line (OBJL pointers)
RC	equ	$37A4		; row caches, RCS words per layer: the key (9
RCS	equ	80		; words: ROW, tile column, WIDE, CHR, BPP, PAL,
				; the two ranks, VGEN), then 33 cells of two
				; words: the character's row 0 | h-flip << 16 |
				; v-flip << 17 | generic << 18, and the base
				; (BASET). Row 0: fast 2-bpp characters, the Y
				; address of their row 0; else (generic) the
				; VRAM word CHR + chr << 3 or 4
EXT	equ	$38E4		; program beyond the boot image, up to SLV
SLV	equ	$3D8A		; 34 x 3: the line's slivers, in drawing order:
				; LBS + 8 + x, the VRAM word of planes 0/1 |
				; h-flip << 16, the base
LBS	equ	$3DF0		; the sprites' layer buffer (272 words)
OBJL	equ	$3F00		; 128 x 2: the frame's sprites (OAMSET)

; BGX / SPX: one 4-bpp pixel from the tables at ra (planes 0/1) and rb
; (planes 2/3), +na/nb = 256 per pixel; x1 = the base, y1 = $7FFFFF.
; S = A + 4B masked to 23 bits is the colour; AND sets Z from A1 alone,
; so Z = transparent. BGX writes TRN + base (y0 = TRN: still negative)
; where transparent; SPX keeps what is there (old - base, + base).
BGX	macro	ra,na,rb,nb
	move	y:(rb)+nb,a
	asl	a	y:(ra)+na,x0
	asl	a
	add	x0,a
	and	y1,a	y0,b
	tne	a,b
	add	x1,b
	move	b1,y:(r2)+n2
	endm
SPX	macro	ra,na,rb,nb
	move	y:(rb)+nb,a
	asl	a	y:(ra)+na,x0
	asl	a	y:(r2),b
	add	x0,a
	sub	x1,b
	and	y1,a
	tne	a,b
	add	x1,b
	move	b1,y:(r2)+n2
	endm

; GRP: four pixels from the tables at rr (+nn = 256 per pixel), base x1
GRP	macro	rr,nn
	move	y:(rr)+nn,a
	add	x1,a	y:(rr)+nn,b
	add	x1,b	a1,y:(r2)+n2
	move	y:(rr)+nn,a
	add	x1,a	b1,y:(r2)+n2
	move	y:(rr)+nn,b
	add	x1,b	a1,y:(r2)+n2
	move	b1,y:(r2)+n2
	endm

	org	p:$0000
	jmp	start

; The vectors above $0006 are free: no interrupt is enabled.
	org	p:$0008
; the first passes: 16 winners to IB through r7; y0 = PL0
CVT:	dc	cv0,cv1,cv2,cv3,cv4,cv5,cvd
cv0:	do	#16,cv0e
	move	y0,y:(r7)+
cv0e:	rts
cv1:	do	#16,cv1e
	move	y:(r0)+,a
	tst	a
	tlt	y0,a
	move	a1,y:(r7)+
cv1e:	rts
cv2:	do	#16,cv2e
	move	y:(r0)+,a
	move	y:(r1)+,x0
	cmp	x0,a
	tlt	x0,a
	tst	a
	tlt	y0,a
	move	a1,y:(r7)+
cv2e:	rts
cv3:	do	#16,cv3e
	move	y:(r0)+,a
	move	y:(r1)+,x0
	cmp	x0,a	y:(r2)+,x1
	tlt	x0,a
	cmp	x1,a
	tlt	x1,a
	tst	a
	tlt	y0,a
	move	a1,y:(r7)+
cv3e:	rts
cv4:	do	#16,cv4e
	move	y:(r0)+,a
	move	y:(r1)+,x0
	cmp	x0,a	y:(r2)+,x1
	tlt	x0,a
	cmp	x1,a	y:(r3)+,x0
	tlt	x1,a
	cmp	x0,a
	tlt	x0,a
	tst	a
	tlt	y0,a
	move	a1,y:(r7)+
cv4e:	rts

	if	@cvi(*)>$40
	fail	'the first passes run into start'
	endif

	org	p:$0040
start:
	clr	a
	move	a1,y:OPTS
	move	a1,y:GTOK
	move	a1,y:OVL
	movep	#1,x:m_pbc		; host port on
	movep	#0,x:m_bcr		; no wait states
	movep	#0,x:m_crb		; SSI transmitter off while set up
	movep	#$4000,x:m_cra		; 16-bit words
	movep	#$01C0,x:m_pcc		; PC6/SCK, PC7/SRD, PC8/STD to the SSI
	movep	#$0030,x:m_pcddr	; PC5 GPIO: the frame line (SC2); PC4
					; (SC1): SSI input requests (get_x)
	movep	#$1a00,x:m_crb		; TE, network, synchronous, clock in (RE:
					; SSI input, from SSION on)
	movep	#$0020,x:m_pcd		; SC2 high: record may take words

main:	jsr	get
	move	#CMDT,r0
	move	#>6,x0			; LOAD first: the table below is loaded by it
	cmp	x0,a
	jeq	cmd_load
	move	a1,n0
	move	#>CMDN,x0
	cmp	x0,a
	jge	main			; unknown command
	movem	p:(r0+n0),r1		; the command's routine
	nop
	jmp	(r1)

get:	jset	#13,x:m_crb,get_x	; RE: commands come over the SSI
g_h:	jclr	#m_hrdf,x:m_hsr,g_h
	movep	x:m_hrx,a
	rts

put:	jclr	#m_htde,x:m_hsr,put
	movep	a1,x:m_htx		; a1, not a: moving A would saturate
	rts				; when A2 is not A1's sign (after lsr)

; ---------------------------------------------------------------------------
; LOAD: space, address, count, then the words.
cmd_load:
	move	y:VGEN,a		; VRAM may change: the row caches go
	move	#>1,x0
	add	x0,a
	move	a1,y:VGEN
	jsr	get
	move	a1,x1			; space
	jsr	get
	move	a1,r0			; address
	jsr	get
	move	a1,x0			; count
	move	x1,a
	tst	a
	jeq	ld_p
	move	#>1,y1
	cmp	y1,a
	jeq	ld_x
	do	x0,ld_ye
	jsr	get
	move	a1,y:(r0)+
ld_ye:	jmp	main
ld_x:	do	x0,ld_xe
	jsr	get
	move	a1,x:(r0)+
ld_xe:	jmp	main
ld_p:	do	x0,ld_pe
	jsr	get
	movem	a1,p:(r0)+
ld_pe:	jmp	main

; ---------------------------------------------------------------------------
; The hot paths: in the internal program RAM, whose fetches do not compete
; with the external data accesses.
; rn_cells: the cells of the layer ren (external) has set up: the hot loop
rn_cells:
	move	y:G_HOFS,a
	move	#>7,x0
	and	x0,a			; fine scroll
	move	#>8,b
	move	a1,x0
	sub	x0,b			; the first cell's leftmost pixel: 8 - fine
	move	y:SLB,x0
	add	x0,b
	move	b1,r6
	move	#8,n6
	move	#>256,n4		; GT0 -> GT1 ..
	move	#>256,n5
	move	y:K0,n0			; the row in the character
	move	y:K1,n1			; v-flipped
	move	#>$8000,y0		; >> 8
	move	#>$FF,y1
	do	#33,rn_end
	move	y:(r3)+,x1		; the cached cell: row 0, flips, generic
	; where the pixels go: forwards, or backwards from the far end
	move	r6,r2
	move	#1,n2
	jclr	#16,x1,rn_nh
	move	#7,n2
	nop
	move	(r2)+n2
	move	#>-1,n2			; (a short #-1 would load 255)
rn_nh:	jset	#18,x1,rn_gen
	jset	#17,x1,rn_vf
	move	x1,r0			; (bits 15-0: the Y address)
	move	y:(r3)+,x1		; the base
	move	y:(r0+n0),x0		; the row: left << 8 | right, bits 15-0
rn_px:	mpy	x0,y0,a	
	and	y1,a			; left
	tfr	x0,b	a1,r4
	and	y1,b			; right
	move	b1,r5
	GRP	r4,n4
	GRP	r5,n5
rn_adv:	move	(r6)+n6
rn_end:
	move	y:SR4,r4
	rts
rn_vf:	move	x1,r1
	move	y:(r3)+,x1
	move	y:(r1+n1),x0
	jmp	rn_px
; a generic cell: the row's VRAM word through vw, rn_d2 or rn_d4
rn_gen:	move	x1,a
	move	n0,x0
	jclr	#17,x1,rn_gv
	move	n1,x0
rn_gv:	add	x0,a
	move	#>$7FFF,x0
	and	x0,a
	move	y:(r3)+,x1		; the base
	jsr	(r7)
	move	#>$8000,y0
	move	#>$FF,y1
	jmp	rn_adv

; rn_d2: a generic 2-bpp cell: a1 = its row's VRAM word, x1 its base
rn_d2:	jsr	vwn			; x0 = left << 8 | right
	move	#>$8000,y0
	mpy	x0,y0,a			; a1 = left
	tfr	x0,b	a1,r4
	move	#>$FF,y0
	and	y0,b
	move	b1,r5
	nop
	GRP	r4,n4
	GRP	r5,n5
	rts

; vw: a1 = VRAM word address (0-$7FFF) -> x0 = the word, VRAM byte 2i in
; bits 7-0 and 2i+1 in bits 15-8. Shifts by multiplying: x * 2^(23 - n)
; leaves x >> n in the upper word, x * 2^(n - 1) leaves x << n in the
; lower. Uses a, b, x0, y0, r0.
vw:	move	#>VPK,x0
OVLN	equ	vw-rn_cells	; the overlay region: rn_cells .. vw
	cmp	x0,a
	jlt	vw_x
	jsr	vw_pk
	jmp	swap
; vwn: the same, its middle nibbles exchanged (as the packed half keeps it)
vwn:	move	#>VPK,x0
	cmp	x0,a
	jge	vw_pk
	jsr	vw_x
	jmp	swap
vw_x:	move	a1,r0
	nop
	move	x:(r0),b		; lo << 14 | hi
	move	b1,x0
	move	#>$200,y0
	mpy	x0,y0,a			; a1 = lo (>> 14)
	move	#>$FF,y0
	and	y0,a
	and	y0,b			; b1 = hi
	move	b1,x0
	move	#>$80,y0
	mpy	x0,y0,b			; b0 = hi << 8
	move	b0,x0
	or	x0,a
	move	a1,x0
	rts
vw_pk:	sub	x0,a			; m = address - VPK (x0 = VPK)
	move	#>XLN,x0
	cmp	x0,a
	jlt	xl_read			; in the X lanes
	sub	x0,a
	move	#>SLOWN,x0
	cmp	x0,a
	jge	vw_fast
	lsl	a			; slow: bytes in Y:PKB+2m, +2m+1, bits 23-16
	move	#>PKB,x0
	add	x0,a
	move	a1,r0
	move	#>$8000,y0
	move	y:(r0)+,x0
	mpy	x0,y0,a			; a1 = Y:2m >> 8 (signed)
	move	y:(r0),x0
	move	#>$80,y0
	mpy	x0,y0,b			; b1 = Y:2m+1 >> 16 (signed)
	move	#>$FF00,x0
	and	x0,a
	move	#>$FF,x0
	and	x0,b
	move	b1,x0
	or	x0,a
	move	a1,x0
	rts
vw_fast:
	move	#>SLOWN-PKB,x0		; fast: Y:(PKB + m - SLOWN) bits 15-0
	sub	x0,a
	move	a1,r0
	move	#>$FFFF,x0
	move	y:(r0),a
	and	x0,a
	move	a1,x0
	rts

; compose's group loop (compose, beyond the boot image, sets it up)
cp_loop:
	do	#16,cp_end
	move	#>PL0,y0		; the backdrop
	move	#IB,r7
	jsr	(r6)
	move	y:SMPP,r5		; the group's first winner: a sample
	move	y:IB,x0
	move	x0,y:(r5)+
	move	r5,y:SMPP
	move	#IB,r5
	clr	a
	clr	b
	move	y:(r5)+,r4
	move	y:(r5)+,r7
	do	#8,cp_px
	asl	a	y:(r4)+n4,x1
	asl	b	y:(r4)+n4,x0
	add	x,a	y:(r4)+n4,y1
	move	y:(r4)+n4,y0
	add	y,b	y:(r5)+,r4
	asl	a	y:(r7)+n7,x1
	asl	b	y:(r7)+n7,x0
	add	x,a	y:(r7)+n7,y1
	move	y:(r7)+n7,y0
	add	y,b	y:(r5)+,r7
cp_px:
	jclr	#0,n5,cp_ns
	jsr	ssi_send		; plane 0
	move	a0,a
	jsr	ssi_send		; plane 1
	move	b1,a
	jsr	ssi_send		; plane 2
	move	b0,a
	jsr	ssi_send		; plane 3
cp_ns:	nop
cp_end:
	jmp	cp_smp

; g4ptr: x0 = planes 2/3 (swapped), y:SPL = planes 0/1 -> r0/r1 the left
; group's tables (planes 0/1, 2/3), r4/r5 the right group's, n = 256
g4ptr:	move	#>$8000,y0
	mpy	x0,y0,a			; left (2/3)
	move	#>$FF,y1
	tfr	x0,b	a1,r1
	and	y1,b
	move	b1,r5			; right (2/3)
	move	y:SPL,x0
	mpy	x0,y0,a			; left (0/1)
	tfr	x0,b	a1,r0
	and	y1,b
	move	b1,r4			; right (0/1)
	move	#>256,n0
	move	#>256,n1
	move	#>256,n4
	move	#>256,n5
	rts

; sppx: the eight pixels of a 4-bpp sprite row (g4ptr's pointers) through
; r2 step n2, base x1; transparent pixels keep what is there
sppx:	move	#>$7FFFFF,y1
	do	#4,spa
	SPX	r0,n0,r1,n1
spa:	do	#4,spb
	SPX	r4,n4,r5,n5
spb:	rts


; sl_draw: the line's slivers (SLV, OSN of them) into LBS
sl_draw:
	move	#SLV,r3
	move	y:OSN,x0
	do	x0,sd_end
	move	y:(r3)+,r2		; the buffer
	move	y:(r3)+,x1		; the VRAM word | h-flip << 16
	move	#1,n2
	jclr	#16,x1,sd_nf
	move	#7,n2
	nop
	move	(r2)+n2
	move	#>-1,n2
sd_nf:	move	x1,a
	move	#>$7FFF,x0
	and	x0,a
	move	a1,r7
	jsr	vwn			; planes 0/1
	move	x0,y:SPL
	move	r7,a
	move	#>8,x0
	add	x0,a
	move	#>$7FFF,x0
	and	x0,a
	jsr	vwn			; planes 2/3
	jsr	g4ptr
	move	y:(r3)+,x1		; the base
	jsr	sppx
	nop				; (a DO loop may not end in a JSR)
sd_end:	rts

; one buffer without transparent pixels (Mode 7 without sprites): copied
cvd:	do	#16,cvde
	move	y:(r0)+,x0
	move	x0,y:(r7)+
cvde:	rts

; five buffers: the fifth through r4 (free until the second pass)
cv5:	move	y:SP5,r4
	nop
	do	#16,cv5e
	move	y:(r0)+,a
	move	y:(r1)+,x0
	cmp	x0,a	y:(r2)+,x1
	tlt	x0,a
	cmp	x1,a	y:(r3)+,x0
	tlt	x1,a
	cmp	x0,a	y:(r4)+,x1
	tlt	x0,a
	cmp	x1,a
	tlt	x1,a
	tst	a
	tlt	y0,a
	move	a1,y:(r7)+
cv5e:	move	r4,y:SP5
	rts

	if	@cvi(*)>$200
	fail	'the boot image is larger than the internal program RAM'
	endif

; Beyond the boot image: loaded with LOAD after Dsp_ExecBoot.
	org	p:EXT

; rn_d4: a 4-bpp cell (a1 = its row's VRAM word, x1 its base): planes 0/1
; and 2/3 (eight words on) through the 2-bpp tables, S = GT[0/1] + 4 *
; GT[2/3]; modulo 2^23 that is the colour, 0 when transparent (TRN =
; -2^23 vanishes, 4 * TRN too). Uses n0/n1 (reloaded from K0/K1).
rn_d4:	move	x1,y:SE
	move	a1,y:SROW
	jsr	vwn			; planes 0/1
	move	x0,y:SPL
	move	y:SROW,a
	move	#>8,x0
	add	x0,a
	move	#>$7FFF,x0
	and	x0,a
	jsr	vwn			; planes 2/3
	jsr	g4ptr
	move	y:SE,x1
	jsr	d4px
	move	y:K0,n0
	move	y:K1,n1
	rts



; d4px: the eight pixels of a 4-bpp BG row (g4ptr's pointers) through r2
; step n2, base x1; transparent: TRN
d4px:	move	#>$7FFFFF,y1
	move	#>TRN,y0
	do	#4,d4a
	BGX	r0,n0,r1,n1
d4a:	do	#4,d4b
	BGX	r4,n4,r5,n5
d4b:	rts

; swap: x0 = a 16-bit word -> x0 with bits 7-4 and 11-8 exchanged (its own
; inverse). The packed half keeps VRAM words so: hi[7:4] lo[7:4] hi[3:0]
; lo[3:0], the two four-pixel group indices of a character row. Uses a,
; b, y0.
swap:	move	x0,a
	move	#>$F00F,y0
	and	y0,a
	move	#>8,y0			; bits 7-4 -> 11-8: x0 << 4 (b0)
	mpy	x0,y0,b
	move	b0,b
	move	#>$0F00,y0
	and	y0,b
	move	b1,y0
	or	y0,a
	move	#>$80000,y0		; bits 11-8 -> 7-4: x0 >> 4 (b1)
	mpy	x0,y0,b
	move	#>$00F0,y0
	and	y0,b
	move	b1,y0
	or	y0,a
	move	a1,x0
	rts

; The command table (after LOAD it lives here, beyond the boot image).
CMDT:	dc	main,main,cmd_colour,cmd_ssion,cmd_ping,cmd_vramset,cmd_load,cmd_vramget
	dc	cmd_peek,cmd_ssioff,cmd_opts,main,cmd_oamset,main,main,cmd_frame
CMDN	equ	*-CMDT

; ren: draw the layer in G_* into the buffer at y:SLB, 33 character cells.
; A tile row stays the same for eight lines: its cells (character row
; words, flips, bases) are cached per layer and rebuilt when the key
; changes (a VRAMSET or LOAD changes VGEN).
ren:
	move	r4,y:SR4
	move	y:G_HOFS,a		; the first tile column: hofs >> 3
	rep	#3
	lsr	a
	move	a1,y:STX
	move	#>4,a			; 2 bpp: chr << 3
	move	#rn_d2,r7
	move	y:G_BPP,b
	move	#>4,x0
	cmp	x0,b
	jne	rn_m2
	move	#>8,a			; 4 bpp: chr << 4
	move	#rn_d4,r7
rn_m2:	move	a1,y:CMUL
	move	y:G_FY,a		; the row in the character, straight or
	move	a1,y:K0			; v-flipped
	move	#>7,b
	move	a1,x0
	sub	x0,b
	move	b1,y:K1
	; this layer's cache: RC + layer * 80
	move	y:G_LAYER,a
	rep	#4
	lsl	a
	move	a1,x0			; * 16
	rep	#2
	lsl	a			; * 64
	add	x0,a
	move	#>RC,x0
	add	x0,a
	move	a1,y:SRCB
	; the key
	move	#KEY,r0
	move	y:G_ROW,x0
	move	x0,y:(r0)+
	move	y:STX,x0
	move	x0,y:(r0)+
	move	y:G_WIDE,x0
	move	x0,y:(r0)+
	move	y:G_CHR,x0
	move	x0,y:(r0)+
	move	y:G_BPP,x0
	move	x0,y:(r0)+
	move	y:G_PAL,x0
	move	x0,y:(r0)+
	move	y:G_LAYER,a		; RANK[layer * 2], RANK[layer * 2 + 1]
	lsl	a
	move	#>RANK,x0
	add	x0,a
	move	a1,r1
	nop
	move	y:(r1)+,x0
	move	x0,y:(r0)+
	move	y:(r1),x0
	move	x0,y:(r0)+
	move	y:VGEN,x0
	move	x0,y:(r0)+
	; compare with the cached key, taking the new one over
	move	#KEY,r0
	move	y:SRCB,r2
	clr	b
	do	#9,rk_cmp
	move	y:(r0)+,a
	move	y:(r2),x0
	move	a1,y:(r2)+
	eor	x0,a			; 0 when the same
	move	a1,x0
	or	x0,b
rk_cmp:
	tst	b
	jeq	rc_hit
	jsr	rc_build
rc_hit:	move	y:SRCB,a
	move	#>9,x0
	add	x0,a
	move	a1,r3			; the cells
	jmp	rn_cells

; rc_build: the cells of the layer's row: BASET, then per cell the
; tilemap entry, its character's row 0 word, flips and base
rc_build:
	; BASET[v], v = priority << 3 | palette:
	; rank << 16 | PL0 + (PAL + palette << BPP) & $FF
	move	#BASET,r3
	clr	b
	do	#16,rn_base
	move	b1,a
	move	#>7,x0
	and	x0,a			; palette
	move	y:G_BPP,x0
	rep	x0
	lsl	a
	move	y:G_PAL,x0
	add	x0,a
	move	#>$FF,x0
	and	x0,a
	move	a1,y0			; the CGRAM base
	move	b1,a			; priority: bit 3
	rep	#3
	lsr	a
	move	y:G_LAYER,x0
	move	a1,x1
	move	x0,a
	lsl	a
	add	x1,a			; layer * 2 + priority
	move	#>RANK,x0
	add	x0,a
	move	a1,r0
	nop
	move	y:(r0),a
	or	y0,a
	move	#>PL0,x0
	add	x0,a
	move	a1,y:(r3)+
	move	#>1,x0
	add	x0,b
rn_base:
	move	y:SRCB,a
	move	#>9,x0
	add	x0,a
	move	a1,r3
	move	#BASET,r5
	do	#33,rb_cells
	; the tilemap word: row + (column & 31), + $400 in a wide map's right half
	move	y:STX,a
	move	y:G_TXM,x0
	and	x0,a
	move	a1,b
	move	#>31,x0
	and	x0,b
	move	y:G_ROW,x0
	add	x0,b
	move	#>32,x0
	cmp	x0,a
	jlt	rb_lh
	move	#>$400,x0
	add	x0,b
rb_lh:	move	#>$7FFF,x0
	and	x0,b
	move	b1,a
	jsr	vw
	move	x0,x1			; the entry
	; CHR + (entry & $3FF) << shift, the flips (bits 15, 14) at 17, 16
	move	x1,a
	move	#>$3FF,x0
	and	x0,a
	move	a1,x0
	move	y:CMUL,y0
	mpy	x0,y0,b			; b0 = chr << shift
	move	b0,a
	move	y:G_CHR,x0
	add	x0,a
	move	#>$7FFF,x0		; (CHR goes up to $F000: keep the flips clear)
	and	x0,a
	; fast: a 2-bpp character whose eight rows lie in FAST0..$7FFF
	move	y:G_BPP,b
	move	#>4,x0
	cmp	x0,b
	jeq	rb_gen
	move	#>FAST0,x0
	cmp	x0,a
	jlt	rb_gen
	move	#>$7FF8,x0
	cmp	x0,a
	jgt	rb_gen
	move	#>FAST0-PKB,x0		; its row 0's Y address
	sub	x0,a
	jmp	rb_fl
rb_gen:	move	#>$40000,x0
	or	x0,a
rb_fl:	move	x1,b
	move	#>$C000,x0
	and	x0,b
	lsl	b
	lsl	b
	move	b1,x0
	or	x0,a
	move	a1,y:(r3)+
	; the base: BASET[(entry >> 10) & 15]
	move	#>$2000,y0		; >> 10: the upper word of entry * 2^13 * 2
	mpy	x1,y0,a
	move	#>15,x0
	and	x0,a
	move	a1,n5
	nop
	move	y:(r5+n5),x0
	move	x0,y:(r3)+
	move	y:STX,a
	move	#>1,x0
	add	x0,a
	move	a1,y:STX
rb_cells:
	rts

; compose: per group of 16 pixels, the largest value of the drawn layers
; (their buffers in CLIST) or PL0 (the backdrop, CGRAM 0) when all are
; transparent, then c2p from the colour tables straight into the
; accumulators: A1/A0 collect planes 0/1, B1/B0 planes 2/3 (A = 2A + X,
; each table value at bit 8), sent over the SSI when SENDF is set.
compose:
	move	#>SMP,x0
	move	x0,y:SMPP
	move	y:CN,a
	move	a1,n0
	move	#CVT,r0
	nop
	movem	p:(r0+n0),r6		; the first pass for CN layers
	move	#CLIST,r5
	nop
	move	y:(r5)+,r0		; LB pointers r0..r3 (+8: the margin)
	move	y:(r5)+,r1
	move	y:(r5)+,r2
	move	y:(r5)+,r3
	move	y:(r5)+,x0		; the fifth, + 8
	move	#>8,a
	add	x0,a
	move	a1,y:SP5
	move	y:SENDF,n5
	move	#8,n0
	move	#8,n1
	move	#8,n2
	move	#8,n3
	move	(r0)+n0
	move	(r1)+n1
	move	(r2)+n2
	move	(r3)+n3
	move	#>$100,n4		; PL0 -> PL1 ..
	move	#>$100,n7
	jmp	cp_loop



cmd_opts:
	jsr	get
	move	a1,y:OPTS
	jmp	main

; PEEK: P address -> the word there (diagnostics)
cmd_peek:
	jsr	get
	move	a1,r0
	jset	#16,a1,pk_y
	movem	p:(r0),a1
	jsr	put
	jmp	main
pk_y:	move	y:(r0),a1		; bit 16: Y instead
	jsr	put
	jmp	main

cmd_ping:
	move	#>$505055,a
	jsr	put
	jmp	main

; ---------------------------------------------------------------------------
; VRAMSET: first word, count, then the words.
cmd_vramset:
	jsr	vgen
	jsr	get
	move	a1,x1			; VRAM word address
	jsr	get
	move	a1,x0			; count
	do	x0,vs_end
	jsr	getw
	move	a1,y1
	jsr	vput
	move	x1,a
	move	#>1,x0
	add	x0,a
	move	a1,x1
vs_end:	jmp	main

vgen:	move	y:VGEN,a
	move	#>1,x0
	add	x0,a
	move	a1,y:VGEN
	rts

; vput: y1 = VRAM word (16 bits), x1 = its address. Uses a, b, x0, y0, r1.
vput:
	move	#>VPK,b
	cmp	x1,b
	jle	vp_pk			; VPK <= address: the packed half
	move	#>$2000,y0		; lo << 14: y1 << 14 (b0) & $3FC000
	mpy	y1,y0,b
	move	b0,b
	move	#>$3FC000,x0
	and	x0,b
	move	b1,x0
	move	#>$8000,y0		; hi: y1 >> 8 (b1)
	mpy	y1,y0,b
	or	x0,b
	move	x1,r1
	move	b1,y0
	move	x:(r1),b		; the lanes stay
	move	#>$C03F00,x0
	and	x0,b
	move	y0,x0
	or	x0,b
	move	b1,x:(r1)
	rts
vp_pk:
	move	y1,x0			; kept with its middle nibbles exchanged
	jsr	swap
	move	x0,y1
	move	x1,a
	move	#>VPK,x0
	sub	x0,a			; m
	move	#>XLN,x0
	cmp	x0,a
	jlt	xl_write		; in the X lanes
	sub	x0,a
	move	#>SLOWN,x0
	cmp	x0,a
	jge	vp_fast
	lsl	a			; slow: the high byte to Y:PKB+2m, the
	move	#>PKB,x0		; low byte to Y:PKB+2m+1, bits 23-16
	add	x0,a
	move	a1,r1
	move	#>$80,y0		; y1 << 16: the lower word of y1 * 2^15 * 2
	mpy	y1,y0,b			; ... >> 8 << 16: y1 * 2^7 * 2, lower word
	move	b0,b			; = (y1 << 8) & $FFFFFF: the high byte at 23-16
	move	#>$FF0000,x0
	and	x0,b
	move	y:(r1),a
	move	#>$00FFFF,x0
	and	x0,a
	move	b1,x0
	or	x0,a
	move	a1,y:(r1)+
	move	#>$8000,y0
	mpy	y1,y0,b			; y1 << 16 (lower word)
	move	b0,b
	move	#>$FF0000,x0
	and	x0,b
	move	y:(r1),a
	move	#>$00FFFF,x0
	and	x0,a
	move	b1,x0
	or	x0,a
	move	a1,y:(r1)
	rts
vp_fast:
	move	#>SLOWN-PKB,x0		; fast: bits 15-0 of Y:(PKB + m - SLOWN)
	sub	x0,a
	move	a1,r1
	move	#>$FF0000,x0
	move	y:(r1),a
	and	x0,a
	move	y1,x0
	or	x0,a
	move	a1,y:(r1)
	rts

; ---------------------------------------------------------------------------

; VRAMGET: first word, count: answers the words.
cmd_vramget:
	jsr	get
	move	a1,x1
	jsr	get
	move	a1,x0
	do	x0,vg_end
	jsr	vget
	jsr	put
	move	x1,a
	move	#>1,x0
	add	x0,a
	move	a1,x1
vg_end:	jmp	main

; vget: x1 = VRAM word address -> a1 = the word. Uses a, b, x0, y0, r1.
vget:
	move	#>VPK,b
	cmp	x1,b
	jle	vg_pk
	move	x1,r1
	nop
	move	x:(r1),b
	move	b1,a
	move	#>$FF,x0
	and	x0,a			; hi
	rep	#8
	lsl	a
	move	a1,y0			; hi << 8
	rep	#14
	lsr	b
	and	x0,b			; lo
	move	b1,a
	or	y0,a
	rts
vg_pk:	jsr	vg_pkr
	jsr	swap
	move	x0,a
	rts
vg_pkr:
	move	x1,a
	move	#>VPK,x0
	jmp	vw_pk			; x0 = the kept word
; ---------------------------------------------------------------------------
; COLOUR: the four colour tables.
cmd_colour:
	move	#PL0,r0
	do	#1024,co_end
	jsr	getw
	move	a1,y:(r0)+
co_end:
	jmp	main

; line_out: compose the line (CN, CLIST) and send it unless OPTIONS bit 0
line_out:
	move	y:OPTS,a
	move	#>1,x0
	and	x0,a
	move	#>1,b
	sub	a,b			; 1 unless bit 0 (not sent) is set
	move	b1,y:SENDF
	jsr	compose
	jmp	obj_clear

; the enabled layers into their buffers (descriptor in LDESC, RANK)
bl_draw_all:
	jsr	ovl_bg
	move	#LDESC,r4
	move	#8,n4
	move	#>LB0,a
	move	a1,y:SLB
	clr	a
	move	a1,y:G_LAYER
	move	a1,y:CN
	do	#4,bl_layers
	move	y:(r4),a		; EN
	tst	a
	jeq	bl_next			; not drawn: compose leaves it out
	move	y:CN,a			; CLIST[CN++] = the buffer
	move	#>CLIST,x0
	add	x0,a
	move	a1,r0
	move	#>1,x0
	move	y:SLB,x1
	move	x1,y:(r0)
	move	y:CN,a
	add	x0,a
	move	a1,y:CN
	move	r4,r0
	move	#G_ROW,r1
	move	(r0)+			; skip EN
	do	#7,bl_cp
	move	y:(r0)+,x0
	move	x0,y:(r1)+
bl_cp:
	move	#>31,a			; tile column mask: 32 or 64 columns
	move	y:G_WIDE,b
	tst	b
	jeq	bl_nw
	move	#>63,a
bl_nw:	move	a1,y:G_TXM
	jsr	ren
	move	y:G_LAYER,a		; windows: its hidden spans transparent
	move	y:SLB,x1
	move	#>TRN,y1
	jsr	win_mask
bl_next:
	move	#8,n4			; (ren's cell loop uses n4)
	nop
	move	(r4)+n4
	move	y:SLB,a
	move	#>LBSIZE,x0
	add	x0,a
	move	a1,y:SLB
	move	y:G_LAYER,a
	move	#>1,x0
	add	x0,a
	move	a1,y:G_LAYER
bl_layers:
	move	y:OBJEN,a
	tst	a
	jeq	bl_nobj
	move	y:CN,a			; CLIST[CN++] = LBS
	move	#>CLIST,x0
	add	x0,a
	move	a1,r0
	move	#>LBS,x1
	move	#>1,x0
	move	x1,y:(r0)
	move	y:CN,a
	add	x0,a
	move	a1,y:CN
	jsr	obj_draw
	move	#>4,a			; windows: the sprites' hidden spans
	move	#>LBS,x1
	move	#>TRN,y1
	jsr	win_mask
bl_nobj:
	rts

; gt_init: build GT0..GT3 (once, before the first line)
gt_init:
	move	#HIST,r0		; the histogram starts empty
	clr	a
	rep	#256
	move	a1,y:(r0)+
	move	#GT,r0
	move	#>256,n1
	clr	b			; b1 = index
	do	#256,gi_end
	move	b1,a
	move	#>$0F,x0
	and	x0,a
	move	a1,x1			; plane 0 nibble
	move	b1,a
	rep	#4
	lsr	a
	move	a1,y1			; plane 1 nibble
	move	r0,r1
	do	#4,gi_k			; pixel k: bit 3 - k
	clr	a
	btst	#3,y1
	rol	a
	btst	#3,x1
	rol	a
	tst	a
	jne	gi_nz
	move	#>TRN,a
gi_nz:	move	a1,y:(r1)+n1
	move	x1,a
	lsl	a
	move	a1,x1
	move	y1,a
	lsl	a
	move	a1,y1
gi_k:
	move	(r0)+
	move	#>1,x0
	add	x0,b
gi_end:
	move	#LBS,r0			; the sprite buffer starts transparent
	move	#>TRN,x0
	do	#LBSIZE,gi_lbs
	move	x0,y:(r0)+
gi_lbs:	clr	a
	move	a1,y:OSN
	; the Mode 7 tables
	move	#TVM,r0
	move	#>$400,n0
	clr	b			; t
	move	#>1,y1
	do	#1024,gi_m7
	move	b1,a			; TVM: (t >> 3) * 128
	rep	#3
	lsr	a
	move	a1,x1			; t >> 3
	rep	#7
	lsl	a
	move	a1,y:(r0)+n0
	move	b1,a			; TVF: (t & 7) * 8
	move	#>7,x0
	and	x0,a
	move	a1,y0			; t & 7
	rep	#3
	lsl	a
	move	a1,y:(r0)+n0
	move	x1,y:(r0)		; TUM
	move	(r0)-n0
	move	(r0)-n0
	move	(r0)+
	add	y1,b
gi_m7:
	move	#>-1,a
	move	a1,y:V7KEY
	move	#rn_cells,r0		; the BG overlay as booted
	move	#BGIMG,r1
	do	#OVLN,gi_ov
	movem	p:(r0)+,x0
	move	x0,y:(r1)+
gi_ov:
	move	#>1,a
	move	a1,y:GTOK
	rts

; OAMSET: n, then n sprites of three words (see the header)
cmd_oamset:
	jsr	oam_read
	jmp	main
oam_read:
	jsr	get
	move	a1,y:OBJN
	tst	a
	jeq	os_x
	lsl	a			; 2n words
	move	a1,x0
	move	#OBJL,r0
	do	x0,os_end
	jsr	get
	move	a1,y:(r0)+
os_end:
os_x:	rts

; obj_draw: the sprites on row OROW into LBS: the first 32 of OBJL whose
; rows cover it, then their slivers from the last one back, 34 at most,
; listed in SLV and drawn by sl_draw. An opaque pixel replaces what is
; there: lower OAM indices, drawn later, win.
obj_draw:
	clr	a
	move	a1,y:OCNT
	move	a1,y:OSN
	move	y:OBJN,a
	tst	a
	jeq	od_end
	move	#OBJL,r0
	move	#SLIST,r1
	move	#2,n0
	move	y:OROW,x1
	move	#>$FF,y1
	move	#>$8000,y0
	move	a1,x0
	do	x0,od_rl
	move	y:(r0),x0		; (X + 64) << 15 | H << 8 | Y
	move	x1,a
	sub	x0,a
	and	y1,a			; dy = (row - Y) & $FF in A1 (A2 keeps
	move	a1,b			; the sign: B takes it clean)
	mpy	x0,y0,a			; w0 >> 8 (a0 keeps the low bits)
	move	#>$7F,x0
	and	x0,a			; H
	move	a1,x0
	cmp	x0,b
	jge	od_rn
	move	r0,y:(r1)+
	move	y:OCNT,a
	move	#>1,x0
	add	x0,a
	move	a1,y:OCNT
	move	#>32,x0
	cmp	x0,a
	jlt	od_rn
	enddo				; 32 found
	jmp	od_rl
od_rn:	move	(r0)+n0
od_rl:
	move	y:OCNT,a
	tst	a
	jeq	od_end
	move	#>SLIST-1,x0		; the last one first
	add	x0,a
	move	a1,r3
	move	#SLV,r1			; the sliver list's end
	move	#34,r6			; free places on it
	move	y:OCNT,x0
	do	x0,od_sl
	move	y:(r3)-,r0
	jsr	od_sprite
	tst	a			; a1 = 1: 34 slivers listed
	jeq	od_nx
	enddo
	jmp	od_sl
od_nx:	nop
od_sl:	move	r6,x0			; OSN = 34 - free
	move	#>34,a
	sub	x0,a
	move	a1,y:OSN
	jsr	sl_draw
od_end:	rts

; od_sprite: r0 = the sprite's OBJL words: its slivers onto the list at r1
; -> a1 = 1 when the list is full (34), else 0
od_sprite:
	move	y:(r0)+,x0		; (X + 64) << 15 | H << 8 | Y
	move	y:OROW,b
	sub	x0,b
	move	#>$FF,y1
	and	y1,b			; dy
	move	#>$8000,y0
	mpy	x0,y0,a
	move	#>$7F,y1
	and	y1,a			; H
	move	#>$100,y0		; w0 >> 15: X + 64
	mpy	x0,y0,a	a1,x1
	move	#>$1FF,y1
	and	y1,a
	move	#>64,y1
	sub	y1,a
	move	a1,y:OX
	move	x1,a			; H
	move	y:(r0),x1
	move	x1,y:OW2
	jclr	#15,x1,os_nv		; v-flip: H - 1 - dy
	move	#>1,x0
	sub	x0,a
	move	b1,x0
	sub	x0,a
	move	a1,b
os_nv:	move	b1,y:ODY
	; the base: RANK[8 + priority] + PL0 + 128 + palette * 16
	move	#>$800,y0		; w2 >> 12 & 3: the priority
	mpy	x1,y0,a
	move	#>3,x0
	and	x0,a
	move	#>RANK+8,x0
	add	x0,a
	move	a1,r5
	move	#>$40000,y0		; w2 >> 9 & 7: the palette, * 16 = w2 >> 5 & $70
	mpy	x1,y0,a
	move	#>$70,x0
	and	x0,a
	move	#>PL0+128,x0
	add	x0,a
	move	y:(r5),x0
	add	x0,a
	move	a1,y:OBASE
	; the character row: table << 12 + ((name >> 4) + dy >> 3 & 15) << 8
	; + (dy & 7)
	move	x1,a
	move	#>$F0,x0
	and	x0,a
	move	y:ODY,b
	asl	b			; (dy >> 3) << 4 = dy << 1 & $F0 (dy < 128)
	and	x0,b
	add	b,a
	and	x0,a			; row of names << 4
	move	a1,y0
	move	#>8,x0			; the row's first name * 16: the low word
	mpy	x0,y0,b			; of y0 * 2^3 * 2
	move	#>$80000,y0		; w2 >> 16 & 7: the table, << 12 = w2 >> 4
	mpy	x1,y0,a	b0,x0		; & $7000
	move	#>$7000,y0
	and	y0,a
	add	x0,a
	move	y:ODY,b
	move	#>7,x0
	and	x0,b
	move	b1,x0
	add	x0,a
	move	a1,y:ORB
	; the columns, in registers: r2 = the buffer address (+8), r4 = the
	; name column * 16 (+16, or -16 h-flipped; masked with $F0), x1 = the
	; row word | h-flip << 16
	move	#>$10,y0		; w2 >> 19 & 7: columns - 1
	mpy	x1,y0,b
	move	#>7,x0
	and	x0,b
	move	x1,a			; the name column
	move	#>$0F,x0
	and	x0,a
	move	#>16,y1
	jclr	#14,x1,oc_nf
	move	b1,x0			; h-flipped: from the last one, backwards
	add	x0,a
	move	#>-16,y1
oc_nf:	rep	#4
	lsl	a
	move	a1,r4
	move	y1,n4
	move	#>1,x0			; the columns: the loop count
	add	x0,b
	move	b1,n5
	move	y:ORB,a
	jclr	#14,x1,oc_nf2
	move	#>$10000,x0
	or	x0,a
oc_nf2:	move	a1,x1
	move	y:OX,a
	move	#>LBS+8,x0
	add	x0,a
	move	a1,r2
	move	#8,n2
	move	#>LBS,y0		; below: x <= -8
	move	#>LBS+264,y1		; from here: x >= 256
	move	#>$F0,x0
	do	n5,oc_cols
	move	r2,b
	cmp	y0,b
	jle	oc_sk
	cmp	y1,b
	jge	oc_sk
	move	r6,a
	tst	a
	jeq	oc_full
	move	(r6)-
	move	r2,y:(r1)+
	move	r4,a
	and	x0,a
	add	x1,a
	move	a1,y:(r1)+
	move	y:OBASE,a
	move	a1,y:(r1)+
oc_sk:	move	(r2)+n2
	move	(r4)+n4
oc_cols:
	clr	a
	rts
oc_full:
	enddo
	move	#>1,a
	rts

; obj_clear: the slivers drawn go back to transparent
obj_clear:
	move	y:OPTS,a		; bit 3: keep them (diagnostics)
	move	#>8,x0
	and	x0,a
	jne	oc_end
	move	y:OSN,a
	tst	a
	jeq	oc_end
	move	#SLV,r1
	move	#3,n1
	move	#>TRN,y0
	move	a1,x0
	do	x0,oc_sl
	move	y:(r1)+n1,r2		; (the h-flip above bit 15 drops)
	nop
	rep	#8
	move	y0,y:(r2)+
oc_sl:	clr	a
	move	a1,y:OSN
oc_end:	rts

; cp_smp: after the line's 64 plane words, its 16 samples (CGRAM indices:
; the winners' low bytes, PL0 being 256-aligned) as 8 words, two a word,
; the first in the high byte (the host reads them for its palette)
cp_smp:	jclr	#0,n5,cs_x
	move	#SMP,r0
	move	#>$FF,y0
	move	#>1,y1
	do	#16,cs_e
	move	y:(r0)+,a		; the group's first winner
	and	y0,a			; its CGRAM index (PL0 is 256-aligned)
	move	#>HIST,x0
	add	x0,a
	move	a1,r1
	nop
	move	y:(r1),b
	add	y1,b
	move	b1,y:(r1)
cs_e:
cs_x:	rts

; Wait for the transmitter to be empty, then hand it the word in a1.
ssi_send:
	jclr	#m_tde,x:m_sr,ssi_send
	movep	a1,x:m_tx
	rts

; t0_set: a TILE0 segment's registers for m7tz (x0 = du, x1 = dv): T0V
; built if needed; a1 = 8 v, a0 = u, x1 = 8 dv (v and dv < 2048, so 8 v
; stays below 2^23 over 256 pixels and u never carries into a1), y1 =
; $8000, n0 = $38, n1 = 7, r3 = T0V, r5 = the count
t0_set:	move	y:T0OK,a
	tst	a
	jne	t0_ok
	move	#>1,a
	move	a1,y:T0OK
	move	#0,r0			; tile 0's texels: X:0..63, bits 7-0
	move	#T0V,r3
	move	#V7,r1
	move	#>$FF,y0
	do	#64,t0_be
	move	x:(r0)+,b
	and	y0,b
	move	b1,n1
	nop
	move	y:(r1+n1),a
	move	a1,y:(r3)+
t0_be:
t0_ok:	move	x1,b			; 8 dv
	lsl	b
	lsl	b
	lsl	b
	move	b1,x1
	move	y:MV,b			; 8 v
	lsl	b
	lsl	b
	lsl	b
	move	b1,a
	move	y:MU,b
	move	b1,a0
	move	#>$8000,y1
	move	#$38,n0
	move	#7,n1
	move	#T0V,r3
	move	y:MCNT,r5
	rts

	if	@cvi(*)>SLV
	fail	'the program runs into the sliver list'
	endif

; ---------------------------------------------------------------------------
; The program's second part, in the Y words the X lanes freed.
	org	p:EXT2

; xl_read / xl_write: VRAM word $4000 + j (a1 = j) in the lanes of X:2j
; (e) and X:2j+1 (o): word bits 15-10 = e 13-8, 9-4 = o 13-8, 3-2 = e
; 23-22, 1-0 = o 23-22. xl_read -> x0 = the word; uses a, b, x0, x1, y0,
; r0. xl_write: y1 = the word; uses a, b, x0, y0, r1.
xl_read:
	lsl	a
	move	a1,r0
	nop
	move	x:(r0)+,x0		; e
	move	x0,a
	move	#>$3F00,y0
	and	y0,a
	lsl	a
	lsl	a			; e 13-8 -> 15-10
	move	#>$8,y0			; e >> 20: 23-22 -> 3-2
	mpy	x0,y0,b
	move	#>$C,y0
	and	y0,b
	move	b1,y0
	or	y0,a
	move	x:(r0),x0		; o
	move	#>$2,y0			; o >> 22: 23-22 -> 1-0
	mpy	x0,y0,b
	move	#>$3,y0
	and	y0,b
	move	b1,y0
	or	y0,a
	move	#>$80000,y0		; o 13-8 -> 9-4: o >> 4 (b1), masked
	mpy	x0,y0,b
	move	#>$3F0,y0
	and	y0,b
	move	b1,y0
	or	y0,a
	move	a1,x0
	rts

xl_write:
	lsl	a
	move	a1,r1
	; the shifts as products: v >> n is the high word of v * 2^(23-n) * 2,
	; v << n the low word of v * 2^(n-1) * 2 (as Cho Ren Sha's DSP code
	; shifts its sprite masks)
	move	#>$80000,y0		; e: word 3-2 -> 23-22: y1 << 20 (b0)
	mpy	y1,y0,b
	move	b0,b
	move	#>$C00000,y0
	and	y0,b
	move	#>$200000,y0		; e: word 15-10 -> 13-8: y1 >> 2 (a1)
	mpy	y1,y0,a	b1,x0
	move	#>$3F00,y0
	and	y0,a
	or	x0,a
	move	a1,y0
	move	#>$3FC0FF,x0		; tile and texel stay
	move	x:(r1),a
	and	x0,a
	or	y0,a
	move	a1,x:(r1)+
	move	#>8,y0			; o: word 9-4 -> 13-8: y1 << 4 (a0)
	mpy	y1,y0,a
	move	a0,a
	move	#>$3F00,y0
	and	y0,a
	move	#>$200000,y0		; o: word 1-0 -> 23-22: y1 << 22 (b0)
	mpy	y1,y0,b
	move	b0,y0
	or	y0,a
	move	a1,y0
	move	x:(r1),a
	and	x0,a
	or	y0,a
	move	a1,x:(r1)
	rts

; ---------------------------------------------------------------------------
; m7_render: BG1 from SEGB into LB0, the sprites into LBS; CN, CLIST
m7_render:
	; V7 for BG1's rank: 0 -> TRN, i -> RANK[0] + PL0 + i
	move	y:RANK,a
	move	y:V7KEY,x0
	cmp	x0,a
	jeq	m7_v7
	move	a1,y:V7KEY
	clr	b
	move	b1,y:T0OK
	move	#>PL0,x0
	add	x0,a
	move	#V7,r0
	move	#>1,x0
	move	#>TRN,x1
	move	x1,y:(r0)+
	add	x0,a
	do	#255,m7_v7b
	move	a1,y:(r0)+
	add	x0,a
m7_v7b:
m7_v7:	jsr	m7_check
	move	#>TRN,x0		; V7[0]: transparent with sprites, else
	move	y:OBJEN,a		; the backdrop's colour directly
	tst	a
	jne	m7_z
	move	#>PL0,x0
m7_z:	move	y:V7,a
	cmp	x0,a
	jeq	m7_zs
	clr	a
	move	a1,y:T0OK
m7_zs:	move	x0,y:V7
	jsr	ovl_m7
	move	#LB0+7,r7		; the pending pixel: LB0's margin first
	move	#0,r6
	move	#V7,n6
	move	#SEGB,r4
	move	y:MNSEG,a
	tst	a
	jeq	m7_done
	move	a1,x0
	do	x0,m7_segs
	move	y:(r4)+,a		; kind
	move	a1,y:MKIND
	move	y:(r4)+,a		; count
	move	a1,y:MCNT
	move	y:(r4)+,a
	move	a1,y:MU
	move	y:(r4)+,a
	move	a1,y:MV
	move	y:(r4)+,a
	move	a1,y:MDU
	move	y:(r4)+,a
	move	a1,y:MDV
	jsr	m7_seg
	nop
m7_segs:
m7_done:
	move	#V7,n6			; the last pending pixel
	nop
	move	y:(r6+n6),b
	move	b1,y:(r7)+
	clr	a			; windows: BG1's hidden spans as V7[0]
	move	#>LB0,x1		; (transparent, or the backdrop when LB0
	move	y:V7,y1			; is copied as it is)
	jsr	win_mask
	; the layers: LB0 (copied as it is), and LBS with sprites
	move	#>6,a
	move	a1,y:CN
	move	#>LB0,x0
	move	x0,y:CLIST
	move	y:OBJEN,a
	tst	a
	jeq	m7_no
	move	#>2,a
	move	a1,y:CN
	move	#>LBS,x0
	move	x0,y:CLIST+1
	jsr	obj_draw
	move	#>4,a			; windows: the sprites' hidden spans
	move	#>LBS,x1
	move	#>TRN,y1
	jsr	win_mask
m7_no:	rts

; m7_seg: one segment (MKIND, MCNT, MU, MV, MDU, MDV) into r7
m7_seg:	move	y:MKIND,a
	move	#>3,x0
	cmp	x0,a
	jeq	m7_fill
	; the step x = dv * 2^24 + du (du's sign borrows from dv)
	move	y:MDU,x0
	move	y:MDV,a
	move	x0,b
	tst	b
	jge	m7_dp
	move	#>1,y0
	sub	y0,a
m7_dp:	move	a1,x1
	move	#V7,n6
	move	y:MKIND,a
	move	#>2,y0
	cmp	y0,a
	jeq	m7_t0
	; INSIDE: a1 = v + TVM * 256, a0 = u + TUM * 256
	move	y:MV,a
	move	#>TVM*256,y0
	add	y0,a
	move	y:MU,b
	move	#>TUM*256,y0
	add	y0,b
	move	b1,a0
	move	#>$8000,y1		; >> 8
	move	#>$400,n1
	move	#7,n3
	move	#$3FC0,n7
	move	#$FF,n4
	move	y:MCNT,r5
	jmp	m7ins
m7_t0:	jsr	t0_set			; TILE0
	jmp	m7tz
m7_fill:
	move	y:(r6+n6),b		; the pending pixel
	move	b1,y:(r7)+
	move	y:V7,y0			; transparent (V7[0])
	move	y:MCNT,b
	move	b1,n4
	do	n4,m7_fe
	move	y0,y:(r7)+
m7_fe:	move	(r7)-			; the last one pending
	move	#0,r6
	rts

	if	@cvi(*)>$2EAB
	fail	'the second program part runs into the scalars'
	endif

; ovl_bg / ovl_m7: the overlay a line needs into internal P
ovl_bg:	move	y:OVL,a
	tst	a
	jeq	ov_ok
	clr	a
	move	a1,y:OVL
	move	#BGIMG,r0
	jmp	ov_cp
ovl_m7:	move	y:OVL,a
	tst	a
	jne	ov_ok
	move	#>1,a
	move	a1,y:OVL
	move	#M7IMG,r0
ov_cp:	move	#rn_cells,r1
	do	#OVLN,ov_e
	move	y:(r0)+,x0
	movem	x0,p:(r1)+
ov_e:
ov_ok:	rts

; ---------------------------------------------------------------------------
; FRAME: the host's register stream; every line is derived here.
cmd_frame:
	move	y:GTOK,a
	tst	a
	jne	fr_gt
	jsr	gt_init
fr_gt:	clr	a
	move	a1,y:FROW
	move	a1,y:M7BAD
	move	a1,y:T0OK
fr_next:
	jsr	get
	move	#>$FFFF,x0
	move	a1,b
	and	x0,b			; the payload
	move	#>$10000,x0
	cmp	x0,a
	jlt	fr_end			; 0: END
	move	#>$20000,x0
	cmp	x0,a
	jlt	fr_line
	move	#>$30000,x0
	cmp	x0,a
	jge	fr_oam
	move	b1,a			; SET: the field
	move	#>SH0,x0
	add	x0,a
	move	a1,r0
	jsr	get
	move	a1,y:(r0)
	jmp	fr_next
fr_oam:	jsr	oam_read
	jmp	fr_next

; fr_end: after the last line the frame's histogram goes out (256 words,
; the counts in the SSI's 16 bits), unless lines are not sent; it is
; cleared for the next frame.
fr_end:	move	#HIST,r0
	clr	b
	move	y:OPTS,a
	jset	#0,a1,fe_clr
	do	#256,fe_e
	move	y:(r0),a
	rep	#8
	lsl	a
	jsr	ssi_send
	move	b1,y:(r0)+
fe_e:	jmp	main
fe_clr:	rep	#256
	move	b1,y:(r0)+
	jmp	main
fr_line:
	tst	b
	jeq	fr_next
	move	b1,x0
	do	x0,fr_le
	jsr	fr_draw
	move	y:FROW,a
	move	#>1,x0
	add	x0,a
	move	a1,y:FROW
fr_le:	jmp	fr_next

; fr_draw: the line FROW from the shadow
fr_draw:
	move	y:FROW,a
	move	a1,y:OROW
	clr	a
	move	a1,y:CN
	move	y:SH_INI,a		; forced blank: the backdrop
	jclr	#7,a1,fd_on
	jmp	line_out
fd_on:	move	y:SH_TM,a		; OBJ EN
	rep	#4
	lsr	a
	move	#>1,x0
	and	x0,a
	move	a1,y:OBJEN
	move	y:SH_MODE,a
	move	#>7,x0
	and	x0,a
	cmp	x0,a
	jeq	fd_m7
	clr	a			; (BG lines use LB2: T0V)
	move	a1,y:T0OK
	jsr	derive_bg
	jsr	bl_draw_all
	jmp	line_out
fd_m7:	jsr	derive_m7
	jsr	m7_render
	jmp	line_out

; Paint orders, back to front, as RANK indices (layer * 2 + priority; OBJ
; priority p = 8 + p); LMASK: each index's TM bit.
PO_M0:	dc	6,4,8,7,5,9,2,0,10,3,1,11	; BG4L BG3L O0 BG4H BG3H O1 BG2L BG1L O2 BG2H BG1H O3
PO_M1:	dc	4,8,5,9,2,0,10,3,1,11		; BG3L O0 BG3H O1 BG2L BG1L O2 BG2H BG1H O3
PO_M1T:	dc	4,8,9,2,0,10,3,1,11,5		; the same, BG3H on top
PO_M7:	dc	8,0,9,10,11			; O0 BG1 O1 O2 O3
LMASK:	dc	1,1,2,2,4,4,8,8,16,16,16,16

; derive_order: r0 = a paint-order list, x1 = its length, y1 = the enabled
; layers (TM) -> RANK
derive_order:
	clr	b
	move	#RANK,r1
	rep	#12
	move	b,y:(r1)+
	move	#RANK,r1
	move	#LMASK,r2
	move	#>$10000,b		; rank 1: the backmost enabled entry
	do	x1,do_e
	movem	p:(r0)+,a		; the entry
	move	a1,n1
	move	a1,n2
	nop
	movem	p:(r2+n2),x0		; its TM bit
	move	y1,a
	and	x0,a
	jeq	do_sk
	move	b1,y:(r1+n1)
	move	#>$10000,x0
	add	x0,b
do_sk:	nop
do_e:	rts

; derive_bg: modes 0 and 1 (2-6 drawn as 1): RANK and LDESC for row FROW
derive_bg:
	move	y:SH_MODE,a
	move	#>7,x0
	and	x0,a
	move	y:SH_TM,y1
	tst	a
	jeq	db_m0
	move	#>$17,x0		; mode 1: BG4 off
	move	y1,a
	and	x0,a
	move	a1,y1
	move	#PO_M1,r0
	move	y:SH_MODE,a
	jclr	#3,a1,db_o
	move	#PO_M1T,r0
db_o:	move	#>10,x1
	jmp	db_ord
db_m0:	move	#PO_M0,r0
	move	#>12,x1
db_ord:	move	y1,y:DTM
	jsr	derive_order
	; the layers
	move	#LDESC,r4
	move	#SH_SC,r5
	move	#SH_OFS,r6
	clr	a
	move	a1,y:DBG
	do	#4,db_e
	; EN: TM & LMASK[2 * bg]
	move	y:DBG,a
	lsl	a
	move	a1,n2
	move	#LMASK,r2
	nop
	movem	p:(r2+n2),x0
	move	y:DTM,a
	and	x0,a
	jeq	db_en
	move	#>1,a
db_en:	move	a1,y:(r4)+		; EN
	; the map row: mapbase + (ty & 31) * 32 (+ $400 / $800 below row 31)
	move	y:(r5)+,a		; BGnSC
	move	a1,y:DSC
	move	(r6)+			; VOFS
	move	y:(r6)-,a
	move	#>$3FF,x0
	and	x0,a
	move	y:FROW,x0
	add	x0,a
	move	#>1,x0
	add	x0,a			; row + 1 + vofs
	move	#>$FF,x0
	move	y:DSC,b
	jclr	#1,b1,db_nt
	move	#>$1FF,x0		; tall
db_nt:	and	x0,a			; y
	move	a1,b
	move	#>7,x0
	and	x0,b
	move	b1,y:DFY		; fy
	rep	#3
	lsr	a			; ty
	move	y:DSC,b
	move	#>$FC,x0
	and	x0,b
	rep	#8
	lsl	b			; mapbase
	move	#>32,x0
	cmp	x0,a
	jlt	db_lo
	sub	x0,a
	move	#>$400,x0
	move	y:DSC,y0
	jclr	#0,y0,db_nw
	move	#>$800,x0		; wide: the lower half is $800 on
db_nw:	add	x0,b
db_lo:	rep	#5
	lsl	a
	move	a1,x0
	add	x0,b
	move	#>$FFFF,x0
	and	x0,b
	move	b1,y:(r4)+		; ROW
	move	y:DSC,a			; WIDE
	move	#>1,x0
	and	x0,a
	move	a1,y:(r4)+
	move	y:(r6)+,a		; HOFS
	move	#>$3FF,x0
	and	x0,a
	move	a1,y:(r4)+
	move	(r6)+
	; CHR: the NBA nibble << 12
	move	y:DBG,a
	lsr	a
	move	#>SH_NBA,x0
	add	x0,a
	move	a1,r2
	move	y:DBG,b
	move	y:(r2),a
	jclr	#0,b1,db_ev
	rep	#4
	lsr	a
db_ev:	move	#>15,x0
	and	x0,a
	rep	#12
	lsl	a
	move	a1,y:(r4)+		; CHR
	move	y:DFY,a
	move	a1,y:(r4)+		; FY
	; BPP and PAL: mode 0: 2, bg * 32; else BG3 2, the others 4; 0
	move	y:SH_MODE,a
	move	#>7,x0
	and	x0,a
	jne	db_m1
	move	#>2,a
	move	a1,y:(r4)+
	move	y:DBG,a
	rep	#5
	lsl	a
	move	a1,y:(r4)+
	jmp	db_nx
db_m1:	move	#>4,a
	move	y:DBG,b
	move	#>2,x0
	cmp	x0,b
	jne	db_4
	move	#>2,a
db_4:	move	a1,y:(r4)+
	clr	a
	move	a1,y:(r4)+
db_nx:	move	y:DBG,a
	move	#>1,x0
	add	x0,a
	move	a1,y:DBG
db_e:	rts

; m7_check: the segments must cover 256 pixels, each 1-256 of them; if
; not, the first bad line's row + 1 goes to M7BAD, its MNSEG and segments
; (up to 12) to M7DUMP, and the line is drawn transparent
M7BAD	equ	$2EFF			; (cleared by FRAME)
M7DUMP	equ	LB0+3*LBSIZE		; (LB3: free on Mode 7 lines)
M7DROW	equ	M7DUMP+80
m7_check:
	move	y:FROW,a		; diagnostics: row M7DROW's segments
	move	y:M7DROW,x0		; to M7DUMP
	cmp	x0,a
	jne	mc_go
	move	y:MNSEG,a
	move	a1,y:M7DUMP
	move	#SEGB,r0
	move	#M7DUMP+1,r1
	do	#72,mc_dp
	move	y:(r0)+,x0
	move	x0,y:(r1)+
mc_dp:
mc_go:	move	y:MNSEG,a
	tst	a
	jle	mc_bad
	move	a1,x1
	move	#SEGB+1,r0
	move	#6,n0
	clr	b
	do	x1,mc_e
	move	y:(r0)+n0,a
	tst	a
	jle	mc_bd
	move	#>256,x0
	cmp	x0,a
	jgt	mc_bd
	move	a1,x0
	add	x0,b
	jmp	mc_nx
mc_bd:	move	#>999,b
mc_nx:	nop
mc_e:	move	#>256,x0
	cmp	x0,b
	jne	mc_bad
	rts
mc_bad:	move	y:M7BAD,a
	tst	a
	jne	mc_fix
	move	y:FROW,a
	move	#>1,x0
	add	x0,a
	move	a1,y:M7BAD
	move	y:MNSEG,a
	move	a1,y:M7DUMP
	move	#SEGB,r0
	move	#M7DUMP+1,r1
	do	#72,mc_cp
	move	y:(r0)+,x0
	move	x0,y:(r1)+
mc_cp:
mc_fix:	move	#>1,a			; one transparent segment
	move	a1,y:MNSEG
	move	#SEGB,r0
	move	#>3,x0
	move	x0,y:(r0)+
	move	#>256,x0
	move	x0,y:(r0)+
	rts

; derive_m7: RANK for a Mode 7 row and its segments (SEGB, MNSEG), as
; ppu_mode7_setup and m7_plan compute them. Integers wider than 24 bits
; live in a2:a1 (a0 = 0). Temporaries in LB3 (MV7):
M_X0	equ	MV7		; sext13(M7X), sext13(M7Y)
M_Y0	equ	MV7+1
M_DX	equ	MV7+2		; clip13(hofs - x0), clip13(vofs - y0)
M_DY	equ	MV7+3
M_A	equ	MV7+4		; the steps a, c (negated when x-flipped)
M_C	equ	MV7+5
M_VX	equ	MV7+6		; vx: a1, a2 (at x = 0)
M_VY	equ	MV7+8		; vy: a1, a2
M_X	equ	MV7+10		; the wrap planner's x
M_LO	equ	MV7+11
M_HI	equ	MV7+12
M_E	equ	MV7+13		; the cut's end; irange's d
M_SW	equ	MV7+14		; SEGB's end
M_YR	equ	MV7+15		; the Mode 7 row (y)
M_S	equ	MV7+16
M_P	equ	MV7+17		; m7v's matrix entries and base
M_Q	equ	MV7+18
M_B	equ	MV7+19
M_K	equ	MV7+20		; the segment being written: kind, count,
M_N	equ	MV7+21		; u, v, du, dv
M_U	equ	MV7+22
M_V	equ	MV7+23
M_DU	equ	MV7+24
M_DV	equ	MV7+25
M_UA	equ	MV7+26		; uv_at: u, v at x (a1 and a2 each)
M_VA	equ	MV7+28
M_ULO	equ	MV7+30		; the u range while irange does v
M_UHI	equ	MV7+31
derive_m7:
	move	#PO_M7,r0
	move	#>5,x1
	move	y:SH_TM,y1
	jsr	derive_order
	move	#>SEGB,x0
	move	x0,y:M_SW
	clr	a
	move	a1,y:MNSEG
	move	y:SH_TM,a		; BG1 off: all transparent
	jset	#0,a1,dm_on
	move	#>256,x0
	jmp	sg_fill
dm_on:	move	y:SH_M7+4,a		; x0, y0: 13-bit signed
	jsr	s13
	move	a1,y:M_X0
	move	y:SH_M7+5,a
	jsr	s13
	move	a1,y:M_Y0
	move	y:SH_M7O,a		; clip13(hofs - x0), clip13(vofs - y0)
	jsr	s13
	move	y:M_X0,x0
	sub	x0,a
	jsr	c13
	move	a1,y:M_DX
	move	y:SH_M7O+1,a
	jsr	s13
	move	y:M_Y0,x0
	sub	x0,a
	jsr	c13
	move	a1,y:M_DY
	move	y:FROW,a		; y = row + 1, 255 - that when y-flipped
	move	#>1,x0
	add	x0,a
	move	y:SH_M7S,b
	jclr	#1,b1,dm_ny
	move	#>255,b
	move	a1,x0
	sub	x0,b
	move	b1,a
dm_ny:	move	a1,y:M_YR
	move	y:SH_M7,x0		; vx: a, b, x0
	move	x0,y:M_P
	move	y:SH_M7+1,x0
	move	x0,y:M_Q
	move	y:M_X0,x0
	move	x0,y:M_B
	jsr	m7v
	move	a1,y:M_VX
	move	a2,y:M_VX+1
	move	y:SH_M7+2,x0		; vy: c, d, y0
	move	x0,y:M_P
	move	y:SH_M7+3,x0
	move	x0,y:M_Q
	move	y:M_Y0,x0
	move	x0,y:M_B
	jsr	m7v
	move	a1,y:M_VY
	move	a2,y:M_VY+1
	move	y:SH_M7,a
	move	a1,y:M_A
	move	y:SH_M7+2,a
	move	a1,y:M_C
	move	y:SH_M7S,a		; x-flip: start at x = 255, step back
	jclr	#0,a1,dm_nf
	move	#>255,x0
	jsr	uv_at			; u, v at x = 255
	move	y:M_UA,a
	move	y:M_UA+1,a2
	move	a1,y:M_VX
	move	a2,y:M_VX+1
	move	y:M_VA,a
	move	y:M_VA+1,a2
	move	a1,y:M_VY
	move	a2,y:M_VY+1
	move	y:M_A,a
	neg	a
	move	a1,y:M_A
	move	y:M_C,a
	neg	a
	move	a1,y:M_C
dm_nf:	move	y:SH_M7S,a
	jclr	#7,a1,dm_wrap
	; clipped: [lo, hi] where u and v stay in 0..$3FFFF
	move	#M_VX,r4
	move	y:M_A,x0
	jsr	irange
	move	a1,y:M_ULO
	move	b1,y:M_UHI
	move	#M_VY,r4
	move	y:M_C,x0
	jsr	irange
	move	y:M_ULO,x0		; lo = max, hi = min
	cmp	x0,a
	tlt	x0,a
	move	a1,y:M_LO
	move	y:M_UHI,x0
	cmp	x0,b
	tgt	x0,b
	move	b1,y:M_HI
	cmp	a,b
	jge	dm_in
	clr	b			; nothing inside
	move	#>256,x0
	jmp	sg_out
dm_in:	clr	b			; outside 0 .. lo - 1
	move	y:M_LO,x0
	jsr	sg_out
	move	y:M_LO,b		; inside lo .. hi
	move	y:M_HI,a
	move	b1,x0
	sub	x0,a
	move	#>1,x0
	add	x0,a
	move	a1,x0
	jsr	sg_in
	move	y:M_HI,a		; outside hi + 1 .. 255
	move	#>1,x0
	add	x0,a
	move	a1,b
	move	#>256,a
	move	b1,x0
	sub	x0,a
	move	a1,x0
	jmp	sg_out
dm_wrap:	; the plane repeats: cut where u or v crosses a multiple of 1024
	clr	a			; texels, each piece reduced
	move	a1,y:M_X
dw_lp:	move	y:M_X,a
	move	#>256,x0
	cmp	x0,a
	jge	dw_e
	move	x0,y:M_E		; end = 256
	move	y:M_X,x0
	jsr	uv_at
	move	y:M_A,x1
	move	#M_UA,r4
	jsr	wcut
	move	y:M_C,x1
	move	#M_VA,r4
	jsr	wcut
	move	y:M_X,b
	move	y:M_E,a
	cmp	b,a
	jgt	dw_ok
	move	#>1,x0			; (end <= x: one pixel)
	add	x0,b
	move	b1,a
dw_ok:	move	a1,y:M_X
	move	b1,x0
	sub	x0,a
	move	a1,y:M_N		; count
	move	#>1,x0
	move	x0,y:M_K
	move	#>$3FFFF,x0
	move	y:M_UA,a
	and	x0,a
	move	a1,y:M_U
	move	y:M_VA,a
	and	x0,a
	move	a1,y:M_V
	move	y:M_A,x0
	move	x0,y:M_DU
	move	y:M_C,x0
	move	x0,y:M_DV
	jsr	wr_seg
	jmp	dw_lp
dw_e:	rts

; wcut: x1 = the step, r4 = the coordinate at x (M_U / M_V, a1 and a2):
; end = min(end, x + ceil((SPAN - umod) / step)) (step > 0) or
; x + ceil((umod + 1) / -step) (step < 0)
wcut:	move	x1,a
	tst	a
	jeq	wc_x
	move	y:(r4),b
	move	#>$3FFFF,x0
	and	x0,b			; umod
	tst	a
	jlt	wc_n
	move	#>$40000,a
	move	b1,x0
	sub	x0,a			; SPAN - umod
	move	x1,x0
	jmp	wc_d
wc_n:	move	#>1,x0
	add	x0,b
	neg	a
	move	a1,x0			; -step
	move	b1,a			; umod + 1
wc_d:	jsr	cdivc
	move	y:M_X,x0
	add	x0,a
	move	y:M_E,x0
	cmp	x0,a
	tgt	x0,a
	move	a1,y:M_E
wc_x:	rts

; uv_at: x0 = x -> M_U, M_V (a1 and a2 each) = vx + a x, vy + c x
uv_at:	move	x0,y:M_S
	move	y:M_A,x0
	move	y:M_S,y0
	jsr	mulx
	move	y:M_VX,a
	move	y:M_VX+1,a2
	add	b,a
	move	a1,y:M_UA
	move	a2,y:M_UA+1
	move	y:M_C,x0
	move	y:M_S,y0
	jsr	mulx
	move	y:M_VY,a
	move	y:M_VY+1,a2
	add	b,a
	move	a1,y:M_VA
	move	a2,y:M_VA+1
	rts

; mulx: x0 = s (16-bit signed), y0 = x (0-255) -> b = s * x (b1, b0 = 0):
; (s << 8) * (x << 15) * 2 = s x << 24. The shifts are products too (the
; low word of v * 2^(n-1) * 2 is v << n): no REP loops. Uses y1.
mulx:	move	#>$80,y1
	mpy	x0,y1,b			; s << 8 in b0
	move	#>$4000,y1
	move	b0,x0
	mpy	y0,y1,b			; x << 15 in b0
	move	b0,y0
	mpy	x0,y0,b
	rts

; fl: x0 = s (16-bit signed), y0 = y (|y| < 1024) -> b1 = floor(s y / 64):
; (s << 7) * (y << 10) * 2 = s y << 18 (b1 = the floor, b0 the rest)
fl:	move	#>$40,y1
	mpy	x0,y1,b			; s << 7 in b0
	move	#>$200,y1
	move	b0,x0
	mpy	y0,y1,b			; y << 10 in b0
	move	b0,y0
	mpy	x0,y0,b
	rts

; m7v: a = 64 (fl(P dx) + fl(Q dy) + fl(Q y)) + B * 256 (a2:a1)
m7v:	move	y:M_P,x0
	move	y:M_DX,y0
	jsr	fl
	move	b1,y:M_S
	move	y:M_Q,x0
	move	y:M_DY,y0
	jsr	fl
	move	y:M_S,a
	move	b1,x0
	add	x0,a
	move	a1,y:M_S
	move	y:M_Q,x0
	move	y:M_YR,y0
	jsr	fl
	move	y:M_S,a
	move	b1,x0
	add	x0,a
	rep	#6
	asl	a
	move	y:M_B,b
	rep	#8
	asl	b
	add	b,a
	rts

; s13: a1 -> a = its low 13 bits, sign-extended
s13:	move	#>$1FFF,x0
	and	x0,a
	move	a1,x0
	move	x0,a
	jclr	#12,x0,s13e
	move	#>$2000,x0
	sub	x0,a
s13e:	rts

; c13: a = v (signed) -> clip13: bit 13 set: (v & $3FF) - $400, else v & $3FF
c13:	move	a1,y0
	move	#>$3FF,x0
	and	x0,a
	move	a1,x0
	move	x0,a
	jclr	#13,y0,c13e
	move	#>$400,x0
	sub	x0,a
c13e:	rts

; irange: r4 = w0 (a1, a2), x0 = d: pixels [a1 = lo, b1 = hi] of 0..255
; with 0 <= w0 + d x <= $3FFFF (lo > hi: none)
irange:	move	x0,y:M_E
	move	x0,b
	tst	b
	jne	ir_d
	move	y:(r4)+,a		; d = 0: all or none
	move	y:(r4)-,a2
	tst	a
	jlt	ir_no
	move	#>$40000,x0
	cmp	x0,a
	jge	ir_no
	clr	a
	move	#>255,b
	rts
ir_no:	move	#>256,a
	clr	b
	rts
ir_d:	jlt	ir_neg
	move	y:(r4)+,a		; d > 0: lo = ceil(-w0 / d)
	move	y:(r4)-,a2
	neg	a
	jsr	cdivc
	move	a1,y:M_LO
	move	y:(r4)+,b		; hi = floor(($3FFFF - w0) / d)
	move	y:(r4)-,b2
	move	#>$3FFFF,a
	sub	b,a
	move	y:M_E,x0
	jsr	fdivc
	jmp	ir_e
ir_neg:	move	y:(r4)+,a		; d < 0: lo = ceil((w0 - $3FFFF) / -d)
	move	y:(r4)-,a2
	move	#>$3FFFF,x0
	sub	x0,a
	move	y:M_E,b
	neg	b
	move	b1,x0
	jsr	cdivc
	move	a1,y:M_LO
	move	y:(r4)+,a		; hi = floor(w0 / -d)
	move	y:(r4)-,a2
	move	y:M_E,b
	neg	b
	move	b1,x0
	jsr	fdivc
ir_e:	move	a1,b			; hi, at most 255
	move	#>255,x0
	cmp	x0,b
	tgt	x0,b
	move	y:M_LO,a
	rts

; cdivc / fdivc: a = N (a2:a1), x0 = D > 0 -> a1 = ceil / floor(N / D),
; clamped: below 0 -> 0 (ceil) or -1 (floor), from 256 on -> 256. With N
; < 256 D: a = N >> 8 across a1:a0; DIV shifts in the previous step's
; quotient bit, so nine steps leave the eight bits in a0. Uses b, x0, y0.
cdivc:	tst	a
	jgt	cd_p
	clr	a
	rts
cd_p:	move	x0,b			; N + D - 1
	move	#>1,y0
	sub	y0,b
	move	b1,y0
	add	y0,a
fdivc:	tst	a
	jge	fd_p
	move	#>-1,a
	rts
fd_p:	move	x0,b
	rep	#8
	asl	b
	cmp	b,a
	jlt	fd_s
	move	#>256,a
	rts
fd_s:	rep	#8
	asr	a
	andi	#$fe,ccr
	rep	#9			; (DIV shifts in the previous step's bit:
	div	x0,a			; the ninth brings the eighth quotient bit)
	move	a0,x0
	move	x0,a
	move	#>$FF,x0
	and	x0,a
	rts

; The segments: sg_fill (x0 pixels, transparent), sg_out (b1 = the first
; pixel, x0 pixels outside: TILE0 or transparent), sg_in (INSIDE)
sg_fill:
	move	x0,y:M_N
	move	#>3,x0
	move	x0,y:M_K
	jmp	wr_seg
sg_out:	move	x0,a
	tst	a
	jle	so_x
	move	y:SH_M7S,a
	jclr	#6,a1,sg_fill
	move	x0,y:M_N		; TILE0: u, v, du, dv modulo 2048
	move	#>2,x0
	move	x0,y:M_K
	move	b1,x0
	jsr	uv_at
	move	#>2047,x0
	move	y:M_UA,a
	and	x0,a
	move	a1,y:M_U
	move	y:M_VA,a
	and	x0,a
	move	a1,y:M_V
	move	y:M_A,a
	and	x0,a
	move	a1,y:M_DU
	move	y:M_C,a
	and	x0,a
	move	a1,y:M_DV
	jmp	wr_seg
so_x:	rts
sg_in:	move	x0,y:M_N
	move	#>1,x0
	move	x0,y:M_K
	move	b1,x0
	jsr	uv_at
	move	y:M_UA,x0
	move	x0,y:M_U
	move	y:M_VA,x0
	move	x0,y:M_V
	move	y:M_A,x0
	move	x0,y:M_DU
	move	y:M_C,x0
	move	x0,y:M_DV
; wr_seg: M_K M_N M_U M_V M_DU M_DV onto SEGB
wr_seg:	move	y:M_SW,r0
	move	#M_K,r1
	do	#6,ws_e
	move	y:(r1)+,x0
	move	x0,y:(r0)+
ws_e:	move	r0,y:M_SW
	move	y:MNSEG,a
	move	#>1,x0
	add	x0,a
	move	a1,y:MNSEG
	rts


;  win_mask: layer a1 (0-3 BG1-4, 4 OBJ): its window spans (SH_WIN) in
; the buffer x1 (pixel x at x1 + 8 + x) set to y1
win_mask:
	move	a1,x0
	lsl	a			; layer * 3
	add	x0,a
	move	#>SH_WIN,x0
	add	x0,a
	move	a1,r1
	move	#>8,a
	add	x1,a
	move	a1,x1			; pixel 0
	do	#3,wm_e
	move	y:(r1)+,b		; x << 12 | len
	move	b1,a
	move	#>$FFF,x0
	and	x0,a			; len
	jeq	wm_nx
	move	a1,n2
	move	b1,x0
	move	#>$800,y0		; x = the word >> 12: a product's high word
	mpy	x0,y0,b
	move	x1,a
	add	b,a
	move	a1,r2
	nop
	rep	n2
	move	y1,y:(r2)+
wm_nx:	nop
wm_e:	rts

;  Input: the host port, or after SSION the SSI (DMA playback through the
; crossbar in handshake mode, falcon/ssitest.s and analysis/translator.md).
; Every SSI slot is a 16-bit word, left-aligned in RX; the DSP asks for
; each slot with PC4 (SC1) and asks for the next one as soon as it has
; one, so one slot is always on its way and none can be lost. A protocol
; word is two slots (its bits 23-8, then 7-0); VRAMSET's and COLOUR's data
; words are one slot each (getw). The receiver's enable bit (CRB RE) is
; the mode: get (boot image) comes here while it is set. get_x may change b.
get_x:
gx_ssi:	jclr	#m_rdf,x:m_sr,gx_ssi
	movep	x:m_rx,a		; bits 23-8
	bset	#4,x:m_pcd		; the next slot
gx_s2:	jclr	#m_rdf,x:m_sr,gx_s2
	movep	x:m_rx,b		; bits 7-0, at 15-8
	bset	#4,x:m_pcd
	rep	#8
	lsr	b
	add	b,a			; (the low byte of a was 0)
	rts

; getw: a 16-bit data word: one slot, or a host-port word
getw:	jclr	#13,x:m_crb,g_h
gw_s:	jclr	#m_rdf,x:m_sr,gw_s
	movep	x:m_rx,a
	bset	#4,x:m_pcd
	rep	#8
	lsr	a
	rts

; SSION (host port): input from the SSI from now on; the first request
; goes out now (Hatari's crossbar runs free until the DSP asked once).
; Answers 1.
cmd_ssion:
	bset	#13,x:m_crb		; RE
	bset	#4,x:m_pcd
	move	#>1,a
	jsr	put
	jmp	main

; SSIOFF (over the SSI): input from the host port again. It must be the
; stream's last slots: the slot already asked for stays pending.
cmd_ssioff:
	bclr	#13,x:m_crb
	jmp	main

	if	@cvi(*)>HIST
	fail	'the second program part runs into the histogram'
	endif

; ---------------------------------------------------------------------------
; The Mode 7 overlay: assembled for internal P at rn_cells, loaded at M7IMG.
	org	p:rn_cells,p:M7IMG

; The Mode 7 loops keep one pixel pending: r6 = its CGRAM index, stored
; (as V7[r6], n6 = V7) at r7 by the next pixel, the next segment or the
; final flush in m7_render.
; m7ins: r5 pixels of an INSIDE segment: a1 = v + TVM * 256, a0 = u + TUM
; * 256, x = the step, y1 = $8000, n1 = $400, n3 = 7, n7 = $3FC0, n4 = $FF
m7ins:	do	r5,m7ie
	move	a1,y0
	mpy	y0,y1,b	a0,y0
	mpy	y0,y1,b	b1,r1		; b1 = tu + TUM, r1 = tv + TVM
	add	x,a	b1,r0
	move	n3,y0
	and	y0,b	y:(r1)+n1,r2	; fine column (tu & 7); TVM, r1 -> TVF
	move	b1,n5
	move	y:(r0),n2		; TUM
	move	y:(r6+n6),y0		; the pending pixel
	move	y0,y:(r7)+
	move	x:(r2+n2),y0		; the map word
	mpy	y0,y1,b	n7,y0		; >> 8: tile * 64 and the lanes
	and	y0,b	y:(r1),y0	; tile * 64; y0 = TVF
	add	y0,b			; + fine row * 8
	move	b1,r5			; (the DO has taken r5's count)
	move	n4,y0
	move	x:(r5+n5),b		; + fine column: the texel word
	and	y0,b
	move	b1,r6			; pending
m7ie:	rts

; m7tz: r5 pixels of a TILE0 segment (t0_set): the pixel is T0V[fine row
; * 8 + fine column], with both fields from one constant: 8 v >> 8 =
; v >> 5 (& $38), u >> 8 (& 7)
m7tz:	move	y:(r6+n6),b		; the pending pixel
	move	b1,y:(r7)+
	move	a1,y0			; 8 v
	do	r5,m7te
	mpy	y0,y1,b	n0,y0		; v >> 5
	and	y0,b	a0,y0		; fine row * 8; u
	mpy	y0,y1,b	b1,n3		; u >> 8
	add	x,a	n1,y0		; the step
	and	y0,b	n3,y0		; fine column
	add	y0,b	a1,y0		; the index; the next 8 v
	move	b1,n3
	nop
	move	y:(r3+n3),b
	move	b1,y:(r7)+
m7te:	move	(r7)-			; the last one pending again: r6 = its
	move	n3,r3			; texel (X:index, bits 7-0)
	move	#>$FF,y0
	move	x:(r3),b
	and	y0,b
	move	b1,r6
	rts
m7ov_end:

	if	(m7ov_end-m7ins)>OVLN
	fail	'the Mode 7 overlay is larger than the region'
	endif
	if	(m7ov_end-m7ins)>64
	fail	'the Mode 7 overlay outgrows its image'
	endif
	if	OVLN>118
	fail	'the Mode 7 overlay is larger than the region'
	endif
