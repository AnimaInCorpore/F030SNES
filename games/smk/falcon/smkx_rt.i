; Imports of the translated SMK program (build/smkx/*.s) from the Falcon
; runtime (falcon/smkx_rt.s). tools/smkx/emit.py documents the register
; mapping the translated code relies on.

	xref	rt_rb,rt_rw,rt_wb,rt_ww,rt_iorb,rt_iorw,rt_iowb,rt_ioww
	xref	rt_setdbr,rt_bit8,rt_bit16,rt_mvn,rt_mvp,rt_sei,rt_cli
	xref	rt_dispatch,rt_probe,rt_trap,rt_unsupported,rt_badtable,rt_badexit,rt_noreturn,rt_tableoob
	xref	rt_inc8,rt_dec8,rt_cmpfix,rt_x2c,rt_ccr2p,rt_p2ccr
	xref	rt_vf
	xref	rt_w4203,rt_w4206,rt_w2122,rt_wapu0,rt_wapu1,rt_wapu2,rt_wapu3
	xref	rt_wdsp,rt_rdsp,rt_rdsp_sr,rt_rdspw,rt_rdsp_srw,rt_io,rt_apu_out
	xref	rt_r4211,rt_r4212,rt_w4200,rt_w420b
	xref	smkx_wram,smkx_rom,smkx_romw
