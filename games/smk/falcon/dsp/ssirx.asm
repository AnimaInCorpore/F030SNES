; SSIRX: the scaffold for CPU -> DSP data without the CPU: DMA playback
; into the DSP's SSI receive (the crossbar route DMAPLAY -> DSPREC). The
; receive interrupt puts every word into a ring in Y; the main program only
; answers the host port. SSITEST.TOS (falcon/ssitest.s) plays a buffer and
; checks what arrived.
;
; SSI: 16-bit words, network mode, synchronous, clock and frame from the
; crossbar (as ppu.asm's transmitter). A 16-bit sample arrives left-aligned:
; the ring word is sample << 8. Two crossbar modes (ARM's argument):
;   0 free-running (Devconnect without handshake): a word every slot
;     whether or not the DSP took the last one; the receiver must be
;     drained by interrupt, and a late interrupt loses words (in Hatari a
;     delayed crossbar interrupt delivers two words back to back)
;   1 handshake (Devconnect with handshake): the DSP asks for each word by
;     writing port C with PC4 (SC1) set, as Hatari's crossbar models it
;     (DSP_SsiTransmit_SC1 -> Crossbar_DmaPlayInHandShakeMode); the receive
;     interrupt asks for the next word after storing one (bset on PCD, as
;     ScummVM's handshake c2p does: scummvm-dsp-c2p devtools/atari-falcon030/
;     tools/ssi-dma-c2p/dsp/hsc2p.asm): nothing is lost, the DSP sets the
;     pace. Hatari's crossbar runs free until the DSP has asked once, so the
;     first request goes out before the host starts the DMA.
;
; Memory
;   Y:$2000-$3FFF  the ring (RINGN words, modulo addressing through r7)
;   Y:$0010        ROEF: bit 0 set by a receive overrun (vector $000E)
;   Y:$0011        SRTMP
;   P:$0000-$01FF  this program (internal P, booted by Dsp_ExecBoot)
;
; Host commands (host port, 24-bit words):
;   1 PING    answers $535352 ("SSR")
;   2 ARM     m (0 free-running, 1 handshake): the ring from its start,
;             overrun flag clear, receiver and its interrupt on; answers 1
;   3 COUNT   answers the ring position (words received, modulo RINGN)
;             and the overrun flag
;   4 STOP    receiver off; answers 1
;   5 READ    n: answers the first n ring words
;   6 SUM     n: answers the sum and the XOR of the first n ring words
;   7 BUSY    n: n passes of a 16-cycle loop (the DSP drawing while the
;             ring fills), then answers the ring position

	include	'ioequ.inc'

RING	equ	$2000
RINGN	equ	$2000
ROEF	equ	$0010
SRTMP	equ	$0011

	org	p:$0000
	jmp	start

	org	p:$000C			; SSI receive data: a fast interrupt
	movep	x:m_rx,y:(r7)+
	bset	#4,x:m_pcd		; handshake: PC4 (SC1), the next word

	org	p:$000E			; SSI receive data with overrun (ROE)
	jsr	rx_roe

	org	p:$0040
start:
	movep	#1,x:m_pbc		; host port on
	movep	#0,x:m_bcr		; no wait states
	movep	#0,x:m_crb		; SSI off while set up
	movep	#$4000,x:m_cra		; 16-bit words
	movep	#$01C0,x:m_pcc		; PC6/SCK, PC7/SRD, PC8/STD to the SSI
	movep	#$3000,x:m_ipr		; SSI interrupts at IPL 2
	move	#RING,r7
	move	#RINGN-1,m7
	clr	a
	move	a1,y:ROEF
	andi	#$FC,mr			; interrupts on

main:	jsr	get
	move	#>1,x0
	cmp	x0,a
	jeq	c_ping
	move	#>2,x0
	cmp	x0,a
	jeq	c_arm
	move	#>3,x0
	cmp	x0,a
	jeq	c_count
	move	#>4,x0
	cmp	x0,a
	jeq	c_stop
	move	#>5,x0
	cmp	x0,a
	jeq	c_read
	move	#>6,x0
	cmp	x0,a
	jeq	c_sum
	move	#>7,x0
	cmp	x0,a
	jeq	c_busy
	jmp	main

c_ping:	move	#>$535352,a
	jsr	put
	jmp	main

c_arm:	jsr	get			; the mode
	movep	#0,x:m_crb
	move	#RING,r7
	clr	b
	move	b1,y:ROEF
	movep	b,x:m_pcddr		; free-running: no port C output
	tst	a
	jeq	ca_go
	movep	#$10,x:m_pcddr		; handshake: PC4 (SC1) an output
ca_go:	movep	#$AA00,x:m_crb		; RIE, RE, network, synchronous
	bset	#4,x:m_pcd		; the first request (ignored free-running)
	move	#>1,a
	jsr	put
	jmp	main

c_count:
	jsr	pos
	jsr	put
	move	y:ROEF,a
	jsr	put
	jmp	main

c_stop:	movep	#0,x:m_crb
	movep	#0,x:m_pcddr
	move	#>1,a
	jsr	put
	jmp	main

c_read:	jsr	get
	move	a1,x0
	move	#RING,r0
	do	x0,cr_e
	move	y:(r0)+,a
	jsr	put
	nop
cr_e:	jmp	main

c_sum:	jsr	get
	move	a1,x0
	move	#RING,r0
	clr	a
	clr	b
	do	x0,cs_e
	move	y:(r0)+,x1
	add	x1,a
	eor	x1,b
cs_e:	jsr	put			; the low 24 bits of the sum
	move	b1,a
	jsr	put
	jmp	main

c_busy:	jsr	get
	move	a1,x0
	do	x0,cb_e
	rep	#14
	nop
	nop
cb_e:	jsr	pos
	jsr	put
	jmp	main

; a1 = the ring position (words written, modulo RINGN)
pos:	move	r7,a
	move	#>RING,x0
	sub	x0,a
	move	#>RINGN-1,x0
	and	x0,a
	rts

; overrun: SR, then RX clears ROE; the word is kept
rx_roe:	movep	x:m_sr,y:SRTMP
	movep	x:m_rx,y:(r7)+
	bset	#0,y:ROEF
	bset	#4,x:m_pcd
	rti

get:	jclr	#m_hrdf,x:m_hsr,get
	movep	x:m_hrx,a
	rts

put:	jclr	#m_htde,x:m_hsr,put
	movep	a1,x:m_htx		; a1, not a: moving A would saturate
	rts

	if	@cvi(*)>$200
	fail	'SSIRX is larger than the internal program RAM'
	endif

	end
