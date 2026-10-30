/*
 * sdk/boot/gx.c -- the GX helpers the boot's matched files call and no
 * matched file defines with external linkage (gx.o, gx_vramcnt.o).
 *
 * sdk/boot/ holds port code that exists only for the game's own boot
 * (cmake/boot.cmake, EIDOS_BOOT_PORT_SOURCES): it calls matched functions
 * that are compiled only into the boot, so it cannot live in the eidos
 * library.  Each function here is the SDK function at its address, written
 * from what the console's code does (the instructions are quoted), for one
 * of three reasons:
 *
 *   - the matched C defines it `static` (GX_VRAMCNT_SetLCDC_ is static in
 *     src/matched/arm9_sdk/GX_VRAMCNT_SetLCDC_.c, a file nothing else in
 *     the boot compiles), while the other gx_vramcnt.o functions call it
 *     across files;
 *   - aletheia has it as an mwcc asm body (GX_ResetBankForBGExtPltt and
 *     its three siblings, src/matched/arm9_sdk/GX_DisableBankFor*.c, named
 *     after the SDK's other spelling): kept here with the rest of the
 *     family rather than in sdk/asm/;
 *   - the matched C passes a DS address as an integer where the callee
 *     takes a pointer (the GX_ResetBankFor* family hands resetBankForX_ the
 *     address of a gGXState field as `(int)0x02150E4E`, which on the host
 *     is a wild pointer: docs/dsaddr-residue.md, "Game code or data
 *     addresses passed as integers").  cmake/boot.cmake lists those matched
 *     files as replaced.
 *
 * gGXState (0x02150E4C) is ARM9 data: its fields are reached here through
 * bw_ds_ptr, the same bytes the matched files reach through the name
 * (docs/armdata.md).  Callers that spell an address (func_020808F4) reach
 * these definitions through tools/bootlink.py's renames by address.
 */

#include "../dsmem.h"
#include "../port.h"

/* resetBankForX_ at 0x020808BC, matched (src/matched/arm9_sdk/func_020808BC.c):
 * takes the field's pending banks, clears the field, maps them to LCDC. */
int func_020808BC(u16 *g3bit);

void GX_VRAMCNT_SetLCDC_(int lcdc);
int  GX_ResetBankForBG(void);
int  GX_ResetBankForOBJ(void);
int  GX_ResetBankForBGExtPltt(void);
int  GX_ResetBankForOBJExtPltt(void);
int  GX_ResetBankForTex(void);
int  GX_ResetBankForTexPltt(void);
int  GX_ResetBankForClearImage(void);
int  GX_ResetBankForSubBG(void);
int  GX_ResetBankForSubOBJ(void);
int  GX_ResetBankForSubBGExtPltt(void);
int  GX_ResetBankForSubOBJExtPltt(void);

#define REG_DISPCNT      0x04000000u
#define REG_DB_DISPCNT   0x04001000u
#define DISPCNT_BG_EXTPLTT  0x40000000u
#define DISPCNT_OBJ_EXTPLTT 0x80000000u
#define VRAMCNT_LCDC     0x80u

/* The gGXState fields resetBankForX_ is handed (the pool words). */
#define GX_FIELD_BG           0x02150E4Eu
#define GX_FIELD_OBJ          0x02150E50u
#define GX_FIELD_TEX          0x02150E54u
#define GX_FIELD_TEXPLTT      0x02150E56u
#define GX_FIELD_CLRIMG       0x02150E58u
#define GX_FIELD_BGEXTPLTT    0x02150E5Au
#define GX_FIELD_OBJEXTPLTT   0x02150E5Cu
#define GX_FIELD_SUBBG        0x02150E5Eu
#define GX_FIELD_SUBOBJ       0x02150E60u
#define GX_FIELD_SUBBGEXTPLTT 0x02150E62u
#define GX_FIELD_SUBOBJEXTPLTT 0x02150E64u

static int reset_bank(u32 field)
{
    return func_020808BC((u16 *)bw_ds_ptr(field));
}

static void clear_dispcnt_bit(u32 reg, u32 bit)
{
    volatile u32 *cnt = (volatile u32 *)bw_ds_ptr(reg);

    *cnt = *cnt & ~bit;
}

/* 0x0207FD34: each bank whose bit is set goes to the LCDC (enable, MST 0).
 * VRAMCNT_A..G at 0x04000240.., H and I at 0x04000248 and 0x04000249. */
void GX_VRAMCNT_SetLCDC_(int lcdc)
{
    static const u32 reg[9] = {
        0x04000240u, 0x04000241u, 0x04000242u, 0x04000243u, 0x04000244u,
        0x04000245u, 0x04000246u, 0x04000248u, 0x04000249u
    };
    int i;

    for (i = 0; i < 9; i++) {
        if (lcdc & (1 << i)) {
            *(volatile u8 *)bw_ds_ptr_w(reg[i], sizeof(*(volatile u8 *)0)) = VRAMCNT_LCDC;
        }
    }
}

/* 0x020808F4..0x020809B4: ldr r0, =<field>; ldr ip, =resetBankForX_; bx ip. */
int GX_ResetBankForBG(void)         { return reset_bank(GX_FIELD_BG); }
int GX_ResetBankForOBJ(void)        { return reset_bank(GX_FIELD_OBJ); }
int GX_ResetBankForTex(void)        { return reset_bank(GX_FIELD_TEX); }
int GX_ResetBankForTexPltt(void)    { return reset_bank(GX_FIELD_TEXPLTT); }
int GX_ResetBankForClearImage(void) { return reset_bank(GX_FIELD_CLRIMG); }
int GX_ResetBankForSubBG(void)      { return reset_bank(GX_FIELD_SUBBG); }
int GX_ResetBankForSubOBJ(void)     { return reset_bank(GX_FIELD_SUBOBJ); }

/* 0x0208091C, 0x02080940 (main engine) and 0x020809C8, 0x020809F0 (sub,
 * asm in aletheia): DISPCNT's extended-palette enable off, then the tail
 * call.  `mov r2, #0x04000000; ldr r1, [r2]; bic r1, r1, #0x40000000;
 * str r1, [r2]` (0x80000000 for OBJ; 0x04001000 for the sub engine). */
int GX_ResetBankForBGExtPltt(void)
{
    clear_dispcnt_bit(REG_DISPCNT, DISPCNT_BG_EXTPLTT);
    return reset_bank(GX_FIELD_BGEXTPLTT);
}

int GX_ResetBankForOBJExtPltt(void)
{
    clear_dispcnt_bit(REG_DISPCNT, DISPCNT_OBJ_EXTPLTT);
    return reset_bank(GX_FIELD_OBJEXTPLTT);
}

int GX_ResetBankForSubBGExtPltt(void)
{
    clear_dispcnt_bit(REG_DB_DISPCNT, DISPCNT_BG_EXTPLTT);
    return reset_bank(GX_FIELD_SUBBGEXTPLTT);
}

int GX_ResetBankForSubOBJExtPltt(void)
{
    clear_dispcnt_bit(REG_DB_DISPCNT, DISPCNT_OBJ_EXTPLTT);
    return reset_bank(GX_FIELD_SUBOBJEXTPLTT);
}
