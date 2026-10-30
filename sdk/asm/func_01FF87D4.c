/*
 * sdk/asm/func_01FF87D4.c -- SCFGi_SwitchCpuSpeed at 0x01FF87D4 (ITCM).
 *
 * Replaces src/matched/itcm_sdk/func_01FF87D4.c (TwlSDK scfg_proc.o, an asm
 * function):
 *
 *     ldr  r2, =0x04004004            REG_CLK (SCFG clock control, TWL)
 *     ldrh r1, [r2]
 *     bic  r1, r1, #1                 clear the CPU speed bit
 *     orr  r1, r0, r1                 or in cpuSpeed (the whole argument)
 *     strh r1, [r2]                   store the low 16 bits
 *     cmp  r0, #0; moveq r0, #10; movne r0, #20
 *   wait:
 *     subs r0, r0, #4; bcs wait       settle delay
 *     bx   lr
 *
 * void SCFGi_SwitchCpuSpeed(SCFGCpuSpeed cpuSpeed), the enum 0 (1x) or 1
 * (2x).  The register write is memory traffic and goes through the address
 * space (0x04004004 lies in the I/O window); the delay loop only lets the
 * clock settle and is dropped.  The caller (func_0208C988.c) uses the SDK
 * name with an int argument; the matched spelling forwards.
 */

#include "asmsdk.h"
#include "../dsmem.h"

void SCFGi_SwitchCpuSpeed(int cpuSpeed)
{
    volatile u16 *clk = (volatile u16 *)bw_ds_ptr_w(0x04004004u, sizeof(*(volatile u16 *)0));
    u32 v = (u32)*clk;

    v &= ~1u;
    v |= (u32)cpuSpeed;
    *clk = (u16)v;
}

void func_01FF87D4(int cpuSpeed)
{
    SCFGi_SwitchCpuSpeed(cpuSpeed);
}
