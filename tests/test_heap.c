/*
 * tests/test_heap.c -- the game's heap, from the manager down to the arena.
 *
 * No game data and no oracle.  Every expected number below is worked out on
 * paper from the rules the matched sources spell out, and the file names
 * the source each rule comes from:
 *
 *   sub_02030098.c   the raw allocator: bit 15 of the id picks the tail, the
 *                    0x1c-byte header (heap id, then the 0x194E signature)
 *                    in front of the payload, result codes 1 (id out of
 *                    range), 2 (no heap behind the id), 3 (exhausted)
 *   func_02030164.c  the raw free: the signature becomes 0x194D
 *   sub_0202FC48.c   the constructor: 0x18 spare entries past the caller's
 *                    heaps, 16 bytes per entry, the id table clamped up to
 *                    the entry count and filled with 0xff, both tables and
 *                    then each heap's region taken off arena 0 in order
 *   func_02060C5C.c  an expanded heap over a region: both ends rounded to
 *                    four, and at least 0x4c bytes or no heap
 *   func_02060814.c  the heap head is 0x38 bytes, signature 'EXPH', and
 *                    one free block covers the rest
 *   func_020607E8.c  a block header is 0x10 bytes: signature, attribute,
 *                    size of what follows the header, prev, next
 *   func_02060888.c  the carve: a remainder under 0x14 bytes is not split
 *                    off; bit 15 of the attribute is the tail flag
 *   func_02060A08.c  first fit from the head of the free list (the heap's
 *   func_02060ABC.c  feature bit 0 is clear), or from its tail, placed at
 *                    the very end of the block
 *   func_02060B6C.c  a freed region merges with a free neighbour on either
 *                    side
 *   sub_0202FDC4.c   a child heap: the region from the parent through the
 *   func_0202FED8.c  expanded heap's own front door, bound to the first
 *                    unused spare entry; result codes 1 (id taken), 2 (no
 *                    parent), 3 (parent exhausted), 4 (no spare entry)
 *   func_020606D0.c  a top-level heap goes on the root list, a heap inside
 *                    another's region on that heap's child list
 *
 * Two halves.  The first runs in every configuration and checks what the
 * manager guarantees whatever is under it: ids, result codes, the live
 * counter, payload alignment, the clear flag, and that fifty interleaved
 * blocks never overlap and all come back.  The second needs
 * EIDOS_HEAP_MATCHED, the 32-bit reference configuration, where the arena
 * and the expanded heap are the game's own: it checks addresses, header
 * words and free-list shapes byte for byte.  On LP64 it says it is skipped.
 */

#include <stdio.h>
#include <string.h>

#include "port.h"
#include "port_test.h"

extern void *sub_02030098(u32 heapId, u32 size);
extern void *GFL_HEAP_Alloc(u32 heapId, u32 size, int clear, const char *site,
                          u16 tag);
extern void  GFL_HEAP_Free(void *p);

#define HDR 0x1cu   /* the manager's block header, sub_02030098 */

/* sub_02030098 hands out the payload, 0x1c bytes past the block the
 * expanded heap carved.  The arena checks below are about the block, so
 * they go through these two. */
static u8 *raw(u32 heap_id, u32 size)
{
    u8 *p = (u8 *)sub_02030098(heap_id, size);
    return p != NULL ? p - HDR : NULL;
}

static void rel(u8 *block)
{
    GFL_HEAP_Free(block + HDR);
}

/* ------------------------------------------------------------------ */
/* Both configurations                                                 */
/* ------------------------------------------------------------------ */

static u32 lcg_state = 12345u;

static u32 lcg(void)
{
    lcg_state = lcg_state * 1103515245u + 12345u;
    return lcg_state >> 8;
}

static int test_manager(void)
{
    static const u32 sizes[3] = { 0x1000, 0x2000, 0x800 };
    void *p;
    u32 i;

    BW_CHECK_EQ("init three heaps", bw_heap_init_sizes(sizes, 3, 0x10), 0);
    BW_CHECK_EQ("init leaves result 0", bw_heap_error(), 0);
    for (i = 0; i < 3; i++) {
        BW_CHECK_EQ("a heap starts empty", bw_heap_live_blocks(i), 0);
        BW_REQUIRE("a heap has a handle", bw_heap_handle(i) != NULL);
    }
    BW_CHECK_EQ("id 5 is valid but names no heap", bw_heap_live_blocks(5), -1);
    BW_CHECK_EQ("an id past the table", bw_heap_live_blocks(0x7000), -1);
    BW_REQUIRE("no handle behind id 5", bw_heap_handle(5) == NULL);

    /* The raw allocator's three failure codes. */
    BW_REQUIRE("id past the table fails", sub_02030098(0x7000, 16) == NULL);
    BW_CHECK_EQ("... with result 1", bw_heap_error(), 1);
    BW_REQUIRE("id with no heap fails", sub_02030098(5, 16) == NULL);
    BW_CHECK_EQ("... with result 2", bw_heap_error(), 2);

    /* One block through the entry point, cleared. */
    p = GFL_HEAP_Alloc(0, 100, 1, "test_heap.c", 7);
    BW_REQUIRE("an allocation succeeds", p != NULL);
    BW_CHECK_EQ("result 0 after it", bw_heap_error(), 0);
    BW_CHECK_EQ("the payload is word aligned", (u32)((uintptr_t)p & 3u), 0);
    BW_CHECK_EQ("the live counter counts it", bw_heap_live_blocks(0), 1);
    BW_CHECK_EQ("other heaps are untouched", bw_heap_live_blocks(1), 0);
    BW_CHECK_EQ("the header's heap id", bw_mem_rd16((u8 *)p - HDR), 0);
    BW_CHECK_EQ("the header's signature", bw_mem_rd16((u8 *)p - HDR + 2), 0x194E);
    {
        u32 nz = 0;
        for (i = 0; i < 100; i++) {
            nz += ((u8 *)p)[i] != 0;
        }
        BW_CHECK_EQ("clear = 1 zeroes the payload", nz, 0);
    }
    GFL_HEAP_Free(p);
    BW_CHECK_EQ("the free is counted", bw_heap_live_blocks(0), 0);

    /* The heap id in the header is the id without its tail bit. */
    p = raw(0x8001, 40);
    BW_REQUIRE("a tail allocation succeeds", p != NULL);
    BW_CHECK_EQ("the header drops bit 15", bw_mem_rd16((u8 *)p), 1);
    BW_CHECK_EQ("counted on heap 1", bw_heap_live_blocks(1), 1);
    rel((u8 *)p);
    BW_CHECK_EQ("and released", bw_heap_live_blocks(1), 0);

    /* Fifty blocks, random sizes, both ends, three heaps, each filled with a
     * pattern of its own; then every pattern is checked (no two blocks
     * overlap) and they are freed in a scrambled order. */
    {
        void *blk[50];
        u32   len[50];
        u32   heap[50];
        u32   bad = 0;
        u32   k;

        for (i = 0; i < 50; i++) {
            heap[i] = i % 3u;
            len[i] = 4u + lcg() % 60u;
            blk[i] = GFL_HEAP_Alloc(heap[i] | ((i & 1u) ? 0x8000u : 0u), len[i],
                                  0, "test_heap.c", (u16)i);
            BW_REQUIRE("a small block fits", blk[i] != NULL);
            memset(blk[i], (int)(0x40 + i), len[i]);
        }
        BW_CHECK_EQ("heap 0 counts 17", bw_heap_live_blocks(0), 17);
        BW_CHECK_EQ("heap 1 counts 17", bw_heap_live_blocks(1), 17);
        BW_CHECK_EQ("heap 2 counts 16", bw_heap_live_blocks(2), 16);
        for (i = 0; i < 50; i++) {
            for (k = 0; k < len[i]; k++) {
                bad += ((u8 *)blk[i])[k] != (u8)(0x40 + i);
            }
        }
        BW_CHECK_EQ("no block overwrote another", bad, 0);
        for (i = 0; i < 50; i++) {
            k = (i * 17u) % 50u;   /* 17 is prime to 50: a permutation */
            GFL_HEAP_Free(blk[k]);
        }
        for (i = 0; i < 3; i++) {
            BW_CHECK_EQ("every heap balances", bw_heap_live_blocks(i), 0);
        }
    }

    /* Starting over is a fresh manager. */
    BW_CHECK_EQ("re-init", bw_heap_init(4), 0);
    BW_CHECK_EQ("a fourth heap now exists", bw_heap_live_blocks(3), 0);
    p = sub_02030098(3, 8);
    BW_REQUIRE("and allocates", p != NULL);
    GFL_HEAP_Free(p);
    BW_CHECK_EQ("and balances", bw_heap_live_blocks(3), 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* The matched arena (ILP32)                                           */
/* ------------------------------------------------------------------ */

#if EIDOS_HEAP_MATCHED

extern int NNSi_FndGetListTotalSize(int *heap);
extern s32 NNSi_FndGetListAllocatableSize(void *heap, s32 align);
extern int NNSi_FndGetMBlockSize(char *p);

struct FndList {
    void *head;
    void *tail;
    u16   count;
    u16   offset;
};
extern struct FndList sRootList;

/* Word and halfword reads at an address inside the arena.  Everything here
 * is the host's own memory, written by the matched code in host byte
 * order, so a plain load is the right read (unlike bw_rd32, which is for
 * the little-endian bytes of a file). */
static u32 w32(const void *p, u32 off)
{
    u32 v;
    memcpy(&v, (const u8 *)p + off, 4);
    return v;
}

static u16 w16(const void *p, u32 off)
{
    u16 v;
    memcpy(&v, (const u8 *)p + off, 2);
    return v;
}

#define A(p) ((u32)(uintptr_t)(p))

/* The expanded heap's head, func_02060814.c / func_020606D0.c. */
#define EXPH_START  0x18u   /* heapStart */
#define EXPH_END    0x1cu   /* heapEnd */
#define EXPH_FREE   0x24u   /* free list head, tail */
#define EXPH_USED   0x2cu   /* used list head, tail */
#define EXPH_HEAD   0x38u
#define CHILDREN    0x0cu   /* the common head's child list */

static u32 free_blocks(const void *heap)
{
    u32 n = 0;
    u32 b = w32(heap, EXPH_FREE);

    while (b != 0 && n < 1000) {
        n++;
        b = w32((void *)(uintptr_t)b, 0xc);
    }
    return n;
}

static u32 used_blocks(const void *heap)
{
    u32 n = 0;
    u32 b = w32(heap, EXPH_USED);

    while (b != 0 && n < 1000) {
        n++;
        b = w32((void *)(uintptr_t)b, 0xc);
    }
    return n;
}

static int test_layout(void)
{
    static const u32 sizes[3] = { 0x1000, 0x2000, 0x800 };
    u8 *base;
    u8 *h0;
    u8 *h1;
    u8 *h2;
    u32 id_len;

    BW_CHECK_EQ("init", bw_heap_init_sizes(sizes, 3, 0x10), 0);

    /* 3 heaps + 0x18 spares = 27 entries of 16 bytes; the id table is
     * clamped from 0x10 up to 27 and takes 28 bytes after rounding. */
    id_len = 28;
    BW_CHECK_EQ("the arena is used to the byte",
                bw_arena_used(), 27u * 16u + id_len + 0x1000u + 0x2000u + 0x800u);
    BW_CHECK_EQ("... and that is all of it", bw_arena_used(), bw_arena_size());

    h0 = (u8 *)bw_heap_handle(0);
    h1 = (u8 *)bw_heap_handle(1);
    h2 = (u8 *)bw_heap_handle(2);
    base = h0 - (27u * 16u + id_len);
    BW_REQUIRE("heap 0 is in the arena", bw_arena_contains(h0));
    BW_CHECK_EQ("heap 1 follows heap 0", A(h1) - A(h0), 0x1000);
    BW_CHECK_EQ("heap 2 follows heap 1", A(h2) - A(h1), 0x2000);
    BW_CHECK_EQ("the id table follows the entries", A(base) + 27u * 16u,
                A(h0) - id_len);

    BW_CHECK_EQ("heap head signature 'EXPH'", w32(h0, 0), 0x45585048u);
    BW_CHECK_EQ("heapStart is past the 0x38-byte head", w32(h0, EXPH_START),
                A(h0) + EXPH_HEAD);
    BW_CHECK_EQ("heapEnd is the region's end", w32(h0, EXPH_END), A(h0) + 0x1000u);
    BW_CHECK_EQ("one free block", free_blocks(h0), 1);
    BW_CHECK_EQ("no used block", used_blocks(h0), 0);
    BW_CHECK_EQ("the free block sits at heapStart", w32(h0, EXPH_FREE),
                A(h0) + EXPH_HEAD);
    BW_CHECK_EQ("free block signature 'FR'", w16(h0 + EXPH_HEAD, 0), 0x4652);
    BW_CHECK_EQ("free total = region - head - one header",
                NNSi_FndGetListTotalSize((int *)h0), 0x1000 - 0x38 - 0x10);
    BW_CHECK_EQ("largest allocatable, same number",
                NNSi_FndGetListAllocatableSize(h2, 4), 0x800 - 0x38 - 0x10);
    BW_CHECK_EQ("three heaps on the root list", sRootList.count, 3);
    BW_CHECK_EQ("the root list's first heap", A(sRootList.head), A(h0));
    BW_CHECK_EQ("the root list's last heap", A(sRootList.tail), A(h2));
    return 0;
}

static int test_carve(void)
{
    u8 *h0;
    u8 *p;
    u8 *q;
    u8 *blk;
    u32 start;
    u32 end;

    static const u32 sizes[1] = { 0x1000 };
    BW_CHECK_EQ("init one heap", bw_heap_init_sizes(sizes, 1, 1), 0);
    h0 = (u8 *)bw_heap_handle(0);
    start = A(h0) + EXPH_HEAD;       /* the first block header */
    end = A(h0) + 0x1000u;

    /* From the head: 100 + 0x1c = 0x80 bytes, right after the first block
     * header; the 0xF38 left behind it becomes a new free block. */
    p = raw(0, 100);
    BW_REQUIRE("head allocation", p != NULL);
    BW_CHECK_EQ("... sits right after the first header", A(p), start + 0x10u);
    BW_CHECK_EQ("used block signature 'UD'", w16(p - 0x10, 0), 0x5544);
    BW_CHECK_EQ("... head flag clear", w16(p - 0x10, 2) & 0x8000u, 0);
    BW_CHECK_EQ("... its size", NNSi_FndGetMBlockSize((char *)p), 0x80);
    BW_CHECK_EQ("the rest is one free block", free_blocks(h0), 1);
    BW_CHECK_EQ("... after the new block", w32(h0, EXPH_FREE), start + 0x10u + 0x80u);
    BW_CHECK_EQ("free total", NNSi_FndGetListTotalSize((int *)h0), 0xFB8 - 0x80 - 0x10);

    /* From the tail: placed at the very end of the free block. */
    q = raw(0x8000, 100);
    BW_REQUIRE("tail allocation", q != NULL);
    BW_CHECK_EQ("... ends at heapEnd", A(q) + 0x80u, end);
    BW_CHECK_EQ("... tail flag set", w16(q - 0x10, 2) & 0x8000u, 0x8000);
    BW_CHECK_EQ("free total", NNSi_FndGetListTotalSize((int *)h0), 0xF28 - 0x80 - 0x10);
    BW_CHECK_EQ("two used blocks", used_blocks(h0), 2);

    /* Free the head block: it merges with the free block behind it. */
    rel(p);
    BW_CHECK_EQ("the freed header is marked 0x194D", w16(p, 2), 0x194D);
    BW_CHECK_EQ("still one free block (merged)", free_blocks(h0), 1);
    BW_CHECK_EQ("... starting at heapStart again", w32(h0, EXPH_FREE), start);
    /* The freed block's 0x80 and its header come back, and so does the
     * header of the free block it merged with, which is now payload. */
    BW_CHECK_EQ("free total", NNSi_FndGetListTotalSize((int *)h0),
                0xE88 + 0x80 + 0x10 + 0x10);

    /* Free the tail block: merges forward too; the heap is new again. */
    rel(q);
    BW_CHECK_EQ("one free block", free_blocks(h0), 1);
    BW_CHECK_EQ("no used block", used_blocks(h0), 0);
    BW_CHECK_EQ("the heap is whole again", NNSi_FndGetListTotalSize((int *)h0), 0xFB8);

    /* First fit, and the 0x14-byte remainder rule.  A, B, C from the head,
     * each 0x40 bytes with the manager header; free B, leaving a 0x40-byte
     * hole between two used blocks. */
    {
        u8 *a = raw(0, 0x40 - HDR);
        u8 *b = raw(0, 0x40 - HDR);
        u8 *c = raw(0, 0x40 - HDR);
        u8 *d;

        BW_REQUIRE("three blocks", a != NULL && b != NULL && c != NULL);
        BW_CHECK_EQ("B follows A", A(b), A(a) + 0x50u);
        BW_CHECK_EQ("C follows B", A(c), A(b) + 0x50u);
        rel(b);
        BW_CHECK_EQ("the hole is a second free block", free_blocks(h0), 2);

        /* 0x38 bytes into the 0x40 hole: 8 would be left, under 0x14, so
         * the block takes the whole hole and reports 0x40. */
        d = raw(0, 0x38 - HDR);
        BW_CHECK_EQ("first fit takes the hole", A(d), A(b));
        BW_CHECK_EQ("... and all of it", NNSi_FndGetMBlockSize((char *)d), 0x40);
        BW_CHECK_EQ("the hole is gone", free_blocks(h0), 1);
        rel(d);

        /* 0x20 bytes into the same hole: 0x20 left, which is split off. */
        d = raw(0, 0x20 - HDR);
        BW_CHECK_EQ("first fit again", A(d), A(b));
        BW_CHECK_EQ("... exactly its size", NNSi_FndGetMBlockSize((char *)d), 0x20);
        BW_CHECK_EQ("the split-off rest is free", free_blocks(h0), 2);
        rel(d);

        /* Too big for the hole: goes past C. */
        d = raw(0, 0x60 - HDR);
        BW_CHECK_EQ("a bigger block skips the hole", A(d), A(c) + 0x50u);
        rel(d);
        rel(a);
        rel(c);
        BW_CHECK_EQ("all merged back", free_blocks(h0), 1);
        BW_CHECK_EQ("the heap is whole", NNSi_FndGetListTotalSize((int *)h0), 0xFB8);
    }

    /* Exhaustion, and the exact fit. */
    blk = raw(0, 0xFB8 - HDR + 4);
    BW_REQUIRE("four bytes too many fails", blk == NULL);
    BW_CHECK_EQ("... with result 3", bw_heap_error(), 3);
    BW_CHECK_EQ("... and is not counted", bw_heap_live_blocks(0), 0);
    blk = raw(0, 0xFB8 - HDR);
    BW_REQUIRE("the exact fit succeeds", blk != NULL);
    BW_CHECK_EQ("no free block is left", free_blocks(h0), 0);
    BW_CHECK_EQ("free total 0", NNSi_FndGetListTotalSize((int *)h0), 0);
    BW_REQUIRE("a word more fails", raw(0, 4) == NULL);
    BW_CHECK_EQ("... with result 3", bw_heap_error(), 3);
    rel(blk);
    BW_CHECK_EQ("and it all comes back", NNSi_FndGetListTotalSize((int *)h0), 0xFB8);

    /* The constructor's own failure: a heap under 0x4c bytes is refused by
     * func_02060C5C, which the constructor does not check; the handle it
     * stores is then null and the id reads as "no heap". */
    {
        static const u32 tiny[2] = { 0x1000, 0x40 };
        BW_CHECK_EQ("init with a too-small heap", bw_heap_init_sizes(tiny, 2, 2), 0);
        BW_REQUIRE("the big one exists", bw_heap_handle(0) != NULL);
        BW_REQUIRE("the tiny one has no heap", bw_heap_handle(1) == NULL);
        BW_REQUIRE("allocating from it fails", raw(1, 4) == NULL);
        BW_CHECK_EQ("... with result 2", bw_heap_error(), 2);
    }
    return 0;
}

static int test_children(void)
{
    static const u32 sizes[2] = { 0x1000, 0x2000 };
    u8 *h1;
    u8 *c7;
    u8 *c8;
    u8 *p;
    u32 i;

    BW_CHECK_EQ("init", bw_heap_init_sizes(sizes, 2, 0x10), 0);
    h1 = (u8 *)bw_heap_handle(1);

    /* The game's own shape: func_0203064C(1, 7, 0x25000) at boot. */
    BW_CHECK_EQ("child 7 from heap 1", bw_heap_create_child(1, 7, 0x400), 0);
    c7 = (u8 *)bw_heap_handle(7);
    BW_REQUIRE("child 7 exists", c7 != NULL);
    BW_CHECK_EQ("... at the head of heap 1's free space",
                A(c7), A(h1) + EXPH_HEAD + 0x10u);
    BW_CHECK_EQ("... its own heap head", w32(c7, 0), 0x45585048u);
    BW_CHECK_EQ("... with 0x400 - head - one header free",
                NNSi_FndGetListTotalSize((int *)c7), 0x400 - 0x38 - 0x10);
    BW_CHECK_EQ("the parent is not counted as allocating",
                bw_heap_live_blocks(1), 0);
    BW_CHECK_EQ("the parent lost 0x400 + a header",
                NNSi_FndGetListTotalSize((int *)h1), 0x2000 - 0x38 - 0x10 - 0x400 - 0x10);
    BW_CHECK_EQ("the child is on the parent's child list",
                w16(h1, CHILDREN + 8), 1);
    BW_CHECK_EQ("not on the root list", sRootList.count, 2);

    p = raw(7, 16);
    BW_REQUIRE("the child allocates", p != NULL);
    BW_REQUIRE("... inside its region",
               A(p) >= A(c7) && A(p) + 16u + HDR <= A(c7) + 0x400u);
    BW_CHECK_EQ("... counted on the child", bw_heap_live_blocks(7), 1);
    rel(p);
    BW_CHECK_EQ("... and released", bw_heap_live_blocks(7), 0);

    /* From the parent's tail. */
    BW_CHECK_EQ("child 8 from heap 1's tail",
                bw_heap_create_child(1, 0x8008, 0x200), 0);
    c8 = (u8 *)bw_heap_handle(8);
    BW_CHECK_EQ("... ends at heap 1's end", A(c8) + 0x200u, A(h1) + 0x2000u);

    /* The four failure codes. */
    BW_CHECK_EQ("id 7 again", bw_heap_create_child(1, 7, 0x100), -1);
    BW_CHECK_EQ("... result 1, taken", bw_heap_error(), 1);
    BW_CHECK_EQ("a parent that is not a heap", bw_heap_create_child(5, 9, 0x100), -1);
    BW_CHECK_EQ("... result 2", bw_heap_error(), 2);
    BW_CHECK_EQ("bigger than the parent", bw_heap_create_child(0, 9, 0x2000), -1);
    BW_CHECK_EQ("... result 3", bw_heap_error(), 3);

    /* 0x18 spare entries, and the twenty-fifth child finds none. */
    {
        static const u32 one[1] = { 0x4000 };
        BW_CHECK_EQ("init one heap, 0x40 ids", bw_heap_init_sizes(one, 1, 0x40), 0);
        for (i = 1; i <= 0x18; i++) {
            BW_CHECK_EQ("a spare child", bw_heap_create_child(0, i, 0x60), 0);
        }
        BW_CHECK_EQ("the next finds no spare",
                    bw_heap_create_child(0, 0x19, 0x60), -1);
        BW_CHECK_EQ("... result 4", bw_heap_error(), 4);
        BW_CHECK_EQ("all 0x18 on heap 0's child list",
                    w16(bw_heap_handle(0), CHILDREN + 8), 0x18);
    }
    return 0;
}

#endif /* EIDOS_HEAP_MATCHED */

int main(void)
{
    /* A BW_REQUIRE that fails ends its own section, whose state the next
     * one would otherwise build on, so the first one to stop ends the run. */
    if (test_manager() != 0) {
        BW_REPORT("test_heap");
    }
#if EIDOS_HEAP_MATCHED
    printf("heap floor: the matched expanded heap (ILP32)\n");
    if (test_layout() != 0 || test_carve() != 0 || test_children() != 0) {
        BW_REPORT("test_heap");
    }
#else
    printf("heap floor: host blocks (LP64); the arena checks are skipped\n");
#endif
    BW_REPORT("test_heap");
}
