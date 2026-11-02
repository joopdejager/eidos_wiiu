/*
 * sdk/armdata.c -- the ARM9 image's initialised data, in the DS address
 * space, with its pointers relocated and the matched code's names bound.
 *
 * Why this exists: the matched code names the game's data by extern names
 * pinned to DS addresses, casts their addresses as literals, and follows
 * pointers stored inside the data.  On the DS all three are the ARM9 image
 * in RAM.  This module puts the player's own image where the DS has it (the
 * port's flat main RAM, sdk/dsmem.c), so all three reach the same bytes in
 * the DS's own layout, and then does the two things a host needs on top: it
 * rewrites every pointer word to a host pointer, and it points each name's
 * variable (tools/armdata.py's forced-include header) at its address.
 * docs/armdata.md is the design and says why the data is not a set of host
 * globals instead.
 *
 * The tables come from tools/armdata.py and contain addresses and names
 * only; the bytes come from bw_arm9_rodata at run time.
 *
 * Order at reset (bw_game_globals_register calls both hooks):
 *   1. bw_data_reset_load, first, while the registry is empty: copy each
 *      initialised range out of arm9_dec.bin into flat main RAM, zero each
 *      .bss range.  Ranges outside main RAM (the ITCM and VRAM autoloads,
 *      which hold code) are skipped: no matched code reads them as data.
 *   2. sdk/game_globals.c registers its objects.
 *   3. bw_data_reset_bind, last: bind every name (a name whose address a
 *      registered or port object owns resolves to that object) and rewrite
 *      every pointer word in the copy.  A pointer word inside a port
 *      object's DS range was left out by the generator: its owner fills it.
 *
 * An overlay's image (img->overlay >= 0, docs/overlays.md) goes through the
 * same three steps when sdk/overlay_image.c loads it, with its bytes out of
 * the overlay's own file.  Its host objects are registered at load like the
 * ARM9's, and bw_data_withdraw takes them back (their contents copied into
 * the window first) when the window passes to another overlay.
 *
 * Every pointer word is checked against the image before it is rewritten:
 * the metadata says which address the word holds, and a different value
 * means the image is not the build aletheia describes.  That is a panic at
 * reset, not a silently wrong pointer.
 *
 * A code pointer reaches a mod's hook.  A word that holds the address of a
 * function is called through, so it does not pass a call site and the slot
 * the build puts there never sees it (docs/mod-interface.md, section 2).
 * The relocation asks the registry what the function's address answers
 * (bw_arm9_code_entry, this module's version of the overlay's
 * bw_ovl_code_entry): a mod's hook wins over the build's own function, and
 * a function nobody hooked keeps its own address.  The overlay's own data
 * is the overlay's image's business (sdk/overlay_image.c).
 *
 * The registry is asked when the words are written, so the answer is the
 * one the registry holds at that moment.  The ARM9's data is written at
 * every reset, and the mod layer loads after the first reset (the boot,
 * demo/eidos_boot.c), so a hook installed after the reset reaches a stored
 * pointer only when the data is written again.  bw_arm9_resolve_code_pointers
 * closes that: it asks the registry again for the words the port rewrote
 * and puts the hooks in their place.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port.h"
#include "dsmem.h"
#include "armdata.h"
#include "overlay_image.h"
#include "mod_slot.h"

#define PIECE 0x10000u            /* sdk/dsmem.c commits main RAM in 64 KiB */
#define MAX_RANGES 16

static const BwDataImage *g_installed;
static unsigned           g_flags;
static char               g_error[256];

/* The flat host address of each loaded range's first byte, or NULL. */
static u8                *g_flat[MAX_RANGES];
static const BwDataImage *g_flat_img;
/* The ARM9's own image, once loaded: an overlay's scan asks it for the
 * ARM9 functions the overlay's data points at. */
static const BwDataImage *g_arm9_img;
static u32                g_scanned;
static u32                g_scanned_data;

/* The DS addresses of the words this relocation wrote a host pointer into:
 * already in the host's order, so swap_to_host leaves them alone. */
static u32 *g_done;
static u32  g_done_count, g_done_cap;

static void done_add(u32 where)
{
    if (g_done_count == g_done_cap) {
        u32 n = g_done_cap != 0u ? g_done_cap * 2u : 1024u;
        u32 *grown = (u32 *)realloc(g_done, n * sizeof(u32));

        if (grown == NULL) {
            bw_panic("bw_data_relocate", where, "out of memory");
        }
        g_done = grown;
        g_done_cap = n;
    }
    g_done[g_done_count++] = where;
}

/*
 * The code words the port rewrote, so a hook installed later can be put in
 * their place (bw_arm9_resolve_code_pointers).  The word's own content says
 * whether the game has written over it since: the port wrote a host
 * address there, and a word that holds something else is the game's now.
 */
#define BW_DATA_CODE_WORDS_MAX 4096

typedef struct BwDataCodeWord {
    u8                 *word;   /* the word the port rewrote              */
    u32                 host;   /* what the port wrote there               */
    const BwDataFunc   *func;   /* the function the word stood for         */
} BwDataCodeWord;

static BwDataCodeWord g_code_words[BW_DATA_CODE_WORDS_MAX];
static u32            g_code_word_count;

/* Remember a rewritten code word, so it can be resolved again later. */
static void remember_code_word(u8 *word, u32 host, const BwDataFunc *func)
{
    if (g_code_word_count >= BW_DATA_CODE_WORDS_MAX) {
        return;
    }
    g_code_words[g_code_word_count].word = word;
    g_code_words[g_code_word_count].host = host;
    g_code_words[g_code_word_count].func = func;
    g_code_word_count++;
}

static int fail(int code, u32 addr, const char *what)
{
    snprintf(g_error, sizeof(g_error), "%s (0x%08lX)", what, (unsigned long)addr);
    return code;
}

const char *bw_data_error(void)
{
    return g_error;
}

int bw_data_relocation_supported(void)
{
    return sizeof(void *) == 4u && sizeof(BwDataFn) == 4u;
}

void bw_data_install(const BwDataImage *img, unsigned flags)
{
    g_installed = img;
    g_flags = flags;
    g_flat_img = NULL;
}

const BwDataImage *bw_data_installed(void)
{
    return g_installed;
}

static int in_main(u32 addr, u32 size)
{
    return addr >= BW_DS_MAIN_BASE && size <= BW_DS_MAIN_SIZE
        && addr - BW_DS_MAIN_BASE <= BW_DS_MAIN_SIZE - size;
}

/*
 * The flat host block for [addr, addr + size), committed, or NULL.  Walks
 * the range a piece at a time, because translating an address is what
 * commits its piece; and checks that every piece continues the first one,
 * which is what an empty registry guarantees and a registered object at a
 * piece start would break.
 */
static u8 *flat_block(u32 addr, u32 size)
{
    u8 *base = (u8 *)bw_ds_try_ptr(addr);
    u32 off;

    if (base == NULL) {
        return NULL;
    }
    off = (PIECE - (addr & (PIECE - 1u))) & (PIECE - 1u);
    for (; off < size; off += PIECE) {
        if ((u8 *)bw_ds_try_ptr(addr + off) != base + off) {
            return NULL;
        }
    }
    if (size != 0 && (u8 *)bw_ds_try_ptr(addr + size - 1u) != base + size - 1u) {
        return NULL;
    }
    return base;
}

int bw_data_load(const BwDataImage *img)
{
    u32 i;

    g_flat_img = NULL;
    if (img->range_count > MAX_RANGES) {
        return fail(BW_DATA_E_LAYOUT, 0, "more ranges than sdk/armdata.c keeps");
    }
    for (i = 0; i < img->range_count; i++) {
        const BwDataRange *r = &img->ranges[i];
        u8 *dst;

        g_flat[i] = NULL;
        if (!in_main(r->addr, r->size)) {
            /* The ARM9's ITCM and VRAM autoloads hold code (see the header).
             * An overlay the game loads into VRAM holds data too: ov095, the
             * battle's, runs from LCDC bank H at 0x06898020 and its handler
             * tables need their pointers rewritten.  The game maps the bank
             * to LCDC before it loads the overlay, so the range is one flat
             * block of the VRAM array then; otherwise it stays code only. */
            if (img->overlay < 0 || (dst = flat_block(r->addr, r->size)) == NULL) {
                continue;
            }
        } else {
            dst = flat_block(r->addr, r->size);
        }
        if (dst == NULL) {
            return fail(BW_DATA_E_LAYOUT, r->addr,
                        "range is not one flat block of main RAM; "
                        "bw_data_load must run before anything is registered");
        }
        if (r->copy && img->overlay >= 0) {
            /* An overlay: image_addr is an offset into its own file. */
            const void *src = bw_ovl_file_bytes((u32)img->overlay, r->image_addr, r->size);
            if (src == NULL) {
                return fail(BW_DATA_E_NOIMAGE, (u32)img->overlay,
                            "the overlay's file (extract/overlays_dec/ or "
                            "extract/overlays/) is missing or shorter than "
                            "layout.json says");
            }
            memcpy(dst, src, r->size);
        } else if (r->copy) {
            const void *src = bw_arm9_rodata(r->image_addr, r->size);
            if (src == NULL) {
                return fail(BW_DATA_E_NOIMAGE, r->image_addr,
                            "extract/arm9_dec.bin is missing or shorter than "
                            "layout.json says");
            }
            memcpy(dst, src, r->size);
        } else {
            memset(dst, 0, r->size);
        }
        g_flat[i] = dst;
    }
    /* The host objects: their bytes from what is at their address now (the
     * copy just made, or, for an address outside this image's ranges, the
     * bytes some other image put there), then registered.  An object whose
     * range a registration already covers (an alias at the same address
     * in this image, an ARM9 object an overlay names too) is filled but
     * left unregistered: the first registration owns the bytes. */
    for (i = 0; i < img->object_count; i++) {
        const BwDataObject *o = &img->objects[i];
        const u8 *cur;

        memset(o->host, 0, BW_DATA_CELLS(o->size) * sizeof(BwDataCell));
        if (bw_ds_registered(o->addr, o->size)) {
            cur = (const u8 *)bw_ds_try_ptr(o->addr);
            if (cur != NULL) {
                memcpy(o->host, cur, o->size);
            }
            continue;
        }
        cur = flat_block(o->addr, o->size);
        if (cur != NULL) {
            memcpy(o->host, cur, o->size);
        }
        bw_ds_register(o->addr, o->host, o->size, o->name);
    }
    g_flat_img = img;
    if (img->overlay < 0) {
        g_arm9_img = img;
    }
    return BW_DATA_OK;
}

/* Index of the function whose stored entry is `value`, or -1 (funcs is
 * sorted by entry, as both generators write it). */
static long func_at(const BwDataImage *img, u32 value)
{
    u32 lo = 0, hi = img->func_count;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        if (img->funcs[mid].addr < value) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo < img->func_count && img->funcs[lo].addr == value ? (long)lo : -1;
}

/* 1 if `where` is a listed relocation's word (relocs sorted by location). */
static int listed(const BwDataImage *img, u32 where)
{
    u32 lo = 0, hi = img->reloc_count;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        if (img->relocs[mid].where < where) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo < img->reloc_count && img->relocs[lo].where == where;
}

/*
 * Whether the word at `where` lies in a run the typing gives another width
 * than a pointer's (BwDataImage.swaps, sorted by address, on every host):
 * halfwords or 64-bit numbers are data, whatever their bytes read as.  The
 * pointer scans skip it: FX_SinCosTable_'s (0x0FB8, 0x02FA) reads as the
 * word 0x02FA0FB8, an address inside the static module's data.
 */
static u32 typed_width(const BwDataImage *img, u32 where);

static int typed_not_word(const BwDataImage *img, u32 where)
{
    u32 w = typed_width(img, where);

    return w != 0u && w != 4u;
}

/* The width of the swap run holding the word at `where`, or 0. */
static u32 typed_width(const BwDataImage *img, u32 where)
{
    u32 lo = 0, hi = img->swap_count;

    if (img->swaps == NULL) {
        return 0;
    }
    while (lo < hi) {                       /* the first run ending past where */
        u32 mid = lo + (hi - lo) / 2u;
        if (img->swaps[mid].addr + img->swaps[mid].size <= where) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo < img->swap_count && img->swaps[lo].addr < where + 4u
           ? img->swaps[lo].width : 0u;
}

/*
 * The host address a code pointer to the ARM9's function `f` holds: the
 * registry's answer for the function's address when a mod installed a hook
 * for it, the build's own function otherwise.
 *
 * The key is the pair (region, address): the ARM9's region and the
 * function's even address.  A data word carries the instruction-set tag the
 * game gave it and the entry table the tag the listing gave the function,
 * and the two disagree for a few (the overlay's lookup has the same note,
 * sdk/overlay_image.c, bw_ovl_code_entry), so the key is the address alone.
 *
 * An overlay's own image is left alone: its data is relocated by
 * sdk/overlay_image.c, which asks the overlay's own table.
 */
static BwDataFn arm9_code_of(const BwDataImage *img, const BwDataFunc *f)
{
    void *h;

    if (img->overlay >= 0) {
        return f->fn;
    }
    h = bw_mod_slot_lookup(BW_MOD_REGION_ARM9, f->addr & ~1u);
    return h != NULL ? (BwDataFn)h : f->fn;
}

/*
 * The ARM9's version of bw_ovl_code_entry (sdk/overlay_image.h): the host
 * function for a code pointer into the ARM9 static module, `value` as the DS
 * stores it (the Thumb bit set for a Thumb function).
 *
 * The registry is consulted before the table, the way the overlay's is: a
 * mod's hook for the function's (region, address) pair wins over the build's
 * own function (docs/mod-interface.md, section 2).  NULL when the image's
 * function table does not know the value: the word is not a code pointer
 * into this region.  An overlay's own image answers with its own function:
 * the registry is asked for the ARM9's region only, and the overlay's data
 * is relocated by sdk/overlay_image.c.
 */
BwDataFn bw_arm9_code_entry(const BwDataImage *img, u32 value)
{
    long f;

    if (img == NULL || img->funcs == NULL) {
        return NULL;
    }
    f = func_at(img, value);
    if (f < 0) {
        return NULL;
    }
    return arm9_code_of(img, &img->funcs[f]);
}

/*
 * Code pointers the metadata does not list (docs/armdata.md): every word of
 * the scan ranges that equals a function's DS entry, Thumb bit included,
 * becomes that function's host address.  Listed words are already done and
 * skipped.  Returns how many words it rewrote.
 */
/* 1 if `value` lies inside one of the image's own ranges. */
static int in_own_ranges(const BwDataImage *img, u32 value)
{
    u32 k;

    for (k = 0; k < img->range_count; k++) {
        if (value - img->ranges[k].addr < img->ranges[k].size) {
            return 1;
        }
    }
    return 0;
}

/*
 * The word at DS address `where` as the game reads it: a registered host
 * object's copy when one covers it (bw_data_load registers the image's
 * objects over their bytes, ov021's gFieldGimmickInitFuncs among them),
 * else the flat copy `flat` of the range.  The scans read and write this
 * one, so what they relocate is what the game loads.
 */
static u8 *scan_word(u8 *flat, u32 where)
{
    u8 *p = (u8 *)bw_ds_try_ptr(where);

    if (p == NULL || p == flat || (u8 *)bw_ds_try_ptr(where + 3u) != p + 3) {
        return flat;
    }
    return p;
}

static u32 scan_code_pointers(const BwDataImage *img)
{
    u32 n = 0;
    u32 s, k;

    /* This image's words into overlay windows are found again below. */
    bw_ovl_window_words_forget(img->overlay);
    if (img->scan == NULL || img->func_count == 0) {
        return 0;
    }
    for (s = 0; s < img->scan_count; s++) {
        const BwDataRange *sr = &img->scan[s];
        for (k = 0; k < img->range_count; k++) {
            const BwDataRange *r = &img->ranges[k];
            u32 lo, hi, a;

            if (g_flat[k] == NULL) {
                continue;
            }
            lo = sr->addr > r->addr ? sr->addr : r->addr;
            hi = sr->addr + sr->size < r->addr + r->size ? sr->addr + sr->size
                                                         : r->addr + r->size;
            for (a = (lo + 3u) & ~3u; a + 4u <= hi; a += 4u) {
                u8 *w = scan_word(g_flat[k] + (a - r->addr), a);
                u32 value = bw_rd32(w);
                long f = func_at(img, value);
                BwDataFn fn = NULL;
                u32 word;

                if (f < 0 && img->overlay >= 0 && (value & 1u) != 0u) {
                    /* an overlay's Thumb function its table lists without
                     * the Thumb bit (the listing did not mark it) */
                    f = func_at(img, value & ~1u);
                }

                if (listed(img, a)) {
                    continue;
                }
                if (img->overlay < 0 ? typed_not_word(img, a)
                                     : typed_width(img, a) == 8u) {
                    /* An overlay's data is typed coarsely: data_objects.json
                     * and the declarations call whole tables u16 (ov092's
                     * pickup items run over its EVENT_BATTLE_RETURN proc
                     * table), so a word there equal to a function's entry
                     * is still taken as a code pointer.  swap_to_host
                     * leaves a word this rewrites alone. */
                    continue;
                }
                if (f >= 0) {
                    /* Into the ARM9's own code: the registry first, so a
                     * mod's hook for the function wins (bw_arm9_code_entry). */
                    fn = arm9_code_of(img, &img->funcs[f]);
                } else if (img->overlay < 0 || !in_own_ranges(img, value)) {
                    /* Into an overlay window: the ARM9 data's save block
                     * table points at ov010's functions, ov021's field
                     * model move tables at ov010's, and ov021's encounter
                     * effect table at ov075's and ov078's, which share
                     * their window.  The overlay resident now answers, or
                     * the one overlay with a function there; the word is
                     * bound again as overlays load over it
                     * (bw_ovl_window_word). */
                    fn = bw_ovl_code_entry(value);
                    if (fn == NULL && img->overlay >= 0) {
                        /* an overlay's data into the ARM9's code */
                        fn = bw_arm9_code_entry(g_arm9_img, value);
                    }
                    if (fn != NULL) {
                        word = (u32)(uintptr_t)fn;
                        memcpy(w, &word, 4);
                    }
                    if (bw_ovl_window_word(w, value, img->overlay) && fn == NULL) {
                        /* No overlay answers yet: the DS value stays,
                         * unswapped, until one with a function there
                         * loads. */
                        done_add(a);
                        continue;
                    }
                }
                if (fn == NULL) {
                    continue;
                }
                word = (u32)(uintptr_t)fn;
                memcpy(w, &word, 4);
                if (img->overlay < 0 && f >= 0) {
                    /* the ARM9's own words only: an overlay's are the
                     * overlay layer's (bw_data_relocate), and a word into
                     * an overlay window has no entry of the ARM9's */
                    remember_code_word(w, word, &img->funcs[f]);
                }
                done_add(a);
                n++;
            }
        }
    }
    return n;
}

/*
 * Data pointers the metadata does not list (docs/armdata.md): a word in the
 * scanned ranges whose value lies in the static module's own data, from the
 * first scanned byte to the end of the last data or .bss object the build
 * names, is a pointer into it (the save block table at 0x0209DA28, which
 * _MainSaveDesc points at and no matched file names), and is rewritten to
 * where that byte lives on the host.  Identity-mapped, a DS address is the
 * host address and nothing needs rewriting, so the scan runs on movable host
 * blocks only (the only mode a host without the DS's addresses has).
 */
static u32 scan_data_pointers(const BwDataImage *img)
{
    u32 lo_t = 0xFFFFFFFFu, hi_t = 0, n = 0;
    u32 i, s, k;

    if (bw_ds_identity() || img->scan == NULL) {
        return 0;
    }
    if (img->overlay >= 0) {
        /* An overlay: a word pointing into its own data or .bss, from its
         * first scanned byte to the end of its image (ov203's demo table
         * d_021C185C, which data_objects.json calls four zero bytes, holds
         * pointers to the camera records after it). */
        for (s = 0; s < img->scan_count; s++) {
            if (img->scan[s].addr < lo_t) {
                lo_t = img->scan[s].addr;
            }
        }
        for (k = 0; k < img->range_count; k++) {
            if (img->ranges[k].addr + img->ranges[k].size > hi_t) {
                hi_t = img->ranges[k].addr + img->ranges[k].size;
            }
        }
    }
    for (i = 0; img->overlay < 0 && i < img->name_count; i++) {
        const BwDataName *nm = &img->names[i];
        u32 end;

        if (nm->bind == BW_DATA_BIND_CONST
            || (nm->section != BW_DATA_SEC_DATA && nm->section != BW_DATA_SEC_BSS)
            || nm->addr >> 24 != BW_DS_MAIN_BASE >> 24) {
            continue;
        }
        end = nm->addr + (nm->size != 0u ? nm->size : 4u);
        if (end > hi_t) {
            hi_t = end;
        }
    }
    for (s = 0; img->overlay < 0 && s < img->scan_count; s++) {
        if (img->scan[s].addr >> 24 == BW_DS_MAIN_BASE >> 24 && img->scan[s].addr < lo_t) {
            lo_t = img->scan[s].addr;
        }
    }
    if (lo_t >= hi_t) {
        return 0;
    }
    for (s = 0; s < img->scan_count; s++) {
        const BwDataRange *sr = &img->scan[s];
        for (k = 0; k < img->range_count; k++) {
            const BwDataRange *r = &img->ranges[k];
            u32 lo, hi, a;

            if (g_flat[k] == NULL) {
                continue;
            }
            lo = sr->addr > r->addr ? sr->addr : r->addr;
            hi = sr->addr + sr->size < r->addr + r->size ? sr->addr + sr->size
                                                         : r->addr + r->size;
            for (a = (lo + 3u) & ~3u; a + 4u <= hi; a += 4u) {
                u8 *w = scan_word(g_flat[k] + (a - r->addr), a);
                u32 value = bw_rd32(w);
                void *p;
                u32 word;

                if (value < lo_t || value >= hi_t || listed(img, a)
                    || (img->overlay < 0 ? typed_not_word(img, a)
                                         : typed_width(img, a) == 8u)) {
                    continue;
                }
                p = bw_ds_try_ptr(value);
                if (p == NULL) {
                    continue;
                }
                if (getenv("EIDOS_DATA_SCAN_LOG") != NULL) {
                    fprintf(stderr, "data scan: 0x%08lX holds 0x%08lX\n",
                            (unsigned long)a, (unsigned long)value);
                }
                word = (u32)(uintptr_t)p;
                memcpy(w, &word, 4);
                done_add(a);
                n++;
            }
        }
    }
    return n;
}

/*
 * The image is the DS's, little-endian, and the matched code reads it with
 * native loads; on a big-endian host the 16- and 32-bit runs the metadata
 * types (BwDataImage.swaps) are turned into the host's order once the
 * relocation is done, except the words it wrote host pointers into.  The
 * bytes are reached the way the game reaches them, so a registered object's
 * copy is turned too.  Strings, byte arrays and objects of unknown layout
 * are left as they are.
 */
static int done_has(u32 where)
{
    u32 lo = 0, hi = g_done_count;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        if (g_done[mid] < where) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo < g_done_count && g_done[lo] == where;
}

static int cmp_u32(const void *a, const void *b)
{
    u32 x = *(const u32 *)a, y = *(const u32 *)b;

    return x < y ? -1 : x > y;
}

static void swap_to_host(const BwDataImage *img)
{
#if BW_HOST_BIG_ENDIAN
    u32 i, a;

    if (img->swaps == NULL) {
        return;
    }
    qsort(g_done, g_done_count, sizeof(u32), cmp_u32);
    for (i = 0; i < img->swap_count; i++) {
        const BwDataSwap *s = &img->swaps[i];

        for (a = s->addr; a + s->width <= s->addr + s->size; a += s->width) {
            u8 *p;

            if ((s->width == 4u || s->width == 2u) && done_has(a & ~3u)) {
                /* a pointer word the relocation or the scan wrote */
                continue;
            }
            p = (u8 *)bw_ds_try_ptr(a);
            if (p == NULL) {
                continue;
            }
            if (s->width == 4u) {
                bw_mem_wr32(p, bw_rd32(p));
            } else if (s->width == 2u) {
                bw_mem_wr16(p, bw_rd16(p));
            } else if (s->width == 8u) {
                u8 t[8];
                u32 j;
                for (j = 0; j < 8u; j++) {
                    t[j] = p[7u - j];
                }
                memcpy(p, t, 8);
            }
        }
    }
#else
    (void)img;
#endif
}

void bw_data_withdraw(const BwDataImage *img)
{
    u32 i;

    for (i = 0; i < img->object_count; i++) {
        const BwDataObject *o = &img->objects[i];
        u8 *flat;

        /* Registered by this image's last load: the registry answers with
         * the object itself. */
        if (bw_ds_try_ptr(o->addr) != o->host) {
            continue;
        }
        (void)bw_ds_unregister(o->addr);
        flat = flat_block(o->addr, o->size);
        if (flat != NULL) {
            memcpy(flat, o->host, o->size);
        }
    }
}

/* The host address of the word at `where`: inside a host object, or in the
 * flat copy of a loaded range. */
static u8 *flat_word(const BwDataImage *img, u32 where)
{
    u32 i;

    for (i = 0; i < img->object_count; i++) {
        const BwDataObject *o = &img->objects[i];
        if (where - o->addr < o->size && o->size - (where - o->addr) >= 4u) {
            return (u8 *)o->host + (where - o->addr);
        }
    }

    for (i = 0; i < img->range_count; i++) {
        const BwDataRange *r = &img->ranges[i];
        if (g_flat[i] != NULL && where - r->addr < r->size
            && r->size - (where - r->addr) >= 4u) {
            return g_flat[i] + (where - r->addr);
        }
    }
    return NULL;
}

int bw_data_bind(const BwDataImage *img)
{
    u32 i;

    for (i = 0; i < img->name_count; i++) {
        const BwDataName *n = &img->names[i];
        void *p;

        if (n->bind == BW_DATA_BIND_OBJECT || n->ref == NULL) {
            continue;               /* its own storage, registered at load */
        }
        switch (n->bind) {
        case BW_DATA_BIND_CONST:
            p = (void *)(uintptr_t)n->addr;
            break;
        case BW_DATA_BIND_HOST:
            p = (u8 *)(uintptr_t)n->anchor + (n->addr - n->anchor_addr);
            break;
        default:
            p = bw_ds_try_ptr(n->addr);
            /* Commit the object's whole known extent. */
            if (p != NULL && n->size > 1u) {
                u32 off;
                for (off = PIECE; off < n->size; off += PIECE) {
                    (void)bw_ds_try_ptr(n->addr + off);
                }
                (void)bw_ds_try_ptr(n->addr + n->size - 1u);
            }
            break;
        }
        if (p == NULL && bw_ds_vram_window(n->addr)) {
            /* Data that lives in a VRAM bank (overlay 95's tables in LCDC
             * bank H), named by an overlay that loads while the bank is
             * mapped elsewhere: the address is the bytes once it is back,
             * as on the DS, where the name is only an address. */
            p = (void *)(uintptr_t)n->addr;
        }
        if (p == NULL) {
            snprintf(g_error, sizeof(g_error),
                     "%s: no DS region or object backs 0x%08lX",
                     n->name, (unsigned long)n->addr);
            return BW_DATA_E_UNBACKED;
        }
        *n->ref = p;
    }
    return BW_DATA_OK;
}

const BwDataName *bw_data_find(const BwDataImage *img, const char *name)
{
    u32 i;

    for (i = 0; i < img->name_count; i++) {
        if (strcmp(img->names[i].name, name) == 0) {
            return &img->names[i];
        }
    }
    return NULL;
}

int bw_data_relocate(const BwDataImage *img)
{
    u32 i;

    if (g_flat_img != img) {
        return fail(BW_DATA_E_LAYOUT, 0, "bw_data_relocate before bw_data_load");
    }
    /* The words this pass rewrites are the ones a later resolve knows
     * about, so the list starts empty.  Only the ARM9's own image has
     * them: an overlay's words are the overlay layer's (sdk/overlay_image.c),
     * and an overlay loading must not take the ARM9's list away. */
    if (img->overlay < 0) {
        g_code_word_count = 0;
    }
    g_done_count = 0;
    /* First pass: every word must hold what the metadata says. */
    for (i = 0; i < img->reloc_count; i++) {
        const BwDataReloc *r = &img->relocs[i];
        u8 *w = flat_word(img, r->where);

        if (w == NULL) {
            return fail(BW_DATA_E_LAYOUT, r->where,
                        "pointer word outside every loaded range");
        }
        if (bw_rd32(w) != r->target) {
            snprintf(g_error, sizeof(g_error),
                     "word 0x%08lX holds 0x%08lX, the metadata says 0x%08lX: "
                     "arm9_dec.bin is not the image aletheia describes",
                     (unsigned long)r->where, (unsigned long)bw_rd32(w),
                     (unsigned long)r->target);
            return BW_DATA_E_MISMATCH;
        }
    }
    if (!bw_data_relocation_supported()) {
        snprintf(g_error, sizeof(g_error),
                 "%lu pointer words left as DS addresses: a host pointer does "
                 "not fit the DS's 32-bit word on this target (LP64); the "
                 "boot needs an ILP32 build",
                 (unsigned long)img->reloc_count);
        return img->reloc_count != 0 ? BW_DATA_E_LP64 : BW_DATA_OK;
    }
    /* Second pass: resolve and write. */
    for (i = 0; i < img->reloc_count; i++) {
        const BwDataReloc *r = &img->relocs[i];
        u8 *w = flat_word(img, r->where);
        uintptr_t v;
        u32 word;
        int is_code = 0;

        switch (r->kind) {
        case BW_DATA_RELOC_CODE:
            if (r->index >= img->func_count) {
                return fail(BW_DATA_E_LAYOUT, r->where, "function index out of range");
            }
            /* The registry first: a mod's hook for the function wins over
             * the build's own function (bw_arm9_code_entry). */
            v = (uintptr_t)arm9_code_of(img, &img->funcs[r->index]);
            is_code = 1;
            break;
        case BW_DATA_RELOC_HOST: {
            const BwDataAnchor *a;
            if (r->index >= img->anchor_count) {
                return fail(BW_DATA_E_LAYOUT, r->where, "anchor index out of range");
            }
            a = &img->anchors[r->index];
            v = (uintptr_t)a->host + (r->target - a->addr);
            break;
        }
        default: {
            void *p = bw_ds_try_ptr(r->target);
            if (p == NULL) {
                return fail(BW_DATA_E_UNBACKED, r->target,
                            "pointer target outside every backed DS region");
            }
            v = (uintptr_t)p;
            break;
        }
        }
        word = (u32)v;
        memcpy(w, &word, 4);
        done_add(r->where);
        if (is_code && img->overlay < 0) {
            /* The ARM9's own words are the ones the resolve asks about
             * again (bw_arm9_resolve_code_pointers); an overlay's own words
             * are the overlay layer's business (sdk/overlay_image.c). */
            remember_code_word(w, word, &img->funcs[r->index]);
        }
    }
    g_scanned = scan_code_pointers(img);
    g_scanned_data = scan_data_pointers(img);
    swap_to_host(img);
    return BW_DATA_OK;
}

u32 bw_data_scanned(void)
{
    return g_scanned;
}

u32 bw_data_scanned_data(void)
{
    return g_scanned_data;
}
void bw_data_trap(u32 addr)
{
    bw_stub_reached("armdata", addr,
                    "called through a pointer in the ARM9 data to code the build does "
                    "not define (tools/armdata.py lists it as a trap)");
}

/*
 * Put the hooks in the ARM9's stored code pointers.
 *
 * The words are written when the data is relocated, which is at the reset,
 * and the mod layer loads after the first reset, so the registry was empty
 * when the words were written and they hold the build's own functions.  This
 * asks the registry again for every word the port rewrote and puts the
 * hook there instead.
 *
 * A word the game has written over since is left alone: the port wrote a
 * host address there, and a word that holds something else is the game's
 * now.  Returns how many words it changed.
 */
u32 bw_arm9_resolve_code_pointers(void)
{
    u32 n = 0;
    u32 i;

    for (i = 0; i < g_code_word_count; i++) {
        BwDataCodeWord *cw = &g_code_words[i];
        BwDataFn fn;
        u32 now;
        u32 word;

        if (cw->word == NULL || cw->func == NULL) {
            continue;
        }
        memcpy(&now, cw->word, 4);
        if (now != cw->host) {
            continue;              /* the game wrote its own value there */
        }
        fn = arm9_code_of(g_installed, cw->func);
        if (fn == NULL || (u32)(uintptr_t)fn == cw->host) {
            continue;              /* nothing changed */
        }
        word = (u32)(uintptr_t)fn;
        memcpy(cw->word, &word, 4);
        cw->host = word;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* The reset hooks                                                     */
/* ------------------------------------------------------------------ */

void bw_data_reset_load(void)
{
    int rc;

    if (g_installed == NULL) {
        return;
    }
    rc = bw_data_load(g_installed);
    if (rc != BW_DATA_OK) {
        bw_panic("bw_data_reset_load", 0, bw_data_error());
    }
}

void bw_data_reset_bind(void)
{
    int rc;

    if (g_installed == NULL) {
        return;
    }
    rc = bw_data_bind(g_installed);
    if (rc != BW_DATA_OK) {
        bw_panic("bw_data_reset_bind", 0, bw_data_error());
    }
    rc = bw_data_relocate(g_installed);
    if (rc == BW_DATA_E_LP64 && (g_flags & BW_DATA_ALLOW_UNRELOCATED)) {
        return;
    }
    if (rc != BW_DATA_OK) {
        bw_panic("bw_data_reset_bind", 0, bw_data_error());
    }
}
