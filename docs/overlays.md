# Overlays compiled into one executable

Everything past the boot's ARM9 code lives in overlays: 237 files the game
copies, one at a time per window, into fixed RAM windows. The title path alone
loads eight of them (ov009, ov010, ov011, ov013, ov017, ov228 during the boot,
ov088 for the save check and the title menu, ov179 for the opening cinematic;
`docs/title-closure.md`). This document is how the port compiles a set of
overlays into one host executable and loads them the way the DS does.

Pieces:

| File | What |
|---|---|
| `tools/ovlink.py` | the generator: per-overlay namespaces, trampolines, stubs, data tables |
| `tools/ovlink_exclusions.txt` | matched overlay sources left out on purpose, with the reason |
| `cmake/overlays.cmake` | `eidos_overlays()`: run the generator, compile the set |
| `sdk/overlay_image.h`, `sdk/overlay_image.c` | the runtime: the overlay files, load, trampolines' and stubs' panics |
| `sdk/overlay.c` | the loader; `bw_overlay_set_provider` is its hook for compiled overlays |
| `sdk/armdata.c` | the copy, bind and relocate steps, shared with the ARM9 image |
| `tests/test_ovlink.c`, `tests/ovl_synth/` | two overlays in one window, synthetic |
| `tests/test_ovlink_real.c` | ov088 and ov179 from the player's extract |

## The problem

On the DS an overlay's code runs from its window, and each overlay has its
own symbols. Overlays that share a window define the same names:
`func_02155244` is one function in ov009 and another in ov010, and about a
thousand names repeat this way. A host executable has one global namespace
and no windows, so compiling two overlays that share a window means renaming,
and a call into a window from outside it means asking which overlay is there.

## Names: a namespace per overlay

`tools/ovlink.py` writes a forced-include header for each overlay
(`include/ovNNN.h`) and one per translation unit that includes it. The
matched files are not edited (portability rule 1). For overlay N:

- every external name one of its sources defines, and every name its sources
  use whose address lies inside N's window, becomes `ovNNN_<name>`
  (`#define func_02155244 ov010_func_02155244`);
- a spelling that differs from the defining file's (an SDK name for a
  `func_` address, `sub_` for `func_`) is renamed to the definition, by
  address: inside the overlay to `ovNNN_<definition>`, and towards ARM9 to
  the name the rest of the link defines at that address;
- a name whose address lies in another overlay's window keeps its plain
  spelling: the plain name is a trampoline (below);
- ARM9 names, runtime helpers and port objects are left alone.

The address of a name comes from the file's own `symbols:` block, else the
address spelled into the name, else `layout/symbols_sdk.txt` (the overlay's
own region first). Which names a file defines, declares and calls is read
the same way the boot link reads the ARM9 sources.

One definition per name. A few matched files define, besides the function
their manifest entry claims, neighbours that another file claims too (a
unit kept next to the single-function files split out of it); mwcc's link
took one. The owner of a name is the file whose entry claims it, else the
first file defining it; every other file's copy is renamed to a name of
its own (`ov010_func_0215A690__ov010_func_0215A684`) by that file's
header, and that file's own calls use its copy. The report counts them.

Two things a host compiler needs that mwcc did not:

- a call with no declaration in scope (a declaration inside a function
  body counts as one) gets `int name();` in the unit's
  header (C89's implicit declaration, the one the DS code was compiled
  against; a definition returning 64 bits gets `long long`), as
  `tools/bootlink.py` does for the boot;
- mwcc's builtins `__builtin__clear` and `__memcpy`, called undeclared by a
  few SDK files, map to `bw_mwcc_clear` and `bw_mwcc_memcpy`.

## Calls into a window: trampolines

A name whose address lies in a window the referring code does not own (ARM9
code calling `func_021DFC20`, ov002 calling ov010) is defined once, in the
generated `ovl_dispatch.c`, as a trampoline: it asks `bw_ovl_resident_at`
(the loader's residency, over the window `[ram_address, ram_address +
ram_size + bss_size)`) which overlay is there and calls that overlay's
function. With nothing resident, or a resident overlay this build does not
compile, it panics naming the address and what is resident. Code pointers
the overlay's data holds into another window, and ARM9 sources' calls into a
window, take the same trampolines; ovlink scans the link's other sources for
the latter. ARM9 data holds no pointer into an overlay window (none of the
1613 pointer words `data_objects.json` and the ARM9 listings list), so the
ARM9 image's relocations never need one.

A trampoline forwards its arguments, so it has a prototype: the definition's
of the first compiled overlay that has a function at the address, normalised
to types every host passes alike (every pointer, data or function, is
`void *`; every integer typedef its C type). An overlay whose function there
has another normalised prototype panics in the trampoline instead of being
called through the wrong type; a prototype that cannot be normalised (a
struct by value, a float, `...`) makes a trampoline that only panics.

## Code addresses written as literals

`(void *)0x021B6279` in an ov179 source is a Thumb function's address passed
as a callback. `tools/dsaddr.py` rewrites every DS-range literal cast into
`bw_ds_ptr`, which would hand out the copied code bytes. For a source that
casts a known function address (a Thumb address, or a function start the
metadata knows), its header renames `bw_ds_ptr` to the overlay's translator
`bw_ovl_ds_ptr_ovNNN`, which answers the host function for those addresses
(the overlay's own, an ARM9 function, or a trampoline) and `bw_ds_ptr` for
every other. Casts in static initialisers are not rewritten by dsaddr at all
and stay as they are.

## Data

An overlay's data is not a set of host objects. The overlay's file is copied
to its window in the port's flat main RAM at every load, the same model as
the ARM9 image (`docs/armdata.md`), so a name, a literal cast and a pointer
read out of a table reach the same bytes, and whichever overlay was loaded
last is what the window holds, as on the DS. A load is, in
`sdk/overlay_image.c`:

1. `bw_data_load`: copy the file (`extract/overlays_dec/y9_NNNN.bin`, or
   `extract/overlays/y9_NNNN.bin` for an overlay stored uncompressed) to
   `[ram_address, ram_address + ram_size)` and zero the `.bss`;
2. `bw_data_bind`: point every data name the overlay's sources use at its
   address. Each overlay has its own pointer variables
   (`#define d_021BD8E8 (*bw_dsref_ov088_d_021BD8E8)`), including for ARM9
   names it uses, so no two overlays and no overlay and the ARM9 image ever
   define the same variable. Bindings are DS (`bw_ds_ptr`, which returns a
   registered object where one owns the address), HOST (a port object) or
   CONST (a linker constant such as `ov228_id`), as for ARM9;
3. `bw_data_relocate`: check each listed pointer word against the metadata
   and rewrite it: to the overlay's own function (`ovNNN_<name>`, or its
   stub), to an ARM9 function the link defines (a trap that panics if not),
   to a trampoline for another window, or to `bw_ds_ptr` of a data target.
   A data target in another overlay's window is that window's bytes, i.e.
   whichever overlay is loaded there when the pointer is followed, which is
   what the DS does.

Unloading leaves the bytes where they are until the next overlay is copied
over them, and the loader stops calling the overlay resident; the
overlay's host objects are withdrawn at the next load (below).

Which pointer words are listed. aletheia's `data_objects.json` describes the
ARM9 image and ov203 only; for overlays the listing annotations of
`asm/overlays/ovNNN.s` are the source, and they mark code pointers (`->
func_`, `-> veneer_`) and a few literal-pool targets (`-> pool_`), not
pointers from overlay data to data. A data-to-data pointer word the metadata
does not list stays a DS address in the copy, which is right wherever the
address space is identity-mapped (docs/boot.md, "Identity mapping"): the DS
address is the host address of the window's bytes. On the movable blocks
it stays wrong; that is a metadata limit to close in aletheia (a
`data_objects.json` region per overlay).

Pointer width: as for ARM9, a relocated word holds a host pointer in the
DS's 32-bit slot, which only fits where pointers are four bytes. On an LP64
host the copy and the bindings work and the relocation is refused; the load
panics unless the set was installed with `BW_DATA_ALLOW_UNRELOCATED`.

Byte order: the file is the DS's, little-endian, and the overlay's matched
code reads its data with native loads. On a big-endian host the relocation
ends by turning the data's 16- and 32-bit runs into the host's order, as
for the ARM9 image (`docs/armdata.md`, "Byte order"; `swap_to_host`). ovlink
writes each overlay's runs (`k_swap`) from the same sources armdata.py uses:
`data_objects.json`'s region for the overlay, every listed pointer word, the
widths the overlay's own sources declare for names the metadata types only
as bytes, and `tools/armdata_widths.txt`. Only runs inside the overlay's
file are kept: an ARM9 name an overlay's source declares is the ARM9 image's
to turn, once. Every load copies the file afresh, so every load turns it
afresh. The hooks `sdk/overlay_image.c` writes into the overlay's code words
are written in the host's order too, as the relocation wrote the words.

A name a matched file also uses as a member, tag, local or in a static
initialiser cannot be a macro over a pointer; it becomes a host object of
its own (`ovNNN_<name>`), filled at load from the bytes at its address and
registered there (`bw_ds_register`), as the ARM9's are, so a literal cast or
a pointer into its range reaches it. Before an overlay loads,
`sdk/overlay_image.c` withdraws the objects of every compiled overlay that
is no longer resident, and of the one loading (a reload): each object's
contents are copied back to its address and its registration dropped
(`bw_data_withdraw`, `bw_ds_unregister`), so the window holds what the DS
would. An object whose range a registration already covers (two names at
one address in one overlay, ov013's `d_021B4BA0` and `g_021B4BA0`) is filled
and left unregistered: the first owns the bytes. ovlink's report lists every
one: the title set has 19 (15 in ov009's network library, mostly SDK state
such as `SOCLiConfigPtr`; 4 in ov013). The conflict test no longer counts an
`extern struct { ... } NAME;` declaration as a use of NAME other than the
global (it kept ov010's `d_02148BCC` from being a macro, and its 0x3C-byte
host object was written at +0x7C).

Data defined by a matched overlay source (a `const` table written in C)
is a host object under the overlay's namespace, not the window's bytes; the
same report lists them.

## What the build lacks: stubs

Every function known in a compiled overlay that no compiled source defines
is a stub `ovNNN_func_XXXXXXXX` that panics with the overlay and the
address. "Known" is the listing's `.global` function labels, the manifest
shards, the SDK symbols in the overlay's region, every address the sources
or the data reach in the window, and the static initialisers. A source left
out (an mwcc `asm` body, or a line in `tools/ovlink_exclusions.txt`) leaves
its functions to stubs. So does a source that declares a `static` function
it uses and does not define (mwcc's link resolved it against another file's
static definition; a host compiler cannot): ovlink leaves it out by itself
and names the functions in its report (two files of ov009's network
library in the title set). So a partly matched overlay links and runs until
it reaches one.

Port C for single overlay functions lives in `stubs/ov/`, and ovlink
compiles it as more units of the overlay (`--port-ov-dir`):

- `stubs/ov/ovNNN_<name>.c`, a stand-in for a function aletheia has not
  matched, written from the listing with the decoding in its header; once a
  matched file of the overlay defines its name, ovlink leaves it out and
  reports it obsolete;
- `stubs/ov/reimpl_ovNNN_<name>.c`, a reimplementation of a matched file
  whose header says ` * replaces: src/matched/ov/...`: the matched file is
  left out and this one compiled in its place, for the reason the ARM9's
  `stubs/reimpl_` files exist (an argument the matched C passes in a
  register, docs/boot.md, "Arguments in registers").

Every function of a compiled overlay is also listed by the value a code
pointer to it holds (`BwOvlImage.entries`, Thumb bit from the listing's
`.thumb_func`), so the ARM9 data's pointers into a window relocate to it
(`bw_ovl_code_entry`, docs/armdata.md); a value two compiled overlays both
define is left alone, since it would need a trampoline.

The loader's presence check is therefore per function, not per overlay: for
an overlay compiled into the executable (`bw_overlay_set_provider`, which
`bw_ovl_install` connects) `FS_LoadOverlayImage` skips the all-present check
and loads through the provider; an overlay that is not compiled in keeps the
old behaviour (the panic naming the first missing function, or the image
hook).

ARM9 functions the overlays call that the rest of the link does not define
are listed in `ovl_arm9_needs.txt`; the standard C functions among them
(`memcpy`, `strlen`, `sprintf`, ...: the ARM9's MSL copies on the DS) are
the host C library's and never stubbed. With `--arm9-stubs`
(`EIDOS_OVERLAYS_ARM9_STUBS`, the default) each is a stub too, which is what
lets a set link without the boot.

## Static initialisers

Only ov230 has any (five, `tools/overlay_table.py`'s `KNOWN_SINIT`). A
compiled overlay carries its initialisers as `ovNNN_` functions (stubs when
unmatched), and `FS_StartOverlay` runs them through the provider, in table
order.

## Building

```cmake
eidos_overlays(<name>
    TARGET <executable>
    OVERLAYS 88 179            # or ALL, or TITLE
    LINK_SOURCES <every other C source in the link>
    [ARM9_STUBS])
```

and before the first load, `bw_ovl_install(&bw_ovl_set_<name>, flags)`.
`EIDOS_OVERLAYS` (ids separated by `;`, `ALL`, `TITLE`; empty means `TITLE`
when `EIDOS_BOOT` is on) puts a set named `boot` into `eidos_boot`.

The generator runs at configure time and writes under
`<build>/ovl/<name>/`; `ovl_report.txt` there has the counts and every
decision worth a second look (excluded sources, host objects, unresolved
names, code literals, trampolines). Each matched source is rewritten by
`tools/dsaddr.py` into that directory rather than the shared `dsaddr/`
tree, because two sets may compile one file with different headers.

## Joining the boot link

With `EIDOS_BOOT=ON` the boot link (`tools/bootlink.py`, `cmake/boot.cmake`)
and this one are planned together (CMakeLists.txt, "Overlays compiled into
eidos_boot"):

1. A first ovlink pass against the eidos library (`eidos_overlays_seeds`)
   lists the matched ARM9 files that define what the overlays call
   (`ovl_arm9_seeds.txt`, about 745 for the title set); they join the boot's
   seeds, so the closure carries the overlays' ARM9 callees (about 6078
   matched files in all).
2. The boot is planned, then the overlays are linked against the eidos
   library plus the boot's closure, so only what nothing defines is stubbed.
   An ARM9 function nothing defines is named by the ARM9's own spelling (an
   asm-bodied file's, the SDK's), and stubbed by ovlink only when no boot
   file reaches it (bootlink stubs those itself).
3. bootlink's generator reads `ovl_dispatch.c` and `ovl_arm9_stubs.c` as
   port sources (`EIDOS_OVERLAY_SHIMS`): the trampolines' plain names are
   defined, not open.
4. `demo/eidos_boot.c` installs the set (`bw_ovl_install(&bw_ovl_set_boot,
   0)`) before the first reset.

## How far the boot runs

With the title set the boot runs to the title menu: the boot sequence
loads ov010 and ov011, runs ov009's and ov013's network set-up, ov228's
save-data set-up, and the frame loop then runs the opening (ov179) and the
title and its menu (ov088). docs/boot.md, "How far it gets", has the frames
and what it took.

What it took on the way, besides the overlays: dwc_memfunc.o's three
functions share one table that aletheia's per-function files each keep a
copy of (`sdk/boot/dwc_memfunc.c`); RTCi_ConvertTimeToSecond and ov009's
gsiMemoryCallbacksSet from their asm bodies (`sdk/asm/`, `sdk/asm/ov/`,
which ovlink picks up by file name as bootlink does `sdk/asm/`); two heap
destroys whose argument the matched C left in r0 (aletheia's C names it
now); the OAM clear's table
address passed as an integer (`stubs/reimpl_02036FD8.c`). The grown closure
also needed eleven more shim-replaced files, one owner per name among boot
files (bootlink gives a second copy a private name, as ovlink does inside an
overlay), mwcc's builtins, and ITCM data names bound as values.
