/*
 * sdk/asm/func_02088ED4.c -- RtcWaitBusy at 0x02088ED4.
 *
 * Replaces src/matched/arm9_sdk/func_02088ED4.c (TwlSDK librtc external.o,
 * an asm function):
 *
 *     ldr ip, =g_0215142C
 *   loop:
 *     ldr r0, [ip]; cmp r0, #1; beq loop     spin while the lock word is 1
 *     bx  lr
 *
 * void RtcWaitBusy(void).  0x0215142C is rtcWork.lock: external.o's .bss
 * is g_02151428 (u16 rtcInitialized, then RTCWork rtcWork at +4, whose
 * first field is u32 lock; RTC_LOCK_ON is 1).  Every other matched RTC file
 * reaches the word as g_02151428.rtcWork.lock and nothing else names
 * g_0215142C, so the host reads it through that same struct prefix, which
 * puts it where the matched C writes it whatever the pointer width (RTCWork
 * holds pointers, so on a 64-bit host it sits at +8, not +4).
 *
 * The lock is cleared when the ARM7's PXI reply arrives.  On the host that
 * clear comes from another host thread or from the interrupt/PXI layer
 * delivering the reply, so the loop reads the word through a volatile
 * lvalue on every pass, as the asm reloads it.  The matched callers
 * (func_02088864.c and others) use the SDK name; the address spelling
 * forwards.
 */

#include "asmsdk.h"

typedef struct BwRtcBssHead {
    u16 rtcInitialized;
    struct {
        u32 lock;
        void (*callback)(int result, void *arg);   /* sets RTCWork's alignment */
    } rtcWork;
} BwRtcBssHead;

extern BwRtcBssHead g_02151428;

/* The lock as the matched C stores it (types.h, bw_gs_rd32), byte by byte
 * through a volatile lvalue so every pass reloads it. */
static u32 rtc_lock(void)
{
    const volatile u8 *p = (const volatile u8 *)&g_02151428.rtcWork.lock;
    u8 b[4];

    b[0] = p[0];
    b[1] = p[1];
    b[2] = p[2];
    b[3] = p[3];
    return bw_gs_rd32(b);
}

void RtcWaitBusy(void)
{
    while (rtc_lock() == 1u) {
    }
}

void func_02088ED4(void)
{
    RtcWaitBusy();
}
