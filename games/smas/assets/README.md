# assets

Put the user-supplied ROM here, untracked:

    assets/Super Mario All-Stars (USA).sfc

A `.smc` with a 512-byte copier header, or a ZIP holding one image, works
too (`make ROM=assets/<file> smasx-source`). The translator accepts only
the U.S. release the disassembly is based on: MD5
`53c038150ba00d5f8d8574b4d36283f2` (headerless; `!Define_Global_BaseROMMD5Hash`
in `SMAS/RomMap/ROM_Map_SMAS_U.asm`).

Nothing in this folder except this file is committed.
