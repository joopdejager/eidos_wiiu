/*
 * sdk/asm/MIi_CpuClear16.c -- MIi_CpuClear16 at 0x02082A48.
 *
 * Replaces src/matched/arm9_sdk/MIi_CpuClear16.c (an asm function).
 * void MIi_CpuClear16(u16 data, void *destp, u32 size):
 *
 *       mov   r3, #0
 *   1:  cmp   r3, r2          ; signed
 *       strlth r0, [r1, r3]
 *       addlt r3, r3, #2
 *       blt   1b
 *
 * One halfword per step while the byte offset is below `size`, compared
 * signed: an odd size writes one halfword past it (size 3 stores 4 bytes),
 * and a size with bit 31 set stores nothing.  Halfwords are stored little
 * endian, the DS's byte order, so the bytes are the same on any host.
 */

#include "asmsdk.h"

void MIi_CpuClear16(u16 data, void *destp, u32 size)
{
    u8 *d = (u8 *)destp;
    u32 off;

    if (BW_HOST_BIG_ENDIAN && bw_host_in_oam(d)) {
        for (off = 0; (s32)off < (s32)size; off += 2) {
            bw_wr16(d + off, data);         /* OAM holds the DS's order */
        }
        return;
    }
    for (off = 0; (s32)off < (s32)size; off += 2) {
        bw_mem_wr16(d + off, data);
    }
}
