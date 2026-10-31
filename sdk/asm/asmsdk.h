/*
 * sdk/asm/asmsdk.h -- portable C for the boot closure's `asm` functions.
 *
 * aletheia matches a few dozen SDK and runtime functions as mwcc `asm`
 * bodies: hand-written ARM in the vendor source, or code no C spelling
 * produces (a computed jump into an unrolled kernel, `swpb`, `clz`, a long
 * branch veneer).  A host compiler cannot build those files.  Each one the
 * NitroMain closure reaches is either
 *
 *   - transliterated here, one file per matched source, when its behaviour
 *     is memory and register traffic (docs/boot.md, "Three layers": compile
 *     what only computes or touches memory); or
 *   - left to the OS shims when it depends on the CPU (CPSR, CP15 cache and
 *     wait-for-interrupt, SWI, the thread context), which this directory does
 *     not touch.
 *
 * Every file keeps the symbol the matched callers link against, and the
 * address spelling (func_/sub_) as a forwarder where a caller uses it.
 * Its header comment names the matched source it replaces and summarises
 * the asm, instruction by instruction where an edge case hangs on it.
 *
 * Two lists in CMakeLists.txt build these files.  BW_ASMSDK_SOURCES need
 * nothing beyond the eidos library itself (the address space and each
 * other) and are in it.  BW_ASMSDK_BOOT_SOURCES call matched game code or
 * name game globals, so they link only with the boot closure; their tests
 * supply those callees themselves (tests/test_asmsdk_glue.c).
 */

#ifndef BW_PORT_ASMSDK_H
#define BW_PORT_ASMSDK_H

#include "../types.h"
#include "../dsmem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* MI: CPU copies and fills (mi_memory.o)                              */
/* ------------------------------------------------------------------ */

void MI_CpuCopy8(const void *srcp, void *dstp, u32 size);       /* 0x02082D44 */
void func_02082D44(const void *srcp, void *dstp, u32 size);
void sub_02082D44(const void *srcp, void *dstp, u32 size);
void MI_CpuFill8(void *dstp, u8 data, u32 size);                 /* 0x02082BCC */
void func_02082BCC(void *dstp, u8 data, u32 size);
void MIi_CpuClear16(u16 data, void *destp, u32 size);            /* 0x02082A48 */
void MIi_CpuCopy16(const void *srcp, void *destp, u32 size);     /* 0x02082A60 */
void func_02082A60(const void *srcp, void *destp, u32 size);
void MIi_CpuClear32(u32 data, void *destp, u32 size);            /* 0x02082A7C */
void func_02082A7C(u32 data, void *destp, u32 size);
void MIi_CpuCopy32(const void *srcp, void *destp, u32 size);     /* 0x02082A90 */

/* MI_UncompressLZ8 at 0x020834C0: LZ77 (type 0x10) and the extended form. */
void MI_UncompressLZ8(const void *srcp, void *destp);
void func_020834C0(const void *srcp, void *destp);

/* ------------------------------------------------------------------ */
/* FX                                                                  */
/* ------------------------------------------------------------------ */

typedef struct BwAsmMtxFx22 {
    fx32 _00, _01;
    fx32 _10, _11;
} BwAsmMtxFx22;

void MTX_Rot22_(BwAsmMtxFx22 *dst, fx32 sinVal, fx32 cosVal);   /* 0x0207AD64 */

/* ------------------------------------------------------------------ */
/* mwccarm runtime division (MSL runtime.a)                            */
/* ------------------------------------------------------------------ */

/*
 * The ARM routines return the quotient in r0 and the remainder in r1.  A C
 * function returns one value, so the SDK-named entries return the quotient
 * (all the matched callers use) and the bw_ forms hand back both.
 */
s32 _s32_div_f(s32 a, s32 b);                                    /* 0x0209C0A4 */
s32 func_0209C0A4(s32 a, s32 b);
u32 _u32_div_f(u32 a, u32 b);                                    /* 0x0209C2B0 */
u32 _div_by_zero_check(u32 a, u32 b);                            /* same entry */
u32 _u32_div_not_0_f(u32 a, u32 b);                              /* 0x0209C2B8 */
s32 bw_rt_s32_divmod(s32 a, s32 b, s32 *rem);
u32 bw_rt_u32_divmod(u32 a, u32 b, u32 *rem);
u32 bw_rt_u32_divmod_not_0(u32 a, u32 b, u32 *rem);
u32 bw_rt_div_kernel(u32 n, u32 d, u32 *rem);      /* the unrolled kernel */

/* ------------------------------------------------------------------ */
/* OS: lock ids, MAC address, coprocessor context                       */
/* ------------------------------------------------------------------ */

s32  OS_GetLockID(void);                                         /* 0x02084EB8 */
s32  func_02084EB8(void);
void OS_ReleaseLockID(u16 lockID);                               /* 0x02084F10 */
void func_02084F10(u16 lockID);
void OS_GetMacAddress(u8 *macAddress);                           /* 0x02087C6C */
void func_02087C6C(u8 *macAddress);
void CP_SaveContext(void *context);                              /* 0x020775B8 */
void CPi_RestoreContext(const void *context);                    /* 0x020775F8 */

/* ------------------------------------------------------------------ */
/* CTRDG                                                               */
/* ------------------------------------------------------------------ */

BOOL CTRDG_IsExistedAtInit(void);                                /* 0x02090EF4 */
u32  CTRDGi_IsTaskAvailable_02010054(void);                      /* 0x02010054 */
u32  func_02010054(void);
u32  sub_02010054(void);

/* ------------------------------------------------------------------ */
/* Small game-side leaves                                              */
/* ------------------------------------------------------------------ */

void func_02034F74(void *obj, u32 mask);                         /* byte +0x38 |= mask */

/* ---- BW_ASMSDK_BOOT_SOURCES: these call matched code ------------- */

s32   func_02084D98(u16 lockID);                                 /* OS_UnLockCartridge */
s32   OS_UnLockCartridge(u16 lockID);
void *ArrayDeleteAt(u32 a, u32 b);                               /* 0x02005D90 */
void *sub_02005D90(u32 a, u32 b);
BOOL  NHTTPi_GetConnection(void *file, u32 pos);                 /* 0x0204960C */
BOOL  sub_0204960C(void *file, u32 pos);
u32   bw_os_init_arena_lo_addr(u32 id);
u32   bw_os_init_arena_hi_addr(u32 id);
void *OS_GetInitArenaLo(u32 id);                                 /* 0x02086694 */
void *func_02086694(u32 id);
void *OS_GetInitArenaHi(u32 id);                                 /* 0x02086594 */
void *func_02086594(u32 id);
u32   SNDi_IncAlarmId(int alarmNo);                              /* 0x02089FC0 */
BOOL  func_02030BA4(u32 size);
void  func_0203129C(u32 a);
void  func_02034F84(u32 mask);
void  func_0203516C(u32 mask);
u32   func_020351D0(u32 mask);
void  func_02049238(void *handle);
void  func_0208C86C(void *info);

/* TWL-only code the NITRO boot never reaches (see each file). */
void  func_020053E4(void *p);
int   func_02005420(void);
int   func_02005428(void);
void  func_02005454(int v);
void  func_02005B6C(void);
void  func_027312B4(void);
void  OSi_InitArenaEx_ltdmain(void);
u64   func_0273131C(u64 titleId, u64 mask);
u64   OSi_SearchTitleIdFromList(u64 titleId, u64 mask);
void  func_027313E8(u32 dmaNo);
void  PMi_PreSleepForNdma(u32 dmaNo);

/* ==================================================================
 * The boot closure's second batch (EIDOS_BOOT).  cmake/boot.cmake picks
 * each up by file name in place of the matched asm source; they are
 * checked by the boot run (tests/boot_run.cmake), not by
 * tests/test_asmsdk_glue.c.
 * ================================================================== */

struct OSEvent;
struct OSMutex;
struct NCFGConfig;
struct PMiGenCallbackInfo;

/* ------------------------------------------------------------------ */
/* Cache maintenance (os_cache.o): no-ops on a coherent host            */
/* ------------------------------------------------------------------ */

void DC_InvalidateRange(void *startAddr, u32 nBytes);             /* 0x020862B8 */
void func_020862B8(void *startAddr, u32 nBytes);
void DC_StoreRange(const void *startAddr, u32 nBytes);            /* 0x020862D4 */
void func_020862D4(const void *startAddr, u32 nBytes);
void DC_FlushRange(const void *startAddr, u32 nBytes);            /* 0x020862F0 */
void func_020862F0(const void *startAddr, u32 nBytes);
void DC_WaitWriteBufferEmpty(void);                               /* 0x02086314 */

/* ------------------------------------------------------------------ */
/* BIOS calls and spin waits                                           */
/* ------------------------------------------------------------------ */

void func_0200421A(const void *srcp, void *destp, u32 dmaCntData); /* 0x0200421A, SVC_CpuSet */
void func_02004490(s32 count);                                    /* 0x02004490 */
void SVC_WaitByLoop(s32 count);
void func_020879F8(u32 cycle);                                    /* 0x020879F8 */
void OS_SpinWaitCpuCycles(u32 cycle);
int  func_020834B8(int val, int *ptr);                            /* 0x020834B8 */
u32  MI_SwapWord(u32 setData, volatile u32 *destp);

/* ------------------------------------------------------------------ */
/* OS                                                                  */
/* ------------------------------------------------------------------ */

void OS_InitEvent(struct OSEvent *event);                         /* 0x020882DC */
BOOL OS_IsRunOnEmulator(void);                                    /* 0x02085D24 */
void OS_UnlockMutex(struct OSMutex *mutex);                       /* 0x02086078 */
void func_02086078(void *mutex);
s32  func_02084E10(u32 lockID);                                   /* 0x02084E10, OS_UnlockCard */
void func_02086E40(void);                                         /* 0x02086E40 */
void OSi_ExceptionHandler(void);

/* ------------------------------------------------------------------ */
/* MI, CARD, NVRAM, PM, RTC                                            */
/* ------------------------------------------------------------------ */

void MI_InitWramManager(void);                                    /* 0x02082198 */
void func_02082198(void);
u32  CARD_GetBackupSectorSize(void);                              /* 0x02076B6C */
void CARD_LockBackup(u16 lock_id);                                /* 0x02075E9C */
void func_02075E9C(u16 lock_id);
void func_02075DC8(BOOL enable);                                  /* 0x02075DC8, CARD_Enable */
BOOL NVRAMi_IsInitialized(void);                                  /* 0x020841E8 */
void PM_DeletePostSleepCallback(struct PMiGenCallbackInfo *info); /* 0x0208C884 */
void func_0208C884(void *info);
void PMi_SetDispOffCount(void);                                   /* 0x0208C944 */
BOOL RTCi_ReadRawDateTimeAsync(void);                             /* 0x02088EEC */
BOOL RTCi_ReadRawDateAsync(void);                                 /* 0x02088EFC */
BOOL RTCi_ReadRawTimeAsync(void);                                 /* 0x02088F0C */
void func_02088ED4(void);                                         /* 0x02088ED4 */
void RtcWaitBusy(void);

/* ------------------------------------------------------------------ */
/* SND interface (snd_interface.o)                                     */
/* ------------------------------------------------------------------ */

void SND_SetPlayerTempoRatio(int playerNo, int ratio);            /* 0x020893B8 */
void func_020893B8(u8 playerNo, u32 ratio);
void SND_SetPlayerVolume(int playerNo, int volume);               /* 0x020893D0 */
void func_020893D0(u32 playerNo, s32 volume);
void SND_SetPlayerChannelPriority(int playerNo, int prio);        /* 0x020893E8 */
void func_020893E8(u8 playerNo, u32 prio);
void SND_SetTrackVolume(int playerNo, u32 trackBitMask, int volume);  /* 0x02089400 */
void func_02089400(u8 playerNo, u32 trackBitMask, int volume);
void SND_SetTrackPitch(int playerNo, u32 trackBitMask, int pitch);    /* 0x0208941C */
void func_0208941C(u8 playerNo, u32 trackBitMask, u32 pitch);
void SND_SetTrackPan(int playerNo, u32 trackBitMask, int pan);        /* 0x02089438 */
void func_02089438(u8 playerNo, u32 trackBitMask, u32 pan);
void SND_SetTrackModDepth(int playerNo, u32 trackBitMask, int depth); /* 0x02089454 */
void func_02089454(u8 playerNo, u32 trackBitMask, u32 depth);
void SND_SetTrackModSpeed(int playerNo, u32 trackBitMask, int speed); /* 0x02089470 */
void func_02089470(u8 playerNo, u32 trackBitMask, u32 speed);

/* ------------------------------------------------------------------ */
/* Wi-Fi libraries                                                     */
/* ------------------------------------------------------------------ */

int  DWCi_Language(void);                                         /* 0x020461E4 */
u8   func_020461E4(void);
int  NCFG_ReadConfig(struct NCFGConfig *config, void *work);      /* 0x0205FAA8 */
void func_0205FAA8(u8 *config, u32 work);

/* ------------------------------------------------------------------ */
/* ITCM reset path (os_reset.o, scfg_proc.o)                           */
/* ------------------------------------------------------------------ */

BOOL func_01FF82A8(const u8 *src, u32 len, const void *digest);   /* 0x01FF82A8 */
void func_01FF8460(void);                                         /* 0x01FF8460 */
void OSi_DoBoot(void);
void func_01FF8540(BOOL ontwl);                                   /* 0x01FF8540 */
void OSi_ReloadRomData(int ontwl);
void func_01FF87D4(int cpuSpeed);                                 /* 0x01FF87D4 */
void SCFGi_SwitchCpuSpeed(int cpuSpeed);

/* TWL-only placeholders the NITRO boot never reaches (see each file). */
void func_0272EA74(void);
void func_0272F8A8(int a0, int a1, int a2, int a3, int a4, int a5, void *a6, int a7, unsigned int a8);
void func_027313AC(void);
void func_027314A4(void);

/* MATHi_SHA1ProcessBlock (sdk/asm/func_02081E7C.c): the SHA-1 compression
 * function on a context (state at +0x00, block at +0x14). */
void func_02081E7C(void *context);                               /* 0x02081E7C */

/* OS_TryLockCartridge (sdk/asm/OS_LockCartridge.c) */
s32 func_02084DA4(u32 lockID);                                   /* 0x02084DA4 */
s32 OS_TryLockCartridge(u16 lockID);

/* TWL-only placeholder (sdk/asm/func_02730468.c). */
void func_02730468(void);                                        /* 0x02730468 */

/* OS_TryLockCard (sdk/asm/OS_LockCard.c) */
s32 func_02084E38(u32 lockID);                                   /* 0x02084E38 */
s32 OS_TryLockCard(u16 lockID);

#ifdef __cplusplus
}
#endif

#endif /* BW_PORT_ASMSDK_H */
