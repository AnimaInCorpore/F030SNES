# assets

Put the user-supplied ROM here, untracked:

    assets/Donkey Kong Country (USA).sfc

A `.smc` with a 512-byte copier header, or a ZIP holding one image, works
too (`make ROM=assets/<file> dkcx-source`). The translator accepts only
the U.S. v1.0 image the disassembly is based on: MD5 `30c5f292ff4cbbfcc00fd8fa96c2de3b` (from
the disassembly's readme).

Nothing in this folder except this file is committed.
