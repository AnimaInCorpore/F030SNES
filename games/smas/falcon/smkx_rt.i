; Imports of the translated SMK program (build/smkx/*.s) from the Falcon
; runtime (falcon/smkx_rt.s). tools/smkx/emit.py documents the register
; mapping the translated code relies on.

	xref	rt_rb,rt_rw,rt_wb,rt_ww,rt_iorb,rt_iorw,rt_iowb,rt_ioww
	xref	rt_setdbr,rt_bit8,rt_bit16,rt_mvn,rt_mvp,rt_sei,rt_cli,rt_tcd,rt_oamreset
	xref	rt_dispatch,rt_probe,rt_trace,rt_trap,rt_unsupported,rt_badtable,rt_badexit,rt_noreturn
	xref	rt_inc8,rt_dec8,rt_cmpfix,rt_x2c,rt_ccr2p,rt_p2ccr
	xref	smkx_wram,smkx_rom,smkx_romw
