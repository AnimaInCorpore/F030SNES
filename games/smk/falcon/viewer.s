; Native 256x224, 4-bitplane Falcon Videl display probe.
; The framebuffer is the byte-for-byte output of the C reference renderer.

    text
start:
    move.w  #89,-(sp)             ; XBIOS Montype
    trap    #14
    addq.l  #2,sp
    tst.w   d0
    bmi     exit                  ; monochrome display unsupported
    move.w  d0,monitor_type

    move.w  #2,-(sp)              ; XBIOS Physbase
    trap    #14
    addq.l  #2,sp
    move.l  d0,old_phys

    clr.l   -(sp)                 ; GEMDOS Super(0)
    move.w  #$20,-(sp)
    trap    #1
    addq.l  #6,sp
    move.l  d0,old_ssp

    bsr     video_save

    ; TOS loads a normal .TOS into ST-RAM. Align the BSS framebuffer to the
    ; Videl's 256-byte base requirement, then copy the immutable test image.
    lea     screen_raw,a0
    move.l  a0,d0
    add.l   #255,d0
    andi.l  #$ffffff00,d0
    move.l  d0,screen_ptr
    move.l  d0,a1
    lea     viewer_frame,a0
    move.w  #7167,d7              ; 256*224*4/8 / 4 = 7168 longs
.copy:
    move.l  (a0)+,(a1)+
    dbra    d7,.copy

    move.l  screen_ptr,d0
    bsr     video_set_base

    cmpi.w  #2,monitor_type
    beq     video_vga

video_rgb:
    ; 256 visible pixels, 224 lines, 50 Hz RGB/TV. Horizontal positions
    ; start from Falcon 16/40 and add 32 pixels of border per side.
    move.l  #$00fe00ab,$ffff8282.w  ; HHT, HBB
    move.l  #$0047002c,$ffff8286.w  ; HBE, HDB
    move.l  #$004d00d8,$ffff828a.w  ; HDE, HSS
    move.l  #$0271021f,$ffff82a2.w  ; VFT, VBB
    move.l  #$005f005f,$ffff82a6.w  ; VBE, VDB
    move.l  #$021f026b,$ffff82aa.w  ; VDE, VSS
    move.w  #$0200,$ffff820a.w     ; PAL 50 Hz
    move.w  #$0181,$ffff82c0.w     ; RGB, 32 MHz, 32-bit bus
    clr.w   $ffff82c2.w           ; 4 cycles/pixel, no line doubling
    bra     video_common

video_vga:
    ; 256x224 with doubled scanlines at VGA scan frequency.
    move.l  #$00c6006d,$ffff8282.w  ; HHT, HBB
    move.l  #$003502aa,$ffff8286.w  ; HBE, HDB (second halfline)
    move.l  #$004b0096,$ffff828a.w  ; HDE, HSS
    move.l  #$041903cf,$ffff82a2.w  ; VFT, VBB
    move.l  #$004d004b,$ffff82a6.w  ; VBE, VDB
    move.l  #$03cb0415,$ffff82aa.w  ; VDE, VSS
    move.w  #$0200,$ffff820a.w
    move.w  #$0182,$ffff82c0.w     ; VGA, 32 MHz, 32-bit bus
    move.w  #5,$ffff82c2.w         ; 2 cycles/pixel, doubled lines

video_common:
    ; SPSHIFT A/8/4 all zero selects Falcon 16-colour, four-plane mode.
    ; (256/16)*4 = 64 words = 128 bytes per visible line.
    clr.w   $ffff8266.w
    clr.l   $ffff8264.w           ; no horizontal scroll
    clr.w   $ffff820e.w           ; no line offset
    move.w  #64,$ffff8210.w

    lea     falcon_palette,a0
    lea     $ffff9800.w,a1
    move.w  #15,d7
.set_palette:
    move.l  (a0)+,(a1)+
    dbra    d7,.set_palette

    move.w  #37,-(sp)             ; Vsync before showing the static frame
    trap    #14
    addq.l  #2,sp

    move.w  #1,-(sp)              ; GEMDOS Cconin: wait for a key
    trap    #1
    addq.l  #2,sp

    bsr     video_restore
    move.l  old_ssp,-(sp)         ; return to user mode
    move.w  #$20,-(sp)
    trap    #1
    addq.l  #6,sp

exit:
    clr.w   -(sp)                 ; GEMDOS Pterm0
    trap    #1

video_save:
    move.b  $ffff8260.w,save_st_shift
    move.w  $ffff8266.w,save_spshift
    move.w  $ffff820a.w,save_sync
    move.l  $ffff820e.w,save_offset_width
    move.l  $ffff8264.w,save_hscroll
    movem.l $ffff8282.w,d0-d3
    movem.l d0-d3,save_htiming
    movem.l $ffff82a2.w,d0-d3
    movem.l d0-d3,save_vtiming
    move.l  $ffff82c0.w,save_control
    lea     $ffff9800.w,a0
    lea     save_palette,a1
    move.w  #15,d7
.save_palette:
    move.l  (a0)+,(a1)+
    dbra    d7,.save_palette
    rts

video_restore:
    lea     save_palette,a0
    lea     $ffff9800.w,a1
    move.w  #15,d7
.restore_palette:
    move.l  (a0)+,(a1)+
    dbra    d7,.restore_palette
    movem.l save_htiming,d0-d3
    movem.l d0-d3,$ffff8282.w
    movem.l save_vtiming,d0-d3
    movem.l d0-d3,$ffff82a2.w
    move.l  save_control,$ffff82c0.w
    move.l  save_hscroll,$ffff8264.w
    move.l  save_offset_width,$ffff820e.w
    clr.w   $ffff8266.w
    move.w  save_spshift,$ffff8266.w
    move.b  save_st_shift,$ffff8260.w
    move.w  save_sync,$ffff820a.w
    move.l  old_phys,d0
    bsr     video_set_base
    rts

video_set_base:
    move.b  d0,$ffff820d.w        ; address bits 7..0
    lsr.l   #8,d0
    move.b  d0,$ffff8203.w        ; bits 15..8
    lsr.l   #8,d0
    move.b  d0,$ffff8201.w        ; bits 23..16
    rts

    data
; Falcon extended CLUT: R in byte 0, G in byte 1, B in byte 3.
falcon_palette:
    dc.l    $00000000,$14140024,$14640028,$248c0038
    dc.l    $48ac0044,$785c0030,$a4800040,$5cb000dc
    dc.l    $54580068,$6c740080,$2c6000d0,$dc380030
    dc.l    $ec440034,$c8d000d8,$f8cc0040,$fcf800e0
viewer_frame:
    incbin  "out/frame_4bpp.bin"

    bss
monitor_type:       ds.w 1
old_ssp:            ds.l 1
old_phys:           ds.l 1
screen_ptr:         ds.l 1
save_st_shift:      ds.b 1
save_spshift:       ds.w 1
save_sync:          ds.w 1
save_offset_width:  ds.l 1
save_hscroll:       ds.l 1
save_htiming:       ds.l 4
save_vtiming:       ds.l 4
save_control:       ds.l 1
save_palette:       ds.l 16
screen_raw:         ds.b 28927
