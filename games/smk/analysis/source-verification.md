# SMK source and ROM verification

## Source identity

- Repository: `Yoshifanatic1/Super-Mario-Kart-Disassembly`
- Submodule path: `third_party/Super-Mario-Kart-Disassembly`
- Pinned revision: tag `V1.0`, commit
  `f5c5c21f263172f20936893fa339ae0819094954`
- Applicable map: `SMK/RomMap/ROM_Map_SMK_U1.asm`
- Declared base ROM MD5: `7f25ce5a283d902694c52fb1152fa61a`
- Supplied `.sfc` MD5: `7f25ce5a283d902694c52fb1152fa61a`

The supplied ROM matches the source repository's U.S. v1 map. This verifies
the ROM variant to use when following the disassembly. It does not independently
verify every decoded routine or prove a byte-identical reassembly.

## Scope and limits

The submodule is a reverse-engineered assembly project built on an Asar SNES
ROM framework, not Nintendo's original source tree. Its own
`SMK/Notes.txt` says some indirect trampoline pointers may be missing and
describes the listing as 98% disassembled at the time of that note. Treat a
routine as source evidence only after checking its entry, range, callers and
data references in the pinned files. The U1 hash match does not upgrade an
unresolved routine to proven behavior.

## Verified asset path

The extractor follows these source landmarks:

| Purpose | Pinned source reference | ROM range or action |
| --- | --- | --- |
| Mario Circuit 1 course map | `SMK/Routine_Macros_SMK.asm`, `SMK_LevelData_MarioCircuit1` at `$868D2C`; `DATA_81EB5B` | ROM `$068D2C..$06952B`; nested decompression in `CODE_81E745` |
| Shared Mode 7 track tiles | `SMK/AsarScripts/AssetPointersAndFiles.asm`, `GFX_Mode7_GlobalTrackTiles` | `$C40000..$C40594`; decoded by `CODE_81E6B9` |
| Mario Circuit Mode 7 tiles | same pointer table, `GFX_Mode7_MarioCircuitTrack` | `$C481C9..$C48F6A`; selected in `CODE_81E6D4` |
| Mario Circuit course palette | `SMK/Routine_Macros_SMK.asm`, `SMK_PaletteData_MarioCircuitCourse` | `$C4117F..$C41313`; selected in `CODE_81E72E` |
| Tile expansion | `CODE_84E378` and `CODE_84E3C7` | Expands the packed Mode 7 definitions referenced by the loader |

The source ROM ranges for the compressed Mario Circuit 1 map and course
palette are byte-for-byte equal to the submodule's checked-in
`TrackData/MarioCircuit1.lz5` and `Palettes/MarioCircuitCourse.lz5` files. The
extractor checks these equalities on each run. Both nested map decompression
stages and the palette decompression produce the lengths consumed by the
source path.

The course texture's RGB quantization to DawnBringer 16 is a target asset
conversion requested for this port. It uses one nearest-color mapping per
source palette entry and no spatial dithering. Sprite art remains in the
original ROM until the driver pose tables, transparency attributes and runtime
CGRAM palette sources are recovered.

## Active race-screen trace

The entry route is being recovered from the pinned listing before Falcon game
logic is added. `CODE_81E258` enters the race setup path and calls
`CODE_81BF93`, `CODE_81E627`, `CODE_81F155`, `CODE_81F818`, and
`CODE_81E561`. `CODE_81E627` loads course assets; `CODE_81E745` resolves the
course-map pointer and performs two decompression calls; `CODE_81E6D4` loads
course graphics; `CODE_81E72E` loads the course palette. `CODE_85989F` writes
Mode 7 registers and HDMA state from runtime memory. The per-frame producers of
those values, the background/layer and OAM composition, and the final race HUD
placement still need to be traced as one connected route.

The Falcon output requirement remains 256×224 in four Videl planes with 16
colors. The original screen composition has not yet been fully recovered, so
there is no split-screen height, custom HUD, movement formula, lap rule or
control mapping asserted here.
