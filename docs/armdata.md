# The ARM9 image's data on the host

The boot closure (docs/boot.md, `EIDOS_BOOT=ON`) names 1138 ARM9 data
objects at 945 addresses (aletheia commit e06766e4): tables, strings, the
SDK's state, linker constants. This document is how the port gives the
matched code those objects with their DS initial contents, without a byte
of the game in the repository.

Pieces:

| File | What |
|---|---|
| `tools/armdata.py` | the generator: reads aletheia's metadata, writes the tables and the forced-include header into the build tree |
| `tools/armdata_exclusions.txt` | the data objects port sources define themselves |
| `sdk/armdata.h`, `sdk/armdata.c` | the runtime: copy, relocate, bind |
| `cmake/armdata.cmake` | `eidos_armdata()`: run the generator, force-include the header |
| `tests/test_armdata.py` | the generator against a synthetic aletheia tree |
| `tests/test_armdata.c` | the runtime against a synthetic image |
| `tests/test_armdata_real.c` | real metadata, real image, matched code compiled with the header |

## The design

**The image is the data.** At every `bw_ds_reset` the runtime copies the
player's own `extract/arm9_dec.bin` into the port's flat main RAM
(`sdk/dsmem.c`) at the addresses it occupies on the console: the static
module at 0x02004000, each autoload block at its address (the TWL "DTCM"
block at 0x02FE0000 among them), and zeroes each `.bss` range. A literal
cast (rewritten by `tools/dsaddr.py` into `bw_ds_ptr`), a pointer read out
of a table and an extern name then all reach the same bytes, in the DS's
own layout: neighbouring objects are neighbours, a write that runs past a
declared size lands where it lands on the DS, and `SDK_AUTOLOAD_DTCM_START +
0x3FF8` is the same byte as `bw_ds_ptr(0x02FE3FF8)`.

**Names are pointer variables.** For every extern data name a matched
source in the build uses and no source in the build defines, the generated
header (force-included into the matched sources only) says

```c
#define d_020A6C58 (*bw_dsref_d_020A6C58)
```

so the matched file's own `extern const u32 d_020A6C58[];` becomes
`extern const u32 (*bw_dsref_d_020A6C58)[];`, a pointer to the same type,
and every use a dereference of it. Each file keeps its own declared type,
including the odd ones (a linker symbol declared as a function, an array
declared `u8[]` in one file and a struct in another). The generated C file
defines `void *bw_dsref_<name>` for each, and the runtime points it at the
name's DS address after every reset. The matched sources are not edited
(portability rule 1): the header is a build flag, like the existing
`-Dg_02146B64=sRootList`.

Four bindings:

- **DS**: `bw_ds_ptr(address)`. The image copy, or an object a port module
  registered there (the registry is consulted first).
- **HOST**: the address lies in an object a port source defines
  (`tools/armdata_exclusions.txt`): the port object plus the offset.
  Needed because most port objects (sdk/os_thread.c's thread tables, sdk/fs.c's
  archive) are not registered, so `bw_ds_ptr` would not find them.
- **CONST**: an SDK linker-defined constant such as `SDK_IRQ_STACKSIZE`
  (0x800) or an overlay id (`ov088_id`, 88). The DS symbol's *address* is
  the value and the matched code only casts it, so the variable holds the
  value as a pointer. Any address below 0x01000000 is one of these.
- **OBJECT**: a name some matched file also uses as a struct tag, a struct
  member, a local or parameter, a label, or in a static initialiser cannot
  be a macro (`func_02078D34.c` has `typedef struct FSiOverlayContext ...`
  next to `extern ... FSiOverlayContext;`; eight thread files have a member
  `OSi_IdleThreadStack`). The generator defines it as a real host object
  instead (untyped cells, doubled on LP64), sized by `data_objects.json` or
  up to the next address anything else starts at; the runtime fills it from
  the image copy and registers it with `bw_ds_register`, so every other way
  of reaching the address reaches it. 23 names in the boot closure.

**Aliases need nothing.** Many addresses have several names (in the boot
closure 1138 names share 945 addresses; 0x020AA260 has twelve). Each name is its own variable, bound to the same
byte. A `#define` from one name to another would also work, but breaks
when one translation unit declares both names with different types, and a
second linker name for one object has no portable C spelling (GCC's
`alias` attribute does not exist under MSVC).

**Pointers are relocated.** Every word of initialised data that holds an
address is listed with its location and target, from
`layout/data_objects.json` (the pointer words of each struct and pointer
table) and the `-> symbol` annotations of the reassembly listings in
`asm/arm9/*.s` (only the annotation is read; the target address comes from
the symbol's name or its section in `layout.json`, never from the word).
After the names are bound, the runtime checks that each word holds the
listed target (a different value means the image is another build: panic),
then rewrites it to the host address: `bw_ds_ptr(target)` for data, the
port object for a target inside one, and for code the build's own function
at that address (Thumb targets keep bit 0 in the table and resolve to the
function at the even address). Code the build does not define resolves to a
generated trap that panics with the DS address when called. Words in the
static module's code (literal pools below the first data object) and in
the ITCM and VRAM autoloads (code) are not relocated: the host never reads
them as data.

**Code pointers the metadata does not list.** The listing annotates a word
only when it points at a label the disassembler knew, and
`data_objects.json` describes some tables only in part: the save block
table at 0x0209DA28 holds 69 {id, size, init} entries, and most of its
function words carry no annotation. A data pointer the metadata misses is
harmless where the address space is identity-mapped (docs/boot.md,
"Identity mapping"): the DS address is the host address. A code pointer is
not: calling 0x0200745D jumps into the copied ARM bytes. So the generator
also lists every function the build defines, by the value a pointer to it
holds (its DS entry, bit 0 set for Thumb: the instruction set comes from the
defining file's `isa:` header or a `symbols:` row naming it), and the
ranges that hold data (the static module from its first data object on, and
the data autoloads). After the listed relocations, `bw_data_relocate`
checks every other word of those ranges against that list and rewrites a
match to the host function, exactly like a listed one; a word equal to the
entry of a function in an overlay window resolves through
`bw_ovl_code_entry` to the one compiled overlay that has a function there.
`bw_data_scanned()` counts what the scan rewrote and `eidos_boot` prints
it. The rule is exact equality with a function entry, instruction-set bit
included, which a number in a table is very unlikely to meet; a false match
in a code range would be harmless (the host never reads ARM code as data),
which is why the static module's code and the ITCM and VRAM autoloads are
not scanned at all. The generator writes the list only with `--code-scan`
(`eidos_armdata(... CODE_SCAN)`), which the boot passes: a program that
links every function its sources define.

**Data pointers the metadata does not list.** Identity-mapped, a data
pointer the metadata misses still works, because the DS address is the host
address. On movable host blocks (`BW_DS_IDENTITY=0`, and every host that
cannot reserve 0x02000000: LP64 macOS and iOS, the Wii U, whose loader puts
the program's code there) it does not: `_MainSaveDesc` at 0x0209D938 holds
the save block table's address 0x0209DA28, which no matched file names and
no metadata lists, and `SAVEDATA_BuildLayout` read through it at frame 0.
So on movable blocks only, after the code scan, `bw_data_relocate` also
rewrites every other word of the scanned ranges whose value lies in the
static module's own data, from the first scanned byte to the end of the last
data or `.bss` object the build names, to `bw_ds_ptr` of that value
(`scan_data_pointers`; `bw_data_scanned_data()` counts them and `eidos_boot`
prints the count; `EIDOS_DATA_SCAN_LOG` lists each word). The range is
narrow (0x0209D86C up to the end of `.bss`), and a word inside it that is a
number rather than an address would be a coincidence of the same order the
code scan accepts. Port code that takes such a word as a DS address has to
accept the relocated host address too: the archive name table's entries are
the one case so far (`sdk/fs.c`, `CRYPTOi__BN_lshift1`, maps it back with
`bw_ds_addr_of`). On the ARM9 image of the pin the scan rewrites 273 words,
and the boot runs to the title menu with every screen the same, pixel for
pixel, as the identity-mapped run.

**Byte order.** The image is the DS's, little-endian, and the matched code
reads it with native loads. On a big-endian host (the Wii U's PowerPC)
`bw_data_relocate` ends by turning the image's 16- and 32-bit runs into the
host's order (`swap_to_host`), except the words the relocation and the two
scans wrote host pointers into, which compared against the image's own
little-endian words first and are host-ordered already. The runs
(`BwDataImage.swaps`) are generated (`armdata.data_swap_runs`, for the
ARM9 image here and for each overlay in `tools/ovlink.py`) from, in this
order of trust:

1. `tools/armdata_widths.txt`, a short hand list of objects that only their
   consumer types (the heap's arena tables, read as `{u32, u32}` records;
   `_MainSaveDesc`, read as `SaveDataParam`; ov170's box menu lists), keyed
   by name, or by `region:address` for an object no source names (ov179's
   cut-demo scripts, reached only through a pointer table);
2. data_objects.json's evidence-based kinds: `struct` (fields typed from the
   pointers found in them), `pointer_table` and `string`;
3. the struct layout a source declares the object with
   (`tools/datalayout.py`): `extern const struct BgTemplate d_021BD470;` is
   u32, u32, u32, u32, four u8, u32, four u8, u32, laid out as mwcc does,
   tiled over the object and replacing data_objects.json's numeric or raw
   guess there (it had 32 u8, or 16 u16, which swapped the u32 fields
   wrong or not at all). Where the declaration is opaque, bytes or a block
   stand-in (`typedef struct { u32 w[8]; } BgTemplateBlock;`, the decomp's
   spelling of "copied whole"), the one struct type the source casts the
   name, or a local copied from it, to is used instead. Two declarations
   of one object combine when no byte is a field of two widths (ov179's
   `CdemoEntry` and `CdemoScriptStep`); a union, a bit-field, a packed
   struct or a type the module cannot lay out gives nothing;
4. a scalar `extern` declaration, for an object data_objects.json types
   only as bytes, when every declaration agrees (`extern u32 GXi_DmaId;`);
5. data_objects.json's numeric types and every listed pointer word.

Strings, byte arrays and objects of unknown layout are left alone. A
little-endian host generates the same table and never reads it. Data that
stays in the DS's order whatever the host, such as a compressed stream's
header word, is not turned at all: the game's views of it are listed in
`tools/sso_types.txt` and read in the DS's order under `EIDOS_SSO`.

**A code pointer and a mod's hook.** A word that holds the address of a
function is called through, so it does not pass a call site and the slot the
build puts there never sees it. The relocation therefore asks the registry
for the function the word points at (`bw_arm9_code_entry`), and a mod's hook
wins over the build's own function. The registry is asked when the words are
written, and the words are written at the reset, which is before the mod
layer loads: the port remembers the words it rewrote, and
`bw_arm9_resolve_code_pointers` asks again after the mods are loaded and
puts the hooks in their place. A word the game has written over since is
left alone.

A pointer is only as good as the function behind it. aletheia's
disassembler took some functions reached only through tables for data
(hidden functions: the save block callbacks at 0x02009ED4, the heap
allocator functions at 0x0206127C-0x020612C4); until aletheia matches one, a
port file can stand in for it with a `function:` header that names it (the
scan then knows its address), and the boot's configure step reports the
clash when the matched file arrives, which is the signal to delete it.

**Pointer width.** A relocated word holds a host pointer in the DS's 32-bit
slot, which is only possible where pointers are 32 bits. On an LP64 host
the copy and the name binding still work (numeric tables, strings and
counters are right), but `bw_data_relocate` refuses with `BW_DATA_E_LP64`
and every pointer word keeps its DS address; the reset path panics on that
unless the installer passed `BW_DATA_ALLOW_UNRELOCATED`. The boot is an
ILP32 configuration.

## Why not one host global per object

Defining each referenced object as a host global of its size, filled from the
image and registered with `bw_ds_register`, does not survive the data:

- Sizes are not known. `.bss` objects have none in the metadata, and the
  matched code indexes past declared sizes on purpose (the 235-entry archive
  name table at 0x020A6C58 is two objects in `data_objects.json`; the
  personal-record memo's write runs 0x2C bytes past its declaration).
  Every guess either overruns a host object or makes registrations overlap.
- `SDK_AUTOLOAD_DTCM_START + 0x3FF8` and the stack arithmetic in
  `OS_InitThread` need the 16 KiB of DTCM to be one contiguous block, while
  sdk/os_interrupt.c's IRQ table lives inside it.
- Pointers inside the data reach objects no source names (the archive table
  points at 235 strings). Those need their contents too, so the image has to
  be there anyway.

The flat copy answers all three by construction, and costs one indirection
per named access.

## The exclusions

`tools/armdata_exclusions.txt` lists the data objects port sources define:
sdk/fs.c's `d_02150DA8`; sdk/os_thread.c's `OSi_ThreadInfo`,
`OSi_LauncherThread`, `OSi_IdleThread`, sdk/os_interrupt.c's `g_02FE0020`
(the IRQ table), `g_02150F38`, `OSi_IrqThreadQueue`; sdk/pxi.c's
`FifoCtrlInit` and `FifoRecvCallbackTable`; and sdk/game_globals.c's objects.
For each it gives the DS address and the DS range the object owns. The generator also excludes
any name a source in the build defines, and warns when such a name is not
in the list. Other spellings of a listed object's address are bound to it
(HOST), pointers into it are relocated to it, and pointer words inside its
range are left to its owner.

## Order at reset

`bw_ds_reset` clears the registry and calls `bw_game_globals_register`,
which calls `bw_data_reset_load` first (copy into flat main RAM while
nothing is registered), registers its own objects, and calls
`bw_data_reset_bind` last (bind names, relocate), so an address a port
object owns resolves to it. Nothing happens unless a program installed an
image with `bw_data_install(&bw_arm9_data, flags)` before the reset.

## Using it

```cmake
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/boot_sources.txt
     CONTENT "$<JOIN:$<TARGET_PROPERTY:eidos,SOURCES>,\n>\n...")
eidos_armdata(bw_arm9_data
    SOURCES_LIST ${CMAKE_BINARY_DIR}/boot_sources.txt
    MATCHED ${the matched sources that are compiled})
target_sources(<the target> PRIVATE ${bw_arm9_data_C})
```

and before the first `bw_ds_reset`, `bw_data_install(&bw_arm9_data, 0)`.

## Overlays

`tools/armdata.py --region ovNNN` describes one overlay: its copy range,
its `.bss`, the relocations `data_objects.json` and the overlay's listing
(`asm/overlays/ovNNN.s`) give, and the names. Compiled overlays use
`tools/ovlink.py` instead, which writes one `BwDataImage` per overlay with
the overlay's own pointer variables (`bw_dsref_ovNNN_<name>`) and its
relocations resolved per overlay; `sdk/overlay_image.c` runs `bw_data_load`,
`bw_data_bind` and `bw_data_relocate` on it at every load, with the bytes out
of the overlay's own decompressed file (`docs/overlays.md`).
