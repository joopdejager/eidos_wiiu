/*
 * tests/test_ovlink_real.c -- two real overlays compiled into one executable
 * and loaded from the player's own extract through FS_LoadOverlay
 * (docs/overlays.md).
 *
 * ov088 (the save check and the title menu) and ov179 (the opening
 * cinematic) are built with tools/ovlink.py from every one of their matched
 * sources; the ARM9 functions they call that the eidos library does not
 * define are stubs.  Without the extract the test reports itself skipped.
 *
 * What is checked, with the real files:
 *   - both load through the SDK name, side by side (their windows do not
 *     overlap), and the registry calls them resident;
 *   - ov088's image is in its window: the root proc's second stage, the
 *     proc data at 0x021BD574, holds three Thumb code pointers, relocated to
 *     the overlay's own functions where pointers are four bytes and still
 *     the DS words (0x021B95A1, ...) where they are not; the .bss is zero;
 *   - a matched ov088 function runs (func_021BD3A0, the state test);
 *   - a matched ov179 function runs on the DS address space: OSi_VBlankIntr
 *     sets bit 0 of the word at DTCM + 0x3FF8, reached through the overlay's
 *     own binding of SDK_AUTOLOAD_DTCM_START;
 *   - ov179's literal 0x021B6279 (the same function, passed as a callback)
 *     resolves to that function.
 *
 * `test_ovlink_real --arm9` calls ov088's func_021BCE70, whose ARM9 callee
 * this link does not have: the stub's panic names it (test_ovlink_real_stub).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "port.h"
#include "dsmem.h"
#include "armdata.h"
#include "overlay.h"
#include "overlay_image.h"
#include "port_test.h"

extern BOOL FS_LoadOverlay(u32 target, u32 id);

extern const BwOvlImageSet bw_ovl_set_title_test;

/* Renamed by tools/ovlink.py: the overlay's own definitions. */
int   ov088_SAVE_CONTROL_INTR_IsFinished(unsigned char *p);
void  ov088_STARTMENU_SaveEraseUpdateTimeIcon(void);
void  ov088_STARTMENU_TitleDemoInit(void);
void  ov088_STARTMENU_TitleDemoMain(void);
void  ov088_STARTMENU_TitleDemoExit(void);
void  ov179_OSi_VBlankIntr(void);
void *bw_ovl_ds_ptr_ov179(unsigned int addr);

static u32 ds_word(u32 addr)
{
    /* As the game reads it: the loaded data is in the host's order. */
    return bw_mem_rd32((const u8 *)bw_ds_ptr(addr));
}

int main(int argc, char **argv)
{
    unsigned char req[8];
    u32 *dtcm_word;

    BW_TEST_SETUP(argc > 1 && argv[1][0] != '-' ? argc : 1, argv);
    if (bw_ovl_file_bytes(88, 0, 4) == NULL || bw_ovl_file_bytes(179, 0, 4) == NULL) {
        printf("SKIPPED: no extract/overlays_dec/y9_0088.bin or y9_0179.bin next to "
               "the data root\n");
        return 0;
    }
    bw_ds_reset();
    bw_overlay_sdk_init();
    bw_ovl_install(&bw_ovl_set_title_test, BW_DATA_ALLOW_UNRELOCATED);

    BW_REQUIRE("load ov088", FS_LoadOverlay(0, 88));
    BW_REQUIRE("load ov179", FS_LoadOverlay(0, 179));
    BW_CHECK_EQ("ov088 resident", bw_overlay_is_loaded(88), 1);
    BW_CHECK_EQ("ov179 resident", bw_overlay_is_loaded(179), 1);
    BW_CHECK_EQ("resident at 0x021BD574", bw_ovl_resident_at(0x021BD574u), 88);
    BW_CHECK_EQ("resident at 0x021B6278", bw_ovl_resident_at(0x021B6278u), 179);

    if (argc > 1 && strcmp(argv[argc - 1], "--arm9") == 0) {
        ov088_STARTMENU_SaveEraseUpdateTimeIcon();
        printf("returned from an ARM9 stub\n");
        return 0;
    }

    /* The proc data (init, main, end) the root proc starts last. */
    if (bw_data_relocation_supported()) {
        BW_CHECK_EQ("proc init relocated", ds_word(0x021BD574u), (u32)(uintptr_t)ov088_STARTMENU_TitleDemoInit);
        BW_CHECK_EQ("proc main relocated", ds_word(0x021BD578u), (u32)(uintptr_t)ov088_STARTMENU_TitleDemoMain);
        BW_CHECK_EQ("proc end relocated", ds_word(0x021BD57Cu), (u32)(uintptr_t)ov088_STARTMENU_TitleDemoExit);
    } else {
        BW_CHECK_EQ("proc init as on the DS", ds_word(0x021BD574u), 0x021B95A1u);
        BW_CHECK_EQ("proc main as on the DS", ds_word(0x021BD578u), 0x021B9751u);
        BW_CHECK_EQ("proc end as on the DS", ds_word(0x021BD57Cu), 0x021B9875u);
    }
    BW_CHECK_EQ("ov088 .bss zero", ds_word(0x021BDA20u), 0);

    /* A matched ov088 function. */
    memset(req, 0, sizeof(req));
    BW_CHECK_EQ("func_021BD3A0 idle", ov088_SAVE_CONTROL_INTR_IsFinished(req), 0);
    req[4] = 3;
    BW_CHECK_EQ("func_021BD3A0 state 3", ov088_SAVE_CONTROL_INTR_IsFinished(req), 1);

    /* A matched ov179 function on the DS address space. */
    dtcm_word = (u32 *)bw_ds_ptr(0x02FE3FF8u);
    *dtcm_word = 0x10u;
    ov179_OSi_VBlankIntr();
    BW_CHECK_EQ("OSi_VBlankIntr sets the flag", *dtcm_word, 0x11u);
    BW_CHECK_EQ("literal 0x021B6279 is OSi_VBlankIntr",
                (long)(bw_ovl_ds_ptr_ov179(0x021B6279u) == (void *)(uintptr_t)ov179_OSi_VBlankIntr), 1);
    BW_CHECK_EQ("other literals are DS memory",
                (long)(bw_ovl_ds_ptr_ov179(0x021B68F0u) == bw_ds_ptr(0x021B68F0u)), 1);

    bw_ovl_install(NULL, 0);
    BW_REPORT("test_ovlink_real");
}
