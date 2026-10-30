/*
 * sdk/asm/MIi_CpuClear32.c -- MIi_CpuClear32 at 0x02082A7C.
 *
 * Replaces src/matched/arm9_sdk/MIi_CpuClear32.c (an asm function).
 * void MIi_CpuClear32(u32 data, void *destp, u32 size):
 *
 *       add   ip, r1, r2      ; end = dest + size
 *   1:  cmp   r1, ip          ; signed
 *       stmltia r1!, {r0}
 *       blt   1b
 *
 * One word per step while the pointer is below the end, so a size that is
 * not a multiple of 4 is rounded *up* (size 5 stores 8 bytes).  The compare
 * is signed on DS addresses (all below 0x08000000), which for any size that
 * fits the DS is the signed compare of the offset with `size` used here: a
 * size with bit 31 set wraps the end below the start and stores nothing.
 * Words are stored little endian, the DS's byte order.
 *
 * sdk/mi.c had a host version of this name that rounded the size down; this
 * file replaces it.
 */

#include "asmsdk.h"

void MIi_CpuClear32(u32 data, void *destp, u32 size)
{
    u8 *d = (u8 *)destp;
    u32 off;

    for (off = 0; (s32)off < (s32)size; off += 4) {
        bw_mem_wr32(d + off, data);
    }
}

void func_02082A7C(u32 data, void *destp, u32 size)
{
    MIi_CpuClear32(data, destp, size);
}
