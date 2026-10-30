/*
 * sdk/mi_gx.c -- the MI functions that send to the geometry FIFO or read the
 * 3D result registers, onto engine/g3d/hw3d.c.
 *
 * Why these are shims, in two groups.
 *
 * The copy loops, hand-written ARM in the SDK's mi_memory.o and `asm` in
 * aletheia, so there is nothing to compile for a host.  On the DS a store
 * burst into 0x04000400..0x0400043F is a burst into the FIFO; through
 * plain-memory registers it would collapse (engine/g3d/hw3d.c, "How a store
 * reaches the FIFO").  Each one is a plain copy unless an end of it is the
 * 3D window:
 *
 *   MIi_CpuSend32  0x02082AA8  (src, dst, size): size/4 words, all to the
 *                  same dst; the game's own GX senders (func_0206C524,
 *                  func_0206C82C, func_0206DDA4, func_0206DB80) use it
 *   MI_Copy36B     0x02082B60  (src, dst): nine words.  G3_MultTransMtx33
 *                  sends a 3x3 matrix to the FIFO with it, and
 *                  G3X_GetVectorMtx reads VECMTX_RESULT with it
 *   MI_Copy64B     0x02082BA0  (src, dst): sixteen words.  G3X_GetClipMtx
 *                  reads CLIPMTX_RESULT with it
 *
 * The DMA-to-FIFO senders, matched C in aletheia (mi_dma_gxcommand.o), whose
 * transfer is a DMA channel in GX-FIFO start mode chained through the GXFIFO
 * interrupt (MIi_FIFOCallback).  The port has no DMA engine, and the GXFIFO
 * interrupt is never raised (hw3d.c, "Timing"), so the matched chain would
 * never finish.  The shims push the whole list and then run the completion
 * callback, before returning, which is the order the game observes on the
 * DS too (it sets its busy flag before the call and the callback clears it):
 *
 *   MI_SendGXCommandAsync      0x02082760
 *   MI_SendGXCommandAsyncFast  0x02082960
 *   0x027300F0, 0x0272FEF4     the TWL NDMA forms of the two, which
 *                              func_0206DC00 calls by address with the same
 *                              arguments (channel, src, size, callback, arg)
 *
 * A source or destination that is a raw DS address rather than a host
 * pointer (a macro the rewrite does not see, `(const void *)REG_..._ADDR`,
 * or an integer argument) is recognised by hw3d.c's g3d_hw_is_fifo /
 * g3d_hw_result_addr; see hw3d.h.
 */

#include <string.h>

#include "dsmem.h"
#include "port.h"
#include "hw3d.h"

typedef void (*BwMiCallback)(void *arg);

/* A copy of `bytes` from src to dst, either end possibly the 3D window. */
static void copy_words(const void *src, void *dst, u32 bytes)
{
    u32 ra;

    if (g3d_hw_is_fifo(dst)) {
        g3d_hw_claim(dst);
        g3d_hw_fifo_send(src, bytes);
        return;
    }
    ra = g3d_hw_result_addr(src);
    if (ra != 0) {
        u32 i;
        g3d_hw_refresh();
        for (i = 0; i + 4u <= bytes; i += 4u) {
            bw_mem_wr32((u8 *)dst + i, bw_io_read32(ra + i));
        }
        return;
    }
    memmove(dst, src, (size_t)bytes);
}

void MIi_CpuSend32(const void *src, volatile void *dst, u32 size)
{
    const u8 *s = (const u8 *)src;
    u32 i;

    if (g3d_hw_is_fifo(dst)) {
        g3d_hw_claim(dst);
        g3d_hw_fifo_send(src, size & ~3u);
        return;
    }
    /* Every word to the same address: only the last one stays. */
    for (i = 0; i + 4u <= size; i += 4u) {
        *(volatile u32 *)dst = bw_mem_rd32(s + i);
    }
}

void func_02082AA8(const void *src, volatile void *dst, u32 size)
{
    MIi_CpuSend32(src, dst, size);
}

void MI_Copy36B(const void *src, void *dst)
{
    copy_words(src, dst, 36u);
}

void func_02082B60(const void *src, void *dst)
{
    MI_Copy36B(src, dst);
}

void MI_Copy64B(const void *src, void *dst)
{
    copy_words(src, dst, 64u);
}

void func_02082BA0(const void *src, void *dst)
{
    MI_Copy64B(src, dst);
}

static void send_list(const void *src, u32 size, BwMiCallback callback,
                      void *arg)
{
    g3d_hw_fifo_send(src, size & ~3u);
    if (callback != NULL) {
        callback(arg);
    }
}

void MI_SendGXCommandAsync(u32 dma, const void *src, u32 size,
                           BwMiCallback callback, void *arg)
{
    (void)dma;
    send_list(src, size, callback, arg);
}

void func_02082760(u32 dma, const void *src, u32 size,
                   BwMiCallback callback, void *arg)
{
    MI_SendGXCommandAsync(dma, src, size, callback, arg);
}

void MI_SendGXCommandAsyncFast(u32 dma, const void *src, u32 size,
                               BwMiCallback callback, void *arg)
{
    (void)dma;
    send_list(src, size, callback, arg);
}

void func_02082960(u32 dma, const void *src, u32 size,
                   BwMiCallback callback, void *arg)
{
    MI_SendGXCommandAsyncFast(dma, src, size, callback, arg);
}

void func_027300F0(u32 ndma, const void *src, u32 size,
                   BwMiCallback callback, void *arg)
{
    (void)ndma;
    send_list(src, size, callback, arg);
}

void func_0272FEF4(u32 ndma, const void *src, u32 size,
                   BwMiCallback callback, void *arg)
{
    (void)ndma;
    send_list(src, size, callback, arg);
}
