/*
 * sdk/arm7/rtc.c -- the ARM7's real-time clock, PXI tag 5, answered from
 * the host clock.
 *
 * Why a stand-in: sdk/arm7/arm7.h.  The protocol, from the matched ARM9
 * side of librtc's external.o (aletheia src/matched/arm9_sdk/):
 *
 * Request: one word, `command << 8` (RtcSendPxiCommand, func_02088F2C.c).
 * The commands the ARM9 sends: RTCi_ReadRawDateTimeAsync 0x10,
 * RTCi_ReadRawDateAsync 0x11, RTCi_ReadRawTimeAsync 0x12,
 * RTCi_WriteRawStatus2Async 0x27; the rest of the SDK's table (reset 0x00,
 * hour format 0x01, reads 0x13-0x19, writes 0x20-0x29) is answered too.
 *
 * The ARM7 does the chip transfer and leaves the raw registers in the
 * shared block at 0x02FFFDE8, then replies `command << 8 | result`
 * (result 0 = success, 1 = invalid command).  RtcCommonCallback
 * (RtcCommonCallback.c) decodes the block by the sequence it started:
 *
 *   0x02FFFDE8  date: BCD year bits 0-7, month 8-12, day 16-21, weekday
 *               24-26 (also status 1 as a halfword)
 *   0x02FFFDEA  status 2: interrupt mode bits 0-3, INT2 enable bit 6
 *   0x02FFFDEC  time: BCD hour bits 0-5, PM flag bit 6, minute 8-14,
 *               second 16-22 (also the alarm registers)
 *
 * The block is ARM9 memory the matched code reads through its RTCRawDate,
 * RTCRawTime, RTCRawStatus2 and RTCRawAlarm bitfield structs, so it is
 * written as those structs store it (types.h, bw_gs_wr32: host order, or
 * the DS order under EIDOS_SSO).  The clock is in 24-hour mode (the
 * SDK sets it so at boot); the chip's PM flag is set for hours 12-23.
 * Writes to the date and time are acknowledged and not applied: the host
 * clock is not the game's to set.  Status and alarm writes are kept and
 * read back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arm7.h"
#include "dsmem.h"
#include "plat_thread.h"
#include "pxi.h"

#define RTC_RAW_BLOCK   0x02FFFDE8u
#define RTC_RAW_STAT2   0x02FFFDEAu
#define RTC_RAW_TIME    0x02FFFDECu

#define RTC_OK          0
#define RTC_INVALID     1

static struct {
    int        fixed;
    int        env_read;        /* EIDOS_RTC looked at                     */
    BwArm7Time fixed_time;
    u16        status1;
    u16        status2;
    u32        alarm[2];
    u32        pulse;
    u32        adjust;
    u32        free_reg;
} g_rtc;

void bw_arm7_rtc_fix_time(const BwArm7Time *t)
{
    if (t) {
        g_rtc.fixed = 1;
        g_rtc.fixed_time = *t;
    } else {
        g_rtc.fixed = 0;
    }
}

static u32 bcd(int v)
{
    return (u32)(((v / 10) % 10) << 4 | (v % 10));
}

static void now(BwArm7Time *out)
{
    time_t t;
    struct tm ltm, *lt;

    if (g_rtc.fixed) {
        *out = g_rtc.fixed_time;
        return;
    }
    if (!g_rtc.env_read) {
        const char *e = getenv("EIDOS_RTC");
        BwArm7Time f;

        g_rtc.env_read = 1;
        memset(&f, 0, sizeof f);
        if (e != NULL && sscanf(e, "%d-%d-%d %d:%d:%d", &f.year, &f.month, &f.day,
                                &f.hour, &f.minute, &f.second) == 6) {
            struct tm d;

            memset(&d, 0, sizeof d);
            d.tm_year = f.year - 1900;
            d.tm_mon = f.month - 1;
            d.tm_mday = f.day;
            d.tm_hour = 12;
            mktime(&d);                 /* the weekday */
            f.week = d.tm_wday;
            bw_arm7_rtc_fix_time(&f);
            *out = g_rtc.fixed_time;
            return;
        }
    }
    t = time(NULL);
    lt = bw_localtime((long long)t, &ltm) == 0 ? &ltm : NULL;
    if (!lt) {
        memset(out, 0, sizeof *out);
        out->year = 2000;
        out->month = 1;
        out->day = 1;
        out->week = 6;                  /* 2000-01-01 was a Saturday        */
        return;
    }
    out->year = lt->tm_year + 1900;
    out->month = lt->tm_mon + 1;
    out->day = lt->tm_mday;
    out->week = lt->tm_wday;
    out->hour = lt->tm_hour;
    out->minute = lt->tm_min;
    out->second = lt->tm_sec > 59 ? 59 : lt->tm_sec;
}

static void put32(u32 addr, u32 v)
{
    bw_gs_wr32((u8 *)bw_ds_ptr(addr), v);
}

static u32 get32(u32 addr)
{
    return bw_gs_rd32((const u8 *)bw_ds_ptr(addr));
}

static void put16(u32 addr, u16 v)
{
    bw_gs_wr16((u8 *)bw_ds_ptr(addr), v);
}

static u16 get16(u32 addr)
{
    return bw_gs_rd16((const u8 *)bw_ds_ptr(addr));
}

static u32 raw_date(const BwArm7Time *t)
{
    int year = t->year - 2000;

    if (year < 0)
        year = 0;
    if (year > 99)
        year = 99;
    return bcd(year) | (bcd(t->month) << 8) | (bcd(t->day) << 16) |
           ((u32)(t->week & 7) << 24);
}

static u32 raw_time(const BwArm7Time *t)
{
    return bcd(t->hour) | (t->hour >= 12 ? 0x40u : 0u) |
           (bcd(t->minute) << 8) | (bcd(t->second) << 16);
}

static int run(u32 command)
{
    BwArm7Time t;

    switch (command) {
    case 0x00:                          /* reset                           */
    case 0x01:                          /* set hour format                 */
        return RTC_OK;
    case 0x10:                          /* read date and time              */
        now(&t);
        put32(RTC_RAW_BLOCK, raw_date(&t));
        put32(RTC_RAW_TIME, raw_time(&t));
        return RTC_OK;
    case 0x11:                          /* read date                       */
        now(&t);
        put32(RTC_RAW_BLOCK, raw_date(&t));
        return RTC_OK;
    case 0x12:                          /* read time                       */
        now(&t);
        put32(RTC_RAW_TIME, raw_time(&t));
        return RTC_OK;
    case 0x13: put32(RTC_RAW_BLOCK, g_rtc.pulse); return RTC_OK;
    case 0x14: put32(RTC_RAW_TIME, g_rtc.alarm[0]); return RTC_OK;
    case 0x15: put32(RTC_RAW_TIME, g_rtc.alarm[1]); return RTC_OK;
    case 0x16: put16(RTC_RAW_BLOCK, g_rtc.status1); return RTC_OK;
    case 0x17: put16(RTC_RAW_STAT2, g_rtc.status2); return RTC_OK;
    case 0x18: put32(RTC_RAW_BLOCK, g_rtc.adjust); return RTC_OK;
    case 0x19: put32(RTC_RAW_BLOCK, g_rtc.free_reg); return RTC_OK;
    case 0x20:                          /* write date and time             */
    case 0x21:                          /* write date                      */
    case 0x22:                          /* write time                      */
        return RTC_OK;
    case 0x23: g_rtc.pulse = get32(RTC_RAW_BLOCK); return RTC_OK;
    case 0x24: g_rtc.alarm[0] = get32(RTC_RAW_TIME); return RTC_OK;
    case 0x25: g_rtc.alarm[1] = get32(RTC_RAW_TIME); return RTC_OK;
    case 0x26: g_rtc.status1 = get16(RTC_RAW_BLOCK); return RTC_OK;
    case 0x27: g_rtc.status2 = get16(RTC_RAW_STAT2); return RTC_OK;
    case 0x28: g_rtc.adjust = get32(RTC_RAW_BLOCK); return RTC_OK;
    case 0x29: g_rtc.free_reg = get32(RTC_RAW_BLOCK); return RTC_OK;
    default:
        return RTC_INVALID;
    }
}

static void rtc_callback(int tag, u32 data, int err)
{
    u32 command = (data & 0x7F00u) >> 8;

    (void)tag;
    if (err)
        return;
    (void)bw_pxi_arm7_send(BW_PXI_TAG_RTC, (command << 8) | (u32)run(command),
                           0);
}

void bw_arm7_rtc_init(void)
{
    int fixed = g_rtc.fixed;
    BwArm7Time fixed_time = g_rtc.fixed_time;

    memset(&g_rtc, 0, sizeof g_rtc);
    g_rtc.fixed = fixed;                /* a test's clock survives a boot   */
    g_rtc.fixed_time = fixed_time;
    g_rtc.status1 = 0x0002;             /* 24-hour mode                     */
    bw_pxi_arm7_set_callback(BW_PXI_TAG_RTC, rtc_callback);
}
