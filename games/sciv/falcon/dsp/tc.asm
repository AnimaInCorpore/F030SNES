; SNES pictures in true colour on the Falcon DSP: every BG mode's tiles
; (0-6; Mode 7 in the TC7 build: --define TC7=1), sprites, both screens with their
; windows, colour math per pixel, brightness; each row streamed as 256
; RGB565 words over the SSI into DMA record (handshake mode, SC2 = frame
; line), for the Videl's true-colour mode.
;
; Reference: ppu_render_565() (src/snes_ppu.c). The host model
; tools/tc_model.c describes the program step by step, src/ppu_tc.h the
; stream and the converted tile formats; TCTEST.TOS checks this program
; against the reference.
;
; A row:
;   keys    per pixel a key depth << 16 | source << 8 | CGRAM index
;           (source 0-3 BG1-4, 4 OBJ; 0 is the backdrop). Depths are
;           snes9x's, so the front pixel is the greatest key.
;   layers  each BG layer on either screen into LK: 33 slivers from the
;           converted character rows (ppu_tc.h). A row flagged TC_CLEAR
;           writes eight zeros; TC_SOLID eight keys from the fields (one
;           multiply each); a mixed row tests each pixel. A horizontally
;           flipped sliver is written right to left.
;   sprites into LK the same way (OBJ key), from the last one in range
;           backwards, only opaque pixels, so the lowest OAM index ends on
;           top.
;   merge   LK into MAIN where TM shows the layer, into SUB where TS does:
;           the greater key; where a window mask (TMW, TSW) can hide the
;           layer, per pixel through its 4-bit mask on WC (below).
;   colour  per pixel. Without colour math or clipping on the row: T565
;           (CGRAM as RGB565, brightness applied). Otherwise the main and
;           sub colours as E words (r | g << 8 | b << 16), added (or the
;           sub one subtracted with a bias of 32 per channel), and the
;           three channels through TCH (clamp, half, brightness, the
;           RGB565 bits).
;   windows WC[x] = in window 1 | in window 2 << 1, rebuilt when WH0-3
;           change; per layer and screen, and for the colour window, a
;           4-bit mask: bit WC[x] set = the window area there.
;   output  a ring of 512 words (RING; r7 writes, r6 reads), sent by the
;           SSI transmit interrupt as in ppu.asm: bit 0 marks a sent word.
;
; Registers r6/m6 belong to the transmit interrupt, r7/m7 to the ring's
; writer.
;
; Internal program RAM: the hot routines (hotcode ... extcode) run there,
; over the stage-2 loader, from HOTR0 on; they load at P:$1000 (the ring,
; later) and init copies them in. From external P every instruction that
; also touches external X or Y costs two more cycles (one external bus;
; Hatari's dsp_cpu.c counts it the same). init runs once from P:$1200 (the
; colour tables, later); the rest of the program is external from P:$0200.
;
; Memory (external RAM: Y:a = P:a for a < $4000, X:a = P:$4000 + a):
;   X:$0000-$00FF  slots and variables
;   Y:$0000-$00FF  POW2 and small tables
;   P:$0000-$0FFF  this program (Y:$0200-$0FFF)
;   Y:$1000-$11FF  RING
;   Y:$1200-$12FF  T565: CGRAM as RGB565 with brightness, bits 23-8
;   Y:$1300-$13FF  E: CGRAM as r | g << 8 | b << 16
;   Y:$1400-$16FF  TCH: per (subtract, half) 3 x 64 channel words
;   Y:$1700-$17FF  the row's sprites in range; OBJ scratch
;   Y:$1800-$190F  MAIN, Y:$1910 SUB, Y:$1A20 LK: 272 keys (x + 8)
;   Y:$1B30-$1C2F  WC
;   Y:$1C30-$1EAF  ES0, TRT, TGT, TBT: the sub screen's colour math (sub_check)
;   Y:$1F00-$25FF  CTAB: per key source (0-3 BG, 4 OBJ palettes 0-3, 5 the
;                  backdrop, 6 OBJ palettes 4-7) and index the pixel's
;                  RGB565, for rows whose colour math is by the fixed colour
;                  and does not depend on the colour window
;   X:$0100-$08FF  the frame's stream
;   X:$0900-$0B93  sliver descriptors: per layer and sliver (33) five
;                  words: DY (its y), DA (the character row's P address),
;                  DS (its step, -1 flipped), DK (the key base), DF (flipped)
;   X:$0D00-$0EFF  PT: P address of page p in format f (fi * 128 + p)
;   X:$0F00-$0FFF  conversion tables
;   P:$2600-$3FFF, P:$5000-$7FFF  74 converted page slots, placed by the
;                  CPU (TPAGE)
;
; The TC7 build (frames whose rows are all Mode 7; ppu_tc.h TC_M7): the
; plane in slots 0-63 (plane word w at P:$2600 + w, from $1A00 on at
; P:$3600 + w), sprite pages in slots 64-73. It has no BG tile layers (no
; F2/F8 conversion, no descriptors), and in their room:
;   X:$0900-$0CFF  TVF: per tv (0-1023) (tv & 7) * 8
;   Y:$0C00-$0FFF  TUF: per tu (0-1023) tu & 7
;   X:$0F80-$0FFF  TVM: per map row r (0-127) its P address + $1000
;   Y:$0080-$017F  KEY7: per texel its key (0 for texel 0)
;   X:$0E00-$0E93  the variables at X:$0BC4 in the tile build
; A row's pixel: v, u from the stream's Mode 7 entry (as in ppu.asm M7K);
; the map row by v >> 11, the column by u >> 11 (one multiply each), the
; map entry gives the character's P address, (tv & 7) * 8 and tu & 7 by
; table; the texel's key into LK.

	include	'ioequ.inc'

TIE	equ	14

; stream slots (ppu_dsp.h)
S_INIDISP	equ	0
S_BGMODE	equ	1
S_TM		equ	2
S_OBSEL		equ	3
S_SC		equ	4
S_NBA		equ	8
S_OFS		equ	10
S_WSEL		equ	18
S_WH01		equ	19
S_WH23		equ	20
S_WLOG		equ	21
S_SUB		equ	22
S_TS		equ	23
S_TSW		equ	24
S_MATH		equ	25
S_FIXED		equ	26
NSLOT		equ	27
SLOT_CG		equ	$FF

; X internal
SLOT	equ	$00
ROW	equ	$20
ROWK	equ	$21
CHGP	equ	$22
CHGN	equ	$23
SEGP	equ	$24
SEGN	equ	$25
CURS	equ	$26		; the row's OAM segment: its sprite list
CURN	equ	$27
OPTP	equ	$28		; the offset-per-tile table, or 0
TXSTART	equ	$29
MODE	equ	$2A
LAY	equ	$2B		; the layer being drawn
BPP	equ	$2C
WDIRTY	equ	$2D		; a window slot changed: masks and WC again
BDIRTY	equ	$2E		; brightness or CGRAM changed: T565/TCH again
BRIGHT	equ	$2F		; the brightness the tables hold (+1)
L_MAP	equ	$30
L_CHR	equ	$31
L_BIG	equ	$32
L_MW	equ	$33		; map width - 1 in pixels
L_MH	equ	$34
L_Y0	equ	$35		; the row's y by the registers
L_PX0	equ	$36		; the first sliver's map x (coarse)
L_FINE	equ	$37
L_HI	equ	$38		; key bases by priority (depth, source)
L_LO	equ	$39
L_OPT	equ	$3A		; this layer's 33 table words, or 0
L_K	equ	$3B		; the sliver: 8 * k
L_WORDS	equ	$3C		; words per character (4 * bpp)
L_PALB	equ	$3D		; palette base (mode 0: 32 * layer)
L_FI	equ	$3E		; the character format's index (0-2)
L_FMUL	equ	$3F		; the field multiplier (2^(field bits - 1))
L_E	equ	$40		; the sliver's map entry
L_T	equ	$41
MASKS	equ	$42		; 10: per layer 0-4, main then sub: 4-bit masks
CWMASK	equ	$4C		; the colour window's mask
MATHF	equ	$4D		; the row's colour: 0 fast (T565), 1 per pixel
ENTAB	equ	$4E		; 6: colour math enabled per source (0-5)
ENTAB6	equ	$54		; (ENTAB's seventh: OBJ palettes 4-7)
CLIP0	equ	$55		; outside the colour window: clip, math
MATH0	equ	$56
CLIP1	equ	$57		; inside
MATH1	equ	$58
FIXEDE	equ	$59		; the fixed colour as an E word
SUBON	equ	$5A		; CGWSEL bit 1: the sub screen, not the fixed colour
HALF	equ	$5B
SUBTR	equ	$5C
O_N	equ	$5D		; sprites
O_SL	equ	$5E
O_KEY	equ	$5F
O_PAL	equ	$60
O_X	equ	$61
O_W	equ	$62
O_H	equ	$63
O_DY	equ	$64
O_ATTR	equ	$65
O_TILE	equ	$66
O_BASE	equ	$67
O_GAP	equ	$68
O_SW	equ	$69		; OBSEL sizes: small w, h, large w, h
O_SH	equ	$6A
O_LW	equ	$6B
O_LH	equ	$6C
O_COL	equ	$6D
O_CNT	equ	$6E
L_DST	equ	$6F		; bg_line: the sliver's first LK word
L_WIDE	equ	$70
L_TSH	equ	$71		; tile size shift (3 or 4)
L_WSH	equ	$72		; words per character shift (3, 4, 5)
L_KEY	equ	$73
L_OPTW	equ	$74
L_Y	equ	$75
L_HC	equ	$76
O_NS	equ	$77		; sprite slivers so far
O_I	equ	$78
L_WMUL	equ	$7D		; 2^(words shift - 1): chr * words by one multiply
L_ROWA	equ	$7E		; the row's map row address (x = 0) by the registers
CASEB	equ	$79		; the row's colour through CTAB
CDIRTY	equ	$7A		; CTAB to be built again
CKEY	equ	$7B		; what CTAB was built for (MATH slot)
CKEY2	equ	$7C		; (FIXED slot)
CASEW	equ	$80		; the row's clip and math do not depend on the colour window
CKIND	equ	$81		; what CTAB holds: 1 ctab_check's, 2 sub_check's
BAND0	equ	$82		; the next frame draws rows [BAND0, BAND1) (BAND);
BAND1	equ	$83		; every row is still walked: changes, segments
RBASE	equ	$85		; rb_all: the row's word base (char base + fy), flipped
RBASEV	equ	$86
KLO	equ	$87		; key bases: low, high priority (+ palette base)
KHI	equ	$88
PSHM	equ	$89		; palette shift multiplier
RB_PA	equ	$8A		; the map row's P address: screen 0, screen 1
RB_PB	equ	$8B
RB_S	equ	$8C		; the run: first map column, screen, slivers left
RB_SCR	equ	$8D
RB_REM	equ	$8E
RB_LEN	equ	$8F
RB_FIM	equ	$90		; format index * 128
VSC	equ	$91		; bg_line: the layer's vertical scroll (masked)
CPART	equ	$F2		; ctab_check: only the fixed colour changed
OBAND0	equ	$F3		; the rows the segment's sprites can touch: [OBAND0, OBAND1)
OBAND1	equ	$F4
OBW	equ	$F5		; a sprite wraps past row 255
BGC	equ	$92		; per layer 24 words: bg_line's results, kept between rows
TMP0	equ	$1B
TMP1	equ	$1C
TMP2	equ	$1D
TMP3	equ	$1E

; Y internal
POW2	equ	$00		; 1 << n, n = 0..23
OBJSZ	equ	$20		; per OBSEL size: small w, h, large w, h (32)
DEPTH	equ	$40		; per mode (8) and layer (4): low | high << 8
MBPP	equ	$60		; per mode and layer: bits per pixel (0: none)

; external
RING	equ	$1000
T565	equ	$1200
EW	equ	$1300
TCH	equ	$1400
OLIST	equ	$1700
TCOMP	equ	$1780		; per component 0-31 its RGB565 bits (3 x 32)
MAIN	equ	$1800
SUB	equ	$1910
LK	equ	$1A20
WC	equ	$1B30
ES0	equ	$1C30		; sub_check: per sub index its E term
TRT	equ	$1D30		; per channel value its RGB565 bits: red (128 + v),
TGT	equ	$1DB0		; green, blue (v: the sum, + 64 halved)
TBT	equ	$1E30
CTAB	equ	$1F00		; per key source and CGRAM index: the pixel (7 x 256)
STREAM	equ	$0100
DESC	equ	$0900
	if	TC7
MISC	equ	$0E00		; (TVF is at X:$0900-$0CFF)
	else
MISC	equ	$0BC4
	endif
M_LO	equ	MISC		; merge's fast path: the first LK word and the count
M_CNT	equ	MISC+1
O_XMIN	equ	MISC+2		; the sprites drawn this row: leftmost, rightmost sx
O_XMAX	equ	MISC+3
L_DST0	equ	MISC+6		; the layer row's first sliver word
L_SP	equ	MISC+7		; step_row's descriptor pointer while a sliver builds
LROW	equ	MISC+12		; per layer the last row it was drawn in
LDIRTY	equ	MISC+16		; a slot changed: every layer builds its descriptors
L_DBASE	equ	MISC+17		; LAY * 33
L_KI	equ	MISC+18		; the sliver's index
L_DIRECT	equ	MISC+8		; the layer draws straight into L_LINE (no LK, no merge)
MFRESH	equ	MISC+4		; MAIN holds only the backdrop so far this row
SFRESH	equ	MISC+5		; SUB the same
L_FRESH	equ	MISC+10		; the layer draws straight into a fresh line:
				; solid rows store without comparing
WSEG	equ	MISC+148	; the row's runs of one window code (WC): their
				; number, then per run its pixels and its code
L_LINE	equ	MISC+9
SDIRTY	equ	MISC+19		; SPK to be built (a new OAM segment, a slot changed)
SPK	equ	MISC+20		; per sprite of the segment: x + 256 | y << 9 |
				; large << 17 | on screen << 18 (128)
L_STEP	equ	$7F		; this layer row steps its descriptors
PT	equ	$0D00
T4	equ	$0F00		; F4: plane q nibble n -> its 4 pixels' bits (64)
T2	equ	$0F40		; F2: plane q nibble n -> pixels 0-3 (32)
	if	TC7
M7P	equ	$30		; the stream's Mode 7 entry of the next row
M7N	equ	$31		; segments left in the row
M7C	equ	$32		; the segment's kind << 16 | count
M7T	equ	$33		; where the row's keys go: LK + 8, or straight
				; into MAIN + 8 or SUB + 8
STEP	equ	$1F		; L: the segment's step (dv << 24 + du: x1, x0)
K1000	equ	$18		; Y: the sampling loops' constants
KFF	equ	$19
K3FF	equ	$1A
TVF	equ	$0900
TUF	equ	$0C00
TVM	equ	$0F80
KEY7	equ	$0080
	endif

; ---------------------------------------------------------------------------
HOTL0	equ	$1000		; the hot routines' load address (RING)
INITP	equ	$1200		; init's (T565 and on)
HOTL	set	HOTL0
hotcode	macro
EXTR	set	@LCV(R)
	org	p:HOTR,p:HOTL
	endm
extcode	macro
HOTR	set	@LCV(R)
HOTL	set	@LCV(L)
	org	p:EXTR
	endm

	org	y:OBJSZ			; snes_ppu.c obj_sizes
	dc	8,8,16,16,8,8,32,32,8,8,64,64,16,16,32,32
	dc	16,16,64,64,32,32,64,64,16,32,32,64,16,32,32,32
	org	y:DEPTH			; snes9x: BG depth low | high << 8
	dc	11+15*256,10+14*256,3+7*256,2+6*256
	dc	11+15*256,10+14*256,3+7*256,0
	dc	7+15*256,3+11*256,0,0
	dc	7+15*256,3+11*256,0,0
	dc	7+15*256,3+11*256,0,0
	dc	7+15*256,3+11*256,0,0
	dc	7+15*256,0,0,0
	dc	6+6*256,0,0,0
	org	y:MBPP			; snes_ppu.c mode_bpp
	dc	2,2,2,2,4,4,2,0,4,4,0,0,8,4,0,0,8,2,0,0,4,2,0,0,4,0,0,0,0,0,0,0
	if	TC7==0
	org	y:$80
BGV	dc	L_FI,L_FMUL,L_WSH,L_WMUL,L_MAP,L_CHR,L_BIG,L_TSH,L_WIDE,L_MW,L_MH,L_FINE,L_PX0,VSC,L_HI,L_LO,L_PALB,L_OPT,L_DIRECT,L_LINE,L_DST,L_DBASE
NBGV	equ	22
	endif

	org	p:$0000
	jmp	start

	org	p:$0010			; SSI transmit data: tx_isr
	jsr	tx_isr
	jsr	tx_isr

	org	p:$0040
start:
	jmp	start2

; The transmit interrupt (ppu.asm): the next ring word unless it is sent
; already, then off until the writer turns it on again.
tx_isr:
	jset	#0,y:(r6),_stop
	movep	y:(r6),x:m_tx
	bset	#0,y:(r6)+
	rti
_stop:	bclr	#TIE,x:m_crb
	rti

HOTR0	equ	@LCV(R)		; internal P from here: the loader, then the hot routines
HOTR	set	HOTR0

start2:
	movep	#1,x:m_pbc		; stage 2: the rest of the program
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
; Once, after loading.
	org	p:INITP
init:
	andi	#$fb,omr		; DE = 0: X/Y:$0100-$01FF external
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
	move	#POW2,r4
	move	#>1,a
	do	#24,_p2
	move	a1,y:(r4)+
	lsl	a
_p2:
	; T4[q * 16 + n] = sum over i of bit 3-i of n << (19 - 5i + q)
	move	#T4,r0
	clr	b
	move	b1,x:TMP0		; q
	do	#4,_t4q
	clr	b
	move	b1,x:TMP1		; n
	do	#16,_t4n
	jsr	spread4
	move	a1,x:(r0)+
	move	x:TMP1,b
	move	#>1,x0
	add	x0,b
	move	b1,x:TMP1
	nop
_t4n:
	move	x:TMP0,b
	move	#>1,x0
	add	x0,b
	move	b1,x:TMP0
	nop
_t4q:
	; T2[q * 32 + half * 16 + n]: pixels 0-3 (half 0), 4-7 (half 1: 12
	; lower): sum of bit 3-i of n << (21 - 3i + q)
	move	#T2,r0
	clr	b
	move	b1,x:TMP0
	do	#2,_t2q
	clr	b
	move	b1,x:TMP1
	do	#16,_t2n
	jsr	spread2
	move	a1,x:(r0)+
	move	x:TMP1,b
	move	#>1,x0
	add	x0,b
	move	b1,x:TMP1
	nop
_t2n:
	clr	b
	move	b1,x:TMP1
	do	#16,_t2m
	jsr	spread2
	rep	#12
	lsr	a
	move	a1,x:(r0)+
	move	x:TMP1,b
	move	#>1,x0
	add	x0,b
	move	b1,x:TMP1
	nop
_t2m:
	move	x:TMP0,b
	move	#>1,x0
	add	x0,b
	move	b1,x:TMP0
	nop
_t2q:
	clr	a
	move	#PT,r0
	do	#512,_pt
	move	a,x:(r0)+
	nop
_pt:
	if	TC7
	move	#>$001000,x0		; the sampling loops' constants
	move	x0,y:K1000
	move	#>$ff,x0
	move	x0,y:KFF
	move	#>$3ff,x0
	move	x0,y:K3FF
	move	#TVF,r0			; TVF[i] = (i & 7) * 8, TUF[i] = i & 7
	move	#TUF,r1
	clr	a
	move	#>7,x1
	move	#>1,y1
	do	#1024,_tf
	move	a1,b
	and	x1,b
	move	b1,y:(r1)+
	lsl	b
	lsl	b
	lsl	b
	move	b1,x:(r0)+
	add	y1,a
_tf:
	move	#TVM,r0			; TVM[r]: tc_m7_p(128 r) + $1000
	clr	a
	move	#>128,y1
	move	#>$1a00,x1
	do	#128,_tm
	move	#>$2600+$1000,b
	cmp	x1,a
	jlt	_t1
	move	#>$3600+$1000,b
_t1:	add	a,b
	move	b1,x:(r0)+
	add	y1,a
_tm:
	move	#KEY7,r0		; KEY7: texel 0 the backdrop's key (as
	move	#>$500,a		; transparent as 0 before the lines' $500),
	move	a,y:(r0)+		; else Mode 7's depth 6, BG1
	move	#>$060001,a
	move	#>1,y1
	do	#255,_k7
	move	a1,y:(r0)+
	add	y1,a
_k7:
	endif
	clr	a			; frames whole until BAND
	move	a1,x:BAND0
	move	#>224,a
	move	a1,x:BAND1
	move	#HOTL0,r0		; the hot routines into internal P (over
	move	#HOTR0,r1		; the loader: it is done)
	do	#HOTLEN,_hc
	movem	p:(r0)+,x0
	movem	x0,p:(r1)+
_hc:
	andi	#$fc,mr			; interrupts on
	jmp	main

; spread4: a1 = sum over i = 0..3 of bit 3-i of n (TMP1) << (19 - 5i + q
; (TMP0)); spread2 the same with 22 - 2i + q.
spread4:
	move	#>19,x1
	move	#>5,y1
	jmp	spread
spread2:
	move	#>21,x1
	move	#>3,y1
spread:
	clr	a
	move	x1,b			; the shift for pixel 0, + q
	move	x:TMP0,x0
	add	x0,b
	move	b1,x:TMP2
	move	x:TMP1,b
	move	b1,x:TMP3		; n
	do	#4,_sp
	jclr	#3,x:TMP3,_s0
	move	x:TMP2,n4
	move	#POW2,r4
	nop
	move	y:(r4+n4),x0
	or	x0,a
_s0:	move	x:TMP3,b		; next bit of n
	lsl	b
	move	b1,x:TMP3
	move	x:TMP2,b
	sub	y1,b
	move	b1,x:TMP2
	nop
_sp:
	rts

; ---------------------------------------------------------------------------
	org	p:$0200
main:	jsr	get
	move	#>4,x0
	cmp	x0,a
	jeq	cmd_ping
	move	#>$10,x0
	cmp	x0,a
	jeq	cmd_frame
	move	#>$11,x0
	cmp	x0,a
	jeq	cmd_band
	move	#>$22,x0
	cmp	x0,a
	jeq	cmd_tpage
	move	#>3,x0
	cmp	x0,a
	jeq	cmd_ssion
	move	#>9,x0
	cmp	x0,a
	jeq	cmd_ssioff
	jmp	main

; Input as in ppu.asm: the host port, or after SSION the SSI.
get:	jset	#13,x:m_crb,get_x
g_h:	jclr	#m_hrdf,x:m_hsr,g_h
	movep	x:m_hrx,a
	rts
get_x:
gx_s1:	jclr	#m_rdf,x:m_sr,gx_s1
	movep	x:m_rx,a		; bits 23-8
	bset	#4,x:m_pcd
gx_s2:	jclr	#m_rdf,x:m_sr,gx_s2
	movep	x:m_rx,b		; bits 7-0, at 15-8
	bset	#4,x:m_pcd
	rep	#8
	lsr	b
	add	b,a
	rts

getw:	jclr	#13,x:m_crb,g_h
gw_s:	jclr	#m_rdf,x:m_sr,gw_s
	movep	x:m_rx,a
	bset	#4,x:m_pcd
	rep	#8
	lsr	a
	rts

cmd_ssion:
	bset	#13,x:m_crb
	bset	#4,x:m_pcd
	move	#>1,a
	jsr	put
	jmp	main

cmd_ssioff:
	bclr	#13,x:m_crb
	jmp	main

put:	jclr	#m_htde,x:m_hsr,put
	movep	a,x:m_htx
	rts

cmd_ping:
	move	#>$544353,a		; "TCS"
	jsr	put
	jmp	main

; BAND: the next frame draws and sends rows [first, last) only (a frame
; whose pages do not fit goes up in bands, each with its own pages).
cmd_band:
	jsr	get
	move	a1,x:BAND0
	jsr	get
	move	a1,x:BAND1
	jmp	main

; TPAGE: page, its format (ppu_tc.h: 1, 2, 4, 8), the slot's P address,
; then the page's 256 VRAM words: converted into the slot, PT set.
cmd_tpage:
	jsr	get
	move	a1,x:TMP0		; page
	jsr	get
	move	a1,x:TMP1		; format bit
	jsr	get
	move	a1,x:TMP2		; P address
	move	#LK,r0			; the raw words, in LK for now
	do	#256,_rd
	jsr	getw
	move	#>$ffff,x0
	and	x0,a
	move	a1,y:(r0)+
_rd:
	if	TC7
	move	x:TMP1,a
	jset	#4,a1,conv_m7		; the Mode 7 plane: no PT entry
	endif
	; PT[fi * 128 + page]
	move	x:TMP1,a
	clr	b
	jclr	#0,a1,_f1
	jmp	_fs
_f1:	move	#>128,b
	jclr	#1,a1,_f2
	jmp	_fs
_f2:	move	#>256,b
	jclr	#2,a1,_f3
	jmp	_fs
_f3:	move	#>384,b
_fs:	move	x:TMP0,x0
	add	x0,b
	move	#PT,r0
	move	b1,n0
	move	x:TMP2,x0
	move	x0,x:(r0+n0)
	move	x:TMP2,r1		; the slot
	move	#LK,r0
	move	x:TMP1,a
	jset	#3,a1,conv_raw
	if	TC7==0
	jset	#0,a1,conv_f2
	jset	#1,a1,conv_f4
	jmp	conv_f8
	else
	jmp	conv_f4

; M7: word w -> P(t) << 8 | texel (t its low byte, the texel its high byte;
; ppu_tc.h tc_m7_p): P(t) = $2600 + 64 t, from t = 104 on $3600 + 64 t.
conv_m7:
	move	x:TMP2,r1
	move	#LK,r0
	do	#256,_cm
	move	y:(r0)+,a
	move	a1,b
	move	#>$ff,x0
	and	x0,a			; t
	move	#>104,x0
	cmp	x0,a
	move	#>$2600,x1
	jlt	_lo
	move	#>$3600,x1
_lo:	rep	#6
	lsl	a			; 64 t
	add	x1,a
	rep	#8
	lsl	a
	rep	#8
	lsr	b			; the texel
	move	b1,x0
	or	x0,a
	movem	a1,p:(r1)+
_cm:
	jmp	main
	endif

conv_raw:
	do	#256,_cr
	move	y:(r0)+,x0
	movem	x0,p:(r1)+
_cr:
	jmp	main

	if	TC7==0
; F2: per word, plane 0 (low byte) and 1 (high byte) through T2 (no
; flags: a clear row is a 0 word).
conv_f2:
	do	#256,_c2
	move	y:(r0)+,a
	jsr	bytes			; b0 = low byte, b1 = high byte in TMP0/1
	clr	b
	move	x:TMP0,x0		; q = 0: T2[n], T2[16 + n]
	move	#T2,r2
	jsr	nib2
	move	x:TMP1,x0		; q = 1
	move	#T2+32,r2
	jsr	nib2
	movem	b1,p:(r1)+
	nop
_c2:
	jmp	main

	endif

; F4: per character row the planes 0/1 word (+fy) and 2/3 word (+8 + fy):
; pixels 0-3 from the planes' high nibbles through T4, 4-7 from the low
; ones; the first word takes the flags.
conv_f4:
	move	r1,x:TMP2		; the slot
	move	#LK,x0
	move	x0,x:TMP3		; the tile's raw words
	do	#16,_c4t
	do	#8,_c4r
	move	x:TMP3,r0
	nop
	move	y:(r0),a		; planes 0/1
	move	#8,n0
	jsr	bytes
	move	x:TMP0,y0		; b0
	move	x:TMP1,y1		; b1
	move	y:(r0+n0),a		; planes 2/3
	jsr	bytes			; b2, b3 in TMP0/1
	clr	a			; A: high nibbles
	clr	b			; B: low nibbles
	move	y0,x0
	move	#T4,r2
	jsr	nib4
	move	y1,x0
	move	#T4+16,r2
	jsr	nib4
	move	x:TMP0,x0
	move	#T4+32,r2
	jsr	nib4
	move	x:TMP1,x0
	move	#T4+48,r2
	jsr	nib4
	move	a1,x:L_T		; flags from b0 | b1 | b2 | b3
	move	y0,a
	or	y1,a
	move	x:TMP0,x0
	or	x0,a
	move	x:TMP1,x0
	or	x0,a
	jsr	flags
	move	x:L_T,a
	or	x0,a
	move	x:TMP2,r1
	move	#8,n1
	nop
	movem	a1,p:(r1)
	movem	b1,p:(r1+n1)
	move	(r1)+
	move	r1,x:TMP2
	move	x:TMP3,r0
	nop
	move	(r0)+
	move	r0,x:TMP3
	nop
_c4r:
	move	x:TMP2,r1		; the next character: +16 (8 done)
	move	#8,n1
	nop
	move	(r1)+n1
	move	r1,x:TMP2
	move	x:TMP3,r0
	move	#8,n0
	nop
	move	(r0)+n0
	move	r0,x:TMP3
	nop
_c4t:
	jmp	main

	if	TC7==0
; F8: per character row the four plane words (+fy, +8, +16, +24): each
; pixel's 8 bits, three pixels to a word as 8-bit fields; the flags at +24.
conv_f8:
	move	r1,x:TMP2
	move	#LK,x0
	move	x0,x:TMP3
	do	#8,_c8t
	do	#8,_c8r
	move	x:TMP3,r0
	move	x:TMP2,r1
	nop
	jsr	row8
	move	x:TMP2,r1
	move	x:TMP3,r0
	nop
	move	(r1)+
	move	(r0)+
	move	r1,x:TMP2
	move	r0,x:TMP3
	nop
_c8r:
	move	x:TMP2,r1		; the next character: +32 (8 done)
	move	#24,n1
	move	x:TMP3,r0
	move	#24,n0
	nop
	move	(r1)+n1
	move	(r0)+n0
	move	r1,x:TMP2
	move	r0,x:TMP3
	nop
_c8t:
	jmp	main

; One 8 bpp row: raw words at r0 + 0, 8, 16, 24; fields to r1 + 0, 8, 16,
; 24, the flags in the first.
row8:
	move	#8,n0
	move	y:(r0),x0		; planes 0/1
	move	x0,x:O_KEY
	move	(r0)+n0
	move	y:(r0),x0
	move	x0,x:O_PAL		; 2/3
	move	(r0)+n0
	move	y:(r0),x0
	move	x0,x:O_X		; 4/5
	move	(r0)+n0
	move	y:(r0),x0
	move	x0,x:O_W		; 6/7
	move	x:O_KEY,a		; coverage: the OR of every plane byte
	move	x:O_PAL,x0
	or	x0,a
	move	x:O_X,x0
	or	x0,a
	move	x:O_W,x0
	or	x0,a
	move	a1,b
	rep	#8
	lsr	b
	move	b1,x0
	or	x0,a
	move	#>$ff,x0
	and	x0,a
	move	a1,x:L_T
	move	#OLIST,r3		; the 8 pixels' values
	move	#>$80,y1		; bit of pixel x in a plane byte
	do	#8,_px
	clr	b			; value
	move	#O_KEY,r2
	move	#>1,x1			; bit of the value
	do	#4,_pw
	move	x:(r2)+,a		; plane pair word
	move	a1,x0
	and	y1,a			; low byte's bit: plane 2q
	jeq	_z0
	or	x1,b
_z0:	move	x1,a
	lsl	a
	move	a1,x1
	move	x0,a			; high byte's bit: plane 2q + 1
	lsr	a
	lsr	a
	lsr	a
	lsr	a
	lsr	a
	lsr	a
	lsr	a
	lsr	a
	and	y1,a
	jeq	_z1
	or	x1,b
_z1:	move	x1,a
	lsl	a
	move	a1,x1
	nop
_pw:
	move	b1,y:(r3)+
	move	y1,a
	lsr	a
	move	a1,y1
	nop
_px:
	; word q (+8q): pixel 2q << 15 | pixel 2q + 1 << 6; flags in word 0
	move	#OLIST,r3
	move	#8,n1
	move	r1,x:TMP0
	do	#4,_pk
	move	y:(r3)+,a
	rep	#15
	lsl	a
	move	y:(r3)+,b
	rep	#6
	lsl	b
	move	b1,x0
	or	x0,a
	movem	a1,p:(r1)
	move	(r1)+n1
	nop
_pk:
	move	x:TMP0,r1
	move	x:L_T,a
	jsr	flags
	nop
	movem	p:(r1),a
	or	x0,a
	movem	a1,p:(r1)
	rts

	endif

; a1 (a VRAM word) -> TMP0 its low byte, TMP1 its high byte
bytes:
	move	a1,b
	move	#>$ff,x0
	and	x0,a
	move	a1,x:TMP0
	rep	#8
	lsr	b
	move	b1,a
	and	x0,a
	move	a1,x:TMP1
	rts

	if	TC7==0
; b |= T2[q][high nibble of x0] | T2[q][16 + low nibble] (r2 = T2 + 32 q)
nib2:
	move	x0,a
	rep	#4
	lsr	a
	move	a1,n2
	nop
	move	x:(r2+n2),x1
	or	x1,b
	move	x0,a
	move	#>$f,x1
	and	x1,a
	move	#>16,x1
	add	x1,a
	move	a1,n2
	nop
	move	x:(r2+n2),x1
	or	x1,b
	rts

	endif

; a |= T4[q][high nibble of x0], b |= T4[q][low nibble] (r2 = T4 + 16 q);
; keeps y0, y1
nib4:
	move	a1,x:L_E		; (a is the high nibbles' sum)
	move	x0,a
	rep	#4
	lsr	a
	move	a1,n2
	nop
	move	x:(r2+n2),x1
	move	x:L_E,a
	or	x1,a
	move	a1,x:L_E
	move	x0,a
	move	#>$f,x1
	and	x1,a
	move	a1,n2
	nop
	move	x:(r2+n2),x1
	or	x1,b
	move	x:L_E,a
	rts

; a1 = the row's coverage (a bit per pixel with a colour) -> x0 its flags:
; TC_CLEAR (1) none, TC_SOLID (2) all eight
flags:
	move	#>$ff,x0
	and	x0,a
	jeq	_c
	cmp	x0,a
	jeq	_s
	clr	a
	move	a1,x0
	rts
_c:	move	#>1,x0
	rts
_s:	move	#>2,x0
	rts

; ---------------------------------------------------------------------------
; FRAME: the stream (ppu_tc.h), then the picture.
cmd_frame:
	jsr	get			; words that follow
	move	a1,x0
	move	#STREAM,r0
	do	x0,_fr
	jsr	get
	move	a1,x:(r0)+
	nop
_fr:
	jsr	render
	jmp	main

render:
	move	#STREAM,r0
	move	#SLOT,r1
	do	#NSLOT,_sl
	move	x:(r0)+,x0
	move	x0,x:(r1)+
_sl:
	move	x:(r0)+,a		; changes
	move	a1,x:CHGN
	move	r0,x:CHGP
	lsl	a
	move	a1,n0
	nop
	move	(r0)+n0
	move	x:(r0)+,a		; OAM segments
	move	a1,x:SEGN
	move	r0,x:SEGP
	tst	a
	jeq	_ss
	move	a1,x0
	do	x0,_sw
	move	(r0)+			; first row
	move	x:(r0)+,a		; sprites
	lsl	a
	move	a1,n0
	nop
	move	(r0)+n0
_sw:
_ss:	move	x:(r0)+,a		; offset-per-tile: 0 or 66
	clr	b
	tst	a
	jeq	_so
	move	r0,b
	move	a1,n0
	nop
	move	(r0)+n0
_so:	move	b1,x:OPTP
	move	#EW,r1			; CGRAM at row 0 -> E
	do	#256,_cg
	move	x:(r0)+,a
	jsr	expand
	move	a1,y:(r1)+
_cg:
	if	TC7
	move	r0,x:M7P		; the Mode 7 section: an entry per row
	endif
	clr	a
	move	a1,x:CURN
	move	a1,x:ROW
	move	a1,x:TXSTART
	move	a1,x:BRIGHT		; (none: the tables are built at row 0)
	move	#>1,a
	move	a1,x:WDIRTY
	move	a1,x:BDIRTY
	move	a1,x:CDIRTY
	move	a1,x:LDIRTY
	move	a1,x:SDIRTY
	ori	#$03,mr			; the ring: empty (all marked sent)
	bclr	#TIE,x:m_crb
	move	#RING,r6
	move	#RING,r7
	move	#>1,a
	rep	#512
	move	a,y:(r7)+
	andi	#$fc,mr
_row:	jsr	apply
	move	x:ROW,a			; outside the band: walked, not drawn
	move	x:BAND0,x0
	cmp	x0,a
	jlt	_skip
	jsr	room
	jsr	row
	jsr	gate
_nx:	move	x:ROW,a
	move	#>1,x0
	add	x0,a
	move	a1,x:ROW
	move	x:BAND1,x0		; (the band's end: the rest is not drawn)
	cmp	x0,a
	jlt	_row
_dr:	jsr	gate
	jsr	fill
	tst	a
	jne	_dr
	bclr	#TIE,x:m_crb
	clr	a			; the next frame: whole unless BAND says
	move	a1,x:BAND0
	move	#>224,a
	move	a1,x:BAND1
	rts
_skip:
	if	TC7
	jsr	m7_skip			; its Mode 7 entry
	endif
	jmp	_nx

; a1 = BGR555 -> a1 = r | g << 8 | b << 16
expand:
	move	a1,b
	move	#>$1f,x0
	and	x0,a			; r
	move	a1,y0
	move	b1,a
	rep	#5
	lsr	a
	and	x0,a			; g
	rep	#8
	lsl	a
	move	a1,y1
	move	b1,a
	rep	#10
	lsr	a
	and	x0,a			; b
	rep	#16
	lsl	a
	or	y1,a
	or	y0,a
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

; The changes that start at this row, and its OAM segment. A CGRAM change
; sets its E and T565 entries; window slots and TM/TS mark WDIRTY,
; INIDISP BDIRTY.
apply:
	move	x:ROW,a
	rep	#8
	lsl	a
	move	a1,x:ROWK		; row << 8
_ch:	move	x:CHGN,a
	tst	a
	jeq	_seg
	move	x:CHGP,r0
	move	#>$ffff00,x1
	nop
	move	x:(r0),a
	and	x1,a
	move	x:ROWK,x0
	cmp	x0,a
	jgt	_seg			; a later row's
	move	x:(r0)+,a
	move	#>$ff,x1
	and	x1,a
	move	a1,x:TMP0		; the slot
	move	x:(r0)+,b		; the value
	move	r0,x:CHGP
	move	x:CHGN,a
	move	#>1,x0
	sub	x0,a
	move	a1,x:CHGN
	move	x:TMP0,a
	move	#>SLOT_CG,x0
	cmp	x0,a
	jeq	_cg
	move	#SLOT,r1
	move	a1,n1
	nop
	move	b1,x:(r1+n1)
	move	#>1,x1			; the dirty marks
	tst	a			; INIDISP: the brightness only
	jne	_nz
	move	x1,x:BDIRTY
	jmp	_ch
_nz:	move	#>S_SUB,x0		; S_SUB: not read here; S_MATH and S_FIXED:
	cmp	x0,a			; the colour only (math_row every row)
	jeq	_ch
	move	#>S_MATH,x0
	cmp	x0,a
	jge	_ch
	move	x1,x:LDIRTY		; the layers, the sprites
	move	x1,x:SDIRTY
_n0:	move	#>S_TM,x0
	cmp	x0,a
	jeq	_wd
	move	#>S_TS,x0
	cmp	x0,a
	jeq	_wd
	move	#>S_TSW,x0
	cmp	x0,a
	jeq	_wd
	move	#>S_WSEL,x0
	cmp	x0,a
	jlt	_ch
	move	#>S_WLOG,x0
	cmp	x0,a
	jgt	_ch
_wd:	move	x1,x:WDIRTY
	jmp	_ch
_cg:	move	b1,x:TMP1		; index << 16 | BGR555
	move	b1,a
	move	#>$7fff,x0
	and	x0,a
	jsr	expand			; (uses b, x0, y0, y1)
	move	a1,y1
	move	x:TMP1,a
	rep	#16
	lsr	a
	move	a1,n1
	move	#EW,r1
	move	#T565,r2
	move	a1,n2
	move	y1,y:(r1+n1)
	move	y1,a
	jsr	t565
	move	a1,y:(r2+n2)
	move	#>1,x0
	move	x0,x:CDIRTY
	jmp	_ch
_seg:	move	x:SEGN,a		; the next OAM segment, from its first row
	tst	a
	jeq	_x
	move	x:SEGP,r0
	nop
	move	x:(r0)+,a
	move	x:ROW,x0
	cmp	x0,a
	jgt	_x
	move	x:(r0)+,a		; its sprites
	move	r0,x:CURS
	move	a1,x:CURN
	move	#>1,x0
	move	x0,x:SDIRTY
	lsl	a
	move	a1,n0
	nop
	move	(r0)+n0
	move	r0,x:SEGP
	move	x:SEGN,a
	move	#>1,x0
	sub	x0,a
	move	a1,x:SEGN
	jmp	_seg
_x:	rts

; a1 = an E word -> a1 = its RGB565 at the tables' brightness, bits 23-8
t565:
	move	a1,b
	move	#>$1f,x0
	and	x0,a
	move	a1,n4
	move	#TCOMP,r4
	nop
	move	y:(r4+n4),y0		; r
	move	b1,a
	rep	#8
	lsr	a
	and	x0,a
	move	a1,n4
	move	#TCOMP+32,r4
	nop
	move	y:(r4+n4),y1		; g
	move	b1,a
	rep	#16
	lsr	a
	and	x0,a
	move	a1,n4
	move	#TCOMP+64,r4
	nop
	move	y:(r4+n4),a		; b
	or	y0,a
	or	y1,a
	rts

; The tables that hold the brightness: TCOMP (per component 0-31 its
; RGB565 bits), T565 from E, TCH.
tables:
	move	#>1,x0
	move	x0,x:CDIRTY
	move	x:SLOT+S_INIDISP,a
	move	#>$f,x0
	and	x0,a
	move	#>1,x0
	add	x0,a
	move	a1,x:BRIGHT		; brightness + 1
	move	#TCOMP,r4
	clr	b
	move	b1,x:TMP0		; c
	do	#32,_tc
	move	x:TMP0,x0
	move	x:BRIGHT,y0
	mpy	x0,y0,a			; c * (b + 1) << 1, in A0
	rep	#19
	asl	a			; a1 = c * (b + 1) >> 4
	move	a1,x:TMP1		; v
	rep	#19
	lsl	a			; r: v << 19
	move	a1,y:(r4)
	move	#32,n4
	move	x:TMP1,a		; g: (v << 1 | v >> 4) << 13
	move	a1,b
	lsl	a
	rep	#4
	lsr	b
	move	b1,x0
	or	x0,a
	rep	#13
	lsl	a
	move	a1,y:(r4+n4)
	move	#64,n4
	move	x:TMP1,a		; b: v << 8
	rep	#8
	lsl	a
	move	a1,y:(r4+n4)
	move	(r4)+
	move	x:TMP0,a
	move	#>1,x0
	add	x0,a
	move	a1,x:TMP0
	nop
_tc:
	move	#EW,r1			; T565 from E
	move	#T565,r2
	do	#256,_t5
	move	y:(r1)+,a
	jsr	t565
	move	a1,y:(r2)+
_t5:
	; TCH[s * 192 + ch * 64 + i], s = subtract * 2 + half: i is the
	; channel's sum (or m - s + 32); clamped to 0-31, halved first
	move	#TCH,r2
	clr	b
	move	b1,x:TMP0		; s
	do	#4,_ts
	clr	b
	move	b1,x:TMP1		; ch
	do	#3,_tch
	clr	b
	move	b1,x:TMP2		; i
	do	#64,_ti
	move	x:TMP2,a
	jclr	#1,x:TMP0,_add
	move	#>32,x0
	sub	x0,a
	jge	_add
	clr	a
_add:	jclr	#0,x:TMP0,_nh
	asr	a
_nh:	move	#>31,x0
	cmp	x0,a
	jle	_cl
	move	x0,a
_cl:	move	x:TMP1,b		; TCOMP + 32 ch + v
	rep	#5
	lsl	b
	add	a,b
	move	#>TCOMP,x0
	add	x0,b
	move	b1,r4
	nop
	move	y:(r4),x0
	move	x0,y:(r2)+
	move	x:TMP2,a
	move	#>1,x0
	add	x0,a
	move	a1,x:TMP2
	nop
_ti:
	move	x:TMP1,a
	move	#>1,x0
	add	x0,a
	move	a1,x:TMP1
	nop
_tch:
	move	x:TMP0,a
	move	#>1,x0
	add	x0,a
	move	a1,x:TMP0
	nop
_ts:
	rts

; ---------------------------------------------------------------------------
; One row into the ring.
row:
	if	TC7
	jclr	#7,x:SLOT+S_INIDISP,_on
	jsr	m7_skip			; every row has its entry
	jmp	blank_row
_on:
	else
	jset	#7,x:SLOT+S_INIDISP,blank_row
	endif
	move	x:BDIRTY,a		; brightness: the tables again when it moved
	tst	a
	jeq	_bd
	clr	a
	move	a1,x:BDIRTY
	move	x:SLOT+S_INIDISP,a
	move	#>$f,x0
	and	x0,a
	move	#>1,x0
	add	x0,a
	move	x:BRIGHT,x0
	cmp	x0,a
	jeq	_bd
	jsr	tables
_bd:	move	x:WDIRTY,a
	tst	a
	jeq	_wd
	clr	a
	move	a1,x:WDIRTY
	jsr	windows
_wd:	jsr	math_row
	move	x:SLOT+S_BGMODE,a
	move	#>7,x0
	and	x0,a
	move	a1,x:MODE
	if	TC7==0
	cmp	x0,a
	jeq	blank_row		; Mode 7: the TC7 build
	endif
	move	#>$500,a		; MAIN, SUB: the backdrop (source 5)
	move	#MAIN,r0
	rep	#272
	move	a,y:(r0)+
	move	x:SLOT+S_TS,a		; SUB is read only where a layer goes into it
	move	#>$1f,x0		; or the colour math takes the sub screen
	and	x0,a
	jne	_cs
	move	x:MATHF,a
	tst	a
	jeq	_ns
	move	x:SUBON,a
	tst	a
	jeq	_ns
_cs:	move	#>$500,a
	move	#SUB,r0
	rep	#272
	move	a,y:(r0)+
_ns:	move	#>1,a
	move	a1,x:MFRESH
	move	a1,x:SFRESH
	if	TC7
	move	x:SLOT+S_TM,a		; BG1 on either screen: the Mode 7 layer
	move	x:SLOT+S_TS,x0
	or	x0,a
	jclr	#0,a1,_m7n
	clr	a
	move	a1,x:LAY
	move	x:SLOT+S_TM,a		; on one screen and no window there:
	move	x:SLOT+S_TS,x0		; straight into its line (the first layer
	eor	x0,a			; drawn: the line holds the backdrop)
	jclr	#0,a1,_m7l
	move	#>MAIN+8,b
	move	x:MASKS,x0
	jset	#0,x:SLOT+S_TM,_m7s
	move	#>SUB+8,b
	move	x:MASKS+5,x0
_m7s:	move	x0,a
	tst	a
	jne	_m7l
	move	b1,x:M7T
	jsr	m7_line
	jmp	_m7d
_m7l:	move	#>LK+8,x0
	move	x0,x:M7T
	jsr	m7_line
	jsr	merges
	jmp	_m7d
_m7n:	jsr	m7_skip
_m7d:
	else
	clr	a
	move	a1,x:LAY
_ly:	move	x:MODE,a		; BG layers: bpp by mode and layer
	lsl	a
	lsl	a
	move	x:LAY,x0
	add	x0,a
	move	#MBPP,r0
	move	a1,n0
	nop
	move	y:(r0+n0),a
	tst	a
	jeq	_ln
	move	a1,x:BPP
	move	x:SLOT+S_TM,a		; on either screen
	move	x:SLOT+S_TS,x0
	or	x0,a
	move	x:LAY,n0
	move	#POW2,r0
	nop
	move	y:(r0+n0),x0
	and	x0,a
	jeq	_ln
	jsr	bg_line
	move	x:L_DIRECT,a
	tst	a
	jne	_dn
	jsr	merges
	jmp	_ln
_dn:	clr	a			; its line holds its keys now
	move	x:L_LINE,x0
	move	#>MAIN,b
	cmp	x0,b
	jne	_dns
	move	a1,x:MFRESH
	jmp	_ln
_dns:	move	a1,x:SFRESH
_ln:	move	x:LAY,a
	move	#>1,x0
	add	x0,a
	move	a1,x:LAY
	move	#>4,x0
	cmp	x0,a
	jlt	_ly
	clr	a			; the layers' descriptors are this row's
	move	a1,x:LDIRTY
	endif
	move	x:SLOT+S_TM,a		; sprites
	move	x:SLOT+S_TS,x0
	or	x0,a
	jclr	#4,a1,_no
	move	x:CURN,a
	tst	a
	jeq	_no
	if	TC7==0
	move	x:SDIRTY,a		; SPK (and the band) built again first
	tst	a
	jne	_ol
	move	x:ROW,a			; rows no sprite of the segment touches
	move	x:OBAND0,x0
	cmp	x0,a
	jlt	_no
	move	x:OBAND1,x0
	cmp	x0,a
	jge	_no
	endif
_ol:	jsr	obj_line
	move	#>4,x0
	move	x0,x:LAY
	jsr	merges
_no:	jsr	colour
	rts

blank_row:
	clr	a
	rep	#256
	move	a,y:(r7)+
	rts

	if	TC7
; ---------------------------------------------------------------------------
; The row's Mode 7 entry (ppu_dsp.h; tc_model.c m7_line) into M7T (LK,
; MAIN or SUB + 8): per segment INSIDE (the plane through its map), TILE0
; (character 0) or FILL (transparent); the rest of the row transparent.
; Transparent is the backdrop's key $500: no key below it, so the same in
; LK as in a line.
m7_line:
	move	x:M7P,r0
	move	x:M7T,r3
	move	x:(r0)+,a		; segments
	move	r0,x:M7P
	move	a1,x:M7N
	tst	a
	jeq	_rest
_seg:	move	x:M7P,r0
	nop
	move	x:(r0)+,b		; kind << 16 | count
	move	b1,x:M7C
	move	#>$030000,x0
	cmp	x0,b
	jge	_fill
	move	x:(r0)+,a		; v (a0 cleared)
	move	x:(r0)+,a0		; u + $800000
	move	x:(r0)+,x1		; the step, high and low
	move	x:(r0)+,x0
	move	r0,x:M7P
	move	x1,x:STEP
	move	x0,y:STEP
	move	#>$008000,y1
	move	#KEY7,r0
	move	#TVF,n4
	move	#>$020000,x0
	cmp	x0,b
	jge	_t0
	jsr	_count
	move	#TVM,n1
	move	#TUF+$8000,n5		; (tu - $8000 as the index)
	move	y:K1000,x1
	jsr	m7_inside
	jmp	_next
_t0:	jsr	_count
	move	#TUF,n5
	move	#$2600,r2		; character 0 (tc_m7_p(0))
	move	y:K3FF,x1
	jsr	m7_tile0
	jmp	_next
_fill:	move	r0,x:M7P
	jsr	_count
	move	#>$500,b
	rep	y0
	move	b,y:(r3)+
_next:	move	x:M7N,a
	move	#>1,x0
	sub	x0,a
	move	a1,x:M7N
	jne	_seg
_rest:	move	r3,a			; up to the line's end (x 256)
	move	x:M7T,b
	move	#>256,x0
	add	x0,b
	sub	a,b
	jle	_x
	move	b1,y0
	move	#>$500,b
	rep	y0
	move	b,y:(r3)+
_x:	rts
_count:	move	x:M7C,b			; y0 = the segment's count
	move	#>$ffff,y0
	and	y0,b
	move	b1,y0
	rts

; A blank row's entry, or one whose BG1 is off: stepped over.
m7_skip:
	move	x:M7P,r0
	nop
	move	x:(r0)+,a
	tst	a
	jeq	_x
	move	a1,x0
	move	#4,n0
	do	x0,_sk
	move	x:(r0)+,b
	move	#>$030000,x1
	cmp	x1,b
	jge	_f
	move	(r0)+n0			; v, u, the step
_f:	nop
_sk:
_x:	move	r0,x:M7P
	rts

	hotcode
; INSIDE: y0 pixels from a1 = v, a0 = u + $800000 (1/256 texel) on, the
; step in L:STEP; x1 = $001000 (a multiply by it: >> 11), y1 = $008000
; (>> 8); n1 = TVM, n4 = TVF, n5 = TUF + $8000, r0 = KEY7, r3 -> LK.
m7_inside:
	do	y0,_ie
	move	a1,y0
	mpy	x1,y0,b				; b1 = tv >> 3: the map row
	mpy	y0,y1,b		b1,r1		; b1 = tv
	move	b1,r4
	move	a0,y0
	mpy	x1,y0,b		x:(r1+n1),r2	; b1 = (tu >> 3) - $1000; r2 = the row
	mpy	y0,y1,b		b1,n2		; b1 = tu - $8000
	move	b1,r5
	movem	p:(r2+n2),y0			; the map entry: P(t) << 8 | ...
	mpy	y0,y1,b		x:(r4+n4),n2	; b1 = P(t); n2 = (tv & 7) * 8
	move	l:STEP,x
	add	x,a		b1,r2		; the next pixel's v, u
	move	y:(r5+n5),y0			; tu & 7
	lua	(r2)+n2,r2
	move	y0,n2
	move	y:KFF,y0
	movem	p:(r2+n2),b			; the texel word
	and	y0,b		y:K1000,x1
	move	b1,n0
	nop
	move	y:(r0+n0),b			; its key
	move	b1,y:(r3)+
_ie:
	rts

; TILE0: character 0's texels (outside the plane, M7SEL = $C0): tv and tu
; masked to the tables' range. x1 = $3ff, r2 = its P address, n5 = TUF;
; otherwise as m7_inside.
m7_tile0:
	do	y0,_te
	move	a1,y0
	mpy	y0,y1,b		a0,y0		; b1 = tv
	and	x1,b
	mpy	y0,y1,b		b1,r4		; b1 = tu - $8000
	and	x1,b		l:STEP,x
	add	x,a		b1,r5
	move	x:(r4+n4),n2			; (tv & 7) * 8
	move	y:(r5+n5),y0			; tu & 7
	lua	(r2)+n2,r1
	move	y0,n1
	move	y:KFF,y0
	movem	p:(r1+n1),b
	and	y0,b		y:K3FF,x1
	move	b1,n0
	nop
	move	y:(r0+n0),b
	move	b1,y:(r3)+
_te:
	rts
	extcode
	endif

; LK into MAIN where TM has layer LAY, into SUB where TS has it.
merges:
	move	x:LAY,a			; BG layers: the whole line (obj_line sets
	move	#>4,x0			; the sprites' range)
	cmp	x0,a
	jeq	_ob
	if	TC7
	move	#>8,a			; (Mode 7: LK's pads are not written)
	move	a1,x:M_LO
	move	#>256,x0
	else
	clr	a
	move	a1,x:M_LO
	move	#>272,x0
	endif
	move	x0,x:M_CNT
_ob:	move	x:LAY,n0
	move	#POW2,r0
	nop
	move	y:(r0+n0),x1		; the layer's bit
	move	x:SLOT+S_TM,a
	and	x1,a
	jeq	_ms
	move	#MASKS,r0		; its main-screen mask
	move	x:LAY,n0
	nop
	move	x:(r0+n0),y1
	move	#MAIN,r1
	clr	a
	move	a1,x:MFRESH
	jsr	merge
_ms:	move	x:SLOT+S_TS,a
	and	x1,a
	jeq	_x
	move	#MASKS+5,r0
	move	x:LAY,n0
	nop
	move	x:(r0+n0),y1
	move	#SUB,r1
	clr	a
	move	a1,x:SFRESH
	jsr	merge
_x:	rts

	hotcode
; LK into the line at r1 (the greater key), except where the 4-bit mask
; y1 hides the layer (bit WC[x] set).
merge:
	move	#LK,r0
	move	y1,a
	tst	a
	jne	_win
	move	x:M_CNT,a		; (the sprites: only where they were drawn)
	tst	a
	jeq	_x
	move	x:M_LO,n0
	move	n0,n1
	move	a1,x0
	move	(r0)+n0
	move	(r1)+n1
	do	x0,_m
	move	y:(r0)+,a
	move	y:(r1),b
	cmp	a,b
	tlt	a,b
	move	b,y:(r1)+
_m:
_x:	rts
_win:	move	#8,n0			; per run of one window code (WSEG):
	move	#8,n1			; stepped over where the mask hides it,
	nop				; else the loop above
	move	(r0)+n0
	move	(r1)+n1
	move	#WSEG,r2
	move	#POW2,r4
	nop
	move	x:(r2)+,x0
	do	x0,_sg
	move	x:(r2)+,n5		; its pixels
	move	x:(r2)+,n4		; its code
	move	n5,n0
	move	n5,n1
	move	y:(r4+n4),x0
	move	y1,b
	and	x0,b
	jne	_hs
	do	n5,_sm
	move	y:(r0)+,a
	move	y:(r1),b
	cmp	a,b
	tlt	a,b
	move	b,y:(r1)+
_sm:
	jmp	_nx
_hs:	move	(r0)+n0
	move	(r1)+n1
_nx:	nop
_sg:
	rts

	extcode

; ---------------------------------------------------------------------------
; WC and the window masks (snes_ppu.c win_spans): a mask's bit c (c = WC[x]:
; in window 1 | in window 2 << 1) is set where the window area hides the
; layer (TMW / TSW), or where the colour window is (CWMASK).
windows:
	clr	a
	move	#WC,r0
	rep	#256
	move	a,y:(r0)+
	move	x:SLOT+S_WH01,a
	move	#>1,y1
	jsr	wc_fill
	move	x:SLOT+S_WH23,a
	move	#>2,y1
	jsr	wc_fill
	move	#WC,r0			; WC's runs (two windows: five at most)
	move	#WSEG+1,r1
	move	#0,r2
	move	y:(r0)+,x0		; the run's code
	move	#>1,a			; its pixels
	move	#>1,y0
	do	#255,_rs
	move	y:(r0)+,b
	cmp	x0,b
	jeq	_same
	move	a1,x:(r1)+
	move	x0,x:(r1)+
	move	(r2)+
	move	b1,x0
	clr	a
_same:	add	y0,a
_rs:
	move	a1,x:(r1)+
	move	x0,x:(r1)+
	move	(r2)+
	move	r2,x:WSEG
	move	#MASKS,r5
	clr	a
	move	a1,x:TMP0		; L
	do	#5,_mk
	move	x:SLOT+S_WLOG,a		; TMW
	rep	#16
	lsr	a
	move	a1,x:TMP1
	jsr	wmask
	move	a1,x:(r5)
	move	#5,n5
	move	x:SLOT+S_TSW,x0
	move	x0,x:TMP1
	jsr	wmask
	move	a1,x:(r5+n5)
	move	(r5)+
	move	x:TMP0,a
	move	#>1,x0
	add	x0,a
	move	a1,x:TMP0
	nop
_mk:
	move	#>5,x0			; the colour window: always on
	move	x0,x:TMP0
	move	#>$ff,x0
	move	x0,x:TMP1
	jsr	wmask
	move	a1,x:CWMASK
	rts

; a1 = l | r << 8: WC[l..r] |= y1 when l <= r
wc_fill:
	move	a1,b
	move	#>$ff,x0
	and	x0,a			; l
	rep	#8
	lsr	b
	and	x0,b			; r
	cmp	a,b
	jlt	_x
	sub	a,b
	move	#>1,x0
	add	x0,b			; count
	move	#WC,r0
	move	a1,n0
	nop
	move	(r0)+n0
	move	b1,x0
	do	x0,_f
	move	y:(r0),a
	or	y1,a
	move	a1,y:(r0)+
_f:
_x:	rts

; TMP0 = layer (0-4 BG1-4, OBJ; 5 the colour window), TMP1 = the mask
; register -> a1 = the 4-bit mask. As bit vectors over c: window 1 is
; 1010, window 2 1100 (inverted by their select bits).
wmask:
	move	x:TMP0,n4
	move	#POW2,r4
	nop
	move	y:(r4+n4),x0
	move	x:TMP1,a
	and	x0,a
	jeq	_z
	move	x:TMP0,a		; sel = WSEL >> 4L & 15
	lsl	a
	lsl	a
	move	a1,n4
	move	x:SLOT+S_WSEL,a
	jsr	shr_n4
	move	#>$f,x0
	and	x0,a
	move	a1,x:TMP2
	move	x:TMP0,a		; logic = WLOG >> 2L & 3
	lsl	a
	move	a1,n4
	move	x:SLOT+S_WLOG,a
	jsr	shr_n4
	move	#>3,x0
	and	x0,a
	move	a1,x:TMP3
	move	#>$a,a			; A: window 1, B: window 2
	jclr	#0,x:TMP2,_a
	move	#>$5,a
_a:	move	#>$c,b
	jclr	#2,x:TMP2,_b
	move	#>$3,b
_b:	move	a1,x0
	move	b1,x1
	jclr	#1,x:TMP2,_n1		; only window 2 enabled: B
	jclr	#3,x:TMP2,_ra		; only window 1: A
	move	x:TMP3,b		; both: the logic
	tst	b
	jeq	_or
	move	#>1,y0
	cmp	y0,b
	jeq	_and
	move	#>2,y0
	cmp	y0,b
	jeq	_xor
	eor	x1,a			; XNOR
	not	a
	move	#>$f,x0
	and	x0,a
	rts
_or:	or	x1,a
	rts
_and:	and	x1,a
	rts
_xor:	eor	x1,a
	rts
_ra:	rts
_n1:	jclr	#3,x:TMP2,_z
	move	x1,a
	rts
_z:	clr	a
	rts

; a1 >>= n4 (n4 < 24)
shr_n4:
	move	n4,b
	tst	b
	jeq	_x
	do	n4,_s
	lsr	a
	nop
_s:
_x:	rts

; The row's colour math (snes_ppu.c rgb_row): CLIP0/MATH0 outside the
; colour window, CLIP1/MATH1 inside, ENTAB per source, HALF, SUBTR, SUBON,
; FIXEDE; MATHF = 0 when no pixel can be clipped or take part.
math_row:
	move	x:SLOT+S_MATH,a
	move	#>$ff,x0
	and	x0,a
	move	a1,x:TMP0		; CGWSEL
	move	x:SLOT+S_MATH,a
	rep	#8
	lsr	a
	and	x0,a
	move	a1,x:TMP1		; CGADSUB
	clr	b
	move	#>1,y1
	move	x:TMP0,a		; clip mode: 0 never, 1 outside, 2 inside, 3 always
	rep	#6
	lsr	a
	move	a1,x:TMP2
	move	b1,x:CLIP0
	move	b1,x:CLIP1
	move	#>3,x0
	cmp	x0,a
	jne	_c1
	move	y1,x:CLIP0
	move	y1,x:CLIP1
_c1:	move	#>1,x0
	cmp	x0,a
	jne	_c2
	move	y1,x:CLIP0
_c2:	move	#>2,x0
	cmp	x0,a
	jne	_c3
	move	y1,x:CLIP1
_c3:	move	x:TMP0,a		; math mode: 0 always, 1 inside, 2 outside, 3 never
	rep	#4
	lsr	a
	move	#>3,x0
	and	x0,a
	move	a1,x:TMP3
	move	b1,x:MATH0
	move	b1,x:MATH1
	tst	a
	jne	_m1
	move	y1,x:MATH0
	move	y1,x:MATH1
_m1:	move	#>1,x0
	cmp	x0,a
	jne	_m2
	move	y1,x:MATH1
_m2:	move	#>2,x0
	cmp	x0,a
	jne	_m3
	move	y1,x:MATH0
_m3:	move	#ENTAB,r0		; ENTAB[s] = CGADSUB bit s; OBJ: palettes
	move	x:TMP1,a		; 4-7 only (source 6)
	do	#6,_en
	move	a1,b
	and	y1,b
	move	b1,x:(r0)+
	lsr	a
_en:
	move	x:ENTAB+4,x0
	move	x0,x:ENTAB6
	clr	b
	move	b1,x:ENTAB+4
	move	x:TMP1,a
	clr	b
	jclr	#6,a1,_h
	move	y1,b
_h:	move	b1,x:HALF
	clr	b
	jclr	#7,a1,_s
	move	y1,b
_s:	move	b1,x:SUBTR
	move	x:TMP0,a
	clr	b
	jclr	#1,a1,_o
	move	y1,b
_o:	move	b1,x:SUBON
	move	x:SLOT+S_FIXED,a
	jsr	expand			; (uses y1)
	move	a1,x:FIXEDE
	move	#>1,y1
	clr	b			; MATHF
	move	x:TMP2,a		; clipping somewhere
	tst	a
	jne	_mf
	move	x:TMP3,a		; math never
	move	#>3,x0
	cmp	x0,a
	jeq	_nf
	move	x:TMP1,a
	move	#>$3f,x0
	and	x0,a
	jeq	_nf
_mf:	move	y1,b
_nf:	move	b1,x:MATHF
	clr	b			; CASEW: neither the clip nor the math
	move	x:TMP2,a		; depends on the colour window (modes 0
	jsr	_zt			; or 3 both)
	jne	_nw
	move	x:TMP3,a
	jsr	_zt
	jne	_nw
	move	y1,b
_nw:	move	b1,x:CASEW
	clr	b			; CASEB: CASEW and the fixed colour (or an
	move	x:CASEW,a		; empty sub screen: TS 0)
	tst	a
	jeq	_nb
	move	x:SUBON,a
	tst	a
	jeq	_fb
	move	x:SLOT+S_TS,a
	move	#>$1f,x0
	and	x0,a
	jne	_nb
_fb:	move	y1,b
_nb:	move	b1,x:CASEB
	rts
_zt:	tst	a			; Z: a is 0 or 3
	jeq	_y
	move	#>3,x0
	cmp	x0,a
_y:	rts

	if	TC7==0
; ---------------------------------------------------------------------------
; Layer LAY's row into LK (tc_model.c layer_line): 33 slivers from the
; 8-pixel boundary at or left of pixel 0 (LK + 8 - fine).
bg_line:
	move	x:LDIRTY,a		; nothing changed and the layer drawn in the
	tst	a			; row before: its results are the same
	jne	bg_full
	move	#LROW,r0
	move	x:LAY,n0
	nop
	move	x:(r0+n0),a
	move	#>1,x0
	add	x0,a
	move	x:ROW,x0
	cmp	x0,a
	jne	bg_full
	jmp	bg_cached
bg_full:
	move	x:BPP,a			; formats by bpp: fi, field multiplier,
	move	#>2,x0			; words per character (shift)
	cmp	x0,a
	jne	_b4
	clr	b
	move	#>4,x1
	move	#>3,y1
	jmp	_bs
_b4:	move	#>4,x0
	cmp	x0,a
	jne	_b8
	move	#>1,b
	move	#>16,x1
	move	#>4,y1
	jmp	_bs
_b8:	move	#>2,b
	move	#>256,x1
	move	#>5,y1
_bs:	move	b1,x:L_FI
	move	x1,x:L_FMUL
	move	y1,x:L_WSH
	move	#>4,x0			; 2^(wsh - 1)
	move	y1,a
	move	#>3,y0
	cmp	y0,a
	jeq	_w3
	move	#>8,x0
	move	#>4,y0
	cmp	y0,a
	jeq	_w3
	move	#>16,x0
_w3:	move	x0,x:L_WMUL
	move	x:LAY,n0		; BGnSC: map base, wide, tall
	move	#SLOT+S_SC,r0
	nop
	move	x:(r0+n0),a
	move	a1,x:TMP0
	move	#>$fc,x0
	and	x0,a
	rep	#8
	lsl	a
	move	a1,x:L_MAP
	move	x:LAY,a			; character base
	lsr	a
	move	a1,n0
	move	#SLOT+S_NBA,r0
	nop
	move	x:(r0+n0),a
	jclr	#0,x:LAY,_lo
	rep	#4
	lsr	a
_lo:	move	#>$f,x0
	and	x0,a
	rep	#12
	lsl	a
	move	a1,x:L_CHR
	move	x:LAY,a			; 16x16 tiles: BGMODE bit 4 + LAY
	move	#>4,x0
	add	x0,a
	move	a1,n4
	move	#POW2,r4
	nop
	move	y:(r4+n4),x0
	move	x:SLOT+S_BGMODE,a
	and	x0,a
	clr	b
	move	#>3,y1			; tile shift
	move	#>8,x1			; tile
	tst	a
	jeq	_sm
	move	#>1,b
	move	#>4,y1
	move	#>16,x1
_sm:	move	b1,x:L_BIG
	move	y1,x:L_TSH
	move	x:TMP0,a		; map width - 1: tile * (32 or 64) - 1
	move	x1,b
	rep	#5
	lsl	b
	clr	a
	jclr	#0,x:TMP0,_nw
	lsl	b
	move	#>1,a
_nw:	move	a1,x:L_WIDE
	move	#>1,x0
	sub	x0,b
	move	b1,x:L_MW
	move	x1,b
	rep	#5
	lsl	b
	jclr	#1,x:TMP0,_nt
	lsl	b
_nt:	sub	x0,b
	move	b1,x:L_MH
	move	x:LAY,a			; H and V scroll
	lsl	a
	move	a1,n0
	move	#SLOT+S_OFS,r0
	nop
	move	x:(r0+n0),a
	move	#>$3ff,x0
	and	x0,a
	move	x:L_MW,x0
	and	x0,a
	move	a1,b
	move	#>7,x0
	and	x0,b
	move	b1,x:L_FINE
	move	#>$fffff8,x0
	and	x0,a
	move	a1,x:L_PX0
	move	(r0)+
	nop
	move	x:(r0+n0),a
	move	#>$3ff,x0
	and	x0,a
	move	a1,x:VSC
	move	x:ROW,x0
	add	x0,a
	move	#>1,x0
	add	x0,a
	move	x:L_MH,x0
	and	x0,a
	move	a1,x:L_Y0
	move	x:MODE,a		; depths: DEPTH[mode * 4 + LAY]
	lsl	a
	lsl	a
	move	x:LAY,x0
	add	x0,a
	move	a1,n4
	move	#DEPTH,r4
	nop
	move	y:(r4+n4),a
	move	a1,b
	move	#>$ff,x0
	and	x0,a			; low
	rep	#8
	lsr	b
	and	x0,b			; high
	move	x:MODE,y0		; mode 1, BG3, BGMODE bit 3: 17
	move	#>1,x1
	move	a1,x:TMP0
	move	y0,a
	cmp	x1,a
	jne	_d
	move	x:LAY,a
	move	#>2,x1
	cmp	x1,a
	jne	_d
	jclr	#3,x:SLOT+S_BGMODE,_d
	move	#>17,b
_d:	move	x:LAY,x1		; keys: depth << 16 | LAY << 8
	move	x1,a
	rep	#8
	lsl	a
	move	a1,y1
	rep	#16
	lsl	b
	or	y1,b
	move	b1,x:L_HI
	move	x:TMP0,a
	rep	#16
	lsl	a
	or	y1,a
	move	a1,x:L_LO
	clr	a			; palette base: mode 0 32 * LAY
	move	x:MODE,b
	tst	b
	jne	_pb
	move	x:LAY,a
	rep	#5
	lsl	a
_pb:	move	a1,x:L_PALB
	clr	b			; offset-per-tile: modes 2, 4, 6, BG1-2
	move	x:OPTP,a
	tst	a
	jeq	_op
	move	x:LAY,a
	move	#>2,x0
	cmp	x0,a
	jge	_op
	move	x:MODE,a
	jclr	#0,a1,_op2		; even modes 2, 4, 6
	jmp	_op
_op2:	tst	a
	jeq	_op
	move	#>7,x0
	cmp	x0,a
	jeq	_op
	move	x:OPTP,b
	jclr	#0,x:LAY,_op
	move	#>33,x0
	add	x0,b
_op:	move	b1,x:L_OPT
	jsr	bg_rowa
	; one screen and no window on it: straight into that line
	move	#>LK,x1
	clr	b
	move	x:LAY,n0
	move	#POW2,r0
	nop
	move	y:(r0+n0),y1		; the layer's bit
	move	x:SLOT+S_TM,a
	and	y1,a
	move	a1,x0
	move	x:SLOT+S_TS,a
	and	y1,a
	move	a1,y0
	move	#MASKS,r0
	move	x0,a			; main only?
	tst	a
	jeq	_ds
	move	y0,a
	tst	a
	jne	_dl
	move	x:(r0+n0),a
	tst	a
	jne	_dl
	move	#>MAIN,x1
	move	#>1,b
	jmp	_dl
_ds:	move	#MASKS+5,r0		; sub only
	nop
	move	x:(r0+n0),a
	tst	a
	jne	_dl
	move	#>SUB,x1
	move	#>1,b
_dl:	move	b1,x:L_DIRECT
	move	x1,x:L_LINE
	clr	a			; L_FRESH: straight into a fresh line
	tst	b
	jeq	_fr
	move	x:MFRESH,a
	move	#>MAIN,y0
	move	x1,b
	cmp	y0,b
	jeq	_fr
	move	x:SFRESH,a
_fr:	move	a1,x:L_FRESH
	move	x1,a			; the first sliver's word
	move	#>8,x0
	add	x0,a
	move	x:L_FINE,x0
	sub	x0,a
	move	a1,x:L_DST
	move	x:LAY,a			; descriptors: LAY * 33
	move	a1,b
	rep	#5
	lsl	a
	add	b,a
	move	a1,x:L_DBASE
	clr	b			; step when nothing changed and the layer
	move	x:LDIRTY,a		; was drawn in the row before
	tst	a
	jne	_bd
	move	#LROW,r0
	move	x:LAY,n0
	nop
	move	x:(r0+n0),a
	move	#>1,x0
	add	x0,a
	move	x:ROW,x0
	cmp	x0,a
	jne	_bd
	move	#>1,b
_bd:	move	b1,x:L_STEP
	move	#LROW,r0
	move	x:LAY,n0
	move	x:ROW,x0
	move	x0,x:(r0+n0)
	move	x:L_DST,x0
	move	x0,x:L_DST0
	jsr	bg_save
	move	x:L_STEP,a
	tst	a
	jne	step_sel
bg_rb:	move	x:L_OPT,a		; every sliver built
	tst	a
	jne	bg_rbo
	jset	#0,x:L_BIG,bg_rbo
	jmp	rb_all
bg_rbo:	clr	a
	move	a1,x:L_K
	move	a1,x:L_KI
	do	#33,_sv
	jsr	sliver
	move	x:L_DST,a
	move	#>8,x0
	add	x0,a
	move	a1,x:L_DST
	move	x:L_K,a
	add	x0,a
	move	a1,x:L_K
	move	x:L_KI,a
	move	#>1,x0
	add	x0,a
	move	a1,x:L_KI
	nop
_sv:
	rts

; A row of a layer whose setup the row before did: the results restored,
; its rows by the scroll.
bg_cached:
	jsr	bg_restore
	move	x:VSC,a
	move	x:ROW,x0
	add	x0,a
	move	#>1,x0
	add	x0,a
	move	x:L_MH,x0
	and	x0,a
	move	a1,x:L_Y0
	move	#>7,x0
	and	x0,a
	jeq	_ra
	move	x:L_OPT,a
	tst	a
	jeq	_nr
_ra:	jsr	bg_rowa
_nr:	clr	a			; L_FRESH: straight into a fresh line
	move	x:L_DIRECT,b
	tst	b
	jeq	_fr
	move	x:MFRESH,a
	move	#>MAIN,y0
	move	x:L_LINE,b
	cmp	y0,b
	jeq	_fr
	move	x:SFRESH,a
_fr:	move	a1,x:L_FRESH
	move	#>1,a
	move	a1,x:L_STEP
	move	#LROW,r0
	move	x:LAY,n0
	move	x:ROW,x0
	move	x0,x:(r0+n0)
	move	x:L_DST,x0
	move	x0,x:L_DST0
	jmp	step_sel

; bg_line's results for the next row of the layer (nothing changed): the
; variables of BGV, into (and from) the layer's save area
bg_save:
	move	x:LAY,x0		; the layer's save area: BGC + LAY * 24
	move	#>12,y0
	mpy	x0,y0,a
	move	a0,a
	move	#>BGC,x0
	add	x0,a
	move	a1,r0
	move	#BGV,r4
	do	#NBGV,_sv
	move	y:(r4)+,r2
	nop
	move	x:(r2),x0
	move	x0,x:(r0)+
_sv:
	rts

bg_restore:
	move	x:LAY,x0		; the layer's save area: BGC + LAY * 24
	move	#>12,y0
	mpy	x0,y0,a
	move	a0,a
	move	#>BGC,x0
	add	x0,a
	move	a1,r0
	move	#BGV,r4
	do	#NBGV,_rs
	move	y:(r4)+,r2
	move	x:(r0)+,x0
	move	x0,x:(r2)
_rs:
	rts

; The layer row's map row address by the registers
bg_rowa:
	move	x:L_Y0,b		; the row's map row by the registers
	lsr	b
	lsr	b
	lsr	b
	jclr	#0,x:L_BIG,_r8
	lsr	b
_r8:	move	x:L_MAP,a
	move	#>32,x0
	cmp	x0,b
	jlt	_r3
	move	#>$400,x1
	add	x1,a
	move	x:L_WIDE,y1
	jclr	#0,y1,_r4
	add	x1,a
_r4:	sub	x0,b
_r3:	rep	#5
	lsl	b
	add	b,a
	move	a1,x:L_ROWA
	rts

	hotcode
; A row of a layer that steps: each sliver from its descriptor (y + 1, the
; character row + DS), built again where it enters a new 8-row block.
step_row:
	move	x:L_DBASE,a		; r2: DESC + base * 5
	move	a1,b
	lsl	a
	lsl	a
	add	b,a
	move	#>DESC,x0
	add	x0,a
	move	a1,r2
	move	x:L_DST0,r5
	move	x:L_MH,y1
	do	#33,_st
	move	#>1,x0			; DY + 1
	move	x:(r2),a
	add	x0,a
	and	y1,a
	move	a1,x:(r2)+
	move	#>7,x0
	and	x0,a
	jeq	_bl			; a new block
	move	r2,r3			; DA + DS
	move	x:(r2)+,a
	move	x:(r2)+,x0
	add	x0,a
	move	a1,x:(r3)
	move	a1,r1
	move	x:(r2)+,x1		; DK
	move	x:(r2)+,b		; DF
	move	x:L_FMUL,y0
	move	r5,r4
	move	#1,n4
	tst	b
	jeq	_dr
	move	#7,n4
	nop
	move	(r4)+n4
	move	#-1,n4
_dr:	jsr	sl_draw
	jmp	_nx
_bl:	move	r2,x:L_SP		; built: k from the sliver's word
	move	r5,x:L_DST
	move	r5,a
	move	x:L_DST0,x0
	sub	x0,a
	move	a1,x:L_K
	lsr	a
	lsr	a
	lsr	a
	move	a1,x:L_KI
	jsr	sliver
	move	x:L_SP,r2
	move	#4,n2			; past the record's other four words
	move	x:L_MH,y1
	nop
	move	(r2)+n2
_nx:	move	#8,n5
	nop
	move	(r5)+n5
	nop
_st:
	rts

	extcode
	endif


	if	TC7==0
; The slivers of a fast row: r0 -> DA, r3 -> DK (five words apart), r5 the
; sliver's first LK word, n2 the fresh flag. Bit 22 of DA: a flipped sliver,
; drawn by the general code. Clear rows step over; solid rows are keys
; without a test (fresh line: stored, else the greater key); mixed rows by
; dsl_draw.
	hotcode
step_f4:
	do	#33,step_fx
	move	x:(r0),r1
	move	x:(r3)+n3,x1
	jset	#22,x:(r0),step_sl
	move	(r1)+
	move	r1,x:(r0)+n0
	movem	p:(r1),x0
	jset	#0,x0,step_fe		; clear
	jset	#1,x0,_so		; solid
	move	r5,r4			; mixed
	move	#1,n4
	jsr	dsl_draw
	move	#8,n1
	move	#8,n5
	jmp	step_fe
_so:	move	r5,r4
	movem	p:(r1+n1),a		; pixels 4-7
	jset	#0,n2,_sf
	do	#4,_n1			; not fresh: the greater key
	mpy	x0,y0,b		y:(r4),y1
	add	x1,b		b0,x0
	cmp	y1,b
	tlt	y1,b
	move	b,y:(r4)+
_n1:	move	a1,x0
	do	#4,_n2
	mpy	x0,y0,b		y:(r4),y1
	add	x1,b		b0,x0
	cmp	y1,b
	tlt	y1,b
	move	b,y:(r4)+
_n2:	jmp	step_fe
_sf:	mpy	x0,y0,b			; fresh: stored
	do	#4,_f1
	add	x1,b		b0,x0
	mpy	x0,y0,b		b1,y:(r4)+
_f1:	move	a1,x0
	mpy	x0,y0,b
	do	#4,_f2
	add	x1,b		b0,x0
	mpy	x0,y0,b		b1,y:(r4)+
_f2:
step_fe:	move	(r5)+n5
step_fx:	rts
	extcode

	endif

; a1 = VRAM word address, b1 = format index * 128 -> r1 = its converted
; word's P address (PT)
ptaddr:
	move	a1,x0
	move	#>$008000,y0		; page = word >> 8: one multiply
	move	b1,y1
	mpy	x0,y0,b
	move	b1,a
	add	y1,a
	move	a1,n0
	move	#PT,r0
	move	#>$ff,y0
	move	x0,a
	and	y0,a			; the word within the page
	move	x:(r0+n0),b
	add	a,b
	move	b1,r1
	rts

	if	TC7==0
; One sliver k (L_K = 8k) of layer LAY at L_DST.
sliver:
	move	x:L_PX0,a
	move	a1,x:L_HC
	move	x:L_Y0,a
	move	a1,x:L_Y
	move	x:L_OPT,a		; offset-per-tile: the sliver's own scroll
	tst	a
	jeq	_no
	move	x:L_K,b
	rep	#3
	lsr	b
	add	b,a
	move	a1,r0
	nop
	move	x:(r0),a
	move	a1,x:L_OPTW
	jclr	#23,a1,_nh
	move	#>$3f8,x0
	and	x0,a
	move	a1,x:L_HC
_nh:	move	x:L_OPTW,a
	jclr	#22,a1,_no
	rep	#10
	lsr	a
	move	#>$3ff,x0
	and	x0,a
	move	x:ROW,x0
	add	x0,a
	move	#>1,x0
	add	x0,a
	move	x:L_MH,x0
	and	x0,a
	move	a1,x:L_Y
_no:	move	x:L_HC,a		; px = (hc + 8k) & MW
	move	x:L_K,x0
	add	x0,a
	move	x:L_MW,x0
	and	x0,a
	move	a1,x:TMP0		; px
	move	x:L_Y,b			; tx = px >> tsh, ty = y >> tsh
	lsr	a
	lsr	a
	lsr	a
	lsr	b
	lsr	b
	lsr	b
	jclr	#0,x:L_BIG,_s8
	lsr	a
	lsr	b
_s8:	move	a1,y0			; tx
	move	b1,y1			; ty
	move	x:L_Y,a			; the registers' row: its address
	move	x:L_Y0,x0
	cmp	x0,a
	jne	_own
	move	x:L_ROWA,a
	move	y0,b
	move	#>32,x0
	cmp	x0,b
	jlt	_rw
	move	#>$400,x1
	add	x1,a
	sub	x0,b
_rw:	move	b1,x0
	add	x0,a
	jmp	_mw
_own:	move	x:L_MAP,a		; the map word: screens of 32 x 32
	move	#>32,x0
	move	y0,b
	cmp	x0,b
	jlt	_t2
	move	#>$400,x1
	add	x1,a
	sub	x0,b
	move	b1,y0
_t2:	move	y1,b
	cmp	x0,b
	jlt	_t3
	move	#>$400,x1
	add	x1,a
	move	x:L_WIDE,y1
	jclr	#0,y1,_t4
	add	x1,a			; wide: $800
_t4:	sub	x0,b
_t3:	rep	#5			; + ty * 32 + tx
	lsl	b
	add	b,a
	move	y0,x0
	add	x0,a
_mw:	move	#>$7fff,x0
	and	x0,a
	move	#>384,b			; raw
	jsr	ptaddr
	movem	p:(r1),a
	move	a1,x:L_E
	; half, fy, flips, the character
	move	x:TMP0,a		; half = (px >> 3) & 1
	rep	#3
	lsr	a
	move	#>1,x0
	and	x0,a
	move	a1,x:TMP1
	move	x:L_Y,a			; fy = y & (tile - 1)
	move	#>7,x0
	jclr	#0,x:L_BIG,_f8
	move	#>15,x0
_f8:	and	x0,a
	move	a1,x:TMP2
	move	x:L_E,a
	jclr	#14,a1,_hf
	move	x:TMP1,b
	move	#>1,x1
	eor	x1,b
	move	b1,x:TMP1
_hf:	jclr	#15,a1,_vf
	move	x0,b			; fy = tile - 1 - fy
	move	x:TMP2,x1
	sub	x1,b
	move	b1,x:TMP2
_vf:	move	#>$3ff,x0		; chr
	and	x0,a
	jclr	#0,x:L_BIG,_c8
	move	x:TMP1,x1
	add	x1,a
	jclr	#3,x:TMP2,_c8
	move	#>16,x1
	add	x1,a
_c8:	and	x0,a
	move	a1,x0		; the row word: chrbase + chr << wsh + (fy & 7)
	move	x:L_WMUL,y0
	mpy	x0,y0,b
	move	b0,a
	move	x:L_CHR,x0
	add	x0,a
	move	x:TMP2,b
	move	#>7,x0
	and	x0,b
	move	b1,x0
	add	x0,a
	move	#>$7fff,x0
	and	x0,a
	move	x:L_FI,b
	rep	#7
	lsl	b
	jsr	ptaddr			; r1 = the row's first word
	; the key: priority's base + palette base + palette << bpp (8 bpp: 0)
	move	x:L_E,a
	move	x:L_LO,b
	jclr	#13,a1,_pl
	move	x:L_HI,b
_pl:	move	x:L_PALB,x0
	add	x0,b
	move	x:BPP,x0
	move	#>8,x1
	move	x0,a
	cmp	x1,a
	jeq	_p8
	move	x:L_E,a			; palette << bpp
	rep	#10
	lsr	a
	move	#>7,x1
	and	x1,a
	lsl	a
	lsl	a
	jclr	#2,x0,_p2		; bpp 4: two more
	lsl	a
	lsl	a
_p2:	add	a,b
_p8:	move	b1,x1			; x1 = the key base
	move	x:L_DBASE,a		; the descriptor: DESC + (base + k) * 5
	move	x:L_KI,x0
	add	x0,a
	move	a1,b
	lsl	a
	lsl	a
	add	b,a
	move	#>DESC,x0
	add	x0,a
	move	a1,r2
	move	x:L_Y,x0
	move	x0,x:(r2)+		; DY
	move	x:L_E,a			; DA; bit 22: flipped either way (the
	move	#>$c000,x0		; address registers take the low 16 bits)
	and	x0,a
	move	r1,b
	tst	a
	jeq	_nf
	move	#>$400000,x0
	or	x0,b
_nf:	move	b1,x:(r2)+
	move	#>1,x0			; DS: -1 flipped vertically
	move	x:L_E,a
	jclr	#15,a1,_up
	move	#>-1,x0
_up:	move	x0,x:(r2)+
	move	x1,x:(r2)+		; DK
	clr	b			; DF
	jclr	#14,a1,_fl
	move	#>1,b
_fl:	move	b1,x:(r2)+
	move	x:L_FMUL,y0
	move	x:L_DST,r4		; the direction: flipped from its right end
	move	#1,n4
	jclr	#14,a1,sl_draw
	move	#7,n4
	nop
	move	(r4)+n4
	move	#-1,n4
	jmp	sl_draw

	hotcode
sl_draw:
	move	x:L_DIRECT,a
	tst	a
	jne	dsl_draw
	move	x:L_FI,a		; by format
	tst	a
	jeq	sl_f2
	move	#>1,x0
	cmp	x0,a
	jeq	sl_f4
	jmp	sl_f8

sl_f2:	movem	p:(r1),x0
	move	x0,a
	tst	a
	jeq	sl_clear
	move	#>8,a
	jmp	px_mixed

sl_f4:	movem	p:(r1),x0		; pixels 0-3, the flags
	move	x0,a
	jset	#0,a1,sl_clear
	move	#8,n1
	nop
	movem	p:(r1+n1),a		; pixels 4-7
	move	a1,x:L_T
	move	x0,a
	jclr	#1,a1,_mx
	do	#4,_s1
	mpy	x0,y0,b
	move	b1,a
	add	x1,a
	move	b0,x0
	move	a1,y:(r4)+n4
_s1:
	move	x:L_T,x0
	do	#4,_s2
	mpy	x0,y0,b
	move	b1,a
	add	x1,a
	move	b0,x0
	move	a1,y:(r4)+n4
_s2:
	rts
_mx:	move	#>4,a
	jsr	px_mix
	move	x:L_T,x0
	move	#>4,a
	jmp	px_mixed

sl_clear:
	clr	a
	rep	#8
	move	a,y:(r4)+n4
	rts

; The same straight into the line: a key where it is greater, nothing for
; a transparent pixel or a clear row.
dsl_draw:
	move	x:L_FI,a
	tst	a
	jeq	_f2
	move	#>1,x0
	cmp	x0,a
	jeq	_f4
	movem	p:(r1),x0		; 8 bpp: pixels 0-1 and the flags
	move	x0,a
	jset	#0,a1,_x
	move	#8,n1
	move	#>2,a
	jsr	px_dmax
	move	(r1)+n1
	movem	p:(r1),x0
	move	#>2,a
	jsr	px_dmax
	move	(r1)+n1
	movem	p:(r1),x0
	move	#>2,a
	jsr	px_dmax
	move	(r1)+n1
	movem	p:(r1),x0
	move	#>2,a
	jmp	px_dmax
_f2:	movem	p:(r1),x0
	move	x0,a
	tst	a
	jeq	_x
	move	#>8,a
	jmp	px_dmax
_f4:	movem	p:(r1),x0
	move	x0,a
	jset	#0,a1,_x
	move	#8,n1
	jset	#1,a1,_s4		; solid: no pixel is transparent
	movem	p:(r1+n1),a
	move	a1,x:L_T
	move	#>4,a
	jsr	px_dmax
	move	x:L_T,x0
	move	#>4,a
	jmp	px_dmax
_s4:	movem	p:(r1+n1),a		; pixels 4-7
	move	x:L_FRESH,b
	tst	b
	jeq	_c4
	mpy	x0,y0,b			; the line's first keys: stored, one
	do	#4,_a4			; multiply and one add a pixel
	add	x1,b		b0,x0
	mpy	x0,y0,b		b1,y:(r4)+n4
_a4:
	move	a1,x0
	mpy	x0,y0,b
	do	#4,_b4
	add	x1,b		b0,x0
	mpy	x0,y0,b		b1,y:(r4)+n4
_b4:
	rts
_c4:	move	a1,x:L_T		; else the greater key, without the test
	move	#>4,a
	jsr	px_dsmax
	move	x:L_T,x0
	move	#>4,a
	jmp	px_dsmax
_x:	rts

px_dmax:
	move	a1,n5
	nop
	do	n5,_pd
	mpy	x0,y0,b
	move	b1,a
	move	b0,x0
	tst	a
	jeq	_s
	add	x1,a
	move	y:(r4),b
	cmp	a,b
	tlt	a,b
	move	b,y:(r4)
_s:	move	(r4)+n4
_pd:
	rts

; px_dmax for a solid row: every field a key.
px_dsmax:
	move	a1,n5
	nop
	do	n5,_pq
	mpy	x0,y0,b
	move	b1,a
	move	b0,x0
	add	x1,a
	move	y:(r4),b
	cmp	a,b
	tlt	a,b
	move	b,y:(r4)+n4
_pq:
	rts

	extcode
	endif

; ---------------------------------------------------------------------------
; The row's sprites into LK (tc_model.c obj_line).
obj_line:
	clr	a
	move	#LK,r0
	rep	#272
	move	a,y:(r0)+
	move	x:SLOT+S_OBSEL,a	; sizes, name tables
	move	a1,b
	rep	#5
	lsr	a
	move	#>7,x0
	and	x0,a
	lsl	a
	lsl	a
	move	#OBJSZ,r4
	move	a1,n4
	nop
	move	(r4)+n4
	move	y:(r4)+,x0
	move	x0,x:O_SW
	move	y:(r4)+,x0
	move	x0,x:O_SH
	move	y:(r4)+,x0
	move	x0,x:O_LW
	move	y:(r4)+,x0
	move	x0,x:O_LH
	move	b1,a			; base: (OBSEL & 7) << 13
	and	x0,a
	move	#>7,x0
	move	b1,a
	and	x0,a
	rep	#13
	lsl	a
	move	a1,x:O_BASE
	move	b1,a			; gap: ((OBSEL >> 3) & 3) + 1 << 12
	rep	#3
	lsr	a
	move	#>3,x0
	and	x0,a
	move	#>1,x0
	add	x0,a
	rep	#12
	lsl	a
	move	a1,x:O_GAP
	move	x:SDIRTY,a		; the segment's sprites decoded once
	tst	a
	jeq	_dk
	clr	a
	move	a1,x:SDIRTY
	jsr	spk_build
_dk:	move	#>999,x0		; nothing drawn yet
	move	x0,x:O_XMIN
	move	#>-999,x0
	move	x0,x:O_XMAX
	clr	a			; the sprites in range, at most 32
	move	a1,x:O_N
	move	#SPK,r0
	move	#OLIST,r5
	move	x:O_SH,y0
	move	x:O_LH,y1
	move	x:ROW,x1
	move	#>$ff,b
	move	b1,x:TMP3
	clr	b
	move	b1,x:O_I
	move	x:CURN,x0
	do	x0,_in
	move	x:(r0)+,a		; on screen?
	jclr	#18,a1,_nx
	move	a1,x:TMP2
	move	a1,b			; dy = (row - y) & $FF
	rep	#9
	lsr	b
	move	x:TMP3,x0
	and	x0,b
	move	x1,a
	sub	b,a
	and	x0,a
	move	a1,a			; (AND leaves A2's sign: dy afresh)
	move	x:TMP2,b		; h by size
	move	y0,x0
	jclr	#17,b1,_sm
	move	y1,x0
_sm:	cmp	x0,a
	jge	_nx
	move	x:O_N,a			; the first 32
	move	#>32,x0
	cmp	x0,a
	jge	_nx
	move	x:O_I,x0
	move	x0,y:(r5)+
	move	#>1,x0
	add	x0,a
	move	a1,x:O_N
_nx:	move	x:O_I,a
	move	#>1,x0
	add	x0,a
	move	a1,x:O_I
	nop
_in:
	clr	a			; slivers from the last one backwards
	move	a1,x:O_NS
	move	x:O_N,a
	tst	a
	jeq	_x
	move	#>1,x0
	sub	x0,a
	move	a1,x:O_I
_sp:	move	x:O_I,n5
	move	#OLIST,r5
	nop
	move	y:(r5+n5),a		; its index
	jsr	sprite
	move	x:O_NS,a		; past 34: done
	move	#>34,x0
	cmp	x0,a
	jgt	_x
	move	x:O_I,a
	move	#>1,x0
	sub	x0,a
	move	a1,x:O_I
	jge	_sp
_x:	clr	a			; the merge's range: LK + 8 + xmin, to xmax + 8
	move	a1,x:M_CNT
	move	x:O_XMAX,a
	move	x:O_XMIN,x0
	sub	x0,a
	jlt	_r
	move	#>8,x1
	add	x1,a
	move	a1,x:M_CNT
	move	x0,a
	add	x1,a
	move	a1,x:M_LO
_r:	rts

; SPK from the segment's sprite list (CURS, CURN): x + 256, y, large, and
; on screen (x > -w and x < 256).
spk_build:
	if	TC7==0
	move	#>256,x0
	move	x0,x:OBAND0
	clr	a
	move	a1,x:OBAND1
	move	a1,x:OBW
	endif
	move	x:CURS,r0
	move	#SPK,r1
	move	x:CURN,x0
	do	x0,_b
	move	x:(r0)+,a		; x | y << 9 | large << 17
	move	(r0)+
	move	a1,b
	move	#>$1ff,x0		; x: from 256 negative
	and	x0,a
	move	#>256,x0
	cmp	x0,a
	jlt	_p
	move	#>512,x0
	sub	x0,a
_p:	move	a1,y1			; x
	move	x:O_SW,x0		; w by size
	jclr	#17,b1,_w
	move	x:O_LW,x0
_w:	move	#>$3fe00,y0		; y and large as they are
	move	b1,a
	and	y0,a
	move	a1,x1
	move	y1,a			; x + 256 in bits 8-0
	move	#>256,y0
	add	y0,a
	or	x1,a
	move	a1,b
	move	y1,a			; on screen: -w < x < 256
	add	x0,a
	jle	_o
	move	y1,a
	move	#>256,x0
	cmp	x0,a
	jge	_o
	move	#>$40000,x0
	or	x0,b
_o:	move	b1,x:(r1)+
	if	TC7==0
	jclr	#18,b1,_bn		; on screen: its rows join the band
	move	b1,a
	rep	#9
	lsr	a
	move	#>$ff,x0
	and	x0,a			; y
	move	a1,y1
	move	x:O_SH,x1
	jclr	#17,b1,_hs
	move	x:O_LH,x1
_hs:	add	x1,a			; y + h
	move	#>256,x0
	cmp	x0,a
	jle	_nw
	move	#>1,x0
	move	x0,x:OBW
_nw:	move	a1,y0
	move	x:OBAND0,x0
	move	y1,a
	cmp	x0,a
	jge	_b0
	move	a1,x:OBAND0
_b0:	move	x:OBAND1,x0
	move	y0,a
	cmp	x0,a
	jle	_bn
	move	a1,x:OBAND1
_bn:
	endif
	nop
_b:
	if	TC7==0
	move	x:OBW,a			; a wrapping sprite: every row
	tst	a
	jeq	_bx
	clr	a
	move	a1,x:OBAND0
	move	#>256,x0
	move	x0,x:OBAND1
_bx:
	endif
	rts

; a1 = the sprite's index in the segment -> O_W, O_H, O_X, O_DY, and r0 at
; its stream words
spr_geom:
	move	a1,n1
	move	#SPK,r1
	lsl	a
	move	a1,n0
	move	x:CURS,r0
	move	x:(r1+n1),a
	move	(r0)+n0
	move	a1,b
	move	x:O_SW,x0
	move	x:O_SH,x1
	jclr	#17,a1,_sm
	move	x:O_LW,x0
	move	x:O_LH,x1
_sm:	move	x0,x:O_W
	move	x1,x:O_H
	move	#>$1ff,x0
	and	x0,a
	move	#>256,x0
	sub	x0,a
	move	a1,x:O_X
	rep	#9
	lsr	b
	move	#>$ff,x0
	and	x0,b
	move	x:ROW,a
	sub	b,a
	and	x0,a
	move	a1,x:O_DY
	rts


	if	TC7

; One sprite (a1 = its index in the segment): its slivers on screen into
; LK, opaque pixels only; O_NS counts them (past 34 the rest is dropped).
sprite:
	jsr	spr_geom
	move	#1,n0
	nop
	move	x:(r0+n0),a		; tile | attr << 8
	move	a1,x:O_TILE
	rep	#8
	lsr	a
	move	#>$ff,x0
	and	x0,a
	move	a1,x:O_ATTR
	jclr	#7,a1,_nv		; vertical flip: dy = h - 1 - dy
	move	x:O_H,b
	move	#>1,x0
	sub	x0,b
	move	x:O_DY,x0
	sub	x0,b
	move	b1,x:O_DY
_nv:	move	x:O_ATTR,a		; the key: (4 + 4 priority) << 16 | 4 << 8
	move	a1,b			; | 128 + palette * 16
	rep	#4
	lsr	a
	move	#>3,x0
	and	x0,a
	move	#>1,x0
	add	x0,a
	rep	#18
	lsl	a
	move	#>$400,x0		; source 4, or 6 with palettes 4-7
	jclr	#3,b1,_s4
	move	#>$600,x0
_s4:	or	x0,a
	move	b1,b
	lsr	b
	move	#>7,x0
	and	x0,b
	rep	#4
	lsl	b
	move	#>128,x0
	add	x0,b
	move	b1,x0
	or	x0,a
	move	a1,x:O_KEY
	clr	a			; columns
	move	a1,x:O_COL
_cl:	move	x:O_COL,a		; sx = x + 8 col
	lsl	a
	lsl	a
	lsl	a
	move	x:O_X,x0
	add	x0,a
	move	a1,x:TMP0
	move	#>-8,x0			; -8 < sx < 256
	cmp	x0,a
	jle	_nx
	move	#>256,x0
	cmp	x0,a
	jge	_nx
	move	x:O_NS,a
	move	#>1,x0
	add	x0,a
	move	a1,x:O_NS
	move	#>34,x0
	cmp	x0,a
	jgt	_x
	jsr	obj_sliver
_nx:	move	x:O_COL,a
	move	#>1,x0
	add	x0,a
	move	a1,x:O_COL
	lsl	a
	lsl	a
	lsl	a
	move	x:O_W,x0
	cmp	x0,a
	jlt	_cl
_x:	rts

; column O_COL of the sprite at sx = TMP0
obj_sliver:
	move	x:O_COL,a		; c = hflip ? w / 8 - 1 - col : col
	move	x:O_ATTR,b
	jclr	#6,b1,_c
	move	x:O_W,b
	rep	#3
	lsr	b
	move	#>1,x0
	sub	x0,b
	move	x:O_COL,x0
	sub	x0,b
	move	b1,a
_c:	move	x:O_TILE,b		; + tcol0, & 15
	move	#>15,x0
	and	x0,b
	add	b,a
	and	x0,a
	move	a1,y0
	move	x:O_TILE,a		; row: (trow0 + (dy >> 3)) & 15
	rep	#4
	lsr	a
	and	x0,a
	move	x:O_DY,b
	rep	#3
	lsr	b
	add	b,a
	and	x0,a
	rep	#4
	lsl	a
	move	y0,x0
	or	x0,a			; chr
	rep	#4
	lsl	a			; * 16
	move	x:O_DY,b
	move	#>7,x0
	and	x0,b
	move	b1,x0
	add	x0,a
	move	x:O_BASE,x0
	add	x0,a
	move	x:O_ATTR,b
	jclr	#0,b1,_g
	move	x:O_GAP,x0
	add	x0,a
_g:	move	#>$7fff,x0
	and	x0,a
	move	#>128,b			; F4
	jsr	ptaddr
	move	x:TMP0,a		; the row's sprite range
	move	x:O_XMIN,x0
	cmp	x0,a
	jge	_n1
	move	a1,x:O_XMIN
_n1:	move	x:O_XMAX,x0
	cmp	x0,a
	jle	_n2
	move	a1,x:O_XMAX
_n2:	movem	p:(r1),x0		; pixels 0-3, flags
	move	x0,a
	jset	#0,a1,_x
	move	#8,n1
	nop
	movem	p:(r1+n1),a
	move	a1,x:L_T
	move	x:TMP0,a		; LK + 8 + sx, flipped from its right end
	move	#>LK+8,x1
	add	x1,a
	move	a1,r4
	move	#1,n4
	move	x:O_ATTR,b
	jclr	#6,b1,_d
	move	#7,n4
	nop
	move	(r4)+n4
	move	#-1,n4
_d:	move	x:O_KEY,x1
	move	#>16,y0
	jsr	px_opaque
	move	x:L_T,x0
	jsr	px_opaque
_x:	rts

	endif
	hotcode
; 4 pixels from the fields of x0: a key where the field is not 0, else
; nothing
px_opaque:
	do	#4,_po
	mpy	x0,y0,b
	move	b1,a
	move	b0,x0
	tst	a
	jeq	_s
	add	x1,a
	move	a1,y:(r4)
_s:	move	(r4)+n4
_po:
	rts

; ---------------------------------------------------------------------------
; MAIN and SUB -> the row's 256 RGB565 words in the ring (the loops in
; internal P: col_lut, col_sub).
	extcode
colour:
	move	x:MATHF,a
	tst	a
	jne	_mt
	move	#MAIN+8,r0		; no colour math: T565
	move	#T565,r1
	move	#>$ff,x0
	jmp	col_lut
_mt:	move	x:CASEB,a		; through CTAB: one lookup per pixel
	tst	a
	jeq	_sub
	jsr	ctab_check
	move	#MAIN+8,r0
	move	#CTAB,r1
	move	#>$7ff,x0
	jmp	col_lut
_sub:	move	x:CASEW,a		; the sub screen, the same everywhere on
	tst	a			; the row: sub_check's tables
	jeq	_slow
	jsr	sub_check
	move	#MAIN+8,r0
	move	#SUB-MAIN-1,n0		; (after the main key's (r0)+)
	move	#CTAB,r1
	move	#ES0,r4
	move	#TRT-128,r5
	move	#TGT,r3
	move	#TBT,r2
	move	#>$7ff,x0
	move	#>$ff,x1
	move	#>$008000,y1
	jmp	col_sub
_slow:	move	#MAIN+8,r0
	move	#SUB+8,r2
	move	#WC,r3
	do	#256,_s
	jsr	pixel
	nop
_s:
	rts
	hotcode

; The row's pixels by one lookup each: r1 the table, x0 the key's mask.
col_lut:
	move	r1,r2			; two pixels an iteration: the second lookup
	do	#128,_f			; through r2, the AGU gaps filled
	move	y:(r0)+,a
	and	x0,a		y:(r0)+,b
	and	x0,b		a1,n1
	move	b1,n2
	move	y:(r1+n1),a
	move	y:(r2+n2),b
	move	a,y:(r7)+
	move	b,y:(r7)+
_f:
	rts

; The row's pixels through sub_check's tables (colour sets the registers).
col_sub:
	do	#256,_sf
	move	y:(r0)+,a		; main key
	and	x0,a		y:(r0+n0),b	; sub key
	and	x1,b		a1,n1
	move	b1,n4
	move	y:(r1+n1),a		; the pixel, or with math Em | $80
	jclr	#7,a1,_sw
	move	y:(r4+n4),y0		; S = Em | $80 + the sub's E term
	add	y0,a
	and	x1,a		a1,y0	; red + 128
	mpy	y0,y1,b		a1,n5	; S >> 8
	and	x1,b		b1,y0	; green
	mpy	y0,y1,a		b1,n3	; S >> 16: blue
	move	a1,n2
	move	y:(r5+n5),a
	move	y:(r3+n3),y0
	or	y0,a		y:(r2+n2),y0
	or	y0,a
_sw:	move	a1,y:(r7)+
_sf:
	rts

	extcode

; One pixel as snes_ppu.c rgb_row composes it.
pixel:
	move	y:(r0)+,a		; main key
	move	a1,x:TMP0
	move	y:(r2)+,b		; sub key
	move	b1,x:TMP1
	move	y:(r3)+,n4		; the colour window here?
	move	#POW2,r4
	move	x:CWMASK,a
	move	y:(r4+n4),x0
	and	x0,a
	move	x:CLIP0,x0
	move	x:MATH0,x1
	jeq	_o
	move	x:CLIP1,x0
	move	x:MATH1,x1
_o:	move	x0,x:TMP2		; clip
	move	x1,x:TMP3		; math here
	move	x:TMP0,a		; mi, ms
	move	#>$ff,x0
	and	x0,a
	move	a1,n5			; mi
	move	x:TMP0,a
	move	#>5,b
	tst	a
	jeq	_bk
	rep	#8
	lsr	a
	move	#>7,x0
	and	x0,a
	move	a1,b
_bk:	move	#ENTAB,r1		; enabled for its source
	move	b1,n1
	nop
	move	x:(r1+n1),a
	move	#>4,x0			; OBJ: palettes 4-7 only
	cmp	x0,b
	jne	_nob
	move	n5,b
	move	#>192,x0
	cmp	x0,b
	jge	_nob
	clr	a
_nob:	move	x:TMP3,x0		; and math here
	and	x0,a
	jne	_math
	move	x:TMP2,a		; no math: clipped is black
	tst	a
	jne	_blk
	move	#T565,r1
	move	n5,n1
	nop
	move	y:(r1+n1),a
	move	a,y:(r7)+
	rts
_blk:	clr	a
	move	a,y:(r7)+
	rts
_math:	clr	a			; Em (0 if clipped)
	move	x:TMP2,b
	tst	b
	jne	_em
	move	#EW,r1
	move	n5,n1
	nop
	move	y:(r1+n1),a
_em:	move	a1,y1			; Em
	move	x:HALF,a		; half, unless clipped
	move	x:TMP2,b
	tst	b
	jeq	_h1
	clr	a
_h1:	move	a1,x:TMP3
	move	x:FIXEDE,y0		; Es: the fixed colour, or with SUBON the
	move	x:SUBON,b		; sub pixel (transparent: the fixed colour
	tst	b			; without half)
	jeq	_es
	move	x:TMP1,b		; the sub pixel: not the backdrop (source 5)
	move	#>$700,x0
	and	x0,b
	move	#>$500,x0
	cmp	x0,b
	jne	_sp
	clr	a
	move	a1,x:TMP3
	jmp	_es
_sp:	move	x:TMP1,b
	move	#>$ff,x0
	and	x0,b
	move	b1,n1
	move	#EW,r1
	nop
	move	y:(r1+n1),y0
_es:	move	y1,a			; S = Em + Es, or Em + $202020 - Es
	move	x:SUBTR,b
	tst	b
	jne	_sb
	add	y0,a
	jmp	_ix
_sb:	move	#>$202020,x0
	add	x0,a
	sub	y0,a
_ix:	move	a1,x:TMP0
	move	x:SUBTR,b		; TCH + (subtract * 2 + half) * 192
	lsl	b
	move	x:TMP3,x0
	add	x0,b
	move	b1,a
	rep	#6
	lsl	a
	move	a1,x0
	move	b1,a
	rep	#7
	lsl	a
	add	x0,a
	move	#>TCH,x0
	add	x0,a
	move	a1,r1
	move	#>$ff,x0
	move	x:TMP0,b		; red
	and	x0,b
	move	b1,n1
	nop
	move	y:(r1+n1),y0
	move	x:TMP0,b		; green
	rep	#8
	lsr	b
	and	x0,b
	move	#>64,x1
	add	x1,b
	move	b1,n1
	nop
	move	y:(r1+n1),y1
	move	x:TMP0,b		; blue
	rep	#16
	lsr	b
	and	x0,b
	move	#>128,x1
	add	x1,b
	move	b1,n1
	nop
	move	y:(r1+n1),a
	or	y0,a
	or	y1,a
	move	a1,y:(r7)+
	rts

; CTAB for this row's colour math, built again when brightness, CGRAM or
; the MATH or FIXED slots changed since: per source s (ENTAB) and index i,
; clip always: 0 (with math: 0 +- the fixed colour, no half); else T565[i],
; or with math E[i] +- the fixed colour through TCH.
	if	TC7==0
ctab_check:
	move	x:CKIND,a
	move	#>1,x0
	cmp	x0,a
	jne	_f
	move	x:CDIRTY,a
	tst	a
	jne	_f
	move	x:CKEY,a
	move	x:SLOT+S_MATH,x0
	cmp	x0,a
	jne	_f
	move	x:CKEY2,a
	move	x:SLOT+S_FIXED,x0
	cmp	x0,a
	jeq	_x
	move	#>1,x0			; only the fixed colour: only the sources
	jmp	_g			; with colour math change
_f:	clr	a
	move	a1,x0
_g:	move	x0,x:CPART
	else
ctab_check:
	move	x:CKIND,a
	move	#>1,x0
	cmp	x0,a
	jne	_b
	move	x:CDIRTY,a
	tst	a
	jne	_b
	move	x:CKEY,a
	move	x:SLOT+S_MATH,x0
	cmp	x0,a
	jne	_b
	move	x:CKEY2,a
	move	x:SLOT+S_FIXED,x0
	cmp	x0,a
	jeq	_x
	endif
_b:	move	#>1,x0
	move	x0,x:CKIND
	clr	a
	move	a1,x:CDIRTY
	move	x:SLOT+S_MATH,x0
	move	x0,x:CKEY
	move	x:SLOT+S_FIXED,x0
	move	x0,x:CKEY2
	move	x:SLOT+S_MATH,a		; clip always: CGWSEL bits 7-6 = 3
	move	#>$c0,x0
	and	x0,a
	clr	b
	cmp	x0,a
	jne	_nc
	move	#>1,b
_nc:	move	b1,x:TMP2		; clip all
	move	x:HALF,a		; half, unless clipped, or the fixed colour
	tst	b			; standing in for an empty sub screen
	jne	_h0
	move	x:SUBON,b
	tst	b
	jeq	_h
_h0:	clr	a
_h:	move	a1,x:TMP3
	move	#CTAB,r2
	move	#ENTAB,r5
	do	#7,_src
	move	x:(r5)+,a		; math for this source: CGWSEL math always
	move	x:MATH0,x0		; (CASEB: MATH0 = MATH1)
	and	x0,a
	move	a1,x:L_T
	if	TC7==0
	tst	a			; no math, and the table is right: on
	jne	_gm
	move	x:CPART,a
	tst	a
	jeq	_gp
	move	#256,n2
	nop
	move	(r2)+n2
	jmp	_nx
	endif
	if	TC7==0
_gm:	move	x:L_T,a			; math, not clipped: the fast entry loop
	tst	a
	jeq	_gp
	move	x:TMP2,a
	tst	a
	jne	_gp
	jsr	ctab_math
	jmp	_nx
	endif
_gp:	move	#EW,r1
	move	#T565,r3
	do	#256,_ix
	move	x:L_T,a
	tst	a
	jne	_m
	move	x:TMP2,a		; no math: T565, or black when clipped
	tst	a
	move	y:(r3),b
	jeq	_st
	clr	b
	jmp	_st
_m:	clr	a			; Em (0 clipped) +- the fixed colour
	move	x:TMP2,b
	tst	b
	jne	_z
	move	y:(r1),a
_z:	move	x:FIXEDE,y0
	move	x:SUBTR,b
	tst	b
	jne	_sb
	add	y0,a
	jmp	_lk
_sb:	move	#>$202020,x0
	add	x0,a
	sub	y0,a
_lk:	move	x:SUBTR,b		; s = subtract * 2 + half
	lsl	b
	move	x:TMP3,x0
	add	x0,b
	jsr	tchlook
	move	a1,b
_st:	move	b,y:(r2)+
	move	(r1)+
	move	(r3)+
	nop
_ix:
_nx:	nop
_src:
_x:	rts

; The tables of a row whose colour math takes the sub screen and does not
; depend on the colour window (CASEW), built again as CTAB is:
;   CTAB (its room) per source and index: without math the pixel (T565, or
;        0 clipped); with math Em (0 clipped) | $80
;   ES0  per sub index the E term: E, or subtracting $202020 - E; + $404040
;        where halved (half unless clipped); index 0, the backdrop: the
;        fixed colour, never halved
;   TRT, TGT, TBT  per channel value v (the TCH index + 64 halved; red at
;        128 + v) its RGB565 bits: TCH's sets for the row's add/subtract
; so a pixel is S = CTAB + ES0, and TRT | TGT | TBT of S's three bytes.
sub_check:
	move	x:CKIND,a
	move	#>2,x0
	cmp	x0,a
	jne	_b
	move	x:CDIRTY,a
	tst	a
	jne	_b
	move	x:CKEY,a
	move	x:SLOT+S_MATH,x0
	cmp	x0,a
	jne	_b
	move	x:CKEY2,a
	move	x:SLOT+S_FIXED,x0
	cmp	x0,a
	jeq	_x
_b:	move	#>2,x0
	move	x0,x:CKIND
	clr	a
	move	a1,x:CDIRTY
	move	x:SLOT+S_MATH,x0
	move	x0,x:CKEY
	move	x:SLOT+S_FIXED,x0
	move	x0,x:CKEY2
	move	x:CLIP0,a		; clipped (CASEW: CLIP0 = CLIP1)
	move	a1,x:TMP2
	move	x:HALF,a		; half, unless clipped
	tst	a
	jeq	_h
	move	#>$404040,a
_h:	move	x:TMP2,b
	tst	b
	jeq	_h1
	clr	a
_h1:	move	a1,x:TMP3
	move	#CTAB,r2
	move	#ENTAB,r5
	do	#7,_src
	move	x:(r5)+,a		; math for this source (CASEW: MATH0 = MATH1)
	move	x:MATH0,x0
	and	x0,a
	move	a1,x:L_T
	move	#EW,r1
	move	#T565,r3
	do	#256,_ix
	move	x:L_T,a
	tst	a
	jne	_m
	move	x:TMP2,a		; no math: T565, or black when clipped
	tst	a
	move	y:(r3),b
	jeq	_st
	clr	b
	jmp	_st
_m:	clr	b			; Em (0 clipped) | $80
	move	x:TMP2,a
	tst	a
	jne	_z
	move	y:(r1),b
_z:	move	#>$80,x0
	or	x0,b
_st:	move	b1,y:(r2)+
	move	(r1)+
	move	(r3)+
	nop
_ix:
	nop
_src:
	move	#ES0,r2			; ES0
	move	x:FIXEDE,a
	clr	b
	jsr	_esv
	move	#EW+1,r1
	do	#255,_es
	move	y:(r1)+,a
	move	x:TMP3,b
	jsr	_esv
	nop
_es:
	move	#TCH,r1			; TRT, TGT, TBT: per channel 64 values of
	move	x:SUBTR,a		; the unhalved set, then 64 of the halved
	tst	a
	jeq	_t0
	move	#TCH+384,r1
_t0:	move	#TRT,r2
	move	r1,x:TMP0
	do	#3,_c
	move	x:TMP0,r1
	nop
	do	#64,_c0
	move	y:(r1)+,x0
	move	x0,y:(r2)+
_c0:
	move	#128,n1
	nop
	move	(r1)+n1
	do	#64,_c1
	move	y:(r1)+,x0
	move	x0,y:(r2)+
_c1:
	move	x:TMP0,a
	move	#>64,x0
	add	x0,a
	move	a1,x:TMP0
_c:
_x:	rts
_esv:	move	a1,x0			; a1 = E, b1 = the half term -> ES0 (r2)+
	move	b1,y0
	move	x:SUBTR,b
	tst	b
	jeq	_ea
	move	#>$202020,b
	sub	x0,b
	jmp	_eh
_ea:	move	x0,b
_eh:	add	y0,b
	move	b1,y:(r2)+
	rts

; a1 = a channel sum (E layout), b1 = the TCH set -> a1 = its RGB565
; (bits 23-8). Uses r4, n4, x0, x1, y0, y1.
tchlook:
	move	a1,y1			; S
	move	b1,a			; TCH + s * 192
	rep	#6
	lsl	a
	move	a1,x0
	move	b1,a
	rep	#7
	lsl	a
	add	x0,a
	move	#>TCH,x0
	add	x0,a
	move	a1,r4
	move	#>$ff,x0
	move	y1,a			; red
	and	x0,a
	move	a1,n4
	nop
	move	y:(r4+n4),y0
	move	y1,a			; green
	rep	#8
	lsr	a
	and	x0,a
	move	#>64,x1
	add	x1,a
	move	a1,n4
	nop
	move	y:(r4+n4),x1
	move	y1,a			; blue
	rep	#16
	lsr	a
	and	x0,a
	move	#>128,x0
	add	x0,a
	move	a1,n4
	nop
	move	y:(r4+n4),a
	or	y0,a
	or	x1,a
	rts


	if	TC7==0
; One source's 256 entries with colour math, not clipped, into CTAB at r2+:
; the pixel of Em + the fixed colour (or - it), through TCH's set for the
; row (tchlook, unrolled: the set's base once, the three lookups added).
; x1 = K (the fixed colour, or $202020 - it), y0 = $ff, y1 = 2^15 (>> 8).
ctab_math:
	move	x:FIXEDE,x1
	move	x:SUBTR,a
	tst	a
	jeq	_ka
	move	#>$202020,a
	sub	x1,a
	move	a1,x1
_ka:	move	x:SUBTR,a		; s = subtract * 2 + half; TCH + s * 192
	lsl	a
	move	x:TMP3,x0
	add	x0,a
	move	a1,x0
	move	#>96,y0
	mpy	x0,y0,a
	move	a0,a
	move	#>TCH,x0
	add	x0,a
	move	a1,r4
	move	#>64,x0
	add	x0,a
	move	a1,r0
	add	x0,a
	move	a1,r3
	move	#EW,r1
	move	#>$ff,y0
	move	#>$008000,y1
	do	#256,_e
	move	y:(r1)+,a		; Em + K = S
	add	x1,a
	move	a1,x0
	and	y0,a			; red
	move	a1,n4
	mpy	x0,y1,b			; S >> 8
	move	b1,a
	and	y0,a			; green
	move	a1,n0
	move	b1,x0
	mpy	x0,y1,b			; S >> 16: blue
	move	b1,n3
	move	y:(r4+n4),a
	move	y:(r0+n0),b
	add	b,a
	move	y:(r3+n3),b
	add	b,a
	move	a,y:(r2)+
_e:
	rts
	endif

; ---------------------------------------------------------------------------
; Cold code of the tile build in the X memory nothing else uses (X:$0C64-$0CFF
; = P:$4C64-$4CFF): the program's own space (P:$0200-$0FFF) is full.
EXTEND	equ	@CVI(@LCV(R))
	if	EXTEND>$1000
	fail	'the external program reaches the hot routines load area (P:$1000)'
	endif
	if	TC7
	if	EXTEND>$0C00
	fail	'the TC7 program reaches TUF (Y:$0C00)'
	endif
	endif
	if	TC7==0
	org	p:$4C64

; A layer row that steps (nothing changed, the row before drawn): the
; specialised loop for the common case, else step_row. Fast: no offset-per-
; tile, straight into the line, 4 bpp characters. The block's first row
; builds every sliver; the fast loop does not keep DY.
step_sel:
	move	x:L_OPT,a
	tst	a
	jne	step_row
	move	x:L_DIRECT,a
	tst	a
	jeq	step_row
	move	x:L_FI,a
	move	#>1,x0
	cmp	x0,a
	jne	step_row
	move	x:L_Y0,a
	move	#>7,x0
	and	x0,a
	jeq	bg_rb			; a new block: every sliver built
step_go:
	move	x:L_DBASE,a		; DESC + base * 5: DA at +1, DK at +3
	move	a1,b
	lsl	a
	lsl	a
	add	b,a
	move	#>DESC+1,x0
	add	x0,a
	move	a1,r0
	move	#>2,x0
	add	x0,a
	move	a1,r3
	move	x:L_DST0,r5
	move	x:L_FRESH,a
	move	a1,n2
	move	#5,n0
	move	#5,n3
	move	#8,n1
	move	#8,n5
	move	x:L_FMUL,y0
	jmp	step_f4

; Every sliver of a layer row drawn from its records, the row's address
; stepped (rb_all's records: one step early).
draw_gen:
	move	x:L_DBASE,a
	move	a1,b
	lsl	a
	lsl	a
	add	b,a
	move	#>DESC+1,x0
	add	x0,a
	move	a1,r2
	move	x:L_DST0,r5
	move	x:L_FMUL,y0
	do	#33,_dg
	move	r2,r3			; DA
	move	x:(r2)+,a
	move	x:(r2)+,x0		; DS
	add	x0,a
	move	a1,x:(r3)
	move	a1,r1
	move	x:(r2)+,x1		; DK
	move	x:(r2)+,b		; DF
	move	r5,r4
	move	#1,n4
	tst	b
	jeq	_dr
	move	#7,n4
	nop
	move	(r4)+n4
	move	#-1,n4
_dr:	jsr	sl_draw
	move	#8,n5
	nop
	move	(r5)+n5
	move	(r2)+			; the next record's DY
_dg:
	rts
; A flipped sliver in a fast row: the general code, from its descriptor
; (r0 -> its DA; x1 the key base as loaded).
step_sl:
	move	r0,r2
	move	x:(r0),a		; DA (bit 22 set)
	move	(r2)+			; r2 -> DS
	move	x:(r2)+,y1		; DS; r2 -> DK
	add	y1,a
	move	a1,x:(r0)+n0		; the stepped DA; r0 on to the next
	move	a1,r1
	move	(r2)+			; r2 -> DF
	move	x:(r2),b
	move	r5,r4
	move	#1,n4
	tst	b
	jeq	_sd
	move	#7,n4
	nop
	move	(r4)+n4
	move	#-1,n4
_sd:	jsr	dsl_draw
	move	#8,n1
	move	#8,n5
	jmp	step_fe
sl_f8:	movem	p:(r1),x0		; pixels 0-1 and the flags
	move	x0,a
	jset	#0,a1,sl_clear
	move	#8,n1
	move	#>2,a
	jsr	px_mix
	move	(r1)+n1
	movem	p:(r1),x0
	move	#>2,a
	jsr	px_mix
	move	(r1)+n1
	movem	p:(r1),x0
	move	#>2,a
	jsr	px_mix
	move	(r1)+n1
	movem	p:(r1),x0
	move	#>2,a
	jmp	px_mixed

; a1 pixels from the fields of x0 (y0 the multiplier, x1 the key base) to
; r4 by n4: a key where the field is not 0, 0 where it is
px_mixed:
px_mix:
	move	a1,n5
	nop
	do	n5,_pm
	mpy	x0,y0,b
	move	b1,a
	move	b0,x0
	tst	a
	jeq	_z
	add	x1,a
_z:	move	a1,y:(r4)+n4
_pm:
	rts

XCEND	equ	@CVI(@LCV(R))
	if	XCEND>$4D00
	fail	'the cold code does not fit X:$0C64-$0CFF'
	endif
	endif


; The tile program's second-last page slot (P:$7E00-$7EFF; TC_SLOTS_TILE 72)
; holds the sprite routines.
	if	TC7==0
	org	p:$7E00

; One sprite (a1 = its index in the segment): its slivers on screen into
; LK, opaque pixels only; O_NS counts them (past 34 the rest is dropped).
sprite:
	jsr	spr_geom
	move	#1,n0
	nop
	move	x:(r0+n0),a		; tile | attr << 8
	move	a1,x:O_TILE
	rep	#8
	lsr	a
	move	#>$ff,x0
	and	x0,a
	move	a1,x:O_ATTR
	jclr	#7,a1,_nv		; vertical flip: dy = h - 1 - dy
	move	x:O_H,b
	move	#>1,x0
	sub	x0,b
	move	x:O_DY,x0
	sub	x0,b
	move	b1,x:O_DY
_nv:	move	x:O_ATTR,a		; the key: (4 + 4 priority) << 16 | 4 << 8
	move	a1,b			; | 128 + palette * 16
	rep	#4
	lsr	a
	move	#>3,x0
	and	x0,a
	move	#>1,x0
	add	x0,a
	rep	#18
	lsl	a
	move	#>$400,x0		; source 4, or 6 with palettes 4-7
	jclr	#3,b1,_s4
	move	#>$600,x0
_s4:	or	x0,a
	move	b1,b
	lsr	b
	move	#>7,x0
	and	x0,b
	rep	#4
	lsl	b
	move	#>128,x0
	add	x0,b
	move	b1,x0
	or	x0,a
	move	a1,x:O_KEY
	clr	a			; columns
	move	a1,x:O_COL
_cl:	move	x:O_COL,a		; sx = x + 8 col
	lsl	a
	lsl	a
	lsl	a
	move	x:O_X,x0
	add	x0,a
	move	a1,x:TMP0
	move	#>-8,x0			; -8 < sx < 256
	cmp	x0,a
	jle	_nx
	move	#>256,x0
	cmp	x0,a
	jge	_nx
	move	x:O_NS,a
	move	#>1,x0
	add	x0,a
	move	a1,x:O_NS
	move	#>34,x0
	cmp	x0,a
	jgt	_x
	jsr	obj_sliver
_nx:	move	x:O_COL,a
	move	#>1,x0
	add	x0,a
	move	a1,x:O_COL
	lsl	a
	lsl	a
	lsl	a
	move	x:O_W,x0
	cmp	x0,a
	jlt	_cl
_x:	rts

; column O_COL of the sprite at sx = TMP0
obj_sliver:
	move	x:O_COL,a		; c = hflip ? w / 8 - 1 - col : col
	move	x:O_ATTR,b
	jclr	#6,b1,_c
	move	x:O_W,b
	rep	#3
	lsr	b
	move	#>1,x0
	sub	x0,b
	move	x:O_COL,x0
	sub	x0,b
	move	b1,a
_c:	move	x:O_TILE,b		; + tcol0, & 15
	move	#>15,x0
	and	x0,b
	add	b,a
	and	x0,a
	move	a1,y0
	move	x:O_TILE,a		; row: (trow0 + (dy >> 3)) & 15
	rep	#4
	lsr	a
	and	x0,a
	move	x:O_DY,b
	rep	#3
	lsr	b
	add	b,a
	and	x0,a
	rep	#4
	lsl	a
	move	y0,x0
	or	x0,a			; chr
	rep	#4
	lsl	a			; * 16
	move	x:O_DY,b
	move	#>7,x0
	and	x0,b
	move	b1,x0
	add	x0,a
	move	x:O_BASE,x0
	add	x0,a
	move	x:O_ATTR,b
	jclr	#0,b1,_g
	move	x:O_GAP,x0
	add	x0,a
_g:	move	#>$7fff,x0
	and	x0,a
	move	#>128,b			; F4
	jsr	ptaddr
	move	x:TMP0,a		; the row's sprite range
	move	x:O_XMIN,x0
	cmp	x0,a
	jge	_n1
	move	a1,x:O_XMIN
_n1:	move	x:O_XMAX,x0
	cmp	x0,a
	jle	_n2
	move	a1,x:O_XMAX
_n2:	movem	p:(r1),x0		; pixels 0-3, flags
	move	x0,a
	jset	#0,a1,_x
	move	#8,n1
	nop
	movem	p:(r1+n1),a
	move	a1,x:L_T
	move	x:TMP0,a		; LK + 8 + sx, flipped from its right end
	move	#>LK+8,x1
	add	x1,a
	move	a1,r4
	move	#1,n4
	move	x:O_ATTR,b
	jclr	#6,b1,_d
	move	#7,n4
	nop
	move	(r4)+n4
	move	#-1,n4
_d:	move	x:O_KEY,x1
	move	#>16,y0
	jsr	px_opaque
	move	x:L_T,x0
	jsr	px_opaque
_x:	rts

SCEND	equ	@CVI(@LCV(R))
	if	SCEND>$7F00
	fail	'the sprite code does not fit P:$7E00-$7EFF'
	endif
	endif

; The last page slot of the tile program (P:$7F00-$7FFF; the CPU keeps
; tile frames to 72 slots, TC_SLOTS_TILE) holds more of its cold code.
	if	TC7==0
	org	p:$7F00

; Every sliver of a layer row built at once (no offset-per-tile, 8 x 8
; characters), then drawn: the fast loop, or draw_gen. The records hold DA
; one step early (the drawing steps it): DA = the row's P address - DS.
; Bit 22 of DA marks a flipped sliver.
rb_all:
	move	x:L_Y0,a		; the row in its character
	move	#>7,x0
	and	x0,a
	move	a1,b
	move	x:L_CHR,x0
	add	x0,a
	move	a1,x:RBASE		; CHR + fy
	move	#>7,a
	sub	b,a
	add	x0,a
	move	a1,x:RBASEV		; CHR + 7 - fy
	move	x:L_PALB,x0		; the key bases (palette base added)
	move	x:L_LO,a
	add	x0,a
	move	a1,x:KLO
	move	x:L_HI,a
	add	x0,a
	move	a1,x:KHI
	clr	b			; palette << bpp: by one multiply
	move	x:BPP,a
	move	#>2,x0
	cmp	x0,a
	jne	_p4
	move	#>$008000,b
	jmp	_pm
_p4:	move	#>4,x0
	cmp	x0,a
	jne	_pm
	move	#>$020000,b
_pm:	move	b1,x:PSHM
	move	x:L_FI,b		; format index * 128
	rep	#7
	lsl	b
	move	b1,x:RB_FIM
	move	x:L_ROWA,a		; the map row in P: screen 0 (and 1)
	move	#>$7fff,x0
	and	x0,a
	move	#>384,b
	jsr	ptaddr
	move	r1,x:RB_PA
	move	r1,x:RB_PB
	move	x:L_WIDE,b
	jclr	#0,b1,_nw
	move	x:L_ROWA,a
	move	#>$400,x0
	add	x0,a
	move	#>$7fff,x0
	and	x0,a
	move	#>384,b
	jsr	ptaddr
	move	r1,x:RB_PB
_nw:	move	x:L_PX0,a		; the first sliver's map column
	rep	#3
	lsr	a
	move	a1,b
	move	#>31,x0
	and	x0,b
	move	b1,x:RB_S
	rep	#5
	lsr	a
	move	a1,x:RB_SCR
	move	#>33,a
	move	a1,x:RB_REM
	move	x:L_DBASE,a		; the records
	move	a1,b
	lsl	a
	lsl	a
	add	b,a
	move	#>DESC,x0
	add	x0,a
	move	a1,r0
	move	#PT,r4
	move	x:L_WMUL,y0
	move	#>$008000,y1
_run:	move	x:RB_REM,a		; a run of one screen's columns
	tst	a
	jeq	_done
	move	x:RB_S,x0
	move	#>32,b
	sub	x0,b
	cmp	b,a
	tlt	a,b
	move	b1,x:RB_LEN
	move	x:RB_SCR,b
	jclr	#0,b1,_s0
	move	x:RB_PB,a
	jmp	_sp
_s0:	move	x:RB_PA,a
_sp:	move	x:RB_S,x0
	add	x0,a
	move	a1,r2
	move	x:RB_LEN,x0
	move	x:RB_REM,a
	sub	x0,a
	move	a1,x:RB_REM
	clr	a
	move	a1,x:RB_S
	move	x:RB_SCR,a
	move	#>1,x0
	add	x0,a
	move	x:L_WIDE,x0
	and	x0,a
	move	a1,x:RB_SCR
	move	x:RB_LEN,x0
	do	x0,_lp
	movem	p:(r2)+,a		; the map entry
	move	a1,n1			; (kept in n1: bit tests)
	move	#>$3ff,x0
	and	x0,a
	move	a1,x0
	mpy	x0,y0,b			; chr << wsh
	move	b0,a
	move	x:RBASE,x0
	jclr	#15,n1,_nv
	move	x:RBASEV,x0
_nv:	add	x0,a
	move	#>$7fff,x0
	and	x0,a			; the row's VRAM word address
	move	a1,x0
	mpy	x0,y1,b			; its page
	move	b1,a
	move	x:RB_FIM,b
	add	b,a
	move	a1,n4
	move	x0,a
	move	#>$ff,x0
	and	x0,a
	move	x:(r4+n4),b
	add	a,b			; the row's P address
	move	x:L_Y0,x0
	move	x0,x:(r0)+		; DY
	move	#>-1,x0			; DA = P - DS
	jclr	#15,n1,_dv
	move	#>1,x0
_dv:	add	x0,b
	jset	#14,n1,_fl
	jclr	#15,n1,_nf
_fl:	move	#>$400000,x0
	or	x0,b
_nf:	move	b1,x:(r0)+		; DA
	move	#>1,x0
	jclr	#15,n1,_ds
	move	#>-1,x0
_ds:	move	x0,x:(r0)+		; DS
	move	x:KLO,b			; DK
	jclr	#13,n1,_kp
	move	x:KHI,b
_kp:	move	n1,a
	move	#>$1c00,x0
	and	x0,a
	move	a1,x0
	move	x:PSHM,x1
	mpy	x0,x1,a
	move	a1,x0
	add	x0,b
	move	b1,x:(r0)+
	clr	b			; DF
	jclr	#14,n1,_df
	move	#>1,b
_df:	move	b1,x:(r0)+
_lp:
	jmp	_run
_done:	move	x:L_DIRECT,a		; drawn: the fast loop for 4 bpp straight
	tst	a			; into the line, else record by record
	jeq	draw_gen
	move	x:L_FI,a
	move	#>1,x0
	cmp	x0,a
	jne	draw_gen
	jmp	step_go
RCEND	equ	@CVI(@LCV(R))
	if	RCEND>$8000
	fail	'the cold code does not fit P:$7F00-$7FFF'
	endif
	endif

HOTLEN	equ	@CVI(HOTL)-HOTL0	; init copies this many words
HOTEND	equ	@CVI(HOTR)
	if	HOTEND>$0200
	fail	'the hot routines do not fit internal P'
	endif
	if	HOTLEN>$0200
	fail	'the hot routines do not fit the ring'
	endif
