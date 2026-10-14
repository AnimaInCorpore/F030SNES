; Imports of the translated SMK program (build/smkx/*.s) from the Falcon
; runtime (falcon/smkx_rt.s). tools/smkx/emit.py documents the register
; mapping the translated code relies on.

	xref	rt_rb,rt_rw,rt_wb,rt_ww,rt_iorb,rt_iorw,rt_iowb,rt_ioww,rt_io,rt_wrmpyb,rt_wrmpyw,rt_wrdivb,rt_rw217f,rt_ww217f
	xref	rt_drb4214,rt_drb4215,rt_drb4216,rt_drb4217,rt_drw4214,rt_drw4216,rt_dwb4202,rt_dwb4204,rt_dwb4205,rt_dwb4203,rt_dwb4206,rt_dww4202
	xref	rt_setdbr,rt_bit8,rt_bit16,rt_mvn,rt_mvp,rt_sei,rt_cli
	xref	rt_dispatch,rt_probe,rt_trap,rt_unsupported,rt_badtable,rt_badexit,rt_noreturn,rt_tableoob
	xref	rt_setsp,rt_ramjml,rt_plpwidth,rt_dreg,rt_ramcode,rt_ovlcheck,rt_lastp,rt_ovlmatch
	xref	rt_inc8,rt_dec8,rt_cmpfix,rt_x2c,rt_ccr2p,rt_p2ccr
	xref	rt_vf,rt_pages_r,rt_pages_w,rt_nmitimen,rt_logging,rt_r2180,rt_w2180,rt_dbr,rt_lotab,rt_hitab
	xref	smkx_wram,smkx_rom
