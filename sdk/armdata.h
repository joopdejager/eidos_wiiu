/*
 * sdk/armdata.h -- the ARM9 image's data, loaded, relocated and bound to the
 * matched code's names.  docs/armdata.md is the design.
 *
 * tools/armdata.py writes, into the build tree, one BwDataImage per region
 * (the ARM9 static module; tools/ovlink.py writes one per compiled overlay,
 * docs/overlays.md) and a forced-include header
 * for the matched sources.  Everything in a BwDataImage is an address, a
 * size or a name: the bytes are copied at run time out of the player's own
 * extract/arm9_dec.bin (bw_arm9_rodata).
 *
 * The data lives where it lives on the DS: in the port's flat main RAM
 * (sdk/dsmem.c) at its DS address.  A matched extern name is a pointer
 * variable the header turns every use of into a dereference, and this module
 * points it at the name's DS address; a literal cast reaches the same byte
 * through bw_ds_ptr; a pointer stored inside the data is rewritten to the
 * host address of what it points at.  Several names for one address are
 * several variables bound to one byte.
 *
 * Pointer width.  A pointer the DS stored in a 32-bit word can hold a host
 * pointer only where pointers are 32 bits.  On an LP64 host the copy and the
 * name binding work (numeric tables, strings, counters are right), the
 * relocation does not: bw_data_relocate refuses with BW_DATA_E_LP64 and
 * leaves every pointer word as the DS address it was, and the reset path
 * panics on it unless the installer said BW_DATA_ALLOW_UNRELOCATED.
 *
 * A code pointer reaches a mod's hook.  A word that holds the address of a
 * function is called through, so it does not pass a call site (the
 * declared set's slots are at call sites, docs/mod-interface.md, section 2).
 * The relocation asks the registry what the function's address answers
 * (bw_arm9_code_entry, below): a mod's hook wins over the build's own
 * function, and a function nobody hooked keeps its own address.
 */

#ifndef BW_PORT_ARMDATA_H
#define BW_PORT_ARMDATA_H

#include <stddef.h>

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*BwDataFn)(void);

/* How a name's pointer variable is bound. */
enum {
    BW_DATA_BIND_DS,      /* bw_ds_ptr(addr): the image copy, or a registered object */
    BW_DATA_BIND_HOST,    /* a port object (tools/armdata_exclusions.txt) + offset   */
    BW_DATA_BIND_CONST,   /* a linker constant: the "address" is the value          */
    BW_DATA_BIND_OBJECT   /* a host object of its own (BwDataObject), no variable  */
};

/* Where the name's address lies (layout.json); informational. */
enum {
    BW_DATA_SEC_DATA,     /* initialised: static module or an autoload block */
    BW_DATA_SEC_BSS,      /* zero-initialised                                */
    BW_DATA_SEC_CONST,    /* below 0x01000000: a linker-defined constant     */
    BW_DATA_SEC_TWL,      /* the DSi-only arm9i block: not loaded, zero      */
    BW_DATA_SEC_OVERLAY,  /* an overlay's range: filled when it is loaded    */
    BW_DATA_SEC_OTHER     /* fixed RAM outside the image (the shared area)   */
};

/* How a relocated word's target is resolved. */
enum {
    BW_DATA_RELOC_DATA,   /* bw_ds_ptr(target)                        */
    BW_DATA_RELOC_CODE,   /* funcs[index].fn                          */
    BW_DATA_RELOC_HOST    /* anchors[index].host + target - its addr  */
};

typedef struct BwDataName {
    const char  *name;
    u32          addr;
    void       **ref;          /* the variable bw_dsref_<name>             */
    u8           bind;         /* BW_DATA_BIND_*                           */
    u8           section;      /* BW_DATA_SEC_*                            */
    u16          align;        /* from the symbols block, 0 if not given   */
    u32          size;         /* from data_objects.json, 0 if not known   */
    const void  *anchor;       /* BW_DATA_BIND_HOST: the port object       */
    u32          anchor_addr;  /* ... and its DS address                   */
} BwDataName;

typedef struct BwDataRange {
    u32 addr;                  /* DS address                                */
    u32 size;
    u32 image_addr;            /* where its bytes are in arm9_dec.bin, as an
                                  address bw_arm9_rodata takes; for an
                                  overlay, the offset into its own file     */
    u32 copy;                  /* 1: copied from the image; 0: zeroed       */
} BwDataRange;

typedef struct BwDataReloc {
    u32 where;                 /* DS address of the word                    */
    u32 target;                /* the DS address it holds (Thumb bit kept)  */
    u16 kind;                  /* BW_DATA_RELOC_*                           */
    u16 index;                 /* into funcs or anchors                     */
} BwDataReloc;

typedef struct BwDataFunc {
    u32         addr;          /* as stored: Thumb functions have bit 0 set */
    BwDataFn    fn;            /* the build's function, or a trap           */
    const char *name;          /* NULL for a trap                           */
} BwDataFunc;

typedef struct BwDataAnchor {
    u32         addr;
    u32         size;          /* DS extent, 0 if unknown                   */
    const void *host;
    const char *name;
} BwDataAnchor;

/*
 * A name that cannot be a macro (some matched file also uses it as a struct
 * tag, a member, a local or in a static initialiser) is a real host object
 * instead: untyped cells, filled from the image copy at load and registered
 * at its DS address, so every other way of reaching the address reaches it.
 * Its storage is doubled where pointers are eight bytes, because a host
 * struct with pointer members is at most twice its DS size.
 */
typedef union BwDataCell {
    u64      q;
    double   d;
    void    *p;
    BwDataFn f;
} BwDataCell;

#define BW_DATA_CELLS(ds_size)     (((ds_size) * (sizeof(void *) / 4u) + sizeof(BwDataCell) - 1u) / sizeof(BwDataCell))

typedef struct BwDataObject {
    u32         addr;
    u32         size;          /* DS extent, the registered range           */
    void       *host;
    const char *name;
} BwDataObject;

typedef struct BwDataImage {
    const char         *region;        /* "arm9", "ov009", ...              */
    int                 overlay;       /* -1 for the ARM9 static module     */
    const BwDataRange  *ranges;
    u32                 range_count;
    const BwDataName   *names;
    u32                 name_count;
    const BwDataReloc  *relocs;
    u32                 reloc_count;
    const BwDataFunc   *funcs;
    u32                 func_count;
    const BwDataAnchor *anchors;
    u32                 anchor_count;
    const BwDataObject *objects;
    u32                 object_count;
    /* Ranges whose words bw_data_relocate also checks against `funcs`: a
     * word there equal to a function's DS entry (Thumb bit included) is a
     * code pointer the metadata did not list, relocated like a listed one
     * (docs/armdata.md, "Code pointers the metadata does not list"). */
    const BwDataRange  *scan;
    u32                 scan_count;
} BwDataImage;

/* tools/armdata.py's output for the ARM9 static module. */
extern const BwDataImage bw_arm9_data;

/* ------------------------------------------------------------------ */
/* Results                                                             */
/* ------------------------------------------------------------------ */

enum {
    BW_DATA_OK          =  0,
    BW_DATA_E_NOIMAGE   = -1,   /* arm9_dec.bin missing or too short          */
    BW_DATA_E_MISMATCH  = -2,   /* a pointer word is not what the metadata
                                   says: the image is another build            */
    BW_DATA_E_LP64      = -3,   /* pointer words cannot hold host pointers    */
    BW_DATA_E_UNBACKED  = -4,   /* an address no DS region or object backs    */
    BW_DATA_E_LAYOUT    = -5,   /* the copy found the registry not empty      */
    BW_DATA_E_UNSUPPORTED = -6  /* no longer returned: overlay images load    */
};

/* Installer flags. */
#define BW_DATA_ALLOW_UNRELOCATED 0x1u   /* LP64: bind and copy, skip relocation */

/*
 * Install `img` (NULL uninstalls).  From the next bw_ds_reset on, every reset
 * copies, binds and relocates it (bw_game_globals_register calls the two
 * hooks below), and a failure panics with the reason.  Nothing is installed
 * by default, so a program that never calls this sees no change.
 */
void bw_data_install(const BwDataImage *img, unsigned flags);
const BwDataImage *bw_data_installed(void);

/*
 * The two reset hooks.  bw_game_globals_register calls the first before it
 * registers anything (the registry is empty, so the copy lands in flat main
 * RAM) and the second after (so a name or pointer whose address a port
 * object owns resolves to that object).  No-ops when nothing is installed.
 */
void bw_data_reset_load(void);
void bw_data_reset_bind(void);

/*
 * The steps, for tests and for a caller that installs nothing.  Each
 * returns BW_DATA_OK or a BW_DATA_E_* code and leaves a description in
 * bw_data_error().
 *   bw_data_load      copy and zero the ranges, fill and register the host
 *                     objects; run it right after bw_ds_reset cleared the
 *                     registry (the reset hook does)
 *   bw_data_bind      point every name's variable at its address
 *   bw_data_relocate  rewrite every pointer word (ILP32 only)
 */
int bw_data_load(const BwDataImage *img);
int bw_data_bind(const BwDataImage *img);
int bw_data_relocate(const BwDataImage *img);

/*
 * Withdraw the host objects `img`'s last bw_data_load registered: each one's
 * contents are copied back to its DS address and its registration dropped,
 * so the address is the flat region's bytes again.  For an overlay whose
 * window passes to another (sdk/overlay_image.c); objects not registered by
 * it are left alone.
 */
void bw_data_withdraw(const BwDataImage *img);

/* How many unlisted code pointers the last bw_data_relocate found by its
 * scan (BwDataImage.scan) and rewrote. */
u32 bw_data_scanned(void);
/* How many unlisted data pointers it rewrote: on movable host blocks only,
 * where a DS address is not the host address (docs/armdata.md, "Data
 * pointers the metadata does not list"). */
u32 bw_data_scanned_data(void);
const char *bw_data_error(void);

/*
 * The ARM9's version of bw_ovl_code_entry (sdk/overlay_image.h): the host
 * function for a code pointer into the ARM9 static module, `value` as the DS
 * stores it (the Thumb bit set for a Thumb function), or NULL when the
 * image's function table does not know the value.
 *
 * The registry is consulted before the table, the way the overlay's is: a
 * mod's hook for the function's (region, address) pair wins over the
 * build's own function, and a function nobody hooked keeps its own address
 * (docs/mod-interface.md, section 2).  The relocation uses it for every code
 * pointer it writes, listed ones and the ones the scan finds, so a stored
 * pointer reaches the installed hook.
 */
BwDataFn bw_arm9_code_entry(const BwDataImage *img, u32 value);

/*
 * Put the hooks in the ARM9's stored code pointers.
 *
 * The words are written when the data is relocated, which is at the reset,
 * and the mod layer loads after the first reset, so the registry was empty
 * when the words were written.  This asks it again for every word the port
 * rewrote and puts the hook there instead.  A word the game has written
 * over since is left alone.  Returns how many words it changed.
 */
u32 bw_arm9_resolve_code_pointers(void);

/* 1 where a pointer fits in the DS's 32-bit word, i.e. relocation works. */
int bw_data_relocation_supported(void);

/* The name entry for `name`, or NULL. */
const BwDataName *bw_data_find(const BwDataImage *img, const char *name);

/* Called by the generated traps: data pointed at code the build lacks. */
void bw_data_trap(u32 addr);

#ifdef __cplusplus
}
#endif

#endif /* BW_PORT_ARMDATA_H */
