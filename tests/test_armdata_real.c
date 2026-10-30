/*
 * tests/test_armdata_real.c -- the ARM9 data, generated from aletheia's real
 * metadata (tools/armdata.py) and loaded from the player's own
 * extract/arm9_dec.bin.  Skipped, and passing, without the image.
 *
 * CMakeLists.txt generates the tables for a handful of matched sources and
 * compiles four of them here with the forced-include header, so this checks
 * the whole chain on real code:
 *
 *   - the archive name table at 0x020A6C58: bound by its matched name
 *     (sub_0200545C.c's _ArcPathTable), its pointer words rewritten to host
 *     pointers (ILP32), entry 0 reaching "a/0/0/0" and entry 234 "a/2/3/4"
 *     (aletheia's docs/ASSETS.md: entry N is N's three digits);
 *   - the proc table at 0x020A0480: its three Thumb words rewritten to the
 *     host functions APP_POWER_IsSupported, OVERLAY_LAUNCH_Driver and APP_POWER_IsReady, the
 *     matched files compiled into this test, and callable through them;
 *   - matched OVERLAY_LAUNCH_Driver run as written: a linker constant (ov088_id),
 *     an overlay address (d_021BD8E8) and one object under two names
 *     (g_02146948, g_02146948_arg) all reach the expected DS addresses;
 *   - matched GFL_USE_OnPreExit's store to g_020A9E80 lands at 0x020A9E80;
 *   - two names for 0x020AA194 (sub_02005610_Sys, sub_020056A0_Sys) bound to
 *     one byte; SDK_AUTOLOAD_DTCM_START bound to 0x02FE0000 with the DTCM
 *     contiguous behind it (+0x3FF8 is bw_ds_ptr(0x02FE3FF8)); the constant
 *     SDK_IRQ_STACKSIZE; d_02146954 bound to the port's g_EnvModule;
 *   - the DTCM IRQ table (the OS lane's object) left unrelocated.
 *
 * On an LP64 host the pointer words are checked to still hold their DS
 * addresses, which is the documented refusal (sdk/armdata.h).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "port.h"
#include "dsmem.h"
#include "armdata.h"
#include "port_test.h"

#define CHECK_PTR(label, got, want)                                        \
    do {                                                                   \
        bw_checks++;                                                       \
        if ((const void *)(got) != (const void *)(want)) {                 \
            bw_failures++;                                                 \
            printf("  FAIL %-40s got %p, want %p\n", (label),              \
                   (const void *)(got), (const void *)(want));             \
        }                                                                  \
    } while (0)

/* The matched functions compiled into this test. */
int  APP_POWER_IsSupported(void);
int  OVERLAY_LAUNCH_Driver(void *unused, u32 *state);
int  APP_POWER_IsReady(void);
void GFL_USE_OnPreExit(void);

extern char g_EnvModule[];

/* What the two matched functions call, recorded. */
static void *g_ovl, *g_proc, *g_arg;
static int   g_calls;

void PROCSYS_PushGlobal(void *ovl, void *proc, void *arg);
void PROCSYS_PushGlobal(void *ovl, void *proc, void *arg)
{
    g_ovl = ovl;
    g_proc = proc;
    g_arg = arg;
    g_calls++;
}

void func_02005454(int v);
void func_02005454(int v)
{
    (void)v;
}

static const BwDataName *need(const char *name)
{
    const BwDataName *n = bw_data_find(&bw_arm9_data, name);
    if (n == NULL) {
        printf("  FAIL no generated name %s\n", name);
        bw_failures++;
    }
    return n;
}

static void *bound(const char *name)
{
    const BwDataName *n = need(name);
    return n != NULL ? *n->ref : NULL;
}

/* A word as the game reads it: the relocated image is in the host's order
 * (sdk/armdata.c, swap_to_host). */
static u32 word_at(u32 addr)
{
    return bw_mem_rd32((const u8 *)bw_ds_ptr(addr));
}

static const BwDataReloc *reloc_at(u32 where)
{
    u32 i;
    for (i = 0; i < bw_arm9_data.reloc_count; i++) {
        if (bw_arm9_data.relocs[i].where == where) {
            return &bw_arm9_data.relocs[i];
        }
    }
    return NULL;
}

/* Entry `i` of the archive name table, as a C string. */
static const char *arc_entry(u32 i)
{
    const BwDataReloc *r = reloc_at(0x020A6C58u + 4u * i);
    u32 w = word_at(0x020A6C58u + 4u * i);

    if (r == NULL) {
        return NULL;
    }
    if (bw_data_relocation_supported()) {
        return (const char *)(uintptr_t)w;
    }
    return (const char *)bw_ds_ptr(w);
}

int main(int argc, char **argv)
{
    int reloc = 0;
    u32 state;
    const char *s;
    const BwDataReloc *r;
    u32 i;

    BW_TEST_SETUP(argc, argv);
    if (bw_arm9_rodata(0x02004000u, 4) == NULL) {
        printf("test_armdata_real: skipped (no extract/arm9_dec.bin next to the data root)\n");
        return 0;
    }
    reloc = bw_data_relocation_supported();
    printf("pointer width %u, relocation %s; %lu names, %lu relocations, %lu code targets\n",
           (unsigned)sizeof(void *), reloc ? "supported" : "refused (LP64)",
           (unsigned long)bw_arm9_data.name_count,
           (unsigned long)bw_arm9_data.reloc_count,
           (unsigned long)bw_arm9_data.func_count);

    bw_data_install(&bw_arm9_data, BW_DATA_ALLOW_UNRELOCATED);
    bw_ds_reset();

    /* --- the archive name table ------------------------------------- */
    CHECK_PTR("_ArcPathTable bound", bound("_ArcPathTable"), bw_ds_ptr(0x020A6C58u));
    for (i = 0; i < 235u; i++) {
        BW_REQUIRE("archive table word listed", reloc_at(0x020A6C58u + 4u * i) != NULL);
    }
    r = reloc_at(0x020A6C58u);
    if (reloc) {
        BW_CHECK_EQ("entry 0 is a host pointer", word_at(0x020A6C58u),
                    (u32)(uintptr_t)bw_ds_ptr(r->target));
    } else {
        BW_CHECK_EQ("LP64: entry 0 still the DS address", word_at(0x020A6C58u), r->target);
    }
    s = arc_entry(0);
    BW_REQUIRE("entry 0", s != NULL && strcmp(s, "a/0/0/0") == 0);
    s = arc_entry(234);
    BW_REQUIRE("entry 234", s != NULL && strcmp(s, "a/2/3/4") == 0);

    /* --- the proc table: three Thumb words to matched functions ------- */
    {
        uintptr_t want[3];
        want[0] = (uintptr_t)APP_POWER_IsSupported;
        want[1] = (uintptr_t)OVERLAY_LAUNCH_Driver;
        want[2] = (uintptr_t)APP_POWER_IsReady;
        for (i = 0; i < 3; i++) {
            r = reloc_at(0x020A0480u + 4u * i);
            BW_REQUIRE("proc word listed", r != NULL && r->kind == BW_DATA_RELOC_CODE);
            BW_REQUIRE("proc target is Thumb", (r->target & 1u) == 1u);
            if (reloc) {
                BW_CHECK_EQ("proc word -> host function", word_at(0x020A0480u + 4u * i),
                            (u32)want[i]);
            } else {
                BW_CHECK_EQ("LP64: proc word still the DS address",
                            word_at(0x020A0480u + 4u * i), r->target);
            }
        }
        if (reloc) {
            int (*fn)(void) = (int (*)(void))(uintptr_t)word_at(0x020A0480u);
            BW_CHECK_EQ("call through the relocated word", fn(), 1);
        }
    }

    /* --- OVERLAY_LAUNCH_Driver as written ------------------------------------ */
    state = 0;
    OVERLAY_LAUNCH_Driver(NULL, &state);
    BW_CHECK_EQ("state 0 -> 1", state, 1);
    BW_CHECK_EQ("one call", g_calls, 1);
    CHECK_PTR("ov088_id is the constant 88", g_ovl, (void *)(uintptr_t)88u);
    CHECK_PTR("d_021BD8E8 at its DS address", g_proc, bw_ds_ptr(0x021BD8E8u));
    CHECK_PTR("&g_02146948.f04", g_arg, (u8 *)bw_ds_ptr(0x02146948u) + 4);
    OVERLAY_LAUNCH_Driver(NULL, &state);
    BW_CHECK_EQ("state 1 -> 2", state, 2);
    CHECK_PTR("g_02146948_arg is g_02146948", g_arg, bw_ds_ptr(0x02146948u));
    CHECK_PTR("ov179_id is the constant 179", g_ovl, (void *)(uintptr_t)179u);
    BW_CHECK_EQ("g_02146948.f04 stored at 0x0214694C", word_at(0x0214694Cu), 1);
    BW_CHECK_EQ("g_02146948.f02 stored at 0x0214694A", *(u8 *)bw_ds_ptr(0x0214694Au), 1);

    /* --- GFL_USE_OnPreExit's store --------------------------------------- */
    *(u32 *)bw_ds_ptr(0x020A9E80u) = 0;
    GFL_USE_OnPreExit();
    BW_CHECK_EQ("g_020A9E80 = 1 landed at 0x020A9E80", word_at(0x020A9E80u), 1);

    /* --- names ---------------------------------------------------------- */
    CHECK_PTR("sub_02005610_Sys", bound("sub_02005610_Sys"), bw_ds_ptr(0x020AA194u));
    CHECK_PTR("sub_020056A0_Sys is the same byte", bound("sub_020056A0_Sys"),
              bound("sub_02005610_Sys"));
    CHECK_PTR("SDK_AUTOLOAD_DTCM_START", bound("SDK_AUTOLOAD_DTCM_START"),
              bw_ds_ptr(0x02FE0000u));
    CHECK_PTR("DTCM contiguous to +0x3FF8",
              (u8 *)bound("SDK_AUTOLOAD_DTCM_START") + 0x3FF8, bw_ds_ptr(0x02FE3FF8u));
    CHECK_PTR("SDK_IRQ_STACKSIZE is 0x800", bound("SDK_IRQ_STACKSIZE"),
              (void *)(uintptr_t)0x800u);
    CHECK_PTR("d_02146954 is the port's g_EnvModule", bound("d_02146954"), g_EnvModule);
    {
        const BwDataName *n = need("SDK_IRQ_STACKSIZE");
        BW_REQUIRE("constant binding", n != NULL && n->bind == BW_DATA_BIND_CONST);
    }

    /* --- the OS lane's IRQ table is not relocated in the copy ---------- */
    BW_REQUIRE("no relocation inside g_02FE0020", reloc_at(0x02FE0020u) == NULL);
    {
        const u8 *img = NULL;
        for (i = 0; i < bw_arm9_data.range_count; i++) {
            const BwDataRange *rg = &bw_arm9_data.ranges[i];
            if (rg->addr == 0x02FE0000u && rg->copy) {
                img = (const u8 *)bw_arm9_rodata(rg->image_addr + 0x20u, 4);
            }
        }
        BW_REQUIRE("autoload source", img != NULL);
        /* The DS's bytes, untouched: no source the image serves reads the
         * table (sdk/os_interrupt.c's g_02FE0020 is the port's own), so it
         * is neither relocated nor turned into a big-endian host's order. */
        BW_CHECK_EQ("IRQ table word as in the image",
                    bw_rd32((const u8 *)bw_ds_ptr(0x02FE0020u)), bw_rd32(img));
    }

    bw_data_install(NULL, 0);
    BW_REPORT("test_armdata_real");
}
