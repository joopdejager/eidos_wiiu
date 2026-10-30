/*
 * engine/platform/settings_none.c -- the settings an EIDOS_BOOT build with
 * no platform layer links against (EIDOS_SDL=OFF: the headless boot a
 * big-endian host or an emulator runs).
 *
 * The game's title-menu patch (ov088) and the options screen call into
 * engine/platform/settings.c and settings_ui.c, which need SDL for the
 * settings file's place.  Here there is no file and no screen: every option
 * reads 0, a store is refused, and the screen leaves as soon as it is
 * entered.  The options screen's proc (engine/platform/mod_options_proc.c)
 * is not linked: eidos_opt_proc_table answers NULL, which the title menu's
 * patches only ask for when the player picks the options entry.
 */

#include "settings.h"
#include "settings_ui.h"

#include <stddef.h>

const void *eidos_opt_proc_table(void)
{
    return NULL;
}

int eidos_settings_get(int id)
{
    (void)id;
    return 0;
}

int eidos_settings_set(int id, int value)
{
    (void)id;
    (void)value;
    return -1;
}

int eidos_settings_commit(void)
{
    return 0;
}

void eidos_sui_leave(void)
{
}

int eidos_sui_input(unsigned pressed, unsigned held)
{
    (void)pressed;
    (void)held;
    return EIDOS_SUI_EXIT;
}

int eidos_sui_row_count(void)
{
    return 1;
}

void eidos_sui_row_text(int row, char *label, int nl, char *value, int nv)
{
    (void)row;
    if (label != NULL && nl > 0) {
        label[0] = '\0';
    }
    if (value != NULL && nv > 0) {
        value[0] = '\0';
    }
}

int eidos_sui_cursor_row(void)
{
    return 1;
}

int eidos_sui_help_count(void)
{
    return 0;
}

void eidos_sui_help_line(int i, char *out, int n)
{
    (void)i;
    if (out != NULL && n > 0) {
        out[0] = '\0';
    }
}
