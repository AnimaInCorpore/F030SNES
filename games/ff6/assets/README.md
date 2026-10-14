# assets

Put the user-supplied ROM here, untracked:

    assets/Final Fantasy VI (Japan).sfc

A `.smc` with a 512-byte copier header, or a ZIP holding one image, works
too (`make ROM=assets/<file> ff6x-source`). The translator accepts only the
unheadered Japanese v1.0 image: CRC32 `45EF5AC8`, 3 MiB (3145728 bytes),
HiROM, 8 KiB SRAM (the disassembly's README lists this CRC as "Final
Fantasy VI 1.0 (J)"). The U.S. releases (Final Fantasy III) are not
accepted.

Nothing in this folder except this file is committed.
