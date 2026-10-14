; SNES tile modes on the Falcon DSP, v2: BG modes 0 and 1 (2-6 as 1, as
; the reference does), sprites, priorities; eight planes whose pixels are
; CGRAM indices, each row streamed over the SSI into DMA record (handshake
; mode, SC2 = frame line), 128 words: per 16-pixel group planes 0..7.
;
; Reference: ppu_render() with the identity map + ppu_planar8(); the host
; model tools/dsp_model.c describes the composition step by step, the
; stream is src/ppu_dsp.h. PPUTEST.TOS checks this program against it.
;
; How a row is composed (plane words sit in bits 23-8, as the SSI sends
; them):
;   layers  33 slivers of 8 pixels from the one at or left of the first
;           pixel, 10 words each: the tile's planes, the opaque mask by
;           priority (MLO, MHI), then the palette base's bits as 00/FF.
;           A 16-pixel group of a plane is its bytes in slivers 2g, 2g+1,
;           2g+2 shifted by the fine scroll f: b0 << (16+f) + b1 << (8+f)
;           + b2 << f, bits 23-8, which the multiplier computes in A0
;           (KONST: the three factors, read modulo 3).
;           Per sliver a descriptor holds the P address of its character
;           row (DA), the step to the next row (DS: +-1 by vertical flip)
;           and the handler that splits the row by bpp, flip and priority
;           (DH). They are built when a layer reaches a new 8-row block of
;           its map (or scrolls), and stepped on each row inside one; the
;           palette planes are written with them.
;   sprites 18 groups (group 0 is x = -16..-1) of planes 0-7, the mask and
;           two priority planes; slivers are laid over each other as the
;           reference does (the lowest OAM index ends on top).
;   compose per group front to back: every pass takes the pixels no
;           earlier pass covered. Plane 7 of a BG pixel is always 0 and
;           BG3's planes 5-7 in mode 1 too: those are left out.
;   output  a ring of 512 words (RING; r7 writes, r6 reads). The SSI
;           transmit interrupt (tx_isr) sends from it and marks what it sent
;           (bit 0); it stops at a marked word, so it never passes the
;           writer however long the writer takes. The frame's first word
;           goes out polled: Hatari raises the transmit interrupt only once
;           record has taken a word (see scummvm's hsc2p).
;
; Registers r6/m6 belong to the transmit interrupt, r7/m7 to the ring's
; writer; no other code touches them.
;
; Memory (external RAM: Y:a = P:a for a < $4000, X:a = P:$4000+a)
;   X:$0000-$00FF  variables, PT (VRAM page -> P address) at $80
;   Y:$0000-$00FF  KONST, POW2, ACC, tables, pass descriptors
;   P:$0000-$0BFF  this program (Y:$0200-$0BFF external)
;   Y:$0C00-$0FFF  sprite row, OM, sprite list, REV
;   Y:$1000-$11FF  RING
;   Y:$1200-$12FF  the OAM segment's y and height per sprite
;   X:$0100-$06EF  the frame's stream; O_B at $06F0
;   P:$4700-$56FF  (X:$0700-$16FF) 16 more VRAM pages
;   X:$1700-$18FF  OBJCA: the same for the two sprite name tables
;   X:$1900-$1FFF  per layer $1C0: strips (34 x 10), DA, DS, DH (34 each)
;   P:$1300-$3FFF, P:$6000-$7FFF  VRAM pages as the CPU places them (93
;                  slots with P:$4700-$56FF; desc finds a character's row
;                  through the page table PT)
;
;
; The window build (WIN, falcon/dsp/ppu.asm with WIN=1) takes frames whose
; rows use main-screen windows or add the sub screen: five more slots
; (ppu_dsp.h DSP_SLOT_WSEL..DSP_SLOT_SUB) and, whenever a window slot
; changes, keep masks per layer and group (win_build: the pixels its
; windows leave). Compose ANDs a BG pass's mask with its layer's keep mask
; (a sixth pass descriptor word), and the sprites' priority masks OM are
; ANDed with OBJ's after obj_row. On a row with the SUB slot's bit 16 it
; draws the sub screen's layers too, composes the sub screen first (into
; SUBBUF, SCOV) and merges it into the main screen's groups as
; ppu_sub_cfg says (fx_compose, comp_group's _mrg: the checkerboard, the
; relocated CGRAM blocks after the OAM segments in the stream, the black
; backdrop). It gives the last four VRAM page slots (P:$5300-$56FF, 89
; left) to its routines and tables:
;   P:$5300-$54FF  its routines (win_build .. fx_compose)
;   X:$1500-$166F  WREG (the slots), KEEP (5 x 16), WR1/WR2 (a window's
;                  range per group), WZ (zeros), LM/RM (edge masks), the
;                  sub screen's groups and the merge's words
;
; Loading: the runtime resets the DSP and hands the bootstrap internal P
; ($0000-$01FF); start then takes the other blocks (space 0 P, 1 X, 2 Y,
; address, count, words) until space 3 (falcon/smkx_dspboot.s).
;
; Host commands (24-bit words):
;   $000004 PING    answers $505055 ("PPU")
;   $000010 FRAME   n (at most 1520), then the n stream words after
;                   DSP_CMD_FRAME
;   $000020 PAGE    VRAM page (0-127), P address, 256 VRAM words
;   $000021 PAGE24  the same with 24-bit words (over the SSI: the Mode 7
;                   plane's packed words take two slots each)
;   $000003 SSION   commands from the SSI from now on (DMA playback through
;                   the crossbar, handshake mode: get_x); answers 1 over the
;                   host port
;   $000009 SSIOFF  (over the SSI) commands from the host port again

	include	'ioequ.inc'

TIE	equ	14			; SSI CRB
; LCB=1 (with M7K=0, WIN=0; F030SMW's list compositor): per frame the host first sends LCMASK (224 words, one per
; row: bit g set = compose group g); the program then composes only those groups, skips rows with no bit set,
; and sends each composed group as its eight plane words, in row order and, in a row, group order (the host knows
; the masks: no headers). Masks and variables live in X:$0500-$05EF, the end of the stream's room (the stream may
; hold 1024 words, ppu_dsp.h DSP_LC_STREAM_MAX; all 93 VRAM page slots stay). Everything else as for the tile
; program. (F030CT, 2026-10-01: they were at X:$3000-$30FF = P:$7000, which cost 16 page slots; CT's field frames
; read up to 91 pages.)
PROF	equ	0			; profiling builds: 1 no layers, 2 no sprites, 4 no compose,
					; 8 no slivers, 16 no descriptor work (wrong output)

; stream slots (ppu_dsp.h)
S_INIDISP	equ	0
S_BGMODE	equ	1
S_TM		equ	2
S_OBSEL		equ	3
S_SC		equ	4
S_NBA		equ	8
S_OFS		equ	10

; X variables; the bit-tested ones below $40
SLOT	equ	$00		; 18
O_AT	equ	$12
FLIP	equ	$13
TXSTART	equ	$14
T_E	equ	$2A		; compose's pass list
T_I	equ	$2B
T_BLK	equ	$2C
T_BPP	equ	$2D
SEGNEW	equ	$33		; bit 0: a new OAM segment (obj_row's table)
DK1	equ	$18		; per layer: configuration key
DK2	equ	$1C		; per layer: 8-row block and first column
DKY	equ	$20		; per layer: the row's y
ANYLO	equ	$24		; per layer: any low-priority pixel on the row
ANYHI	equ	$35		; per layer: any high-priority pixel
OBJANY	equ	$39		; per sprite priority: any pixel on the row
ATKEY	equ	$15		; per layer: BPP | PALB << 8 of its AT table ($15-$17, $2E)
L_AT	equ	$2F		; the layer's AT table
L_NPP	equ	$30		; 8 - bpp: palette planes per sliver
LAY	equ	$28
BPP	equ	$29
L_WIDE	equ	$31
L_TALL	equ	$32
L_BIG	equ	$34
L_E	equ	$3D
L_FYE	equ	$3E
MODE	equ	$3F
ROW	equ	$40
ROWK	equ	$41
CHGP	equ	$42
CHGN	equ	$43
SEGP	equ	$44
SEGN	equ	$45
CURS	equ	$46
CURN	equ	$47
PALB	equ	$48
NPASS	equ	$49
CA_PG	equ	$4A
SPM	equ	$4B
L_MAP	equ	$4C
L_CHR	equ	$4D
L_TM1	equ	$4E
L_MW	equ	$4F
L_MH	equ	$50
L_FY	equ	$51
L_ROWB	equ	$52
L_PX	equ	$53
L_FINE	equ	$54
L_CA	equ	$55
L_K	equ	$56
L_RP0	equ	$57
L_RP1	equ	$58
L_LB	equ	$59
L_Y	equ	$5A
L_K1	equ	$5B
L_K2	equ	$5C
L_DS	equ	$5D
C_UNC	equ	$5E
C_G	equ	$5F
C_G20	equ	$60
O_N	equ	$61
O_SL	equ	$62
O_X	equ	$63
O_DY	equ	$64
O_W	equ	$65
O_H	equ	$66
O_W1	equ	$67
O_TC	equ	$68
O_TR	equ	$69
O_BASE	equ	$6A
O_HI	equ	$6B
O_PRI	equ	$6C
O_NCOL	equ	$6D
O_COL	equ	$6E
O_SX	equ	$6F
O_YH	equ	$70
O_YL	equ	$71
O_MG	equ	$72
O_MG1	equ	$73
O_SW	equ	$74
O_SH	equ	$75
O_LW	equ	$76
O_LH	equ	$77
CAKEY	equ	$78		; per layer, then OBJKEY
OBJKEY	equ	$7C
O_G	equ	$7D
L_DI	equ	$7E
OCLEAN	equ	$7F		; OBJA, OM and OBJANY hold no sprite
PT	equ	$80

; Y internal
KONST	equ	$00		; 4 per layer: window factors (modulo 3)
POW2	equ	$10		; 1 << n, n = 0..23
ACC	equ	$28		; the group's eight plane words
OBJSZ	equ	$30		; per OBSEL size: small w, h, large w, h
LBTAB	equ	$50		; layer blocks
CATAB	equ	$54		; layer CADDR tables
HND	equ	$58		; handler by 4bpp * 4 + hflip * 2 + priority
OFF4	equ	$60		; plane -> word in a 4bpp sliver
OFF2	equ	$68		; the same, 2bpp
TPL0	equ	$70		; pass lists: BG i priority h = 2i + h, OBJ p = 8 + p
TPL1	equ	$80
TPL1T	equ	$90
PASSD	equ	$A0		; per pass PASSW words (comp_group): 12 passes in mode 0
	if	WIN
PASSW	equ	6		; + the layer's keep masks
ATTAB	equ	$E8		; (after the longer pass descriptors)
PBTAB	equ	$EC
	else
PASSW	equ	5
ATTAB	equ	$E0		; per layer: its AT table
PBTAB	equ	$E4		; per layer: its palette plane blocks
	endif
OFF8	equ	$E8		; M7K: plane -> word in an 8bpp sliver
TPL7	equ	$F0		; M7K: the Mode 7 pass list
TPL2	equ	$F0		; the tile programs: modes 2-6 (snes9x's order:
				; OBJ 3, BG1 H, OBJ 2, BG2 H, OBJ 1, BG1 L, OBJ 0,
				; BG2 L)
POW8	equ	$F8		; M7K: c2p factors 2^(8-i), pixel i of a sliver

; Y external
OBJA	equ	$0C00		; 10 arrays x 18 groups: planes 0-6, the mask
				; (= plane 7), priority bits 0 and 1
OM	equ	$0CD0		; OBJ mask per priority, 4 x 16 groups
LIST	equ	$0D10		; in-range sprites, 32
PBLK	equ	$0D30		; palette plane blocks: 4 layers x 8 x 6
REV	equ	$0E00
AT	equ	$0F00		; attribute tables: 4 layers x 64 (desc)
RING	equ	$1000
SPRY	equ	$1200		; a segment's sprites: y, then height at +$80

; X external
	if	M7K==0
STREAM	equ	$0100
O_B	equ	$06F0		; a sprite sliver's four plane bytes
O_NMG	equ	$06F8		; its mask's complement in groups G and G + 1
O_NMG1	equ	$06F9
O_LP	equ	$06FA		; obj_row: the sprite list, from the last back
OPTP	equ	$06FB		; the stream's offset-per-tile table, or 0
OPTK	equ	$06FC		; desc_opt: the next sliver's table word
OPTY	equ	$06FD		;           the row's y and first map x by the
OPTX	equ	$06FE		;           registers
OPTI	equ	$06FF		;           8 * the sliver
CADDR	equ	$0700
OBJCA	equ	$1700
LAYERS	equ	$1900
	else
; The Mode 7 program (M7K): VRAM words 0-$3FFF at P:$1300-$52FF, packed by
; the CPU as (tile * 64) << 8 | texel (the tile from the word's low byte,
; the texel from its high byte); so X:$0100-$12FF (P:$4100-$52FF) is VRAM
; and everything X keeps moves above it. Sprite pages: P:$7400-$7FFF.
STREAM	equ	$1300		; up to 2000 words (ppu_dsp.h DSP_M7_STREAM_MAX)
O_B	equ	$1AD0
O_NMG	equ	$1AD8
O_NMG1	equ	$1AD9
O_LP	equ	$1ADA
OBJCA	equ	$1B00
LAYERS	equ	$1D00		; layer 0's block only
CADDR	equ	$1D00		; (no tile layers: the table entries are not used)
LINE	equ	$1EC0		; the row's 256 texels
C2PT	equ	$1FC0		; c2p: T1, T2, T3 (256 each)
TVM	equ	$2300		; per tv (0..1023): $1300 + (tv >> 3) * 128
TUM	equ	$2700		;          tu:      tu >> 3
TVF	equ	$2B00		;          tv:      $1300 + (tv & 7) * 8
TUF	equ	$2F00		;          tu:      tu & 7
M7P	equ	L_MAP		; the stream's Mode 7 entry of the next row
M7N	equ	L_CHR		; segments left in the row
M7C	equ	L_TM1		; the segment's kind << 16 | count
M7S0	equ	L_MW		; TILE0: the step while x holds a mask
M7S1	equ	L_MH
	endif
	if	WIN
WREG	equ	$1500		; the slots: WSEL, WH01, WH23, WLOG, SUB
WDIRTY	equ	$1505		; nonzero: a window slot changed (win_build)
WSELS	equ	$1506		; win_layer: selects, this layer's in bits 3-0
WLOGS	equ	$1507		;            logic, this layer's in bits 1-0
WTM	equ	$1508		;            this layer's TMW bit (in WLOG)
MMV	equ	$1509		; comp_group: the pixels whose layer takes part
				; in colour math
SUBMODE	equ	$150A		; nonzero: compose the sub screen (into SUBBUF)
CMASK	equ	$150B		; compose's layers: TM, or the sub screen's
DCHK	equ	$150C		; the row's checkerboard (x + row even)
SAVETM	equ	$150D		; TM while S_TM holds TM | TS (the layers drawn)
NREL	equ	$150E		; the frame's relocated CGRAM blocks (RELT)
EXV	equ	$150F		; merge: backdrop pixels shown from the sub screen
KEEP	equ	$1510		; per layer (BG1-4, OBJ) and group: pixels kept
WR1	equ	$1560		; per group: window 1's pixels
WR2	equ	$1570		; window 2's
WZ	equ	$1580		; zeros (a window not enabled)
LM	equ	$1590		; LM[a]: pixels a-15 of a group (bits 23-8)
RM	equ	$15A0		; RM[b]: pixels 0-b
SUBBUF	equ	$15B0		; the sub screen: per group its eight planes
SCOV	equ	$1630		;   and the pixels it covers
RELT	equ	$1640		; relocations: block | target block << 4
MDV	equ	$1650		; merge: the checkerboard's pixels
INCV	equ	$1651		;   those of relocated blocks
RR	equ	$1652		;   their planes 4-7, relocated
PV	equ	$1656		;   the sub screen's planes 4-7
NPV	equ	$165A		;   and their complements
SRCBUF	equ	$1660		;   the planes the checkerboard shows
	endif
	if	LCB
MASKS	equ	$0500		; X: per row, bit g set = compose group g (224 words)
RMASK	equ	$05E0		; the row's mask, shifted right per group
LCN	equ	$05E1		; words sent this frame (mod 8 matters)
SL0	equ	$05E2		; the row's first sliver (2 * the first group)
SL0W	equ	$05E3		; SL0 * SLW: its strip's offset
SLN	equ	$05E4		; slivers the row needs (33: all, with the normal descriptor keys)
SLN1	equ	$05E5		; SLN - 1
LCLO	equ	$05E6		; the row's first group
	endif
SLW	equ	10		; words per sliver
DA	equ	340		; in a layer block
DS	equ	374
DH	equ	408

	org	y:OBJSZ
	dc	8,8,16,16,8,8,32,32,8,8,64,64,16,16,32,32
	dc	16,16,64,64,32,32,64,64,16,32,32,64,16,32,32,32
	dc	LAYERS,LAYERS+$1C0,LAYERS+$380,LAYERS+$540
	dc	0,0,0,0			; (CATAB: character tables, no longer)
	if	M7K==0
	dc	h2nl,h2nh,h2fl,h2fh,h4nl,h4nh,h4fl,h4fh
	else
	dc	0,0,0,0,0,0,0,0
	endif
	dc	0,1,2,3,6,7,8,9
	dc	0,1,4,5,6,7,8,9
	org	y:TPL0
	dc	11,1,3,10,0,2,9,5,7,8,4,6,$FFFFFF
	org	y:TPL1
	dc	11,1,3,10,0,2,9,5,8,4,$FFFFFF
	org	y:TPL1T
	dc	5,11,1,3,10,0,2,9,8,4,$FFFFFF
	if	M7K==0
	org	y:TPL2
	dc	11,1,10,3,9,0,8,2,$FFFFFF
	endif
	org	y:ATTAB
	dc	AT,AT+64,AT+128,AT+192
	dc	PBLK,PBLK+48,PBLK+96,PBLK+144
	if	M7K
	org	y:OFF8
	dc	0,1,2,3,4,5,6,7
	org	y:TPL7
	dc	11,10,9,0,8,$FFFFFF
	org	y:POW8
	dc	256,128,64,32,16,8,4,2

; Coordinate tables (as m7c2p.asm's, with the P base folded in) and the c2p
; tables: T1[v] = b0 << 22 | b1 << 13 | b2 << 5, T2 the same for bits
; 3-5, T3 for bits 6, 7 and v != 0 (the opaque mask).
	org	x:TVM
T	set	0
	dup	1024
	dc	$1300+(T/8)*128
T	set	T+1
	endm
	org	x:TUM
T	set	0
	dup	1024
	dc	T/8
T	set	T+1
	endm
	org	x:TVF
T	set	0
	dup	1024
	dc	$1300+(T-(T/8)*8)*8
T	set	T+1
	endm
	org	x:TUF
T	set	0
	dup	1024
	dc	T-(T/8)*8
T	set	T+1
	endm
	org	x:C2PT
T	set	0
	dup	256
	dc	(T-(T/2)*2)*$400000+((T/2)-(T/4)*2)*$2000+((T/4)-(T/8)*2)*$20
T	set	T+1
	endm
T	set	0
	dup	256
	dc	((T/8)-(T/16)*2)*$400000+((T/16)-(T/32)*2)*$2000+((T/32)-(T/64)*2)*$20
T	set	T+1
	endm
T	set	0
	dup	256
	dc	((T/64)-(T/128)*2)*$400000+((T/128)-(T/256)*2)*$2000+((T+255)/256)*$20
T	set	T+1
	endm
	endif

	if	WIN
	org	x:WZ
	dup	16
	dc	0
	endm
	org	x:LM			; ($10000 / 2^a - 1) << 8
P	set	1
	dup	16
	dc	(($10000/P)-1)*256
P	set	P*2
	endm
	org	x:RM			; ($FFFF - ($10000 / 2^(b+1) - 1)) << 8
P	set	2
	dup	16
	dc	($FFFF-(($10000/P)-1))*256
P	set	P*2
	endm
	endif

	org	p:$0000
	jmp	start

	org	p:$0010			; SSI transmit data: tx_isr
	jsr	tx_isr
	jsr	tx_isr			; with exception status: the same

	org	p:$0040
start:
	jmp	start2

; The transmit interrupt: the next ring word, unless it is sent already
; (bit 0 set: the writer has not reached it); then the interrupt goes off
; until the writer turns it on again. Plane words have bits 7-0 clear and
; the SSI sends bits 23-8, so the mark costs nothing.
tx_isr:
	jset	#0,y:(r6),_stop
	movep	y:(r6),x:m_tx
	bset	#0,y:(r6)+
	rti
_stop:	bclr	#TIE,x:m_crb
	rti

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

; ===========================================================================
; Hot code: internal P.
; ===========================================================================

	if	M7K==0
; One layer's row of slivers from its descriptors: r0 -> DA, r1 -> DH,
; r3 -> the strip, n3 = the rest of a sliver after the handler, n2 = 8,
; r5 = REV, y0 = $8000, y1 = $FF.
slivers:
	if	LCB
	move	x:SLN,x0
	do	x0,_sl
	else
	do	#33,_sl
	endif
	move	x:(r1)+,r4
	move	x:(r0)+,r2
	jsr	(r4)
	move	(r3)+n3
_sl:
	rts

; Handlers: r2 = P address of the character row, r3 -> the sliver. They
; store the tile's planes, then MLO and MHI.
h4nl:
	movem	p:(r2),x0		; planes 0 and 1
	move	x0,a
	and	y1,a
	mpy	x0,y0,b		a1,x:(r3)+
	move	b1,x1
	or	x1,a		b1,x:(r3)+
	movem	p:(r2+n2),x0		; planes 2 and 3
	move	x0,b
	and	y1,b
	move	b1,x1
	or	x1,a		b1,x:(r3)+
	mpy	x0,y0,b
	move	b1,x1
	or	x1,a		b1,x:(r3)+
	clr	b		a1,x:(r3)+	; MLO = the mask, MHI = 0
	move	b1,x:(r3)+
	rts

h4nh:
	movem	p:(r2),x0		; planes 0 and 1
	move	x0,a
	and	y1,a
	mpy	x0,y0,b		a1,x:(r3)+
	move	b1,x1
	or	x1,a		b1,x:(r3)+
	movem	p:(r2+n2),x0		; planes 2 and 3
	move	x0,b
	and	y1,b
	move	b1,x1
	or	x1,a		b1,x:(r3)+
	mpy	x0,y0,b
	move	b1,x1
	or	x1,a		b1,x:(r3)+
	clr	b
	move	b1,x:(r3)+		; MLO = 0, MHI = the mask
	move	a1,x:(r3)+
	rts

h4fl:
	movem	p:(r2),x0		; planes 0 and 1, bits reversed
	move	x0,a
	and	y1,a
	move	a1,n5
	mpy	x0,y0,b
	move	y:(r5+n5),a
	move	b1,n5
	move	a1,x:(r3)+
	move	y:(r5+n5),x1
	or	x1,a		x1,x:(r3)+
	movem	p:(r2+n2),x0		; planes 2 and 3
	move	x0,b
	and	y1,b
	move	b1,n5
	mpy	x0,y0,b
	move	y:(r5+n5),x1
	move	b1,n5
	or	x1,a		x1,x:(r3)+
	move	y:(r5+n5),x1
	or	x1,a		x1,x:(r3)+
	clr	b		a1,x:(r3)+	; MLO = the mask, MHI = 0
	move	b1,x:(r3)+
	rts

h4fh:
	movem	p:(r2),x0		; planes 0 and 1, bits reversed
	move	x0,a
	and	y1,a
	move	a1,n5
	mpy	x0,y0,b
	move	y:(r5+n5),a
	move	b1,n5
	move	a1,x:(r3)+
	move	y:(r5+n5),x1
	or	x1,a		x1,x:(r3)+
	movem	p:(r2+n2),x0		; planes 2 and 3
	move	x0,b
	and	y1,b
	move	b1,n5
	mpy	x0,y0,b
	move	y:(r5+n5),x1
	move	b1,n5
	or	x1,a		x1,x:(r3)+
	move	y:(r5+n5),x1
	or	x1,a		x1,x:(r3)+
	clr	b
	move	b1,x:(r3)+		; MLO = 0, MHI = the mask
	move	a1,x:(r3)+
	rts

h2nl:
	movem	p:(r2),x0		; planes 0 and 1
	move	x0,a
	and	y1,a
	mpy	x0,y0,b		a1,x:(r3)+
	move	b1,x1
	or	x1,a		b1,x:(r3)+
	clr	b		a1,x:(r3)+	; MLO = the mask, MHI = 0
	move	b1,x:(r3)+
	rts

h2nh:
	movem	p:(r2),x0		; planes 0 and 1
	move	x0,a
	and	y1,a
	mpy	x0,y0,b		a1,x:(r3)+
	move	b1,x1
	or	x1,a		b1,x:(r3)+
	clr	b
	move	b1,x:(r3)+		; MLO = 0, MHI = the mask
	move	a1,x:(r3)+
	rts

h2fl:
	movem	p:(r2),x0		; planes 0 and 1, bits reversed
	move	x0,a
	and	y1,a
	move	a1,n5
	mpy	x0,y0,b
	move	y:(r5+n5),a
	move	b1,n5
	move	a1,x:(r3)+
	move	y:(r5+n5),x1
	or	x1,a		x1,x:(r3)+
	clr	b		a1,x:(r3)+	; MLO = the mask, MHI = 0
	move	b1,x:(r3)+
	rts

h2fh:
	movem	p:(r2),x0		; planes 0 and 1, bits reversed
	move	x0,a
	and	y1,a
	move	a1,n5
	mpy	x0,y0,b
	move	y:(r5+n5),a
	move	b1,n5
	move	a1,x:(r3)+
	move	y:(r5+n5),x1
	or	x1,a		x1,x:(r3)+
	clr	b
	move	b1,x:(r3)+		; MLO = 0, MHI = the mask
	move	a1,x:(r3)+
	rts

	else
; The Mode 7 program's pixel loops (m7_row sets them up): a segment's
; texels into the line buffer, as m7c2p.asm samples. y0 = count, r3 -> the
; line buffer, a1:a0 = v, u + $800000 (1/256 texel), x1:x0 = the 48-bit
; step, y1 = $008000 (a multiply by it shifts right by 8).
; INSIDE: n1 = TVM, n0 = TUM - $8000, n4 = TVF, n5 = TUF - $8000.
m7_inside:
	do	y0,_ie
	move	a1,y0
	mpy	y0,y1,b		a0,y0		; b1 = tv, y0 = u'
	mpy	y0,y1,b		b1,r1		; b1 = tu + $8000, r1 = tv
	move	b1,r0
	move	r1,r4
	move	x:(r1+n1),r2			; $1300 + (tv >> 3) * 128
	move	x:(r0+n0),n2			; tu >> 3
	move	r0,r5
	add	x,a				; the next pixel's coordinates
	movem	p:(r2+n2),y0			; the map word: (tile * 64) << 8 | ...
	move	x:(r4+n4),n2			; $1300 + (tv & 7) * 8
	mpy	y0,y1,b				; b1 = tile * 64
	move	b1,r2
	move	x:(r5+n5),y0			; tu & 7
	lua	(r2)+n2,r2
	move	y0,n2
	move	#>$ff,y0
	movem	p:(r2+n2),b			; the texel word: ... | texel
	and	y0,b
	move	b1,x:(r3)+
_ie:
	rts

; TILE0: tile 0's texels (the plane's outside, M7SEL = $C0): u and v arrive
; reduced mod 2048 texels, so tv and tu are masked to the tables' range.
; n4 = TVF, n5 = TUF; the step is in M7S0/M7S1 (x0 holds the mask).
m7_tile0:
	do	y0,_te
	move	a1,y0
	mpy	y0,y1,b		a0,y0		; b1 = tv, y0 = u'
	move	#>$3ff,x0
	and	x0,b
	move	b1,r4
	mpy	y0,y1,b				; b1 = tu + $8000
	and	x0,b
	move	b1,r5
	move	x:M7S0,x0
	move	x:M7S1,x1
	add	x,a
	move	x:(r4+n4),r2			; $1300 + (tv & 7) * 8
	move	x:(r5+n5),n2			; tu & 7
	move	#>$ff,y0
	movem	p:(r2+n2),b			; tile 0's texel
	and	y0,b
	move	b1,x:(r3)+
_te:
	rts

; The line buffer into layer 0's strip: 32 slivers of eight plane bytes,
; the opaque mask (MLO) and MHI = 0, then two empty slivers. Per sliver the
; tables T1, T2, T3 (C2PT) of its eight texels are summed with the factors
; 2^(8-i): plane bits land in a1 bits 7-0, a0 bits 22-15 and a0 bits 14-7.
m7_c2p:
	move	#LINE,r0
	move	#LAYERS,r3
	move	#C2PT,r1
	move	#C2PT+256,r2
	move	#C2PT+512,r5
	move	#POW8,r4
	move	#7,m4
	move	#8,n0
	move	#>$ff,x1
	do	#32,_cs
	clr	a
	clr	b
	do	#8,_c1
	move	x:(r0)+,n1
	move	y:(r4)+,y0
	move	n1,n2
	move	x:(r1+n1),x0			; T1[p]
	mac	x0,y0,a
	move	x:(r2+n2),x0			; T2[p]
	mac	x0,y0,b
_c1:
	move	#>$000100,y0			; shift right 15
	move	#>$010000,y1			; shift right 7
	move	a1,x:(r3)+			; plane 0
	move	a0,x0
	mpy	x0,y0,a
	move	a1,x:(r3)+			; plane 1
	mpy	x0,y1,a
	and	x1,a
	move	a1,x:(r3)+			; plane 2
	move	b1,x:(r3)+			; plane 3
	move	b0,x0
	mpy	x0,y0,b
	move	b1,x:(r3)+			; plane 4
	mpy	x0,y1,b
	and	x1,b
	move	b1,x:(r3)+			; plane 5
	move	(r0)-n0
	clr	a
	do	#8,_c2
	move	x:(r0)+,n5
	move	y:(r4)+,y0
	move	x:(r5+n5),x0			; T3[p]
	mac	x0,y0,a
_c2:
	move	#>$000100,y0
	move	a1,x:(r3)+			; plane 6
	move	a0,x0
	mpy	x0,y0,a
	move	a1,x:(r3)+			; plane 7
	mpy	x0,y1,a
	and	x1,a
	move	a1,x:(r3)+			; MLO
	clr	a
	move	a1,x:(r3)+			; MHI
_cs:
	move	#$ffff,m4
	clr	a
	rep	#2*SLW
	move	a,x:(r3)+
	rts

	endif

; ---------------------------------------------------------------------------
; One 16-pixel group, front to back, into the ring. m4 = 2.
; A pass descriptor: BG: mask base, (WIN: the layer's KEEP,) KONST, OFF
; table, planes, block; OBJ: -(OM base) - 1 and unused words.
comp_group:
	clr	a
	move	#ACC,r5
	rep	#8
	move	a,y:(r5)+
	if	WIN
	move	a1,x:MMV
	endif
	move	#>$ffff00,x0
	move	x0,x:C_UNC
	move	x:NPASS,a
	tst	a
	jeq	_out
	move	#PASSD,r1
	move	#10,n0
	move	a1,x0
	do	x0,_ps
	move	y:(r1)+,a		; kind/base
	tst	a
	jlt	_obj
	if	WIN
	move	y:(r1)+,b		; the layer's keep masks
	move	x:C_G,x0
	add	x0,b
	move	b1,r2			; (r2 is OFF only from _bg on)
	endif
	move	x:C_G20,x0		; BG: its mask window
	add	x0,a
	move	a1,r0
	move	y:(r1)+,r4		; KONST
	nop
	move	x:(r0)+n0,x0	y:(r4)+,y0
	mpy	x0,y0,a		x:(r0)+n0,x0	y:(r4)+,y0
	mac	x0,y0,a		x:(r0),x0	y:(r4)+,y0
	mac	x0,y0,a
	move	a0,x0
	clr	a			; A2 = 0: Z tests A1 alone
	move	x:C_UNC,a1
	and	x0,a
	if	WIN
	move	x:(r2),x0		; less what the windows hide
	and	x0,a
	endif
	jne	_bg
	move	(r1)+			; OFF, planes, block: not needed
	move	(r1)+
	move	(r1)+
	jmp	_next
_bg:	move	a1,x1			; eff
	if	WIN
	jclr	#23,b1,_nm		; the layer takes part in colour math
	move	x:MMV,a
	or	x1,a
	move	a1,x:MMV
_nm:
	endif
	move	x:C_UNC,a
	eor	x1,a
	move	a1,x:C_UNC
	move	y:(r1)+,r2		; OFF
	move	y:(r1)+,b		; planes
	move	y:(r1)+,a		; block
	move	x:C_G20,x0
	add	x0,a
	move	a1,r3
	move	#ACC,r5
	move	b1,x0
	do	x0,_pl
	move	y:(r2)+,n3
	nop
	lua	(r3)+n3,r0
	nop
	move	x:(r0)+n0,x0	y:(r4)+,y0
	mpy	x0,y0,a		x:(r0)+n0,x0	y:(r4)+,y0
	mac	x0,y0,a		x:(r0),x0	y:(r4)+,y0
	mac	x0,y0,a
	move	a0,x0
	move	x1,a
	and	x0,a
	move	a1,x0
	move	y:(r5),a
	or	x0,a
	move	a1,y:(r5)+
_pl:
	jmp	_next
_obj:	not	a			; OBJ: the OM base
	move	x:C_G,x0
	add	x0,a
	move	a1,r0
	if	WIN
	move	y:(r1)+,b		; bit 23: palettes 4-7 take part
	else
	move	(r1)+			; the rest of the descriptor
	endif
	move	(r1)+
	move	(r1)+
	move	(r1)+
	if	WIN
	move	(r1)+
	endif
	move	y:(r0),x0
	clr	a
	move	x:C_UNC,a1
	and	x0,a
	jeq	_next
	move	a1,x1
	if	WIN
	jclr	#23,b1,_nq		; palettes 4-7: plane 6
	move	x:C_G,a
	move	#>OBJA+1+6*18,x0
	add	x0,a
	move	a1,r3
	nop
	move	y:(r3),a
	and	x1,a
	move	a1,x0
	move	x:MMV,a
	or	x0,a
	move	a1,x:MMV
_nq:
	endif
	move	x:C_UNC,a
	eor	x1,a
	move	a1,x:C_UNC
	move	x:C_G,a
	move	#>OBJA+1,x0
	add	x0,a
	move	a1,r0
	move	#18,n0
	move	#ACC,r5
	do	#8,_op
	move	y:(r0)+n0,a
	and	x1,a
	move	a1,x0
	move	y:(r5),a
	or	x0,a
	move	a1,y:(r5)+
_op:
	move	#10,n0
_next:	move	x:C_UNC,a
	tst	a
	jne	_nx
	enddo
	jmp	_ps
_nx:	nop
_ps:
_out:
	if	WIN
	move	x:SUBMODE,a
	tst	a
	jne	_sto			; the sub screen: kept for the merge
	move	x:WREG+4,a
	jset	#16,a1,_mrg
	endif
_emit:	move	#ACC,r5
	do	#8,_o
	move	y:(r5)+,x0
	move	x0,y:(r7)+
_o:
	rts
	if	WIN
; The sub screen's group into SUBBUF + 8g, the pixels it covers into SCOV.
_sto:	move	x:C_G,a
	lsl	a
	lsl	a
	lsl	a
	move	#>SUBBUF,x0
	add	x0,a
	move	a1,r0
	move	#ACC,r5
	do	#8,_ss
	move	y:(r5)+,x0
	move	x0,x:(r0)+
_ss:
	move	x:C_G,a
	move	#>SCOV,x0
	add	x0,a
	move	a1,r0
	move	x:C_UNC,a
	not	a
	move	#>$ffff00,x0
	and	x0,a
	move	a1,x:(r0)
	rts

; The main screen's group with the sub screen (ppu_sub_cfg in WREG+4):
; the checkerboard's pixels md (whose layer takes part in colour math,
; with an opaque sub pixel, on DCHK) and the black backdrop's ex show the
; sub screen; with bit 18 md only where a relocated block's pixel is, its
; planes 4-7 those of the target block.
_mrg:	move	x:C_G,b
	move	#>SCOV,x0
	add	x0,b
	move	b1,r0
	move	x:C_G,b
	lsl	b
	lsl	b
	lsl	b
	move	#>SUBBUF,x0
	add	x0,b
	move	b1,r2
	move	x:(r0),x1		; the sub screen's pixels
	move	x:C_UNC,b
	move	#>$ffff00,x0
	and	x0,b
	move	b1,y0			; the backdrop's
	clr	b			; ex
	jclr	#17,a1,_e0
	move	y0,b
	and	x1,b
_e0:	move	b1,x:EXV
	move	x:MMV,b			; md
	jclr	#13,a1,_m1
	or	y0,b
_m1:	and	x1,b
	move	x:DCHK,x0
	and	x0,b
	move	x:EXV,y1
	move	b1,x0
	move	y1,b
	not	b
	move	b1,y1
	move	x0,b
	and	y1,b
	move	b1,x:MDV
	move	r2,r3			; the planes the checkerboard shows
	jclr	#18,a1,_mo
	jsr	fx_reloc
	move	#SRCBUF,r3
_mo:	move	x:MDV,y1		; md
	move	x:EXV,x1		; ex
	move	x1,a
	or	y1,a
	not	a
	move	a1,y0			; the main screen's
	move	#ACC,r5
	do	#8,_mg
	move	y:(r5),a
	and	y0,a
	move	a1,b
	move	x:(r2)+,a
	and	x1,a
	move	a1,x0
	or	x0,b
	move	x:(r3)+,a
	and	y1,a
	move	a1,x0
	or	x0,b
	move	b1,y:(r5)+
_mg:
	jmp	_emit
	endif


; ===========================================================================
; Cold code.
; ===========================================================================
init:
	andi	#$fb,omr		; DE = 0: X/Y:$0100-$01FF external
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
	movep	#$3000,x:m_ipr		; SSI interrupts at level 2
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
	move	#REV,r5			; REV[b] = b with its bits reversed
	move	#>0,x1
	do	#256,_rv
	move	x1,a
	clr	b
	do	#8,_rb
	lsr	a
	rol	b
_rb:
	move	b1,y:(r5)+
	move	#>1,x0
	move	x1,a
	add	x0,a
	move	a1,x1
_rv:
	clr	a
	move	#PT,r0
	rep	#128
	move	a,x:(r0)+
	move	#>$ffffff,x0		; no AT table built yet
	move	x0,x:ATKEY
	move	x0,x:ATKEY+1
	move	x0,x:ATKEY+2
	move	x0,x:$2E
	andi	#$fc,mr			; interrupts on

main:	jsr	get
	move	#>4,x0
	cmp	x0,a
	jeq	cmd_ping
	move	#>$10,x0
	cmp	x0,a
	jeq	cmd_frame
	move	#>$20,x0
	cmp	x0,a
	jeq	cmd_page
	move	#>$21,x0
	cmp	x0,a
	jeq	cmd_page24
	move	#>3,x0
	cmp	x0,a
	jeq	cmd_ssion
	move	#>9,x0
	cmp	x0,a
	jeq	cmd_ssioff
	if	LCB
	move	#>$30,x0
	cmp	x0,a
	jeq	cmd_lcmask
	endif
	jmp	main

; Input: the host port, or after SSION the SSI (DMA playback through the
; crossbar in handshake mode; from F030SMK's renderer). Every SSI slot is
; a 16-bit word, left-aligned in RX; the DSP asks for each slot with PC4
; (SC1) and for the next one as soon as it has one, so one slot is always
; on its way and none is lost. A protocol word is two slots (its bits
; 23-8, then 7-0); PAGE's data words one slot each (getw). The receiver's
; enable bit (CRB RE) is the mode. get_x changes b.
get:	jset	#13,x:m_crb,get_x
g_h:	jclr	#m_hrdf,x:m_hsr,g_h
	movep	x:m_hrx,a
	rts
get_x:
gx_s1:	jclr	#m_rdf,x:m_sr,gx_s1
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
	if	LCB
cmd_lcmask:
	move	#MASKS,r0
	do	#224,_lm
	jsr	get
	move	a1,x:(r0)+
_lm:	jmp	main
	endif

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

put:	jclr	#m_htde,x:m_hsr,put
	movep	a,x:m_htx
	rts

cmd_ping:
	move	#>$505055,a
	jsr	put
	jmp	main

cmd_page:
	jsr	get			; page
	move	a1,n0
	jsr	get			; P address
	move	a1,r1
	move	#PT,r0
	nop
	move	a1,x:(r0+n0)
	jset	#13,x:m_crb,_ps		; over the SSI: getw
	do	#256,_pg
_pw:	jclr	#m_hrdf,x:m_hsr,_pw
	movep	x:m_hrx,x0
	movem	x0,p:(r1)+
_pg:
	jmp	main
_ps:	do	#256,_pgs
	jsr	getw
	movem	a1,p:(r1)+
_pgs:
	jmp	main

cmd_page24:
	jsr	get			; page
	move	a1,n0
	jsr	get			; P address
	move	a1,r1
	move	#PT,r0
	nop
	move	a1,x:(r0+n0)
	do	#256,_p24
	jsr	get
	movem	a1,p:(r1)+
_p24:
	jmp	main

cmd_frame:
	jsr	get			; words that follow
	move	a1,x0
	move	#STREAM,r0
	jset	#13,x:m_crb,_fs		; over the SSI: get
	do	x0,_fr
_fw:	jclr	#m_hrdf,x:m_hsr,_fw
	movep	x:m_hrx,x:(r0)+
_fr:
	jmp	_fd
_fs:	do	x0,_frs
	jsr	get
	move	a1,x:(r0)+
_frs:
_fd:
	jsr	render
	jmp	main

; ---------------------------------------------------------------------------
render:
	move	#STREAM,r0
	move	#SLOT,r1
	do	#18,_sl
	move	x:(r0)+,x0
	move	x0,x:(r1)+
_sl:
	if	WIN
	move	#WREG,r1		; the window and sub screen slots
	do	#5,_wl
	move	x:(r0)+,x0
	move	x0,x:(r1)+
_wl:
	move	#>1,x0
	move	x0,x:WDIRTY
	endif
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
	if	(WIN==0)&&(M7K==0)
	tst	a			; the offset-per-tile table after them
	jeq	_ps
	move	a1,x0
	do	x0,_pw
	move	(r0)+			; first row
	move	x:(r0)+,a		; sprites
	lsl	a
	move	a1,n0
	nop
	move	(r0)+n0
_pw:
_ps:
	endif
	if	WIN
	tst	a			; the relocations after them
	jeq	_rs
	move	a1,x0
	do	x0,_rw
	move	(r0)+			; first row
	move	x:(r0)+,a		; sprites
	lsl	a
	move	a1,n0
	nop
	move	(r0)+n0
_rw:
_rs:	move	x:(r0)+,a
	move	a1,x:NREL
	tst	a
	jeq	_rn
	move	#RELT,r1
	move	a1,x0
	do	x0,_rl
	move	x:(r0)+,x1
	move	x1,x:(r1)+
_rl:
_rn:
	endif
	if	M7K==0
	move	x:(r0)+,a		; offset-per-tile: 0 or 66 words (ppu_dsp.h)
	clr	b
	tst	a
	jeq	_po
	move	r0,b
_po:	move	b1,x:OPTP
	endif
	if	M7K
	tst	a			; the Mode 7 section after them
	jeq	_m7s
	move	a1,x0
	do	x0,_m7w
	move	(r0)+			; first row
	move	x:(r0)+,a		; sprites
	lsl	a
	move	a1,n0
	nop
	move	(r0)+n0
_m7w:
_m7s:	move	r0,x:M7P
	endif
	clr	a
	if	LCB
	move	a1,x:LCN
	endif
	move	a1,x:CURN
	move	a1,x:ROW
	move	a1,x:TXSTART
	move	a1,x:OCLEAN
	move	#>$ffffff,x0		; tables and descriptors are built again
	move	#CAKEY,r1
	rep	#5
	move	x0,x:(r1)+
	move	#DK1,r1
	rep	#4
	move	x0,x:(r1)+
	ori	#$03,mr			; the ring: empty (all marked sent), the
	bclr	#TIE,x:m_crb		; interrupt off
	move	#RING,r6
	move	#RING,r7
	move	#>1,a
	rep	#512
	move	a,y:(r7)+
	andi	#$fc,mr
_row:
	if	LCB
	move	x:ROW,a			; rows without a group to compose are skipped
	move	#MASKS,r0
	move	a1,n0
	nop
	move	x:(r0+n0),a
	tst	a
	jeq	_rk
	move	a1,x:RMASK
	jsr	lc_range
	endif
	jsr	apply
	jsr	room
	jsr	row
	jsr	gate
_rk:	move	x:ROW,a
	move	#>1,x0
	add	x0,a
	move	a1,x:ROW
	move	#>224,x0
	cmp	x0,a
	jlt	_row
	if	LCB
	jsr	room
	endif
_dr:	jsr	gate			; the rest: the interrupt sends it
	jsr	fill
	tst	a
	jne	_dr
	bclr	#TIE,x:m_crb
	rts

; Words queued in the ring -> a (positive: AND leaves A2 as it was).
fill:	move	r7,a
	move	r6,x0
	sub	x0,a
	move	#>$1ff,x0
	and	x0,a
	move	a1,x0
	move	x0,a
	rts

; The transmit interrupt on when words are queued (it turns itself off).
gate:	jsr	fill
	tst	a
	jeq	_x
	jsr	tx_start
	bset	#TIE,x:m_crb
_x:	rts

; The frame's first word, polled; the transmit interrupt must be off.
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

; The changes and the OAM segment that start at this row.
apply:
	move	x:ROW,a
	rep	#8
	lsl	a
	move	#>$ff,x0
	or	x0,a
	move	a1,x:ROWK		; row << 8 | $FF
_ch:	move	x:CHGN,a
	tst	a
	jeq	_seg
	move	x:CHGP,r0
	move	x:ROWK,x0
	move	x:(r0),a
	cmp	x0,a
	jgt	_seg
	move	#>$ff,x0
	and	x0,a
	if	WIN
	move	#>18,x0			; a window slot: WREG, and the keep
	cmp	x0,a			; masks are built again
	jlt	_sl
	move	#WREG-18,r1
	move	#>1,x0
	move	x0,x:WDIRTY
	jmp	_sw
_sl:	move	#SLOT,r1
_sw:
	else
	move	#SLOT,r1
	endif
	move	a1,n1
	move	(r0)+
	move	x:(r0)+,x0
	move	x0,x:(r1+n1)
	move	r0,x:CHGP
	move	x:CHGN,a
	move	#>1,x0
	sub	x0,a
	move	a1,x:CHGN
	jmp	_ch
_seg:	move	x:SEGN,a
	tst	a
	jeq	_x
	move	x:SEGP,r0
	move	x:ROW,x0
	nop
	move	x:(r0),a
	cmp	x0,a
	jgt	_x
	move	(r0)+
	move	x:(r0)+,a		; sprites
	move	a1,x:CURN
	move	r0,x:CURS
	bset	#0,x:SEGNEW
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

blank_row:
	if	LCB
	move	x:RMASK,a		; the list holds only the groups the row's mask names: eight zero words each
	clr	b
	do	#16,_bg
	lsr	a
	jcc	_bn
	rep	#8
	move	b,y:(r7)+
_bn:	nop
	nop
_bg:
	rts
	else
	clr	a
	rep	#128
	move	a,y:(r7)+
	rts
	endif

; ---------------------------------------------------------------------------
row:
	if	M7K
	jset	#7,x:SLOT+S_INIDISP,_m7b
	move	x:SLOT+S_BGMODE,a
	move	#>7,x0
	and	x0,a
	cmp	x0,a
	jne	_m7b			; (the planner sends no such frame)
	move	x0,x:MODE
	jsr	m7_row
	jsr	gate
	jsr	obj_row
	jsr	gate
	jsr	compose
	rts
_m7b:	jsr	m7_skip			; every row has its entry
	jmp	blank_row
	else
	jset	#7,x:SLOT+S_INIDISP,blank_row
	if	WIN
	move	x:WDIRTY,a
	tst	a
	jeq	_wk
	jsr	win_build
_wk:	move	x:SLOT+S_TM,x0		; the sub screen's layers drawn too
	move	x0,x:SAVETM
	move	x:WREG+4,a
	jclr	#16,a1,_ns
	move	#>$1f,x1
	and	x1,a
	or	x0,a
	move	a1,x:SLOT+S_TM
_ns:
	endif
	if	PROF&4
	jsr	_prof
	endif
	move	x:SLOT+S_BGMODE,a
	move	#>7,x0
	and	x0,a
	move	a1,x:MODE
	cmp	x0,a
	jeq	blank_row		; Mode 7: not yet
	tst	a
	jne	_m1
	move	#>2,x0			; mode 0: four 2bpp layers, palettes 32 apart
	move	x0,x:BPP
	clr	a
	move	a1,x:LAY
	move	a1,x:PALB
	jclr	#0,x:SLOT+S_TM,_a
	jsr	layer_row
_a:	move	#>1,x0
	move	x0,x:LAY
	move	#>32,x0
	move	x0,x:PALB
	jclr	#1,x:SLOT+S_TM,_b
	jsr	layer_row
_b:	move	#>2,x0
	move	x0,x:LAY
	move	#>64,x0
	move	x0,x:PALB
	jclr	#2,x:SLOT+S_TM,_c
	jsr	layer_row
_c:	move	#>3,x0
	move	x0,x:LAY
	move	#>96,x0
	move	x0,x:PALB
	jclr	#3,x:SLOT+S_TM,_o
	jsr	layer_row
	jmp	_o
_m1:	move	#>4,x0			; mode 1: 4bpp, 4bpp, 2bpp; mode 2: 4bpp, 4bpp
	move	x0,x:BPP
	clr	a
	move	a1,x:LAY
	move	a1,x:PALB
	jclr	#0,x:SLOT+S_TM,_d
	jsr	layer_row
_d:	move	#>1,x0
	move	x0,x:LAY
	jclr	#1,x:SLOT+S_TM,_e
	jsr	layer_row
_e:	move	x:MODE,a		; BG3: mode 1 only (mode 2 has BG1 and BG2)
	move	#>1,x0
	cmp	x0,a
	jne	_o
	move	#>2,x0
	move	x0,x:LAY
	move	x0,x:BPP
	jclr	#2,x:SLOT+S_TM,_o
	jsr	layer_row
_o:	jsr	gate
	if	(PROF&2)==0
	jsr	obj_row
	if	WIN
	jsr	win_obj
	endif
	endif
	jsr	gate
	if	PROF&4
	jmp	blank_row
	endif
	if	WIN
	jsr	fx_compose
	else
	jsr	compose
	endif
	rts
_prof:	rts
	endif

	if	M7K
; ---------------------------------------------------------------------------
; A Mode 7 row: its entry of the stream's Mode 7 section (M7P) into the line
; buffer, segment by segment, then layer 0's strip for compose.
m7_row:
	move	x:M7P,r0
	move	#LINE,r3
	move	x:(r0)+,a		; segments
	tst	a
	jeq	_none
	move	a1,x:M7N
_seg:	move	x:(r0)+,b		; kind << 16 | count
	move	b1,x:M7C
	move	#>$030000,x0
	cmp	x0,b
	jge	_fill
	move	#>$020000,x0
	cmp	x0,b
	jge	_t0
	move	x:(r0)+,a		; INSIDE: v (a0 cleared)
	move	x:(r0)+,a0		; u + $800000
	move	x:(r0)+,x1		; the step
	move	x:(r0)+,x0
	move	r0,x:M7P
	move	#>$008000,y1
	move	#TVM,n1
	move	#TUM-$8000,n0
	move	#TVF,n4
	move	#TUF-$8000,n5
	jsr	_count
	jsr	m7_inside
	jmp	_next
_t0:	move	x:(r0)+,a		; TILE0
	move	x:(r0)+,a0
	move	x:(r0)+,x1
	move	x:(r0)+,x0
	move	r0,x:M7P
	move	x0,x:M7S0
	move	x1,x:M7S1
	move	#>$008000,y1
	move	#TVF,n4
	move	#TUF,n5
	jsr	_count
	jsr	m7_tile0
	jmp	_next
_fill:	move	r0,x:M7P		; FILL: transparent
	jsr	_count
	clr	b
	rep	y0
	move	b,x:(r3)+
_next:	move	x:M7P,r0
	move	x:M7N,a
	move	#>1,x0
	sub	x0,a
	move	a1,x:M7N
	jne	_seg
	move	r0,x:M7P
	jsr	m7_c2p
	clr	a			; layer 0 at fine scroll 0, 8 planes
	move	a1,x:LAY
	move	a1,x:L_FINE
	move	#>8,x0
	move	x0,x:BPP
	move	#>LAYERS,x0
	move	x0,x:L_LB
	jsr	set_konst
	move	#ANYLO,r1
	jsr	any_mask
	move	#ANYHI,r1
	jsr	any_mask
	rts
_none:	move	r0,x:M7P		; BG1 off: nothing of layer 0
	clr	a
	move	a1,x:ANYLO
	move	a1,x:ANYHI
	rts
_count:	move	x:M7C,b			; y0 = the segment's count
	move	#>$ffff,y0
	and	y0,b
	move	b1,y0
	rts

; A blank row's entry: stepped over.
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
	endif

	if	M7K==0
; ---------------------------------------------------------------------------
; a1 = VRAM word address -> a1 = its P address. Uses x0, y0, b, r0, n0.
vaddr:
	move	#>$7fff,x0
	and	x0,a
	move	a1,b
	move	#>$ff,x0
	and	x0,b			; offset in the page
	move	a1,x0
	move	#>$008000,y0
	mpy	x0,y0,a			; a1 = page
	move	#PT,r0
	move	a1,n0
	nop
	move	x:(r0+n0),a
	move	b1,x0
	add	x0,a
	rts

	endif

; a1 = PT[CA_PG & 127], CA_PG + 1. Uses x0, b, r0, n0.
ca_page:
	move	x:CA_PG,a
	move	#>127,x0
	and	x0,a
	move	#PT,r0
	move	a1,n0
	move	x:CA_PG,b
	move	#>1,x0
	add	x0,b
	move	b1,x:CA_PG
	move	x:(r0+n0),a
	rts
	if	M7K==0

	endif


; KONST for layer LAY and fine scroll L_FINE: 2^(15+f), 2^(7+f), 2^(f-1)
; (0 for f = 0: that byte lands below bit 8).
set_konst:
	move	x:LAY,a
	lsl	a
	lsl	a
	move	#>KONST,x0
	add	x0,a
	move	a1,r4
	move	#POW2,r5
	move	x:L_FINE,a
	move	#>15,x0
	add	x0,a
	move	a1,n5
	nop
	move	y:(r5+n5),y0
	move	y0,y:(r4)+
	move	x:L_FINE,a
	move	#>7,x0
	add	x0,a
	move	a1,n5
	nop
	move	y:(r5+n5),y0
	move	y0,y:(r4)+
	clr	b
	move	x:L_FINE,a
	tst	a
	jeq	_z
	move	#>1,x0
	sub	x0,a
	move	a1,n5
	nop
	move	y:(r5+n5),b
_z:	move	b1,y:(r4)
	rts
	if	M7K==0

; ---------------------------------------------------------------------------
; One layer's slivers for this row: LAY, BPP, PALB.
layer_row:
	if	PROF&1
	rts
	endif
	move	x:LAY,n0
	move	#SLOT+S_SC,r0
	move	x:LAY,a
	lsl	a
	move	a1,n1
	move	#SLOT+S_OFS,r1
	move	x:(r0+n0),a		; BGnSC
	move	#>$fc,x0
	and	x0,a
	rep	#8
	lsl	a
	move	a1,x:L_MAP
	move	x:(r0+n0),a
	move	#>1,x0
	and	x0,a
	move	a1,x:L_WIDE
	move	x:(r0+n0),a
	lsr	a
	and	x0,a
	move	a1,x:L_TALL
	move	#POW2+4,r4		; 16x16 tiles: BGMODE bit 4 + L
	move	x:LAY,n4
	move	x:SLOT+S_BGMODE,a
	nop
	move	y:(r4+n4),x0
	and	x0,a
	clr	b
	tst	a
	jeq	_small
	move	#>1,b
_small:	move	b1,x:L_BIG
	move	#>7,a
	tst	b
	jeq	_t8
	move	#>15,a
_t8:	move	a1,x:L_TM1
	move	#>1,x0			; map width and height in pixels - 1
	add	x0,a
	rep	#5
	lsl	a
	jclr	#0,x:L_WIDE,_nw
	lsl	a
_nw:	sub	x0,a
	move	a1,x:L_MW
	move	x:L_TM1,a
	add	x0,a
	rep	#5
	lsl	a
	jclr	#0,x:L_TALL,_nt
	lsl	a
_nt:	sub	x0,a
	move	a1,x:L_MH
	move	x:LAY,a			; character base
	lsr	a
	move	#SLOT+S_NBA,r2
	move	a1,n2
	nop
	move	x:(r2+n2),a
	jclr	#0,x:LAY,_lo
	rep	#4
	lsr	a
_lo:	move	#>$f,x0
	and	x0,a
	rep	#12
	lsl	a
	move	a1,x:L_CHR
	move	x:(r1+n1),a		; H scroll
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
	move	a1,x:L_PX
	move	(r1)+
	nop
	move	x:(r1+n1),a		; V scroll
	move	#>$3ff,x0
	and	x0,a
	move	x:ROW,x0
	add	x0,a
	move	#>1,x0
	add	x0,a
	move	x:L_MH,x0
	and	x0,a			; y
	move	a1,x:L_Y
	jsr	set_konst
	move	#LBTAB,r4
	move	x:LAY,n4
	nop
	move	y:(r4+n4),x0
	move	x0,x:L_LB
	jsr	at_build
	; keys: the configuration; the 8-row block and the first column
	move	x:L_CHR,a
	move	x:BPP,x0
	or	x0,a
	move	x:L_BIG,x0
	or	x0,a
	move	x:PALB,b		; mode 0 palettes
	tst	b
	jeq	_k0
	move	#>8,x0
	or	x0,a
_k0:	move	a1,x1
	move	#SLOT+S_SC,r0
	move	x:LAY,n0
	nop
	move	x:(r0+n0),a
	rep	#16
	lsl	a
	or	x1,a
	move	a1,x:L_K1
	; offset-per-tile (BG mode 2, the stream's table): each sliver its own
	; scroll. Key 2 is the registers' first column and vertical scroll
	; (y - row), DKY the row | $400000: the row after a row with the same
	; keys steps the slivers that stay in their 8-row block (desc_opt)
	move	x:OPTP,a
	tst	a
	jeq	_kp
	move	x:MODE,a
	move	#>2,x0
	cmp	x0,a
	jne	_kp
	move	x:LAY,a
	cmp	x0,a
	jge	_kp
	move	x:L_PX,a
	rep	#12
	lsl	a
	move	a1,b
	move	x:L_Y,a
	move	x:ROW,x0
	sub	x0,a
	move	#>$3ff,x0
	and	x0,a
	move	a1,x0
	or	x0,b
	move	b1,y1			; key 2
	move	#DK1,r0
	move	x:LAY,n0
	move	#DK2,r1
	move	x:LAY,n1
	move	#DKY,r2
	move	x:LAY,n2
	clr	a			; C_UNC: 0 build every sliver, 1 step
	move	a1,x:C_UNC
	move	x:L_K1,x0
	move	x:(r0+n0),a
	cmp	x0,a
	jne	_ob
	move	x:(r1+n1),a
	cmp	y1,a
	jne	_ob
	move	x:ROW,a
	move	#>1,x0
	sub	x0,a
	move	#>$400000,x0
	or	x0,a			; (row - 1) | $400000
	move	x:(r2+n2),b
	cmp	b,a
	jne	_ob
	move	#>1,x0
	move	x0,x:C_UNC
_ob:	move	x:L_K1,x0
	move	x0,x:(r0+n0)
	move	y1,x:(r1+n1)
	move	x:ROW,a
	move	#>$400000,x0
	or	x0,a
	move	a1,x:(r2+n2)
	jsr	desc_opt
	jmp	_run
_kp:
	if	LCB
	move	x:SLN,a
	move	#>33,x0
	cmp	x0,a
	jeq	_kq
	; a partial row: only the slivers SL0 .. SL0 + SLN - 1 get descriptors; the row's keys are left invalid, so
	; that a full row builds everything again
	move	#DK1,r0
	move	x:LAY,n0
	move	#>$ffffff,x0
	move	x0,x:(r0+n0)
	jsr	desc_row
	move	x:L_LB,x0
	move	x:SL0W,a
	add	x0,a
	move	a1,x:L_K		; its strip
	move	x:SL0,a
	add	x0,a
	move	a1,x:L_DI		; its descriptor
	move	x:SL0,a
	rep	#3
	lsl	a			; 8 * SL0 pixels on in the map
	move	x:L_PX,x0
	add	x0,a
	move	x:L_MW,x0
	and	x0,a
	move	a1,x:L_PX
	move	x:SLN,x0
	do	x0,_dvp
	jsr	desc
	nop
_dvp:
	jmp	_run
_kq:
	endif
	move	x:L_Y,a
	lsr	a
	lsr	a
	lsr	a
	rep	#8
	lsl	a
	move	x:L_PX,b
	lsr	b
	lsr	b
	lsr	b
	move	b1,x0
	or	x0,a
	move	a1,x:L_K2
	move	#DK1,r0
	move	x:LAY,n0
	move	#DK2,r1
	move	x:LAY,n1
	move	#DKY,r2
	move	x:LAY,n2
	move	x:L_K1,x0
	move	x:(r0+n0),a
	cmp	x0,a
	jne	_build
	move	x:L_K2,x0
	move	x:(r1+n1),a
	cmp	x0,a
	jne	_build
	move	x:L_Y,x0
	move	x:(r2+n2),a
	cmp	x0,a
	jeq	_run			; the same row again
	move	#>1,x1
	add	x1,a
	cmp	x0,a
	jne	_build
	move	x0,x:(r2+n2)		; the next row of the block: step
	move	x:L_LB,a
	move	#>DA,x0
	add	x0,a
	move	a1,r0
	move	#>DS-DA,x0
	add	x0,a
	move	a1,r1
	nop
	do	#33,_up
	move	x:(r0),a
	move	x:(r1)+,x0
	add	x0,a
	move	a1,x:(r0)+
_up:
	jmp	_run
_build:	move	x:L_K1,x0
	move	x0,x:(r0+n0)
	move	x:L_K2,x0
	move	x0,x:(r1+n1)
	move	x:L_Y,x0
	move	x0,x:(r2+n2)
	if	(PROF&16)==0
	jsr	desc_build
	endif
_run:	move	x:L_LB,a		; the slivers
	if	LCB
	move	x:SL0W,x0		; (a partial row: from its first sliver on)
	add	x0,a
	move	a1,r3
	move	x:L_LB,a
	move	x:SL0,x0
	add	x0,a			; DA and DH: a word a sliver
	else
	move	a1,r3
	endif
	move	#>DA,x0
	add	x0,a
	move	a1,r0
	move	#>DH-DA,x0
	add	x0,a
	move	a1,r1
	move	#8,n2
	move	#REV,r5
	move	#>$008000,y0
	move	#>$ff,y1
	move	#SLW-6,n3
	jclr	#2,x:BPP,_n2
	jmp	_go
_n2:	move	#SLW-4,n3
_go:
	if	(PROF&8)==0
	jsr	slivers
	endif
	move	#ANYLO,r1		; which priorities the row has: compose
	jsr	any_mask		; leaves out the passes it does not
	move	#ANYHI,r1
	jsr	any_mask
	jsr	gate
	rts
	endif


	if	(LCB==1)&&(WIN==0)
; The slivers a row needs from its mask of groups (A, not 0): group g uses slivers 2g .. 2g + 2. Up to six
; groups apart: from the first group's first sliver to the last group's last; otherwise all 33 (and the
; descriptors and keys as for any full row).
lc_range:
	move	#0,r0
_l0:	lsr	a
	jcs	_l1
	move	(r0)+
	jmp	_l0
_l1:	move	r0,x:LCLO
	move	x:RMASK,a
	move	#0,r0
	move	#0,r1
	do	#16,_h0
	lsr	a
	jcc	_h1
	move	r0,r1			; the last group so far
_h1:	move	(r0)+
_h0:
	move	r1,a
	move	x:LCLO,x0
	sub	x0,a			; hi - lo
	lsl	a
	move	#>3,x0
	add	x0,a			; 2 * (hi - lo) + 3 slivers
	move	#>17,x0
	cmp	x0,a
	jge	_lf			; more than 16: the whole row
	move	a1,x:SLN
	move	#>1,x0
	sub	x0,a
	move	a1,x:SLN1
	move	x:LCLO,a
	move	a1,x0
	rep	#4
	lsl	a			; 16 * lo
	move	x0,b
	lsl	b
	lsl	b			; 4 * lo
	add	b,a			; 20 * lo = SL0 * SLW
	move	a1,x:SL0W
	move	x:LCLO,a
	lsl	a
	move	a1,x:SL0
	rts
_lf:	clr	a
	move	a1,x:SL0
	move	a1,x:SL0W
	move	#>33,a
	move	a1,x:SLN
	move	#>32,a
	move	a1,x:SLN1
	rts
	endif

; OR of a mask over the 33 slivers into x:(r1 + LAY); MLO for ANYLO,
; MHI (one word further) for ANYHI.
any_mask:
	move	x:L_LB,a
	if	LCB
	move	x:SL0W,x0
	add	x0,a
	endif
	move	x:BPP,x0
	add	x0,a
	move	#>ANYHI,x0
	move	r1,b
	cmp	x0,b
	jne	_lo
	move	#>1,x0
	add	x0,a
_lo:	move	a1,r0
	move	#SLW,n0
	move	x:LAY,n1
	clr	b
	move	x:(r0)+n0,x0
	if	LCB
	move	x:SLN1,y0
	do	y0,_an
	else
	do	#32,_an
	endif
	or	x0,b		x:(r0)+n0,x0
_an:
	or	x0,b
	move	b1,x:(r1+n1)
	rts
	if	M7K==0

; The layer's descriptors and palette planes for the block L_Y is in.
desc_build:
	jsr	desc_row
	move	x:L_LB,x0
	move	x0,x:L_K
	move	x0,x:L_DI
	do	#33,_dv
	jsr	desc
	nop
_dv:
	rts

; L_Y's fine row, map row and its screens' row addresses: L_FY, L_ROWB,
; L_RP0, L_RP1.
desc_row:
	move	x:L_Y,a			; fy, ty, the map row
	move	a1,b
	move	x:L_TM1,x0
	and	x0,b
	move	b1,x:L_FY
	lsr	a
	lsr	a
	lsr	a
	jclr	#0,x:L_BIG,_ty
	lsr	a
_ty:	move	a1,b			; ty
	move	#>31,x0
	and	x0,a
	rep	#5
	lsl	a
	move	x:L_MAP,x0
	add	x0,a
	jclr	#5,b1,_ty2
	move	#>$400,x0
	jclr	#0,x:L_WIDE,_ty1
	move	#>$800,x0
_ty1:	add	x0,a
_ty2:	move	a1,x:L_ROWB
	jsr	vaddr
	move	a1,x:L_RP0
	move	x:L_ROWB,a
	move	#>$400,x0
	add	x0,a
	jsr	vaddr
	move	a1,x:L_RP1
	rts

; Offset-per-tile (snes_ppu.c ppu_opt_slivers): a BG mode 2 row's 33
; descriptors, each sliver with its own scroll from the stream's table:
; bit 22 the vertical scroll (bits 19-10) instead of the register's, bit
; 23 the horizontal one's coarse part (bits 9-3). L_Y and L_PX are the
; registers' again afterwards.
desc_opt:
	move	x:L_Y,x0
	move	x0,x:OPTY
	move	x:L_PX,x0
	move	x0,x:OPTX
	move	x:L_LB,x0		; C_G: sliver k's strip, C_G20: its descriptors
	move	x0,x:C_G
	move	x0,x:C_G20
	move	x:OPTP,a		; BG2's 33 words after BG1's
	jclr	#0,x:LAY,_o0
	move	#>33,x0
	add	x0,a
_o0:	move	a1,x:OPTK
	clr	a
	move	a1,x:OPTI
	move	#>$ffffff,x0		; L_K2: the y desc_row last took (key 2 is
	move	x0,x:L_K2		; not used on these rows)
	do	#33,_ov
	move	x:OPTK,r0
	nop
	move	x:(r0)+,a
	move	r0,x:OPTK
	move	a1,x:L_E		; (desc sets L_E afterwards)
	move	x:OPTY,b
	jclr	#22,a1,_ny
	rep	#10
	lsr	a
	move	#>$3ff,x0
	and	x0,a
	move	x:ROW,x0
	add	x0,a
	move	#>1,x0
	add	x0,a
	move	x:L_MH,x0
	and	x0,a			; (row + 1 + V) & MH
	move	a1,b
_ny:	move	x:C_UNC,a		; stepping, and still in its 8-row block:
	tst	a			; the next character row
	jeq	_bd
	move	b1,a
	move	#>7,x0
	and	x0,a
	jeq	_bd
	move	x:C_G20,a
	move	#>DA,x0
	add	x0,a
	move	a1,r0
	move	#DS-DA,n0
	nop
	move	x:(r0+n0),x0
	move	x:(r0),a
	add	x0,a
	move	a1,x:(r0)
	jmp	_nx
_bd:	move	b1,x:L_Y
	move	x:L_K2,a		; the same y as the last sliver built: its
	cmp	b,a			; row addresses stand
	jeq	_sy
	move	b1,x:L_K2
	jsr	desc_row
_sy:	move	x:L_E,a
	move	x:OPTX,b
	jclr	#23,a1,_nh
	move	#>$3f8,x0
	and	x0,a
	move	a1,b
_nh:	move	x:OPTI,x0
	add	x0,b
	move	x:L_MW,x0
	and	x0,b
	move	b1,x:L_PX
	move	x:C_G,x0
	move	x0,x:L_K
	move	x:C_G20,x0
	move	x0,x:L_DI
	jsr	desc
_nx:	move	x:C_G,a
	move	#>SLW,x0
	add	x0,a
	move	a1,x:C_G
	move	x:C_G20,a
	move	#>1,x0
	add	x0,a
	move	a1,x:C_G20
	move	x:OPTI,a
	move	#>8,x0
	add	x0,a
	move	a1,x:OPTI
	nop
_ov:
	move	x:OPTY,x0
	move	x0,x:L_Y
	move	x:OPTX,x0
	move	x0,x:L_PX
	rts

; One sliver's descriptor and palette planes: L_K -> its strip, L_DI =
; the block + its index, L_PX its map x (stepped here).
desc:
	move	x:L_PX,a
	lsr	a
	lsr	a
	lsr	a
	move	a1,b			; px >> 3
	jclr	#0,x:L_BIG,_s1
	lsr	a
_s1:	move	x:L_RP0,x1		; tx = a1: screen 1 from 32 on
	jclr	#5,a1,_s2
	move	x:L_RP1,x1
_s2:	move	#>31,x0
	and	x0,a
	add	x1,a
	move	a1,r0
	nop
	movem	p:(r0),x0		; map entry
	move	x0,x:L_E
	move	x:L_FY,a		; vertical flip
	move	#>1,x1
	jclr	#15,x:L_E,_s3
	move	x:L_TM1,a
	move	x:L_FY,x0
	sub	x0,a
	move	#>$ffffff,x1
_s3:	move	a1,x:L_FYE
	move	x1,x:L_DS
	move	x:L_E,a
	move	#>$3ff,x0
	and	x0,a			; character
	jclr	#0,x:L_BIG,_s6
	jclr	#14,x:L_E,_s4		; the right half, or the left one flipped
	move	#>1,x0
	eor	x0,b
_s4:	jclr	#0,b1,_s5
	move	#>1,x0
	add	x0,a
_s5:	jclr	#3,x:L_FYE,_s5b
	move	#>16,x0
	add	x0,a
_s5b:	move	#>$3ff,x0
	and	x0,a
_s6:	lsl	a			; the character's row 0: its VRAM word
	lsl	a			; L_CHR + chr * 8 (2bpp) or * 16 (4bpp),
	lsl	a			; through the page table
	jclr	#2,x:BPP,_w2
	lsl	a
_w2:	move	x:L_CHR,x0
	add	x0,a
	move	#>$7fff,x0
	and	x0,a
	move	a1,x0
	move	#>$008000,y0
	mpy	x0,y0,b			; b1 = page
	move	#PT,r1
	move	b1,n1
	move	#>$ff,y0
	and	y0,a			; a1 = offset in the page
	move	x:(r1+n1),x0		; PT[page]
	add	x0,a
	move	a1,x1
	move	x:L_FYE,a
	move	#>7,x0
	and	x0,a
	add	x1,a
	move	a1,x1			; DA
	move	x:L_DI,a
	move	#>DA,x0
	add	x0,a
	move	a1,r2
	move	#DS-DA,n2
	nop
	move	x1,x:(r2)
	move	x:L_DS,x1
	move	x1,x:(r2+n2)
	move	#DH-DA,n2
	move	x:L_E,x0		; attribute index: e >> 10 & 31
	move	#>$002000,y0
	mpy	x0,y0,a
	move	#>31,x0
	and	x0,a
	move	x:L_AT,r4
	move	a1,n4
	nop
	move	y:(r4+n4),x0		; the handler
	move	x0,x:(r2+n2)
	move	#>32,x0
	add	x0,a
	move	a1,n4
	move	x:L_K,a			; palette planes at bpp + 2 in the sliver
	move	y:(r4+n4),r5
	move	x:BPP,x0
	add	x0,a
	move	#>2,x0
	add	x0,a
	move	a1,r3
	move	x:L_NPP,x0
	do	x0,_pp
	move	y:(r5)+,x1
	move	x1,x:(r3)+
_pp:
	move	x:L_K,a			; the next sliver
	move	#>SLW,x0
	add	x0,a
	move	a1,x:L_K
	move	x:L_DI,a
	move	#>1,x0
	add	x0,a
	move	a1,x:L_DI
	move	x:L_PX,a
	move	#>8,x0
	add	x0,a
	move	x:L_MW,x0
	and	x0,a
	move	a1,x:L_PX
	rts

; The attribute table of layer LAY for BPP and PALB, built when they
; change: AT[i], i = hflip << 4 | priority << 3 | palette, the handler;
; AT[32 + i] its palette planes (bits bpp..7 of PALB + palette << bpp as
; 00/FF, 8 - bpp words). Sets L_AT and L_NPP.
at_build:
	move	x:BPP,x0
	move	#>8,a
	sub	x0,a
	move	a1,x:L_NPP
	move	#ATTAB,r4
	move	x:LAY,n4
	nop
	move	y:(r4+n4),x0
	move	x0,x:L_AT
	move	x:PALB,a		; key
	rep	#8
	lsl	a
	move	x:BPP,x0
	or	x0,a
	move	a1,x1
	move	x:LAY,a			; ATKEY[L]: $15-$17, then $2E
	move	#>ATKEY,x0
	add	x0,a
	move	#>ATKEY+3,x0
	cmp	x0,a
	jne	_k
	move	#>$2E,a
_k:	move	a1,r0
	nop
	move	x:(r0),b
	cmp	x1,b
	jeq	_x
	move	x1,x:(r0)
	move	#PBTAB,r4		; the eight palette blocks
	move	x:LAY,n4
	nop
	move	y:(r4+n4),r5
	clr	b
	move	b1,x:CA_PG		; (the palette counter)
	do	#8,_blk
	move	x:CA_PG,a		; PALB + pal << bpp, >> bpp
	lsl	a
	lsl	a
	jclr	#2,x:BPP,_b1
	lsl	a
	lsl	a
_b1:	move	x:PALB,x0
	add	x0,a
	lsr	a
	lsr	a
	jclr	#2,x:BPP,_b2
	lsr	a
	lsr	a
_b2:	move	#>$ff,x1
	move	x:L_NPP,x0
	do	x0,_bw
	clr	b
	jclr	#0,a1,_bz
	move	x1,b
_bz:	move	b1,y:(r5)+
	lsr	a
_bw:
	move	x:CA_PG,a
	move	#>1,x0
	add	x0,a
	move	a1,x:CA_PG
_blk:
	move	x:L_AT,r4		; the table: handlers at r4,
	move	x:L_AT,a		; palette blocks at r0
	move	#>32,x0
	add	x0,a
	move	a1,r0
	move	#PBTAB,r5
	move	x:LAY,n5
	nop
	move	y:(r5+n5),y1		; block 0
	clr	b			; i
	do	#32,_tab
	move	b1,a			; handler: HND + 4bpp * 4 + (i >> 3 & 3)
	lsr	a
	lsr	a
	lsr	a
	move	#>3,x0
	and	x0,a
	jclr	#2,x:BPP,_t2
	move	#>4,x0
	add	x0,a
_t2:	move	#>HND,x0
	add	x0,a
	move	a1,r5
	nop
	nop
	move	y:(r5),x0
	move	x0,y:(r4)+
	move	b1,a			; block: block 0 + (i & 7) * (8 - bpp)
	move	#>7,x0
	and	x0,a
	lsl	a			; pal * 2
	move	a1,x0
	lsl	a			; pal * 4
	jclr	#1,x:BPP,_t4		; 2bpp: 6 words a block
	add	x0,a
_t4:	add	y1,a
	move	a1,y:(r0)+
	move	#>1,x0
	add	x0,b
_tab:
_x:	rts
	endif


; ---------------------------------------------------------------------------
; The row's pass descriptors, then its 16 groups.
compose:
	if	M7K
	move	#TPL7,r1		; Mode 7: OBJ 3, 2, 1, BG1, OBJ 0
	else
	move	#TPL1,r1
	jset	#3,x:SLOT+S_BGMODE,_t1t
	jmp	_t
_t1t:	move	#TPL1T,r1
_t:	move	x:MODE,a
	tst	a
	jne	_tn
	move	#TPL0,r1
	jmp	_t0
_tn:	move	#>1,x0
	cmp	x0,a
	jeq	_t0
	move	#TPL2,r1		; BG mode 2 (the planner sends no 3-6)
	endif
_t0:	move	#PASSD,r0
	clr	b
	move	b1,x:NPASS
_f:	move	y:(r1)+,a
	tst	a
	jlt	_fe
	move	#>8,y0
	cmp	y0,a
	jlt	_bg
	if	WIN
	move	x:CMASK,b		; OBJ q (on this screen)
	jclr	#4,b1,_f
	else
	jclr	#4,x:SLOT+S_TM,_f	; OBJ q
	endif
	sub	y0,a
	move	#OBJANY,r4
	move	a1,n4
	nop
	move	x:(r4+n4),b
	tst	b
	jeq	_f
	rep	#4
	lsl	a
	move	#>OM,x1
	add	x1,a
	not	a
	move	a1,y:(r0)+
	if	WIN
	clr	a			; bit 23: its palettes 4-7 take part in
	move	x:SUBMODE,b		; colour math on this row (the main screen)
	tst	b
	jne	_of
	move	x:WREG+4,b
	jclr	#16,b1,_of
	jclr	#12,b1,_of
	move	#>$800000,a
_of:	move	a1,y:(r0)+
	clr	a
	rep	#PASSW-2
	move	a,y:(r0)+
	else
	clr	a
	rep	#PASSW-1
	move	a,y:(r0)+
	endif
	jmp	_cnt
_bg:	move	a1,x:T_E		; BG i, priority h
	lsr	a
	move	a1,x:T_I
	move	#POW2,r4
	move	a1,n4
	nop
	move	y:(r4+n4),x1
	if	WIN
	move	x:CMASK,a		; the layers of this screen
	else
	move	x:SLOT+S_TM,a
	endif
	and	x1,a
	jeq	_f
	move	#ANYLO,r4		; and pixels of this priority
	jclr	#0,x:T_E,_al
	move	#ANYHI,r4
_al:	move	x:T_I,n4
	nop
	move	x:(r4+n4),b
	tst	b
	jeq	_f
	move	#LBTAB,r4
	move	x:T_I,n4
	nop
	move	y:(r4+n4),x0
	move	x0,x:T_BLK
	if	M7K
	move	#>8,x1			; Mode 7: eight planes
	else
	move	#>4,x1			; bpp: 4, 2 for BG3 and in mode 0
	move	x:MODE,a
	tst	a
	jeq	_b2
	move	x:T_I,a
	move	#>2,x0
	cmp	x0,a
	jne	_b4
_b2:	move	#>2,x1
	endif
_b4:	move	x1,x:T_BPP
	move	x:T_BLK,a		; mask base = block + bpp + h
	add	x1,a
	jclr	#0,x:T_E,_h0
	move	#>1,x0
	add	x0,a
_h0:	move	a1,y:(r0)+
	if	WIN
	move	x:T_I,a			; KEEP + 16i, bit 23: the layer takes
	rep	#4			; part in colour math (the main screen)
	lsl	a
	move	#>KEEP,x0
	add	x0,a
	move	x:SUBMODE,b
	tst	b
	jne	_kf
	move	x:WREG+4,b
	jclr	#16,b1,_kf
	move	x:T_I,n5
	move	#POW2+8,r5
	nop
	move	y:(r5+n5),x0
	and	x0,b
	jeq	_kf
	move	#>$800000,x0
	or	x0,a
_kf:	move	a1,y:(r0)+
	endif
	move	x:T_I,a			; KONST + 4i
	lsl	a
	lsl	a
	move	#>KONST,x0
	add	x0,a
	move	a1,y:(r0)+
	if	M7K
	move	#>OFF8,a		; Mode 7: plane q in word q, all eight
	move	a1,y:(r0)+
	move	#>8,a
	else
	move	#>OFF4,a		; plane offsets
	jclr	#1,x:T_BPP,_o4
	move	#>OFF2,a
_o4:	move	a1,y:(r0)+
	move	#>7,a			; planes: 7; BG3 in mode 1 5
	move	x:MODE,b
	tst	b
	jeq	_p7
	move	x:T_I,b
	move	#>2,x0
	cmp	x0,b
	jne	_p7
	move	#>5,a
	endif
_p7:	move	a1,y:(r0)+
	move	x:T_BLK,x0
	move	x0,y:(r0)+
_cnt:	move	x:NPASS,a
	move	#>1,x0
	add	x0,a
	move	a1,x:NPASS
	jmp	_f
_fe:	move	#2,m4
	clr	a
	move	a1,x:C_G
	move	a1,x:C_G20
	do	#16,_grp
	if	LCB
	move	x:RMASK,a
	lsr	a
	move	a1,x:RMASK
	jcc	_nog			; the bit shifted out: this group's
	move	x:LCN,a
	move	#>8,x0
	add	x0,a
	move	a1,x:LCN
	endif
	jsr	comp_group
	jsr	gate
_nog:	move	x:C_G,a
	move	#>1,x0
	add	x0,a
	move	a1,x:C_G
	move	x:C_G20,a
	move	#>20,x0
	add	x0,a
	move	a1,x:C_G20
_grp:
	move	#$ffff,m4
	rts

; ---------------------------------------------------------------------------
; Sprites: the row's planes, mask and priorities in OBJA, and OM.
obj_row:
	jclr	#4,x:SLOT+S_TM,_none
	move	x:CURN,a
	tst	a
	jeq	_none
	jsr	obj_ca
	move	x:SLOT+S_OBSEL,a	; sizes
	rep	#5
	lsr	a
	move	#>7,x0
	and	x0,a
	lsl	a
	lsl	a
	move	#>OBJSZ,x0
	add	x0,a
	move	a1,r4
	nop
	nop
	move	y:(r4)+,x0
	move	x0,x:O_SW
	move	y:(r4)+,x0
	move	x0,x:O_SH
	move	y:(r4)+,x0
	move	x0,x:O_LW
	move	y:(r4)+,x0
	move	x0,x:O_LH
	jclr	#0,x:SEGNEW,_have	; a new segment: its sprites' y and height
	bclr	#0,x:SEGNEW
	move	x:CURS,r0
	move	#2,n0
	move	#SPRY,r4
	move	#$80,n4
	move	#>$004000,y0
	move	#>$ff,x1
	move	x:CURN,x0
	do	x0,_bt
	move	x:(r0),x0		; w0: y = w0 >> 9 & 255
	mpy	x0,y0,a
	and	x1,a
	move	a1,y:(r4)
	move	x:O_SH,b
	jclr	#17,x:(r0),_sm
	move	x:O_LH,b
_sm:	move	b1,y:(r4+n4)
	move	(r4)+
	move	(r0)+n0
_bt:
_have:	move	x:CURS,r1		; the first 32 in range, in OAM order
	move	#2,n1			; (the planner keeps only sprites that are
	move	#SPRY,r4		; on screen horizontally)
	move	#$80,n4
	move	#LIST,r5
	clr	a
	move	a1,x:O_N
	move	x:ROW,x1
	move	#>$ff,y1
	move	x:CURN,x0
	do	x0,_scan
	move	x1,a
	move	y:(r4),y0
	sub	y0,a			; (row - y) & 255
	and	y1,a
	move	a1,b
	move	y:(r4+n4),y0
	cmp	y0,b
	jge	_skip
	move	r1,y:(r5)+
	move	x:O_N,a
	move	#>1,x0
	add	x0,a
	move	a1,x:O_N
	move	#>32,x0
	cmp	x0,a
	jlt	_skip
	enddo
	jmp	_scan
_skip:	move	(r4)+
	move	(r1)+n1
_scan:
	move	x:O_N,a			; slivers from the last one back
	tst	a
	jeq	_none
	clr	a
	move	a1,x:OCLEAN
	move	#OBJA,r0
	rep	#180
	move	a,y:(r0)+
	clr	b
	move	b1,x:O_SL
	move	r5,x:O_LP		; (the slivers use r5)
	move	x:O_N,x0
	do	x0,_spr
	move	x:O_LP,r5
	nop
	move	y:-(r5),r1
	move	r5,x:O_LP
	move	#1,n1
	nop
	jsr	obj_geom
	jsr	obj_sprite
	move	x:O_SL,a
	move	#>34,x0
	cmp	x0,a
	jle	_nx
	enddo
	jmp	_spr
_nx:	nop
_spr:
	move	#OBJA+7*18+1,r0		; OM from the mask and priority planes
	move	#OBJA+8*18+1,r1
	move	#OBJA+9*18+1,r2
	move	#OM,r3
	move	#OM+16,r4
	move	#OM+32,r5
	move	#16,n5
	do	#16,_omg
	move	y:(r0)+,x0		; m
	move	y:(r1)+,x1		; p0
	move	y:(r2)+,y0		; p1
	move	x0,a
	and	x1,a			; m & p0
	move	a1,b
	and	y0,b			; OM3 = m & p0 & p1
	move	b1,y1
	eor	y1,a			; OM1 = m & p0 & ~p1
	move	a1,x1
	move	x0,a
	and	y0,a
	eor	y1,a			; OM2 = m & ~p0 & p1
	move	a1,y0
	move	x0,a
	eor	x1,a
	eor	y0,a
	eor	y1,a			; OM0
	move	a1,y:(r3)+
	move	x1,y:(r4)+
	move	y1,y:(r5+n5)
	move	y0,y:(r5)+
_omg:
	move	#OM,r0			; which priorities the row has
	move	#OBJANY,r1
	move	#>$ffff00,y1		; (bits 7-0 of the arrays are not pixels)
	do	#4,_oq
	clr	b
	move	y:(r0)+,x0
	do	#15,_og
	or	x0,b		y:(r0)+,x0
_og:
	or	x0,b
	and	y1,b
	move	b1,x:(r1)+
_oq:
	rts
_none:	move	x:OCLEAN,a		; no sprite on the row: all clear once
	tst	a
	jne	_x
	move	#>1,a
	move	a1,x:OCLEAN
	clr	a
	move	#OBJA,r0
	rep	#180
	move	a,y:(r0)+
	move	#OM,r0
	rep	#64
	move	a,y:(r0)+
	move	#OBJANY,r0
	rep	#4
	move	a,x:(r0)+
_x:	rts

; r1 -> sprite (w0, w1; n1 = 1): O_X, O_DY = (row - y) & 255, O_W, O_H, O_W1.
obj_geom:
	move	x:(r1),a
	move	#>$1ff,x0
	and	x0,a
	move	#>256,x0
	cmp	x0,a
	jlt	_g1
	move	#>512,x0
	sub	x0,a
_g1:	move	a1,x:O_X
	move	x:(r1),x0
	move	#>$004000,y0
	mpy	x0,y0,a			; a1 = w0 >> 9
	move	a1,b
	move	#>$ff,x1
	and	x1,b			; y
	move	x:ROW,a
	sub	b,a
	and	x1,a
	move	a1,x:O_DY
	move	x:O_SW,a
	move	x:O_SH,b
	jclr	#17,x:(r1),_g2
	move	x:O_LW,a
	move	x:O_LH,b
_g2:	move	a1,x:O_W
	move	b1,x:O_H
	move	x:(r1+n1),x0
	move	x0,x:O_W1
	rts

; One sprite's slivers on this row.
obj_sprite:
	move	x:O_W1,a
	rep	#8
	lsr	a
	move	a1,x:O_AT
	jclr	#7,x:O_AT,_v		; vertical flip
	move	x:O_H,a
	move	#>1,x0
	sub	x0,a
	move	x:O_DY,x0
	sub	x0,a
	move	a1,x:O_DY
_v:	move	x:O_W1,a
	move	#>15,x0
	and	x0,a
	move	a1,x:O_TC
	move	x:O_W1,a
	rep	#4
	lsr	a
	and	x0,a
	move	a1,b
	move	x:O_DY,a
	lsr	a
	lsr	a
	lsr	a
	add	b,a
	and	x0,a
	rep	#4
	lsl	a
	move	a1,x:O_TR		; character row << 4
	move	#>OBJCA,a
	jclr	#0,x:O_AT,_t
	move	#>OBJCA+256,a
_t:	move	a1,x:O_BASE
	move	x:O_AT,a		; (128 + pal * 16) >> 4 = 8 | pal
	lsr	a
	move	#>7,x0
	and	x0,a
	move	#>8,x0
	or	x0,a
	move	a1,x:O_HI
	move	x:O_AT,a
	rep	#4
	lsr	a
	move	#>3,x0
	and	x0,a
	move	a1,x:O_PRI
	move	x:O_AT,a
	rep	#6
	lsr	a
	move	#>1,x0
	and	x0,a
	move	a1,x:FLIP
	move	x:O_W,a
	lsr	a
	lsr	a
	lsr	a
	move	a1,x:O_NCOL
	clr	b
	move	b1,x:O_COL
	move	a1,x0
	do	x0,_cols
	jsr	obj_sliver
	move	x:O_SL,a
	move	#>34,x0
	cmp	x0,a
	jle	_c
	enddo
	jmp	_cols
_c:	move	x:O_COL,a
	move	#>1,x0
	add	x0,a
	move	a1,x:O_COL
_cols:
	rts

obj_sliver:
	move	x:O_COL,a		; sx = x + 8 col
	lsl	a
	lsl	a
	lsl	a
	move	x:O_X,x0
	add	x0,a
	move	a1,x:O_SX
	move	#>256,x0
	cmp	x0,a
	jge	_x
	move	#>$fffff8,x0
	cmp	x0,a
	jle	_x
	move	x:O_SL,b
	move	#>1,x0
	add	x0,b
	move	b1,x:O_SL
	move	#>34,x0
	cmp	x0,b
	jgt	_x
	move	x:O_COL,a		; its character column
	jclr	#6,x:O_AT,_nf
	move	x:O_NCOL,a
	move	#>1,x0
	sub	x0,a
	move	x:O_COL,x0
	sub	x0,a
_nf:	move	x:O_TC,x0
	add	x0,a
	move	#>15,x0
	and	x0,a
	move	x:O_TR,x0
	or	x0,a
	move	x:O_BASE,x0
	add	x0,a
	move	a1,r0
	nop
	move	x:(r0),a
	move	x:O_DY,b
	move	#>7,x0
	and	x0,b
	move	b1,x0
	add	x0,a
	move	a1,r2
	move	#8,n2
	clr	a
	move	a1,x:SPM
	move	#O_B,r3
	move	#1,n3
	movem	p:(r2),x0
	jsr	split
	movem	p:(r2+n2),x0
	jsr	split
	; position: pos = sx + 16, group G = pos >> 4, off = pos & 15; a byte
	; goes into group G as b << (16 - off) and into G + 1 as b << (32 - off),
	; both in A0 of a multiplication (bits 7-0 below: never read, the
	; composer's masks have them clear)
	move	x:O_SX,a
	move	#>16,x0
	add	x0,a
	move	a1,b
	move	#>15,x0
	and	x0,b			; off
	lsr	a
	lsr	a
	lsr	a
	lsr	a
	move	#>OBJA,x0
	add	x0,a
	move	a1,x:O_G
	move	#POW2,r5
	move	b1,x0
	move	#>15,a
	sub	x0,a
	move	a1,n5
	nop
	move	y:(r5+n5),y0
	move	y0,x:O_YH
	clr	a
	move	a1,x:O_YL
	move	b1,a
	move	#>8,x0
	cmp	x0,a
	jle	_nyl
	move	b1,x0
	move	#>31,a
	sub	x0,a
	move	a1,n5
	nop
	move	y:(r5+n5),y0
	move	y0,x:O_YL
_nyl:	move	x:SPM,x0		; the mask's parts, and their complements
	move	x:O_YH,y0
	mpy	x0,y0,a
	move	x:O_YL,y1
	mpy	x0,y1,b
	move	a0,x0
	move	x0,x:O_MG
	move	x0,a
	not	a
	move	a1,x:O_NMG
	move	b0,x0
	move	x0,x:O_MG1
	move	x0,a
	not	a
	move	a1,x:O_NMG1
	move	x:O_G,r4		; array p, group G at r4, G + 1 at r5
	move	x:O_G,a
	move	#>1,x0
	add	x0,a
	move	a1,r5
	move	#18,n4
	move	#18,n5
	move	#O_B,r3
	do	#4,_tp			; the tile's four planes
	move	x:(r3)+,x0
	move	x:O_YH,y0
	mpy	x0,y0,a
	move	x:O_YL,y1
	mpy	x0,y1,b
	move	a0,x0
	move	b0,x1
	move	x:O_NMG,y0
	move	x:O_NMG1,y1
	jsr	merge
	nop
_tp:
	move	x:O_HI,a		; then all or nothing: palette bits 0-2, the
	move	#>7,x0			; mask (plane 7), the two priority bits
	and	x0,a
	move	#>8,x0
	or	x0,a
	move	x:O_PRI,b
	lsl	b
	lsl	b
	lsl	b
	lsl	b
	move	b1,x0
	or	x0,a
	move	x:O_NMG,y0
	move	x:O_NMG1,y1
	do	#6,_cp
	clr	b
	move	b1,x0
	move	b1,x1
	jclr	#0,a1,_cz
	move	x:O_MG,x0
	move	x:O_MG1,x1
_cz:	jsr	merge
	lsr	a
_cp:
_x:	rts

; One plane array's groups G (r4) and G + 1 (r5): keep what the sprite's
; mask leaves (y0, y1 = its complements), add x0, x1; both pointers step
; to the next array (n4 = n5 = 18).
merge:	move	y:(r4),b
	and	y0,b
	or	x0,b
	move	b1,y:(r4)+n4
	move	y:(r5),b
	and	y1,b
	or	x1,b
	move	b1,y:(r5)+n5
	rts

; x0 = a character row word (plane 1 << 8 | plane 0): both bytes to
; x:(r3)+n3, bit-reversed when FLIP; their OR into SPM.
split:
	move	x0,a
	move	#>$ff,x1
	and	x1,a
	jclr	#0,x:FLIP,_f1
	jsr	rev
_f1:	move	a1,x:(r3)+n3
	move	a1,x1
	move	x:SPM,a
	or	x1,a
	move	a1,x:SPM
	move	#>$008000,y0
	mpy	x0,y0,a			; a1 = word >> 8
	jclr	#0,x:FLIP,_f2
	jsr	rev
_f2:	move	a1,x:(r3)+n3
	move	a1,x1
	move	x:SPM,a
	or	x1,a
	move	a1,x:SPM
	rts

rev:	move	#REV,r5
	move	a1,n5
	nop
	move	y:(r5+n5),a
	rts

; OBJCA for this OBSEL: both name tables, character -> P address.
obj_ca:
	move	x:SLOT+S_OBSEL,a
	move	#>$1f,x0
	and	x0,a
	move	a1,x1
	move	x:OBJKEY,b
	cmp	x1,b
	jeq	_x
	move	x1,x:OBJKEY
	move	x1,a			; table 0: (obsel & 7) << 13 words = << 5 pages
	move	#>7,x0
	and	x0,a
	rep	#5
	lsl	a
	move	a1,x:CA_PG
	move	a1,y1
	move	#OBJCA,r1
	jsr	oca16
	move	x:OBJKEY,a		; table 1: + ((obsel >> 3 & 3) + 1) << 4 pages
	lsr	a
	lsr	a
	lsr	a
	move	#>3,x0
	and	x0,a
	move	#>1,x0
	add	x0,a
	rep	#4
	lsl	a
	add	y1,a
	move	a1,x:CA_PG
	jsr	oca16
_x:	rts

oca16:	move	#>16,x1
	do	#16,_p
	jsr	ca_page
	do	#16,_c
	move	a1,x:(r1)+
	add	x1,a
_c:	nop
_p:	rts

	if	WIN
; ---------------------------------------------------------------------------
; The window build's routines, in two of the page slots it gives up
; (P:$5300-$54FF, X:$1300-$14FF): they run on rows that use windows or the
; sub screen.
	org	p:$5300
; The window build: keep masks from the window slots (WREG), per layer
; (BG1-4, OBJ) 16 words, bits 23-8 the pixels of a group its windows leave
; (the host model's win_keep, tools/dsp_model.c).
win_build:
	clr	a
	move	a1,x:WDIRTY
	move	x:WREG+1,a		; window 1: WH0 | WH1 << 8
	jsr	win_edges
	move	#WR1,r0
	jsr	win_range
	move	x:WREG+2,a		; window 2: WH2 | WH3 << 8
	jsr	win_edges
	move	#WR2,r0
	jsr	win_range
	move	x:WREG,x0		; W12SEL | W34SEL << 8 | WOBJSEL << 16
	move	x0,x:WSELS
	move	x:WREG+3,x0		; WBGLOG | WOBJLOG << 8 | TMW << 16
	move	x0,x:WLOGS
	move	#>$010000,x0
	move	x0,x:WTM
	move	#KEEP,r4
	jsr	win_layer		; BG1
	jsr	win_layer		; BG2
	jsr	win_layer		; BG3
	jsr	win_layer		; BG4
	move	x:WREG+3,a		; OBJ: WOBJLOG
	rep	#8
	lsr	a
	move	a1,x:WLOGS
	jsr	win_layer
	rts

; a1 = left | right << 8 -> y0 = left, y1 = right
win_edges:
	move	a1,b
	move	#>$ff,x0
	and	x0,a
	move	a1,y0
	rep	#8
	lsr	b
	move	b1,y1
	rts

; A window's pixels [y0, y1] (none when y0 > y1) per group into x:(r0)+,
; 16 words: LM[max(y0 - lo, 0)] & RM[min(y1 - lo, 15)], lo = 16 g.
win_range:
	clr	b			; lo
	move	#LM,r1
	move	#RM,r3
	do	#16,_wr
	move	y1,a
	sub	b,a			; right - lo
	jlt	_z			; ends left of the group
	move	#>15,x0
	cmp	x0,a
	jle	_b1
	move	x0,a
_b1:	move	a1,x1			; last pixel in the group
	move	y0,a
	sub	b,a			; left - lo
	jge	_a1
	clr	a
_a1:	cmp	x1,a
	jgt	_z			; starts after it (or left > right)
	move	a1,n1
	move	x1,n3
	nop
	move	x:(r1+n1),x0
	move	x:(r3+n3),a
	and	x0,a
	move	a1,x:(r0)+
	jmp	_n
_z:	clr	a
	move	a1,x:(r0)+
_n:	move	#>16,x0
	add	x0,b
_wr:
	rts

; One layer's 16 keep masks into x:(r4)+: its select nibble (WSELS bits
; 3-0: window 1 inverted, enabled, window 2 inverted, enabled), logic
; (WLOGS bits 1-0: OR, AND, XOR, XNOR; OR when only one window is
; enabled, the other then counting as none) and TMW bit (WTM); then the
; next layer's are moved into place.
win_layer:
	move	x:WSELS,a
	move	#>$f,x0
	and	x0,a
	move	a1,x1			; the select nibble
	move	x:WREG+3,a
	move	x:WTM,x0
	and	x0,a
	jeq	_keep			; not in TMW
	move	x1,a
	move	#>$a,x0
	and	x0,a
	jeq	_keep			; no window enabled
	move	#WZ,r0			; window 1: its pixels or none,
	clr	b			; inverted or not
	move	x1,a
	move	#>2,x0
	and	x0,a
	jeq	_n1
	move	#WR1,r0
	move	x1,a
	move	#>1,x0
	and	x0,a
	jeq	_n1
	move	#>$ffff00,b
_n1:	move	b1,y0
	move	#WZ,r1			; window 2
	clr	b
	move	x1,a
	move	#>8,x0
	and	x0,a
	jeq	_n2
	move	#WR2,r1
	move	x1,a
	move	#>4,x0
	and	x0,a
	jeq	_n2
	move	#>$ffff00,b
_n2:	move	b1,y1
	clr	b			; OR, unless both are enabled
	move	x1,a
	move	#>$a,x0
	and	x0,a
	cmp	x0,a
	jne	_lg
	move	x:WLOGS,a
	move	#>3,x0
	and	x0,a
	move	a1,b
_lg:	move	#>$ffff00,x1		; kept: the complement
	tst	b
	jeq	_or
	move	#>1,x0
	cmp	x0,b
	jeq	_and
	move	#>3,x0
	cmp	x0,b
	jne	_xor
	clr	b			; XNOR: the XOR is what is kept
	move	b1,x1
_xor:	do	#16,_xe
	move	x:(r0)+,a
	eor	y0,a
	move	a1,x0
	move	x:(r1)+,a
	eor	y1,a
	eor	x0,a
	eor	x1,a
	move	a1,x:(r4)+
_xe:	jmp	_next
_or:	do	#16,_oe
	move	x:(r0)+,a
	eor	y0,a
	move	a1,x0
	move	x:(r1)+,a
	eor	y1,a
	or	x0,a
	eor	x1,a
	move	a1,x:(r4)+
_oe:	jmp	_next
_and:	do	#16,_ae
	move	x:(r0)+,a
	eor	y0,a
	move	a1,x0
	move	x:(r1)+,a
	eor	y1,a
	and	x0,a
	eor	x1,a
	move	a1,x:(r4)+
_ae:	jmp	_next
_keep:	move	#>$ffff00,x0
	rep	#16
	move	x0,x:(r4)+
_next:	move	x:WSELS,a
	rep	#4
	lsr	a
	move	a1,x:WSELS
	move	x:WLOGS,a
	lsr	a
	lsr	a
	move	a1,x:WLOGS
	move	x:WTM,a
	lsl	a
	move	a1,x:WTM
	rts

; The sub screen's lightened copies (ppu_sub_cfg bit 18): md (MDV) only
; where the sub pixel is in a relocated CGRAM block (RELT: block | target
; << 4, NREL of them; a block's pixels are those whose planes 4-7 spell
; it), SRCBUF the group's sub planes 0-3 and the target blocks' 4-7.
; r2 -> the group's sub planes. Uses r0, r1.
fx_reloc:
	move	r2,r0
	move	#SRCBUF,r1
	do	#4,_c03
	move	x:(r0)+,x0
	move	x0,x:(r1)+
_c03:
	move	#PV,r1
	do	#4,_c47
	move	x:(r0)+,a
	move	a1,x:(r1)+
_c47:
	move	#PV,r0
	move	#NPV,r1
	do	#4,_cn
	move	x:(r0)+,a
	not	a
	move	a1,x:(r1)+
_cn:
	clr	a
	move	a1,x:INCV
	move	a1,x:RR
	move	a1,x:RR+1
	move	a1,x:RR+2
	move	a1,x:RR+3
	move	x:NREL,a
	tst	a
	jeq	_rd
	move	#RELT,r1
	move	a1,x0
	do	x0,_rl
	move	x:(r1)+,b		; block | target << 4
	move	#>$ffff00,a		; the block's pixels: planes 4-7 = block
	move	x:PV,x0
	jset	#0,b1,_k0
	move	x:NPV,x0
_k0:	and	x0,a
	move	x:PV+1,x0
	jset	#1,b1,_k1
	move	x:NPV+1,x0
_k1:	and	x0,a
	move	x:PV+2,x0
	jset	#2,b1,_k2
	move	x:NPV+2,x0
_k2:	and	x0,a
	move	x:PV+3,x0
	jset	#3,b1,_k3
	move	x:NPV+3,x0
_k3:	and	x0,a
	move	a1,x1
	move	x:INCV,a
	or	x1,a
	move	a1,x:INCV
	jclr	#4,b1,_f0		; the target's planes 4-7
	move	x:RR,a
	or	x1,a
	move	a1,x:RR
_f0:	jclr	#5,b1,_f1
	move	x:RR+1,a
	or	x1,a
	move	a1,x:RR+1
_f1:	jclr	#6,b1,_f2
	move	x:RR+2,a
	or	x1,a
	move	a1,x:RR+2
_f2:	jclr	#7,b1,_f3
	move	x:RR+3,a
	or	x1,a
	move	a1,x:RR+3
_f3:	nop
	nop
_rl:
_rd:	move	x:MDV,a
	move	x:INCV,x0
	and	x0,a
	move	a1,x:MDV
	move	#RR,r0
	move	#SRCBUF+4,r1
	do	#4,_cr
	move	x:(r0)+,x0
	move	x0,x:(r1)+
_cr:
	rts

; Compose the row: with the sub screen (the SUB slot's bit 16) that
; first, into SUBBUF, then the main screen merging it; S_TM back to TM.
fx_compose:
	move	x:SAVETM,x0
	move	x0,x:SLOT+S_TM
	move	x0,x:CMASK
	clr	a
	move	a1,x:SUBMODE
	move	x:WREG+4,a
	jclr	#16,a1,compose
	move	#>$1f,x1
	and	x1,a
	move	a1,x:CMASK
	move	#>1,x0
	move	x0,x:SUBMODE
	jsr	compose
	move	#>$aaaa00,x0		; x + row even
	move	x:ROW,a
	jclr	#0,a1,_ev
	move	#>$555500,x0
_ev:	move	x0,x:DCHK
	move	x:SAVETM,x0
	move	x0,x:CMASK
	clr	a
	move	a1,x:SUBMODE
	jmp	compose

; After obj_row: the sprites' priority masks less what OBJ's windows hide.
win_obj:
	move	#OM,r0
	do	#4,_q
	move	#KEEP+64,r1
	do	#16,_g
	move	x:(r1)+,x0
	move	y:(r0),a
	and	x0,a
	move	a1,y:(r0)+
_g:
	nop
_q:
	rts

	if	LCB
; The list compositor's sliver range, here (not in the main body, which has no room left with both builds):
; The slivers a row needs from its mask of groups (A, not 0): group g uses slivers 2g .. 2g + 2. Up to six
; groups apart: from the first group's first sliver to the last group's last; otherwise all 33 (and the
; descriptors and keys as for any full row).
lc_range:
	move	#0,r0
_l0:	lsr	a
	jcs	_l1
	move	(r0)+
	jmp	_l0
_l1:	move	r0,x:LCLO
	move	x:RMASK,a
	move	#0,r0
	move	#0,r1
	do	#16,_h0
	lsr	a
	jcc	_h1
	move	r0,r1			; the last group so far
_h1:	move	(r0)+
_h0:
	move	r1,a
	move	x:LCLO,x0
	sub	x0,a			; hi - lo
	lsl	a
	move	#>3,x0
	add	x0,a			; 2 * (hi - lo) + 3 slivers
	move	#>17,x0
	cmp	x0,a
	jge	_lf			; more than 16: the whole row
	move	a1,x:SLN
	move	#>1,x0
	sub	x0,a
	move	a1,x:SLN1
	move	x:LCLO,a
	move	a1,x0
	rep	#4
	lsl	a			; 16 * lo
	move	x0,b
	lsl	b
	lsl	b			; 4 * lo
	add	b,a			; 20 * lo = SL0 * SLW
	move	a1,x:SL0W
	move	x:LCLO,a
	lsl	a
	move	a1,x:SL0
	rts
_lf:	clr	a
	move	a1,x:SL0
	move	a1,x:SL0W
	move	#>33,a
	move	a1,x:SLN
	move	#>32,a
	move	a1,x:SLN1
	rts
	endif
	endif

	end
