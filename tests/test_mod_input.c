/*
 * tests/test_mod_input.c -- the input injection: a mod's press of a button
 * and what the game sees of it (docs/mod-services.md, "Input injection";
 * sdk/mod_input.c, sdk/input.c).
 *
 * The injection is the port's own way of pressing a button as if the
 * player had pressed it.  The service (bw_mod_input_press) takes the
 * press, the port's input layer holds it for the number of frames it was
 * asked for and then lets it go, and the game sees the button as if the
 * player had pressed it: the injected buttons are a third source beside
 * the player's own and the script's, and they are ORed into what the game
 * reads, so the player's own keys are never disturbed.
 *
 * What is checked here:
 *
 *   - the game sees the injected key for exactly the number of frames the
 *     press asked for, and the press ends on its own;
 *   - the player's own input is untouched: the player's key and the
 *     injected one are seen at the same time, the player's key stays held
 *     after the injection is over, and the injection holds its button when
 *     the player lets go of his own;
 *   - a press of zero frames does nothing;
 *   - a press longer than the port allows is refused, the player's own
 *     setting lowers the ceiling and turns the injection off at 0;
 *   - a button that is not one of the game's twelve is refused, and so is
 *     an empty mask;
 *   - two presses of the same button hold it until the later of the two
 *     ends.
 *
 * The reader is the game's: the test reads the two registers the game
 * reads, the way the game's own per-frame pad update reads them (key.c's
 * func_02035F38: PAD_Read is (KEYINPUT | the ARM7's XY halfword) ^ 0x2FFF
 * & 0x2FFF, and UP cancels DOWN and LEFT cancels RIGHT), so X and Y,
 * which the ARM9 cannot see in KEYINPUT at all, are seen where the game
 * sees them.  Both registers are the port's input layer's own: sdk/input.c
 * writes KEYINPUT and sdk/arm7/pad.c the halfword.
 *
 * The refusals are the service's own: the services table's call for the
 * injection is frozen as a void one, so a refusal is a report on the
 * port's stderr, which the test takes over for the moment and reads back.
 *
 * No game data and no game code: the button word is all u32, so the test
 * is the same at both pointer widths.
 */

#include <stdio.h>
#include <string.h>

#include "dsmem.h"
#include "input.h"
#include "mod_input.h"
#include "port_test.h"
#include "types.h"

/* ------------------------------------------------------------------ */
/* The port's stderr, taken over for the moment                        */
/* ------------------------------------------------------------------ */

/*
 * The service refuses a press by reporting it on the port's stderr, so
 * the test takes the stream over, lets the service write into a file, and
 * reads the file back.  The stream is put back where it was, so the
 * report of the test itself (stdout) and every later message are as they
 * were.
 */

#ifdef _WIN32
#include <io.h>
#define T_DUP(fd)    _dup(fd)
#define T_DUP2(a, b) _dup2(a, b)
#define T_CLOSE(fd)  _close(fd)
#define T_FILENO(f)  _fileno(f)
#else
#include <unistd.h>
#define T_DUP(fd)    dup(fd)
#define T_DUP2(a, b) dup2(a, b)
#define T_CLOSE(fd)  close(fd)
#define T_FILENO(f)  fileno(f)
#endif

/* The file the reports are taken into, in the test's own directory. */
#define T_ERR_FILE "mod_input_stderr.txt"

/* The stderr file descriptor. */
#define T_STDERR 2

static int g_err_saved = -1;
static FILE *g_err_file;
static char g_err_text[512];

/*
 * Take the port's stderr over, so what is written into it can be read.
 * The file is opened for reading and writing, so it is emptied for every
 * capture and read back from its own start.
 */
static void err_begin(void)
{
    fflush(stderr);
    g_err_file = fopen(T_ERR_FILE, "w+b");
    if (g_err_file == NULL) {
        return;
    }
    g_err_saved = T_DUP(T_STDERR);
    if (g_err_saved >= 0) {
        T_DUP2(T_FILENO(g_err_file), T_STDERR);
    }
}

/*
 * Give the port's stderr back and keep what was written into it.  The
 * file is read from its own start, so the whole report is there whatever
 * the stream had written.
 */
static const char *err_end(void)
{
    size_t n = 0;

    fflush(stderr);
    if (g_err_saved >= 0) {
        T_DUP2(g_err_saved, T_STDERR);
        T_CLOSE(g_err_saved);
        g_err_saved = -1;
    }
    g_err_text[0] = '\0';
    if (g_err_file != NULL) {
        fseek(g_err_file, 0, SEEK_SET);
        n = fread(g_err_text, 1, sizeof g_err_text - 1, g_err_file);
        fclose(g_err_file);
        g_err_file = NULL;
    }
    g_err_text[n] = '\0';
    return g_err_text;
}

/* The file the reports were taken into is the test's own litter: it goes. */
static void err_cleanup(void)
{
    remove(T_ERR_FILE);
}

/* Whether the report the service wrote says `what`. */
static int reported(const char *what)
{
    return strstr(g_err_text, what) != NULL;
}

/* ------------------------------------------------------------------ */
/* The keypad, as the game reads it                                    */
/* ------------------------------------------------------------------ */

/*
 * The button word the game sees this frame: PAD_Read, the SDK's inline as
 * the game's own per-frame pad update has it (key.c's func_02035F38),
 * followed by the d-pad's own rule, UP cancels DOWN and LEFT cancels
 * RIGHT.  X and Y come out of the ARM7's halfword, which is why both
 * registers are read.
 */
static u32 game_keys(void)
{
    u16 keyinput = bw_io_read16(BW_REG_KEYINPUT);
    u16 xy = *(volatile u16 *)bw_ds_ptr_w(BW_HW_BUTTON_XY_BUF, sizeof(volatile u16));
    u32 key = ((u32)keyinput | (u32)xy) ^ 0x2FFFu;

    key &= 0x2FFFu;
    key = key & ~((key & 0x40u) << 1) & ~((key & 0x20u) >> 1);
    return key;
}

/* One DS frame: the merged state becomes what the hardware reads, and the
 * injection ages by one frame. */
static void frame(void)
{
    bw_input_frame();
}

/* The player's own keys, held from the next frame on. */
static void host(u32 buttons)
{
    BwInputState s;

    memset(&s, 0, sizeof s);
    s.buttons = buttons;
    bw_input_set_host(&s);
}

/*
 * A fresh address space and a fresh input layer: the space the input
 * layer writes the keypad into, the layer's own state (the script, the
 * injection and the frame counter), and the longest press the port
 * allows, which is a setting rather than part of a run and so is left
 * alone by the reset.  A press touches these and nothing else, so the
 * machine's own power-on (bw_port_reset) is not needed here.
 */
static void start(void)
{
    bw_ds_reset();
    bw_input_reset();
    bw_input_set_max_press(BW_INPUT_INJECT_MAX_FRAMES);
}

/* ------------------------------------------------------------------ */
/* The press                                                           */
/* ------------------------------------------------------------------ */

static void test_press(void)
{
    u32 i;

    start();
    printf("a press of the game's button\n");
    BW_CHECK_EQ("press: the port takes it", bw_input_inject(BW_KEY_A, 3), 0);
    BW_CHECK_EQ("press: the injection holds A", bw_input_injected(), BW_KEY_A);
    for (i = 0; i < 5; i++) {
        frame();
        if (i < 3) {
            BW_CHECK_EQ("press: the game sees A", game_keys(), BW_KEY_A);
        } else {
            BW_CHECK_EQ("press: the game sees nothing", game_keys(), 0);
        }
    }
    BW_CHECK_EQ("press: the injection holds nothing", bw_input_injected(), 0);

    printf("the press ends on its own\n");
    start();
    bw_mod_input_press(BW_KEY_B, 2);
    frame();
    BW_CHECK_EQ("own: the game sees B", game_keys(), BW_KEY_B);
    frame();
    BW_CHECK_EQ("own: the game sees B", game_keys(), BW_KEY_B);
    frame();
    BW_CHECK_EQ("own: the game sees nothing", game_keys(), 0);
    BW_CHECK_EQ("own: the injection holds nothing", bw_input_injected(), 0);

    printf("several buttons at once\n");
    start();
    bw_mod_input_press(BW_KEY_A | BW_KEY_UP, 2);
    frame();
    BW_CHECK_EQ("two: the game sees A and UP", game_keys(),
                BW_KEY_A | BW_KEY_UP);
    frame();
    BW_CHECK_EQ("two: the game sees A and UP", game_keys(),
                BW_KEY_A | BW_KEY_UP);
    frame();
    BW_CHECK_EQ("two: the game sees nothing", game_keys(), 0);

    printf("the buttons the ARM7 carries\n");
    start();
    bw_mod_input_press(BW_KEY_X | BW_KEY_Y, 2);
    frame();
    BW_CHECK_EQ("xy: the game sees X and Y", game_keys(),
                BW_KEY_X | BW_KEY_Y);
    frame();
    BW_CHECK_EQ("xy: the game sees X and Y", game_keys(),
                BW_KEY_X | BW_KEY_Y);
    frame();
    BW_CHECK_EQ("xy: the game sees nothing", game_keys(), 0);

    printf("the injection lets go\n");
    start();
    bw_input_inject(BW_KEY_A, 100);
    frame();
    BW_CHECK_EQ("clear: the game sees A", game_keys(), BW_KEY_A);
    bw_input_inject_clear();
    frame();
    BW_CHECK_EQ("clear: the game sees nothing", game_keys(), 0);
}

/* ------------------------------------------------------------------ */
/* The player's own input                                              */
/* ------------------------------------------------------------------ */

static void test_player_untouched(void)
{
    start();
    printf("the player's key and the injected one\n");
    host(BW_KEY_A);
    bw_mod_input_press(BW_KEY_B, 2);
    frame();
    BW_CHECK_EQ("both: the game sees A and B", game_keys(),
                BW_KEY_A | BW_KEY_B);
    BW_CHECK_EQ("both: the injection holds B", bw_input_injected(), BW_KEY_B);
    frame();
    BW_CHECK_EQ("both: the game sees A and B", game_keys(),
                BW_KEY_A | BW_KEY_B);
    frame();
    BW_CHECK_EQ("both: the injection is over", game_keys(), BW_KEY_A);
    BW_CHECK_EQ("both: the injection holds nothing", bw_input_injected(), 0);

    printf("the player's key stays held\n");
    frame();
    BW_CHECK_EQ("held: the game still sees A", game_keys(), BW_KEY_A);

    printf("the injection holds its button when the player lets go\n");
    start();
    host(BW_KEY_A);
    bw_mod_input_press(BW_KEY_B, 3);
    frame();
    BW_CHECK_EQ("release: the game sees A and B", game_keys(),
                BW_KEY_A | BW_KEY_B);
    host(0);
    frame();
    BW_CHECK_EQ("release: the game sees only the injected B", game_keys(),
                BW_KEY_B);
    frame();
    BW_CHECK_EQ("release: the injected B is still held", game_keys(),
                BW_KEY_B);
    frame();
    BW_CHECK_EQ("release: the game sees nothing", game_keys(), 0);
}

/* ------------------------------------------------------------------ */
/* The refusals                                                        */
/* ------------------------------------------------------------------ */

static void test_zero_frames(void)
{
    start();
    printf("a press of zero frames\n");
    BW_CHECK_EQ("zero: the port takes it", bw_input_inject(BW_KEY_A, 0), 0);
    BW_CHECK_EQ("zero: nothing is held", bw_input_injected(), 0);
    err_begin();
    bw_mod_input_press(BW_KEY_A, 0);
    err_end();
    BW_CHECK_EQ("zero: the service says nothing", g_err_text[0], 0);
    frame();
    BW_CHECK_EQ("zero: the game sees nothing", game_keys(), 0);
    BW_CHECK_EQ("zero: the injection holds nothing", bw_input_injected(), 0);
}

static void test_limit(void)
{
    start();
    printf("the port's own limit\n");
    BW_CHECK_EQ("limit: the port allows a minute", bw_input_max_press(),
                BW_INPUT_INJECT_MAX_FRAMES);
    BW_CHECK_EQ("limit: a press of that many frames is taken",
                bw_input_inject(BW_KEY_A, BW_INPUT_INJECT_MAX_FRAMES), 0);
    bw_input_inject_clear();
    BW_CHECK_EQ("limit: a press over it is refused",
                bw_input_inject(BW_KEY_A, BW_INPUT_INJECT_MAX_FRAMES + 1u),
                -2);
    BW_CHECK_EQ("limit: nothing is held", bw_input_injected(), 0);
    err_begin();
    bw_mod_input_press(BW_KEY_A, BW_INPUT_INJECT_MAX_FRAMES + 1u);
    err_end();
    BW_CHECK_EQ("limit: the service reports the refusal",
                reported("longer than the port allows"), 1);

    printf("the player's own setting\n");
    bw_input_set_max_press(10);
    BW_CHECK_EQ("setting: the ceiling is the setting", bw_input_max_press(),
                10);
    BW_CHECK_EQ("setting: a press under it is taken",
                bw_input_inject(BW_KEY_B, 10), 0);
    bw_input_inject_clear();
    BW_CHECK_EQ("setting: a press over it is refused",
                bw_input_inject(BW_KEY_B, 11), -2);
    BW_CHECK_EQ("setting: nothing is held", bw_input_injected(), 0);
    err_begin();
    bw_mod_input_press(BW_KEY_B, 11);
    err_end();
    BW_CHECK_EQ("setting: the service reports the refusal",
                reported("longer than the port allows"), 1);

    printf("the setting never raises the port's own limit\n");
    bw_input_set_max_press(BW_INPUT_INJECT_MAX_FRAMES + 100u);
    BW_CHECK_EQ("setting: the ceiling is the port's own", bw_input_max_press(),
                BW_INPUT_INJECT_MAX_FRAMES);
    BW_CHECK_EQ("setting: a press over the port's own limit is refused",
                bw_input_inject(BW_KEY_B, BW_INPUT_INJECT_MAX_FRAMES + 1u),
                -2);

    printf("the injection turned off\n");
    bw_input_set_max_press(0);
    BW_CHECK_EQ("off: the ceiling is zero", bw_input_max_press(), 0);
    BW_CHECK_EQ("off: every press is refused", bw_input_inject(BW_KEY_B, 1),
                -2);
    BW_CHECK_EQ("off: nothing is held", bw_input_injected(), 0);
    err_begin();
    bw_mod_input_press(BW_KEY_B, 1);
    err_end();
    BW_CHECK_EQ("off: the service says the injection is off",
                reported("the input injection is off"), 1);
}

static void test_bad_buttons(void)
{
    start();
    printf("the debug button is not one of the game's\n");
    BW_CHECK_EQ("debug: the port refuses it",
                bw_input_inject(BW_KEY_DEBUG, 3), -1);
    BW_CHECK_EQ("debug: nothing is held", bw_input_injected(), 0);
    err_begin();
    bw_mod_input_press(BW_KEY_DEBUG, 3);
    err_end();
    BW_CHECK_EQ("debug: the service names it",
                reported("DEBUG is not one of the game's twelve buttons"), 1);

    printf("an empty mask\n");
    BW_CHECK_EQ("empty: the port refuses it", bw_input_inject(0, 3), -1);
    BW_CHECK_EQ("empty: nothing is held", bw_input_injected(), 0);
    err_begin();
    bw_mod_input_press(0, 3);
    err_end();
    BW_CHECK_EQ("empty: the service says there is no button",
                reported("there is no button to press"), 1);

    printf("a bit that is not a button at all\n");
    BW_CHECK_EQ("bit: the port refuses it", bw_input_inject(0x1000u, 3), -1);
    BW_CHECK_EQ("bit: nothing is held", bw_input_injected(), 0);
    err_begin();
    bw_mod_input_press(0x1000u, 3);
    err_end();
    BW_CHECK_EQ("bit: the service names it",
                reported("0x1000 is not one of the game's twelve buttons"), 1);
}

/* ------------------------------------------------------------------ */
/* Two presses of the same button                                      */
/* ------------------------------------------------------------------ */

static void test_two_presses(void)
{
    /* The button is held until the later of the two ends: the first
     * press ends at frame 4 and the second at frame 3, so it is let go
     * at frame 4. */
    static const u32 first[] = { BW_KEY_A, BW_KEY_A, BW_KEY_A, 0 };
    /* Here the second press ends later, at frame 5. */
    static const u32 second[] = { BW_KEY_A, BW_KEY_A, BW_KEY_A, BW_KEY_A, 0 };
    u32 i;

    start();
    printf("two presses of the same button\n");
    BW_CHECK_EQ("two: the first press is taken",
                bw_input_inject(BW_KEY_A, 4), 0);
    frame();
    BW_CHECK_EQ("two: the game sees A", game_keys(), BW_KEY_A);
    BW_CHECK_EQ("two: the second press is taken",
                bw_input_inject(BW_KEY_A, 2), 0);
    for (i = 0; i < 4; i++) {
        frame();
        BW_CHECK_EQ("two: the button is held until the later end",
                    game_keys(), first[i]);
    }

    printf("the second press is the later one\n");
    start();
    BW_CHECK_EQ("two: the first press is taken",
                bw_input_inject(BW_KEY_A, 2), 0);
    frame();
    BW_CHECK_EQ("two: the game sees A", game_keys(), BW_KEY_A);
    BW_CHECK_EQ("two: the second press is taken",
                bw_input_inject(BW_KEY_A, 4), 0);
    for (i = 0; i < 5; i++) {
        frame();
        BW_CHECK_EQ("two: the button is held until the later end",
                    game_keys(), second[i]);
    }
    BW_CHECK_EQ("two: the injection holds nothing", bw_input_injected(), 0);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_press();
    test_player_untouched();
    test_zero_frames();
    test_limit();
    test_bad_buttons();
    test_two_presses();
    err_cleanup();
    BW_REPORT("test_mod_input");
}
