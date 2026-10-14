# assets

Put the user-supplied ROM here, untracked:

    assets/Super Mario World 2 - Yoshi's Island (USA).sfc

A `.smc` with a 512-byte copier header, or a ZIP holding one image, works
too (`make ROM=assets/<file> yix-source`). The translator accepts only
the U.S. v1.0 image the disassembly is based on: MD5
`cb472164c5a71ccd3739963390ec6a50`, the hash the disassembly's framework
checks (`!Define_Global_BaseROMMD5Hash`; the readme's `bb9c2f66…` is not
it), header checksum `$132C`.

Nothing in this folder except this file is committed.
