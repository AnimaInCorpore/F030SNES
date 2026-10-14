"""The games of F030SNES, in one place: the root Makefile's list, tools/release.py and every port's info screen
(tools/port_info.py) read it.  Keep the states in step with the games' README.md and ROADMAP.md and the root
README's table.

Each entry: make target, program (in games/<game>/build/), extra make variables for a release, title, state, and
the files the player supplies (the 8.3 name the program opens next to itself, what it is).  Super Mario All-Stars
has no translation yet and builds no program.
"""

GAMES = {
    "smk": dict(
        target="smkx", program="smkx/SMKX.TOS", make_vars=[],
        title="Super Mario Kart (USA)",
        state="runs to the title screen; 16 colours per frame (four planes), Mode 7 on the DSP",
        files=[("SMK.SFC", "the game's ROM"), ("DSP1.BIN", "the cartridge's DSP-1 (uPD77C25) dump, 8 KiB")]),
    "smw": dict(
        target="smwx", program="smwx/SMWX.TOS", make_vars=[],
        title="Super Mario World (USA)",
        state="title screen and attract demo; 256 colours",
        files=[("SMW.SFC", "the game's ROM")]),
    "smw2": dict(
        target="yix", program="yix/YIX.TOS", make_vars=["YIX_GSU=interp"],
        title="Super Mario World 2: Yoshi's Island (USA)",
        state="logo, intro, title and file menu (SuperFX interpreted); stops in game mode $38, no sound CPU yet",
        files=[("YI.SFC", "the game's ROM")]),
    "dkc": dict(
        target="dkcx", program="dkcx/DKCX.TOS", make_vars=[],
        title="Donkey Kong Country (USA)",
        state="intro, file select and player-mode menu",
        files=[("DKC.SFC", "the game's ROM")]),
    "ct": dict(
        target="ctx", program="ctx/CTX.TOS", make_vars=[],
        title="Chrono Trigger (USA)",
        state="intro, title, attract scenes and the menu shell; music on the SPC700 interpreter",
        files=[("CT.SFC", "the game's ROM")]),
    "ff6": dict(
        target="ff6x", program="ff6x/FF6X.TOS", make_vars=[],
        title="Final Fantasy VI (Japan, v1.0)",
        state="title, opening and the first dialogue at Narshe",
        files=[("FF6.SFC", "the game's ROM")]),
    "sciv": dict(
        target="scivx", program="scivx/SCIVX.TOS", make_vars=[],
        title="Super Castlevania IV (USA)",
        state="intro, title menu, name entry and stage 1",
        files=[("SCIV.SFC", "the game's ROM")]),
}

# The keyboard is pad 1 in every port (falcon/smkx_rt.s, the ACIA handler).
CONTROLS = [
    "Cursor keys D-pad   Return Start",
    "X B  Z Y  S A  A X  Space  Select",
    "Q L  W R  Esc quit to the desktop",
]
