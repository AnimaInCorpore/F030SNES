# assets

Put the user-supplied ROM here, untracked:

    assets/Super Mario World (USA).sfc

A `.smc` with a 512-byte copier header, or a ZIP holding one image, works
too (`make ROM=assets/<file> smwx-source`). The translator accepts only the
U.S. release: LoROM, 512 KiB, header checksum `$A0DA` / complement `$5F25`,
as `third_party/SMWDisX/smw.asm` assembles it for `!_VER` = U.

Nothing in this folder except this file is committed.
