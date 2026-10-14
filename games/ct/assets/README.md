# assets

Put the user-supplied ROM here, untracked:

    assets/Chrono Trigger (USA).sfc

A `.smc` with a 512-byte copier header, or a ZIP holding one image, works
too (`make ROM=assets/<file> ctx-source`). The translator accepts only the
unheadered U.S. v1.0 image the disassembly is built from: CRC32
`2D206BF7` (from the disassembly's README).

Nothing in this folder except this file is committed.
