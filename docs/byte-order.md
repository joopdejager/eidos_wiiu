# A big-endian host (the Wii U)

The DS is little-endian; the Wii U's PowerPC is big-endian. The matched
code reads and writes everything with native loads and stores, so on a
big-endian host every number it reads must already be in the host's order.
The port keeps one rule: what the game's code reads natively is in the
host's order; what only the port's hardware models read (samples, a
compressed stream) stays in the DS's. This is where each piece of that is
done. A little-endian host runs none of it.

| What | Where it is turned | When |
| --- | --- | --- |
| The ARM9 image's and each overlay's data | `sdk/armdata.c` `swap_to_host`, runs from `tools/armdata.py` / `tools/ovlink.py` (`docs/armdata.md`, "Byte order") | at load |
| A NARC's framing | `install/byteorder.c` | extraction |
| Message banks | `install/byteorder.c` (`bw_msg_to_host`) | extraction |
| Fonts (NFTR, `a/0/2/3`) | `install/font_order.c` (`bw_nftr_to_host`) | extraction (GF_FONT reads them in pieces) |
| The sound archive's structure | `sdk/sdat_order.c` (`docs/audio.md`, "Byte order on a big-endian host") | extraction; a mod's appended sound files at merge |
| NITRO 2D files (NCLR, NCGR, NSCR, NCER, NANR, NMCR, NMAR) | `sdk/g2d_order.c`, from the port's `NNS_G2dFindBinaryBlock` | the first time the game looks into one |
| NITRO 3D files (BMD0, BTX0, BCA0, BMA0, BTA0, BTP0, BVA0), members and those embedded in members (map chunks, building packs) | `sdk/g3d_order.c` (`bw_g3d_member_to_host`), from `install/byteorder.c` | extraction |
| Camera animations (ICA, `src/game/arm9/ica_anime.c`), known by their shape | `install/byteorder.c` (`bw_ica_to_host`) | extraction |
| The title demo's direct-colour bitmaps | `src/game/ov179/cdemo_scene.c` (host branch) | at load |
| The I/O register file | `sdk/dsmem.h` `bw_io_host_index` | every access |

`byte_order.txt` beside `data/` records which formats an extraction turned
(`BW_BYTE_ORDER_MARK`, `sdk/types.h`); the game refuses a tree turned by an
older extractor.

**Data images.** The metadata (aletheia's `layout/data_objects.json`) often
guesses an object's element type. The swap runs prefer, in order: the hand
list (`tools/armdata_widths.txt`), the metadata's evidence-based kinds, a
struct the sources declare the object with (or cast it to, or pass it to a
struct-pointer parameter, by name or literal address:
`tools/datalayout.py`), a scalar every declaration agrees on, and only then
the metadata's guess.

Struct layouts are laid out over a declared array's length (`sHandlerKeys[10]`,
`sTexStartAddrTable[16]`), through a pointer field to the object its
relocation points at (the title's `G3dUtilSetup` to its tables, a light
set to its records), and through unions whose members share one layout
(`MtxFx33`).  The ARM9 image's typing also reads the overlays' sources, for
the tables only an overlay casts (`gClActorInitParam`), without binding
anything from them.

**The port's own readers of the cartridge's layout.** `engine/g3d/nsbmd.c`
and `engine/world/` read 3D files as the DS lays them out; each turns the
member it loads back (`bw_g3d_member_to_ds`), the same walk run the other
way.

**The register file.** It is kept as host-order 32-bit words, so a u32
store and the u8 or u16 accesses of its parts agree, and the divider's and
square-root unit's 64-bit registers as host-order 64-bit words, so a u64
store (`FX_InvAsync`'s `DIV_NUMER`) and its 32-bit halves agree.
`bw_ds_ptr_w` applies the rule to the matched code's accesses; port code
reaches a register through it, or through `bw_io_peek`/`bw_io_poke`, never
through `bw_ds_ptr` and a cast.  `tools/dsaddr.py` passes each cast's
width; a `void *` cast takes the width of the parameter it is passed as
(`GXx_SetMasterBrightness_`'s `vu16 *`), else a word (a block of registers:
the clip matrix `MI_Copy64B` reads back).  An index off one register's
address cannot follow the rule, so `FX_DivAsync` has a host branch that
names each register by its own address.

**Kept in the DS's order.** A compressed member's header word (the game's
views of it are in `tools/sso_types.txt`), PCM and ADPCM samples, sequence
bytecode, and the hardware words `tools/sso_types.txt` lists.

**OAM holds the DS's order** on a big-endian host.  The game views an
entry as four u16 halves, as two u32 words (`attr01`, `attr23`) and as
bit-fields over those words, and only the DS's order makes the three
agree.  So the views are stored that way (`GXOamAttr`, its copies
`OamAttr` and `OamEntry`, and `GXOamAffine` are in `tools/sso_types.txt`),
the compositor reads OAM little-endian, a fill of OAM writes its value
little-endian (`bw_host_in_oam`, `sdk/mi.c`, `sdk/mi_dma.c`), and the
ARM9 data's image of hidden entries (`wm_icon_HiddenOam`) is not turned.
The few places that take a field's address or store raw words into an
entry have host branches in the decomp.

**VRAM and palette RAM** hold the host's order: the game's own stores
there are native, and the palettes and screens it copies in come from
turned NITRO files. Raw 16-bit data copied in from an archive member with
no header (the title demo's bitmaps) has to be turned where the game loads
it.
