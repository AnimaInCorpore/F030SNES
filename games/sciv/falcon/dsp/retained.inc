; DSP-owned page requests and retained group output. RETAIN ($31):
; stream length, force-full flag, 128 host dirty-page flags, state stream.
; $52pppp requests page pppp; host returns P base, flags (upload/new/error),
; and, for upload, RAWPAGE + page/base + raw SSI data. $53nnnn completes
; nnnn output words (bit 15 error). Records: group index, eight planes;
; $ffff terminates, zero padding to eight SSI slots. No host dirty planner.

DRPREV equ $0F00
DRVD equ $3B00
DRUSED equ $3B80
DRSEEN equ $3C00
DRLAST equ $3C80
DRCUR equ $3D60
DRSAVE equ $3E40
DRVALID equ $3E60
DRLEN equ $3E61
DRPLEN equ $3E62
DRBGN equ $3E63
DRPBGN equ $3E64
DRFULL equ $3E65
DRACT equ $3E66
DRSCAN equ $3E67
DRERR equ $3E68
DRPG equ $3E69
DRWI equ $3E6A
DRVA equ $3E6B
DRGR equ $3E6C
DRBASE equ $3E6D
DRFLAGS equ $3E6E
DRSPP equ $3E70
DRSEGN equ $3E71
DRSPRN equ $3E72
DRX equ $3E73
DRY equ $3E74
DRW equ $3E75
DRH equ $3E76
DRLO equ $3E77
DRHI equ $3E78
DRSPR equ $3E79
DRAT equ $3E7A

 org p:DRTAIL
dr_init:
 clr a
 move a1,x:DRVALID
 move #DRUSED,r0
 rep #128
 move a1,x:(r0)+
 move #DRLAST,r0
 rep #224
 move a1,x:(r0)+
 rts

dr_frame:
 jsr get
 move a1,x:DRLEN
 jsr get
 move a1,x:DRFULL
 clr a
 move a1,x:DRACT
 move a1,x:DRERR
 move a1,x:DRSCAN
 move #DRUSED,r0
 move #DRVD,r1
 move #DRSEEN,r2
 do #128,_flags
 jsr get
 tst a
 jeq _clean
 move x:(r0),a
 tst a
 jeq _clean
 move #>1,a
 move a1,x:DRACT
_clean:
 clr a
 move a1,x:(r1)+
 move a1,x:(r2)+
 move (r0)+
_flags:
 move x:DRLEN,x0
 move #STREAM,r0
 do x0,_input
 jsr get
 move a1,x:(r0)+
_input:
 move x:STREAM+23,a
 lsl a
 move #>24,x0
 add x0,a
 move a1,x:DRBGN
 move x:DRVALID,b
 tst b
 jeq _full
 move x:DRPBGN,x0
 cmp x0,a
 jne _full
 move a1,x0
 move #STREAM,r0
 move #DRPREV,r1
 do x0,_bgcmp
 move x:(r0)+,a
 move x:(r1)+,x1
 cmp x1,a
 jeq _bgok
 move #>1,a
 move a1,x:DRFULL
_bgok: nop
_bgcmp:
 jmp _masks
_full:
 move #>1,a
 move a1,x:DRFULL
_masks:
 clr a
 move x:DRFULL,b
 tst b
 jeq _zero
 move #>$ffff,a
_zero:
 move #MASKS,r0
 rep #224
 move a1,x:(r0)+
 move x:DRFULL,a
 tst a
 jne _draw
 move x:DRPLEN,x0
 move x:DRLEN,a
 cmp x0,a
 jne _sprites
 move a1,x0
 move #STREAM,r0
 move #DRPREV,r1
 clr b
 do x0,_allcmp
 move x:(r0)+,a
 move x:(r1)+,x1
 eor x1,a
 move a1,x1
 or x1,b
_allcmp:
 tst b
 jeq _scan
_sprites:
 move #STREAM,r0
 jsr dr_sprites
 move #DRPREV,r0
 jsr dr_sprites
_scan:
 move x:DRACT,a
 tst a
 jeq _draw
 move #>1,a
 move a1,x:DRSCAN
 jsr render
 clr a
 move a1,x:DRSCAN
_draw:
 move #MASKS,r0
 move #DRLAST,r1
 move #DRCUR,r2
 do #224,_union
 move x:(r0),a
 move a1,x:(r2)+
 move x:(r1)+,x0
 or x0,a
 move a1,x:(r0)+
_union:
 jsr render
 jsr room
 move #>$ffff00,a
 move a1,y:(r7)+
 move x:LCN,a
 move #>1,x0
 add x0,a
 move a1,x:LCN
_pad:
 move x:LCN,a
 move #>7,x0
 and x0,a
 jeq _drain
 clr a
 move a1,y:(r7)+
 move x:LCN,a
 move #>1,x0
 add x0,a
 move a1,x:LCN
 jmp _pad
_drain:
 jsr gate
 jsr fill
 tst a
 jne _drain
 bclr #TIE,x:m_crb
 move #DRCUR,r0
 move #DRLAST,r1
 do #224,_remembermask
 move x:(r0)+,a
 move a1,x:(r1)+
_remembermask:
 move x:DRLEN,x0
 move #STREAM,r0
 move #DRPREV,r1
 do x0,_remember
 move x:(r0)+,a
 move a1,x:(r1)+
_remember:
 move x:DRLEN,a
 move a1,x:DRPLEN
 move x:DRBGN,a
 move a1,x:DRPBGN
 move #>1,a
 move a1,x:DRVALID
 move x:LCN,a
 move x:DRERR,b
 tst b
 jeq _success
 move #>$8000,x0
 or x0,a
_success:
 move #>$530000,x0
 or x0,a
 jsr put
 jmp main

; Caller-visible register state survives page-service transfers. a1 is
; virtual word in, physical address out; DRGR marks its affected groups.
dr_vaddr:
 move b0,x:DRSAVE
 move b1,x:DRSAVE+1
 move b2,x:DRSAVE+2
 move x0,x:DRSAVE+3
 move x1,x:DRSAVE+4
 move y0,x:DRSAVE+5
 move y1,x:DRSAVE+6
 move r0,x:DRSAVE+7
 move r1,x:DRSAVE+8
 move r2,x:DRSAVE+9
 move n0,x:DRSAVE+10
 move n1,x:DRSAVE+11
 move n2,x:DRSAVE+12
 move #>$7fff,x0
 and x0,a
 move a1,x:DRVA
 rep #8
 lsr a
 move a1,x:DRPG
 move a1,n0
 move #DRSEEN,r0
 nop
 move x:(r0+n0),a
 tst a
 jne _resident
 move #>1,a
 move a1,x:(r0+n0)
 move #DRUSED,r0
 nop
 move a1,x:(r0+n0)
 move x:DRPG,a
 move #>$520000,x0
 or x0,a
 jsr put
 jsr get
 move a1,x:DRBASE
 jsr get
 move a1,x:DRFLAGS
 jclr #2,a1,_good
 move #>1,a
 move a1,x:DRERR
_good:
 move x:DRPG,n0
 move #PT,r0
 move x:DRBASE,a
 move a1,x:(r0+n0)
 move x:DRFLAGS,a
 jclr #1,a1,_existing
 move #DRVD,r0
 move #>$ffff,a
 move a1,x:(r0+n0)
_existing:
 move x:DRFLAGS,a
 jclr #0,a1,_resident
 jsr get                         ; RAWPAGE opcode (host service)
 jsr cmd_rawpage
_resident:
 move x:DRPG,n0
 move #DRVD,r0
 move x:DRVA,a
 rep #4
 lsr a
 move #>15,x0
 and x0,a
 move a1,n1
 move #POW2,r1
 nop
 move y:(r1+n1),x0
 move x:(r0+n0),a
 and x0,a
 jeq _address
 move x:ROW,n0
 move #MASKS,r0
 move x:DRGR,x0
 nop
 move x:(r0+n0),a
 or x0,a
 move a1,x:(r0+n0)
_address:
 move x:DRPG,n0
 move #PT,r0
 move x:DRVA,a
 move #>$ff,x0
 and x0,a
 move x:(r0+n0),x0
 add x0,a
 move a1,x:DRAT
 move x:DRSAVE,b0
 move x:DRSAVE+1,b1
 move x:DRSAVE+2,b2
 move x:DRSAVE+3,x0
 move x:DRSAVE+4,x1
 move x:DRSAVE+5,y0
 move x:DRSAVE+6,y1
 move x:DRSAVE+7,r0
 move x:DRSAVE+8,r1
 move x:DRSAVE+9,r2
 move x:DRSAVE+10,n0
 move x:DRSAVE+11,n1
 move x:DRSAVE+12,n2
 move x:DRAT,a
 rts

dr_char_addr:
 jsr dr_vaddr
 rts

dr_ca_page:
 move x:CA_PG,a
 move #>127,x0
 and x0,a
 rep #8
 lsl a
 move a1,x1
 move x:CA_PG,a
 move #>1,x0
 add x0,a
 move a1,x:CA_PG
 move x1,a
 rts

dr_bg_group:
 move a1,x:DRAT
 move x:L_DI,a
 move x:L_LB,x0
 sub x0,a
 lsr a
 move a1,n0
 move #POW2,r0
 clr b
 move #>16,x0
 cmp x0,a
 jge _previous
 nop
 move y:(r0+n0),b
_previous:
 move x:L_DI,a
 move x:L_LB,x0
 sub x0,a
 jset #0,a1,_set
 move x:L_FINE,a
 tst a
 jeq _set
 move n0,a
 tst a
 jeq _set
 move #>1,x0
 sub x0,a
 move a1,n0
 nop
 move y:(r0+n0),x0
 or x0,b
_set:
 move b1,x:DRGR
 move x:DRAT,a
 rts

dr_obj_addr:
 move a1,x:DRAT
 move x:O_SX,a
 asr a
 asr a
 asr a
 asr a
 move a1,n0
 move #POW2,r0
 clr b
 tst a
 jlt _right
 move #>16,x0
 cmp x0,a
 jge _right
 nop
 move y:(r0+n0),b
_right:
 move x:O_SX,a
 move #>7,x0
 add x0,a
 asr a
 asr a
 asr a
 asr a
 tst a
 jlt _mask
 move #>16,x0
 cmp x0,a
 jge _mask
 move a1,n0
 nop
 move y:(r0+n0),x0
 or x0,b
_mask:
 move b1,x:DRGR
 move x:DRAT,a
 jsr dr_vaddr
 rts

dr_upload_dirty:
 move a1,x:DRAT
 movem p:(r1),x0
 cmp x0,a
 jeq _step
 move x:DRWI,a
 rep #4
 lsr a
 move a1,n0
 move #POW2,r0
 move x:DRPG,n2
 move #DRVD,r2
 nop
 move y:(r0+n0),x0
 move x:(r2+n2),a
 or x0,a
 move a1,x:(r2+n2)
_step:
 move x:DRWI,a
 move #>1,x0
 add x0,a
 move a1,x:DRWI
 move x:DRAT,a
 rts

dr_group_header:
 move x:SUBMODE,a
 tst a
 jeq _main
 move x:LCN,a                 ; sub screen emits no SSI words
 move #>8,x0
 sub x0,a
 move a1,x:LCN
 rts
_main:
 move x:ROW,a
 rep #4
 lsl a
 move x:C_G,x0
 add x0,a
 rep #8
 lsl a
 move a1,y:(r7)+
 move x:LCN,a
 move #>1,x0
 add x0,a
 move a1,x:LCN
 rts

dr_blank:
 move x:DRSCAN,a
 tst a
 jne _done
 clr a
 move a1,x:C_G
 do #16,_blank
 move x:RMASK,a
 lsr a
 move a1,x:RMASK
 jcc _next
 jsr dr_group_header
 clr a
 rep #8
 move a1,y:(r7)+
 move x:LCN,a
 move #>8,x0
 add x0,a
 move a1,x:LCN
_next:
 move x:C_G,a
 move #>1,x0
 add x0,a
 move a1,x:C_G
_blank:
_done: rts

; Changed OAM stream: conservatively invalidate old/new visible footprints.
; Filtering, priority and pixel selection remain the existing DSP renderer's.
dr_sprites:
 move r0,a
 move x:DRBGN,x0
 add x0,a
 move a1,r0
 nop
 move x:(r0)+,a
 move a1,x:DRSEGN
_segment:
 move x:DRSEGN,a
 tst a
 jeq _done
 move (r0)+
 move x:(r0)+,a
 move a1,x:DRSPRN
_sprite:
 move x:DRSPRN,a
 tst a
 jeq _nextseg
 move x:(r0)+,a
 move a1,x:DRSPR
 move (r0)+
 move r0,x:DRSPP
 move #>$1ff,x0
 and x0,a
 move #>256,x0
 cmp x0,a
 jlt _x
 move #>512,x0
 sub x0,a
_x:
 move a1,x:DRX
 move x:DRSPR,a
 rep #9
 lsr a
 move #>$ff,x0
 and x0,a
 move a1,x:DRY
 move x:STREAM+3,a
 rep #5
 lsr a
 lsl a
 lsl a
 move x:DRSPR,b
 jclr #17,b1,_size
 move #>2,x0
 add x0,a
_size:
 move #>OBJSZ,x0
 add x0,a
 move a1,r0
 nop
 move y:(r0)+,a
 move a1,x:DRW
 move y:(r0),a
 move a1,x:DRH
 move x:DRX,a
 move x:DRW,x0
 add x0,a
 tst a
 jle _skip
 move #>1,x0
 sub x0,a
 rep #4
 asr a
 move #>15,x0
 cmp x0,a
 jle _hi
 move x0,a
_hi:
 move a1,x:DRHI
 move x:DRX,a
 tst a
 jge _lo
 clr a
_lo:
 rep #4
 asr a
 move a1,x:DRLO
 move #>16,x0
 cmp x0,a
 jge _skip
 clr b
 move #POW2,r0
 move x:DRLO,n0
_bits:
 nop
 move y:(r0+n0),x0
 or x0,b
 move n0,a
 move x:DRHI,x0
 cmp x0,a
 jge _rows
 move #>1,x0
 add x0,a
 move a1,n0
 jmp _bits
_rows:
 move b1,x:DRGR
 move #MASKS,r0
 move x:DRH,x1
 move x:DRY,n0
 do x1,_height
 move n0,a
 move #>$ff,x0
 and x0,a
 move a1,n0
 move #>224,x0
 cmp x0,a
 jge _outside
 nop
 move x:(r0+n0),a
 move x:DRGR,x0
 or x0,a
 move a1,x:(r0+n0)
_outside:
 move n0,a
 move #>1,x0
 add x0,a
 move a1,n0
_height:
_skip:
 move x:DRSPP,r0
 move x:DRSPRN,a
 move #>1,x0
 sub x0,a
 move a1,x:DRSPRN
 jmp _sprite
_nextseg:
 move x:DRSEGN,a
 move #>1,x0
 sub x0,a
 move a1,x:DRSEGN
 jmp _segment
_done: rts
