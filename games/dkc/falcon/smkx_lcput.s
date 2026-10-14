; The list compositor's walker and copier (src/smkx_lc.c), C ABI (d2-d7/a2-a6 preserved).
;
;   void smkx_lc_cpu(uint8_t *dst, const uint8_t *src, uint32_t sstride, uint32_t groups, uint32_t rows)
;   void smkx_lc_blit(uint8_t *dst, const uint8_t *src, uint32_t sstride, uint32_t groups, uint32_t rows)
;       A block of groups a row, rows rows of them, into a ring page (its rows are 32 groups = 512 bytes apart) from
;       a source whose rows are sstride bytes apart: by the CPU, or as one blitter run (no hog: the CPU goes on
;       meanwhile; waits for a run still going first).
;   int smkx_lc_blit_busy(void); void smkx_lc_dcache_clear(void)

	section	text,code


smkx_lc_cpu:
	movem.l	d2-d4,-(sp)
	movea.l	16(sp),a0		; dst
	movea.l	20(sp),a1		; src
	move.l	24(sp),d0		; source row stride
	move.l	28(sp),d1		; groups a row
	move.l	32(sp),d2		; rows
	move.l	d1,d3
	lsl.l	#4,d3			; bytes of a row's groups
	move.l	#512,d4
	sub.l	d3,d4			; to the next row of the destination
	sub.l	d3,d0			; and of the source
	subq.w	#1,d2
.row:	move.w	d1,d3
	subq.w	#1,d3
.cl:	move.l	(a1)+,(a0)+
	move.l	(a1)+,(a0)+
	move.l	(a1)+,(a0)+
	move.l	(a1)+,(a0)+
	dbra	d3,.cl
	adda.l	d4,a0
	adda.l	d0,a1
	dbra	d2,.row
	movem.l	(sp)+,d2-d4
	rts

smkx_lc_blit:
	movem.l	d2-d4/a2,-(sp)
	movea.l	20(sp),a0		; dst
	movea.l	24(sp),a1		; src
	move.l	28(sp),d0		; source row stride
	move.l	32(sp),d1		; groups a row
	move.l	36(sp),d2		; rows
	lea	$ffff8a00.w,a2
.bw:	btst	#7,$3c(a2)		; a run still going
	bne.s	.bw
	moveq	#2,d3
	move.w	d3,$20(a2)		; source x increment
	move.w	d3,$2e(a2)		; destination x increment
	move.l	d1,d3
	lsl.l	#4,d3			; bytes of a row's groups
	move.w	#514,d4
	sub.w	d3,d4			; 512 - 16 * groups + 2: no x increment after a row's last word
	move.w	d4,$30(a2)		; destination y increment
	move.l	d0,d4
	sub.l	d3,d4
	addq.w	#2,d4
	move.w	d4,$22(a2)		; source y increment
	moveq	#-1,d3
	move.w	d3,$28(a2)		; endmasks
	move.w	d3,$2a(a2)
	move.w	d3,$2c(a2)
	move.l	a1,$24(a2)		; source
	move.l	a0,$32(a2)		; destination
	lsl.w	#3,d1
	move.w	d1,$36(a2)		; x count: words
	move.w	d2,$38(a2)		; y count
	move.b	#2,$3a(a2)		; HOP: source
	move.b	#3,$3b(a2)		; OP: D = S
	clr.b	$3d(a2)			; no skew
	move.b	#$c0,$3c(a2)		; start, hog
	movem.l	(sp)+,d2-d4/a2
	rts

; int smkx_lc_blit_busy(void)
smkx_lc_blit_busy:
	moveq	#0,d0
	btst	#7,$ffff8a3c.w
	sne	d0
	neg.b	d0
	rts

; void smkx_lc_dcache_clear(void): the blitter wrote behind the data cache.
smkx_lc_dcache_clear:
	movec	cacr,d0
	ori.w	#$0800,d0		; CD: clear the data cache
	movec	d0,cacr
	rts
