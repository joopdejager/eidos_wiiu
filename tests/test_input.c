/*
 * tests/test_input.c -- the buttons and the hinge from host state and a
 * script onto KEYINPUT and HW_BUTTON_XY_BUF, read back by the game's own
 * per-frame pad update; and the divider unit the touch calibration runs on.
 *
 * The reader is aletheia's src/matched/arm9/KEY_MainSys.c (key.c's pad
 * update: PAD_DetectFold, PAD_Read, the d-pad cancel, trigger and repeat),
 * compiled as written through tools/dsaddr.py, so a bit in the wrong place,
 * an inverted level or a missing XY halfword shows up as the game would see
 * it.  Its two callees are doubles here: func_0203521C hands it the key
 * work, KEY_ApplyControlMode (the button remapper) does nothing.  Every expected
 * value is worked out by hand from the hardware rules in sdk/input.h.
 *
 * No game data; both pointer widths (the key work is all u32).
 */

#include <stdio.h>
#include <string.h>

#include "arm7/arm7.h"
#include "cp.h"
#include "dsmem.h"
#include "input.h"
#include "port.h"
#include "port_test.h"

/* ------------------------------------------------------------------ */
/* The game's pad update and its doubles                               */
/* ------------------------------------------------------------------ */

struct Key {                    /* KEY_MainSys.c's view                  */
    u8  pad00[0x0c];
    u32 cont, trg, rpt;
    u32 f18, f1c, f20, f24, f28, f2c, f30, f34, f38;
    s32 wait, speed, delay;
};

struct Ctx {
    u8 pad00[0x3e];
    u8 f3e;
};

static struct Key g_key;
static struct Ctx g_ctx;

struct Key *func_0203521C(struct Ctx *ctx);
void KEY_ApplyControlMode(struct Key *k);
void KEY_MainSys(struct Ctx *ctx);

struct Key *func_0203521C(struct Ctx *ctx)
{
    (void)ctx;
    return &g_key;
}

void KEY_ApplyControlMode(struct Key *k)
{
    (void)k;
}

static u16 keyinput(void)
{
    return bw_io_read16(BW_REG_KEYINPUT);
}

static u16 xybuf(void)
{
    return *(volatile u16 *)bw_ds_ptr_w(BW_HW_BUTTON_XY_BUF, sizeof(volatile u16));
}

/* One DS frame: the hardware latches the input, then the game reads it.   */
static void frame(void)
{
    bw_input_frame();
    KEY_MainSys(&g_ctx);
}

static void host(u32 buttons, int lid)
{
    BwInputState s;

    memset(&s, 0, sizeof s);
    s.buttons = buttons;
    s.lid_closed = lid;
    bw_input_set_host(&s);
}

static void start(void)
{
    bw_port_reset();
    bw_input_reset();
    memset(&g_key, 0, sizeof g_key);
    memset(&g_ctx, 0, sizeof g_ctx);
    g_key.speed = 4;
    g_key.delay = 8;
}

/* ------------------------------------------------------------------ */

static void test_registers(void)
{
    start();
    BW_CHECK_EQ("reset: KEYINPUT released", keyinput(), 0x03FF);
    bw_input_frame();
    BW_CHECK_EQ("idle: KEYINPUT", keyinput(), 0x03FF);
    BW_CHECK_EQ("idle: XY released, lid open", xybuf(), 0x2C00);

    /* A (bit 0), UP (bit 6), R (bit 8): KEYINPUT 0x3FF & ~0x141 = 0x2BE.
     * X (bit 10) and Y (bit 11) are not KEYINPUT's: they clear in XY.     */
    host(BW_KEY_A | BW_KEY_UP | BW_KEY_R | BW_KEY_X, 0);
    bw_input_frame();
    BW_CHECK_EQ("A+UP+R+X: KEYINPUT", keyinput(), 0x02BE);
    BW_CHECK_EQ("A+UP+R+X: XY", xybuf(), 0x2800);
    host(BW_KEY_Y | BW_KEY_DEBUG | BW_KEY_SELECT, 0);
    bw_input_frame();
    BW_CHECK_EQ("Y+DEBUG+SELECT: KEYINPUT", keyinput(), 0x03FB);
    BW_CHECK_EQ("Y+DEBUG+SELECT: XY", xybuf(), 0x0400);
    host(0, 1);
    bw_input_frame();
    BW_CHECK_EQ("lid closed: XY bit 15", xybuf(), 0xAC00);
    BW_CHECK_EQ("lid closed: KEYINPUT untouched", keyinput(), 0x03FF);

    /* Host input waits for the frame; bw_input_publish does not.           */
    host(BW_KEY_B, 0);
    BW_CHECK_EQ("host alone does not reach KEYINPUT", keyinput(), 0x03FF);
    bw_input_publish();
    BW_CHECK_EQ("publish: KEYINPUT", keyinput(), 0x03FD);
    BW_CHECK_EQ("publish: no frame passed", bw_input_frame_number(), 4);
}

static void test_game_reads(void)
{
    start();
    frame();
    BW_CHECK_EQ("game: nothing held", g_key.cont, 0);

    /* PAD_Read = ((KEYINPUT | XY) ^ 0x2FFF) & 0x2FFF, then UP cancels DOWN
     * and LEFT cancels RIGHT.                                              */
    host(BW_KEY_A | BW_KEY_X | BW_KEY_L, 0);
    frame();
    BW_CHECK_EQ("game: cont A+X+L", g_key.cont, 0x0601);
    BW_CHECK_EQ("game: trg A+X+L", g_key.trg, 0x0601);
    host(BW_KEY_A | BW_KEY_X | BW_KEY_L | BW_KEY_Y, 0);
    frame();
    BW_CHECK_EQ("game: cont +Y", g_key.cont, 0x0E01);
    BW_CHECK_EQ("game: trg only Y", g_key.trg, 0x0800);
    host(BW_KEY_UP | BW_KEY_DOWN | BW_KEY_LEFT | BW_KEY_RIGHT, 0);
    frame();
    BW_CHECK_EQ("game: UP cancels DOWN, LEFT cancels RIGHT",
                g_key.cont, BW_KEY_UP | BW_KEY_LEFT);
    host(BW_KEY_DEBUG, 0);
    frame();
    BW_CHECK_EQ("game: DEBUG through the XY halfword", g_key.cont, 0x2000);

    /* Lid closed: PAD_DetectFold clears the six edge words and returns.    */
    host(BW_KEY_A, 0);
    frame();
    BW_CHECK_EQ("game: A before the lid", g_key.f18, BW_KEY_A);
    host(BW_KEY_A, 1);
    frame();
    BW_CHECK_EQ("game: lid closed clears hold", g_key.f18, 0);
    BW_CHECK_EQ("game: lid closed clears trigger", g_key.f1c, 0);
}

static void test_script(void)
{
    u32 i;
    u16 seen[12];
    char err[128];

    start();
    /* press(2, A, 3): A held for frames 2, 3 and 4.  press(3, B) with no
     * hold, released at 6.  The host's START is ORed in throughout.       */
    BW_CHECK_EQ("script: press", bw_input_script_press(2, BW_KEY_A, 3), 0);
    BW_CHECK_EQ("script: press B", bw_input_script_press(3, BW_KEY_B, 0), 0);
    BW_CHECK_EQ("script: release B", bw_input_script_release(6, BW_KEY_B), 0);
    BW_CHECK_EQ("script: lid", bw_input_script_lid(8, 1), 0);
    BW_CHECK_EQ("script: events", bw_input_script_pending(), 5);
    host(BW_KEY_START, 0);
    for (i = 0; i < 10; i++) {
        BW_CHECK_EQ("script: frame number", bw_input_frame_number(), i);
        bw_input_frame();
        seen[i] = keyinput();
    }
    BW_CHECK_EQ("frame 0", seen[0], 0x03F7);
    BW_CHECK_EQ("frame 1", seen[1], 0x03F7);
    BW_CHECK_EQ("frame 2: A", seen[2], 0x03F6);
    BW_CHECK_EQ("frame 3: A+B", seen[3], 0x03F4);
    BW_CHECK_EQ("frame 4: A+B", seen[4], 0x03F4);
    BW_CHECK_EQ("frame 5: B", seen[5], 0x03F5);
    BW_CHECK_EQ("frame 6: host only", seen[6], 0x03F7);
    BW_CHECK_EQ("frame 9: lid closed", xybuf(), 0xAC00);
    BW_CHECK_EQ("script: drained", bw_input_script_pending(), 0);

    /* Bad arguments.                                                       */
    BW_CHECK_EQ("script: no button", bw_input_script_press(1, 0, 0), -1);
    BW_CHECK_EQ("script: not a button", bw_input_script_press(1, 0x1000, 0),
                -1);
    BW_CHECK_EQ("script: touch off screen",
                bw_input_script_touch(1, 256, 0, 0), -1);

    /* The text form, and a past frame applied at the next frame.           */
    start();
    BW_CHECK_EQ("text: load",
                bw_input_script_load(
                    "# a comment line\n"
                    "\n"
                    "0 press a+Up 2   # trailing comment\r\n"
                    "1 touch 100 50 1\n"
                    "3 press R\n"
                    "4 release r\n"
                    "4 lid closed\n"
                    "5 lid open\n", err, sizeof err), 0);
    BW_CHECK_EQ("text: no error", err[0], 0);
    BW_CHECK_EQ("text: events", bw_input_script_pending(), 8);
    for (i = 0; i < 6; i++) {
        BwInputState s;

        bw_input_frame();
        bw_input_current(&s);
        seen[i] = (u16)s.buttons;
        seen[6 + i] = (u16)((s.touch ? 0x100 : 0) | (s.lid_closed ? 1 : 0));
        if (i == 1) {
            BW_CHECK_EQ("text: touch x", s.touch_x, 100);
            BW_CHECK_EQ("text: touch y", s.touch_y, 50);
        }
    }
    BW_CHECK_EQ("text: frame 0 A+UP", seen[0], BW_KEY_A | BW_KEY_UP);
    BW_CHECK_EQ("text: frame 1 A+UP", seen[1], BW_KEY_A | BW_KEY_UP);
    BW_CHECK_EQ("text: frame 2 none", seen[2], 0);
    BW_CHECK_EQ("text: frame 3 R", seen[3], BW_KEY_R);
    BW_CHECK_EQ("text: frame 4 none", seen[4], 0);
    BW_CHECK_EQ("text: touch only frame 1", seen[7], 0x100);
    BW_CHECK_EQ("text: no touch frame 2", seen[8], 0);
    BW_CHECK_EQ("text: lid frame 4", seen[10], 1);
    BW_CHECK_EQ("text: lid open frame 5", seen[11], 0);

    BW_CHECK_EQ("text: past frame", bw_input_script_press(0, BW_KEY_X, 0), 0);
    bw_input_frame();
    BW_CHECK_EQ("text: past frame applied next", xybuf(), 0x2800);

    /* Errors name the line and add nothing.                                */
    start();
    BW_CHECK_EQ("text: bad button",
                bw_input_script_load("0 press A\n1 press Z\n", err,
                                     sizeof err), -1);
    BW_CHECK_EQ("text: bad button message",
                strcmp(err, "line 2: expected button names joined by '+'"), 0);
    BW_CHECK_EQ("text: nothing added", bw_input_script_pending(), 0);
    BW_CHECK_EQ("text: bad touch",
                bw_input_script_load("5 touch 10 192\n", err, sizeof err), -1);
    BW_CHECK_EQ("text: bad action",
                bw_input_script_load("5 jump\n", err, sizeof err), -1);
    BW_CHECK_EQ("text: trailing",
                bw_input_script_load("5 untouch now\n", err, sizeof err), -1);
    BW_CHECK_EQ("text: trailing plus",
                bw_input_script_load("5 press A+\n", err, sizeof err), -1);
    BW_CHECK_EQ("text: zero hold",
                bw_input_script_load("5 press A 0\n", err, sizeof err), -1);
    BW_CHECK_EQ("text: still nothing", bw_input_script_pending(), 0);

    /* Clear releases what the script holds.                                */
    BW_CHECK_EQ("clear: press", bw_input_script_press(0, BW_KEY_L, 0), 0);
    bw_input_frame();
    BW_CHECK_EQ("clear: L held", keyinput(), 0x01FF);
    bw_input_script_clear();
    bw_input_frame();
    BW_CHECK_EQ("clear: L released", keyinput(), 0x03FF);
}

static void test_touch_merge(void)
{
    BwInputState s;

    start();
    memset(&s, 0, sizeof s);
    s.touch = 1;
    s.touch_x = 300;            /* the host may drag past the edge          */
    s.touch_y = -4;
    bw_input_set_host(&s);
    bw_input_frame();
    bw_input_current(&s);
    BW_CHECK_EQ("merge: x clamped", s.touch_x, 255);
    BW_CHECK_EQ("merge: y clamped", s.touch_y, 0);
    (void)bw_input_script_touch(1, 10, 20, 0);
    bw_input_frame();
    bw_input_current(&s);
    BW_CHECK_EQ("merge: script stylus wins x", s.touch_x, 10);
    BW_CHECK_EQ("merge: script stylus wins y", s.touch_y, 20);
    (void)bw_input_script_untouch(2);
    bw_input_frame();
    bw_input_current(&s);
    BW_CHECK_EQ("merge: back to the host", s.touch_x, 255);
}

static void test_names(void)
{
    BW_CHECK_EQ("name: a", bw_input_button_by_name("a"), BW_KEY_A);
    BW_CHECK_EQ("name: Select", bw_input_button_by_name("Select"),
                BW_KEY_SELECT);
    BW_CHECK_EQ("name: debug", bw_input_button_by_name("DEBUG"), BW_KEY_DEBUG);
    BW_CHECK_EQ("name: unknown", bw_input_button_by_name("ZL"), 0);
    BW_CHECK_EQ("name: round trip",
                bw_input_button_by_name(bw_input_button_name(BW_KEY_Y)),
                BW_KEY_Y);
    BW_CHECK_EQ("name: not a single bit",
                bw_input_button_name(BW_KEY_A | BW_KEY_B) == NULL, 1);
}

/* ------------------------------------------------------------------ */
/* The divider and square-root unit (sdk/cp.c), GBATEK "DS Maths"      */
/* ------------------------------------------------------------------ */

static void div_set(u16 mode, u64 n, u64 d)
{
    *(volatile u16 *)bw_ds_ptr_w(BW_REG_DIVCNT, sizeof(*(volatile u16 *)0)) = mode;
    *(volatile u32 *)bw_ds_ptr(BW_REG_DIV_NUMER) = (u32)n;
    *(volatile u32 *)bw_ds_ptr(BW_REG_DIV_NUMER + 4u) = (u32)(n >> 32);
    *(volatile u32 *)bw_ds_ptr(BW_REG_DIV_DENOM) = (u32)d;
    *(volatile u32 *)bw_ds_ptr(BW_REG_DIV_DENOM + 4u) = (u32)(d >> 32);
}

static u64 rd64(u32 addr)
{
    u32 lo = *(volatile u32 *)bw_ds_ptr(addr);
    u32 hi = *(volatile u32 *)bw_ds_ptr(addr + 4u);

    return ((u64)hi << 32) | lo;
}

static void test_divider(void)
{
    start();
    /* TP_CalcCalibrateParam's first division: (640 - 3328) << 8 by
     * 32 - 224, both as u32 stores of negative numbers.                    */
    div_set(0, (u32)(-688128), (u32)(-192));
    BW_CHECK_EQ("div 32: busy never set",
                *(volatile u16 *)bw_ds_ptr_w(BW_REG_DIVCNT, sizeof(*(volatile u16 *)0)) & 0x8000, 0);
    BW_CHECK_EQ("div 32: signed quotient",
                *(volatile u32 *)bw_ds_ptr(BW_REG_DIV_RESULT), 3584);
    /* 2^28 / 3584 = 74898 remainder 1024 (TP_SetCalibrateParam).          */
    div_set(0, 0x10000000u, 3584);
    BW_CHECK_EQ("div 32: 2^28/3584", rd64(BW_REG_DIV_RESULT) == 74898, 1);
    BW_CHECK_EQ("div 32: remainder", rd64(BW_REG_DIV_REM) == 1024, 1);
    /* -7 / 2 = -3 remainder -1 (truncation), as 64-bit values.            */
    div_set(0, (u32)-7, 2);
    BW_CHECK_EQ("div 32: -7/2 lo", (u32)rd64(BW_REG_DIV_RESULT), 0xFFFFFFFDu);
    BW_CHECK_EQ("div 32: -7/2 hi", (u32)(rd64(BW_REG_DIV_RESULT) >> 32),
                0xFFFFFFFFu);
    BW_CHECK_EQ("div 32: -7%2", (u32)rd64(BW_REG_DIV_REM), 0xFFFFFFFFu);
    /* 32-bit mode ignores the upper words of the operands.                 */
    div_set(0, 0x1234567800000064ull, 0x7700000000000005ull);
    BW_CHECK_EQ("div 32: low words only", rd64(BW_REG_DIV_RESULT) == 20, 1);
    /* Division by zero: RESULT = -1 for n >= 0 with the upper word
     * inverted in mode 0, REM = NUMER, DIVCNT bit 14.                      */
    div_set(0, 5, 0);
    BW_CHECK_EQ("div 0: result",
                rd64(BW_REG_DIV_RESULT) == 0x00000000FFFFFFFFull, 1);
    BW_CHECK_EQ("div 0: rem", rd64(BW_REG_DIV_REM) == 5, 1);
    BW_CHECK_EQ("div 0: DIVCNT bit 14",
                *(volatile u16 *)bw_ds_ptr_w(BW_REG_DIVCNT, sizeof(*(volatile u16 *)0)) & 0x4000, 0x4000);
    div_set(2, (u64)-9, 0);
    BW_CHECK_EQ("div 0 64: result +1", rd64(BW_REG_DIV_RESULT) == 1, 1);
    /* 64/32 and 64/64.                                                     */
    div_set(1, 0x0000001000000000ull, 0xFFFFFFF0ull);    /* 2^36 / -16  */
    BW_CHECK_EQ("div 64/32", rd64(BW_REG_DIV_RESULT) ==
                (u64)-(s64)0x100000000ll, 1);
    BW_CHECK_EQ("div 64/32: DIVCNT bit 14 clear",
                *(volatile u16 *)bw_ds_ptr_w(BW_REG_DIVCNT, sizeof(*(volatile u16 *)0)) & 0x4000, 0);
    div_set(2, 0x0000001000000000ull, 0x0000000100000000ull);
    BW_CHECK_EQ("div 64/64", rd64(BW_REG_DIV_RESULT) == 16, 1);
    div_set(2, 0x8000000000000000ull, (u64)-1);
    BW_CHECK_EQ("div 64: INT64_MIN / -1",
                rd64(BW_REG_DIV_RESULT) == 0x8000000000000000ull, 1);

    /* Square root: floor, 32- and 64-bit parameters.                       */
    *(volatile u16 *)bw_ds_ptr_w(BW_REG_SQRTCNT, sizeof(*(volatile u16 *)0)) = 0;
    *(volatile u32 *)bw_ds_ptr(BW_REG_SQRT_PARAM) = 99;
    *(volatile u32 *)bw_ds_ptr(BW_REG_SQRT_PARAM + 4u) = 1;
    BW_CHECK_EQ("sqrt 32: ignores the upper word",
                *(volatile u32 *)bw_ds_ptr(BW_REG_SQRT_RESULT), 9);
    *(volatile u16 *)bw_ds_ptr_w(BW_REG_SQRTCNT, sizeof(*(volatile u16 *)0)) = 1;
    *(volatile u32 *)bw_ds_ptr(BW_REG_SQRT_PARAM) = 0xFFFFFFFFu;
    *(volatile u32 *)bw_ds_ptr(BW_REG_SQRT_PARAM + 4u) = 0xFFFFFFFFu;
    BW_CHECK_EQ("sqrt 64: 2^64 - 1",
                *(volatile u32 *)bw_ds_ptr(BW_REG_SQRT_RESULT), 0xFFFFFFFFu);
}

int main(void)
{
    test_registers();
    test_game_reads();
    test_script();
    test_touch_merge();
    test_names();
    test_divider();
    BW_REPORT("test_input");
}
