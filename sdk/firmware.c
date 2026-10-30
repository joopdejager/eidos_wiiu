/*
 * sdk/firmware.c -- the console's user settings: the settings file, the
 * shared-memory area the ARM7's boot code fills, and the firmware flash's
 * user-settings block (sdk/firmware.h says why each exists).
 *
 * The byte layout is the firmware's (GBATEK, "DS Firmware User Settings"),
 * as far as the matched readers reach it: OS_GetOwnerInfo's OSiNvramOwner
 * (nickname at +0x06, its length at +0x1A, message at +0x1C, its length at
 * +0x50, language in bits 0-2 of +0x64), TP_GetUserInfo's NvTpPoints (the
 * two calibration points at +0x58), OS_GetOwnerRtcOffset (+0x68, +0x6C).
 * The rest (version 5 at +0x00, the alarm at +0x52..+0x56, the backlight in
 * bits 4-5 of +0x64, the year at +0x66) is GBATEK's.  Values are stored
 * little-endian, as the flash holds them and as the port's main RAM holds
 * every DS value (docs/armdata.md: the image is copied byte for byte).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsmem.h"
#include "firmware.h"
#include "save.h"

#define BW_FW_FILE_NAME "firmware.ini"

static struct {
    int                 overridden;  /* bw_firmware_set gave the settings   */
    int                 loaded;      /* `current` holds the file's reading  */
    BwFirmwareSettings  current;
} g_fw;

static const char *const k_languages[8] = {
    "japanese", "english", "french", "german",
    "italian", "spanish", "chinese", "korean"
};

/* GBATEK's names for the sixteen favourite colours, in index order.        */
static const char *const k_colors[16] = {
    "gray", "brown", "red", "pink", "orange", "yellow", "lime", "green",
    "dark_green", "sea_green", "light_blue", "blue", "dark_blue",
    "dark_purple", "light_purple", "dark_pink"
};

/* ------------------------------------------------------------------ */
/* Defaults                                                            */
/* ------------------------------------------------------------------ */

void bw_firmware_defaults(BwFirmwareSettings *s)
{
    static const char nick[] = "Player";
    u32 i;

    memset(s, 0, sizeof *s);
    for (i = 0; nick[i] != '\0'; i++)
        s->nickname[i] = (u16)(u8)nick[i];
    s->nickname_len = (u8)i;
    s->birthday_month = 1;
    s->birthday_day = 1;
    s->favorite_color = 0;
    s->language = BW_FW_LANG_ENGLISH;
    s->backlight = 3;
    s->tp_raw_x1 = 512;
    s->tp_raw_y1 = 512;
    s->tp_x1 = 32;
    s->tp_y1 = 32;
    s->tp_raw_x2 = 3584;
    s->tp_raw_y2 = 2560;
    s->tp_x2 = 224;
    s->tp_y2 = 160;
    /* A locally administered address: not any vendor's.                    */
    s->mac[0] = 0x02;
    s->mac[5] = 0x01;
    s->channels = 0x3FFE;           /* channels 1-13, the usual value       */
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

static int lower_eq(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;

        if (ca != *b)
            return 0;
    }
    return *a == *b;
}

/* An unsigned number, decimal or 0x hex, the whole string.                 */
static int parse_uint(const char *v, unsigned long max, unsigned long *out)
{
    char *end;
    unsigned long n;

    if (*v == '\0' || *v == '-')
        return -1;
    n = strtoul(v, &end, 0);
    if (*end != '\0' || n > max)
        return -1;
    *out = n;
    return 0;
}

/* UTF-8 into UTF-16 code units, the Basic Multilingual Plane only.         */
static int parse_text(const char *v, u16 *out, u32 max, u8 *len_out)
{
    const u8 *p = (const u8 *)v;
    u16 tmp[BW_FW_MESSAGE_MAX];
    u32 n = 0;

    while (*p != '\0') {
        u32 c;

        if (p[0] < 0x80) {
            c = p[0];
            p += 1;
        } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            c = ((u32)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80
                   && (p[2] & 0xC0) == 0x80) {
            c = ((u32)(p[0] & 0x0F) << 12) | ((u32)(p[1] & 0x3F) << 6)
                | (p[2] & 0x3F);
            p += 3;
        } else {
            return -1;              /* not UTF-8, or outside the BMP        */
        }
        if (c >= 0xD800 && c < 0xE000)
            return -1;              /* an encoded surrogate is not text     */
        if (n == max)
            return -1;
        tmp[n++] = (u16)c;
    }
    /* Only a whole, valid value replaces the old one.                      */
    memset(out, 0, max * sizeof *out);
    memcpy(out, tmp, n * sizeof *out);
    *len_out = (u8)n;
    return 0;
}

static int parse_name(const char *v, const char *const *names, u32 count,
                      u8 *out)
{
    unsigned long n;
    u32 i;

    for (i = 0; i < count; i++) {
        if (lower_eq(v, names[i])) {
            *out = (u8)i;
            return 0;
        }
    }
    if (parse_uint(v, count - 1, &n) != 0)
        return -1;
    *out = (u8)n;
    return 0;
}

static int parse_line(BwFirmwareSettings *s, const char *key, const char *v)
{
    unsigned long a, b;

    if (lower_eq(key, "nickname"))
        return parse_text(v, s->nickname, BW_FW_NICKNAME_MAX, &s->nickname_len);
    if (lower_eq(key, "message"))
        return parse_text(v, s->message, BW_FW_MESSAGE_MAX, &s->message_len);
    if (lower_eq(key, "language"))
        return parse_name(v, k_languages, 8, &s->language);
    if (lower_eq(key, "favorite_color"))
        return parse_name(v, k_colors, 16, &s->favorite_color);
    if (lower_eq(key, "birthday")) {
        unsigned m, d;
        char extra;

        if (sscanf(v, "%u/%u%c", &m, &d, &extra) != 2
            || m < 1 || m > 12 || d < 1 || d > 31)
            return -1;
        s->birthday_month = (u8)m;
        s->birthday_day = (u8)d;
        return 0;
    }
    if (lower_eq(key, "alarm")) {
        unsigned h, m;
        char state[8];

        if (sscanf(v, "%u:%u %7s", &h, &m, state) != 3 || h > 23 || m > 59)
            return -1;
        if (lower_eq(state, "on"))
            s->alarm_enabled = 1;
        else if (lower_eq(state, "off"))
            s->alarm_enabled = 0;
        else
            return -1;
        s->alarm_hour = (u8)h;
        s->alarm_minute = (u8)m;
        return 0;
    }
    if (lower_eq(key, "backlight")) {
        if (parse_uint(v, 3, &a) != 0)
            return -1;
        s->backlight = (u8)a;
        return 0;
    }
    if (lower_eq(key, "touch_calibration")) {
        unsigned rx1, ry1, x1, y1, rx2, ry2, x2, y2;
        char extra;

        if (sscanf(v, "%u %u %u %u %u %u %u %u%c", &rx1, &ry1, &x1, &y1,
                   &rx2, &ry2, &x2, &y2, &extra) != 8
            || rx1 > 4095 || ry1 > 4095 || rx2 > 4095 || ry2 > 4095
            || x1 > 255 || y1 > 191 || x2 > 255 || y2 > 191
            || x1 == x2 || y1 == y2 || rx1 == rx2 || ry1 == ry2)
            return -1;
        s->tp_raw_x1 = (u16)rx1;
        s->tp_raw_y1 = (u16)ry1;
        s->tp_x1 = (u8)x1;
        s->tp_y1 = (u8)y1;
        s->tp_raw_x2 = (u16)rx2;
        s->tp_raw_y2 = (u16)ry2;
        s->tp_x2 = (u8)x2;
        s->tp_y2 = (u8)y2;
        return 0;
    }
    if (lower_eq(key, "rtc_offset")) {
        char *end;
        long long n = strtoll(v, &end, 0);

        if (*v == '\0' || *end != '\0')
            return -1;
        s->rtc_offset = (s64)n;
        return 0;
    }
    if (lower_eq(key, "mac")) {
        unsigned m[6];
        char extra;
        int i;

        if (sscanf(v, "%x:%x:%x:%x:%x:%x%c", &m[0], &m[1], &m[2], &m[3],
                   &m[4], &m[5], &extra) != 6)
            return -1;
        for (i = 0; i < 6; i++) {
            if (m[i] > 0xFF)
                return -1;
        }
        for (i = 0; i < 6; i++)
            s->mac[i] = (u8)m[i];
        return 0;
    }
    if (lower_eq(key, "wireless_channels")) {
        if (parse_uint(v, 0xFFFF, &b) != 0)
            return -1;
        s->channels = (u16)b;
        return 0;
    }
    return -1;
}

static char *trim(char *p)
{
    char *e;

    while (*p == ' ' || *p == '\t')
        p++;
    e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = '\0';
    return p;
}

int bw_firmware_parse(BwFirmwareSettings *s, const char *text,
                      char *err, size_t err_size)
{
    int bad = 0, line_no = 0;
    const char *p = text;

    if (err != NULL && err_size > 0)
        err[0] = '\0';
    while (*p != '\0') {
        char line[512];
        const char *nl = strchr(p, '\n');
        size_t n = nl != NULL ? (size_t)(nl - p) : strlen(p);
        char *key, *value = NULL, *eq;

        line_no++;
        if (n >= sizeof line)
            n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        p = nl != NULL ? nl + 1 : p + strlen(p);

        key = trim(line);
        if (*key == '\0' || *key == '#' || *key == ';')
            continue;
        eq = strchr(key, '=');
        if (eq != NULL) {
            *eq = '\0';
            value = trim(eq + 1);
            key = trim(key);
        }
        if (eq == NULL || parse_line(s, key, value) != 0) {
            if (bad == 0 && err != NULL && err_size > 0) {
                char shown[64];

                strncpy(shown, key, sizeof shown - 1);
                shown[sizeof shown - 1] = '\0';
                snprintf(err, err_size, "line %d: cannot use \"%s\"",
                         line_no, shown);
            }
            bad++;
        }
    }
    return bad;
}

/* ------------------------------------------------------------------ */
/* Formatting                                                          */
/* ------------------------------------------------------------------ */

static void put_utf8(char *out, const u16 *text, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        u32 c = text[i];

        if (c < 0x80) {
            *out++ = (char)c;
        } else if (c < 0x800) {
            *out++ = (char)(0xC0 | (c >> 6));
            *out++ = (char)(0x80 | (c & 0x3F));
        } else if (c >= 0xD800 && c < 0xE000) {
            *out++ = '?';           /* a lone surrogate has no UTF-8        */
        } else {
            *out++ = (char)(0xE0 | (c >> 12));
            *out++ = (char)(0x80 | ((c >> 6) & 0x3F));
            *out++ = (char)(0x80 | (c & 0x3F));
        }
    }
    *out = '\0';
}

int bw_firmware_format(const BwFirmwareSettings *s, char *out, size_t out_size)
{
    char nick[BW_FW_NICKNAME_MAX * 3 + 1];
    char msg[BW_FW_MESSAGE_MAX * 3 + 1];
    int n;

    put_utf8(nick, s->nickname, s->nickname_len);
    put_utf8(msg, s->message, s->message_len);
    n = snprintf(out, out_size,
        "# eidos console settings: what a DS owner enters in the system menu.\n"
        "# The port writes them where the console's firmware leaves them\n"
        "# before the game starts (docs/firmware.md).  Edit and restart.\n"
        "# A line the port cannot use is reported and ignored; a missing key\n"
        "# keeps its default.\n"
        "\n"
        "# Up to 10 characters, UTF-8.\n"
        "nickname = %s\n"
        "# Up to 26 characters, UTF-8.\n"
        "message = %s\n"
        "# month/day\n"
        "birthday = %u/%u\n"
        "# 0-15 or a name: gray brown red pink orange yellow lime green\n"
        "# dark_green sea_green light_blue blue dark_blue dark_purple\n"
        "# light_purple dark_pink\n"
        "favorite_color = %s\n"
        "# japanese english french german italian spanish chinese korean\n"
        "language = %s\n"
        "# HH:MM on|off\n"
        "alarm = %02u:%02u %s\n"
        "# 0 (dimmest) to 3\n"
        "backlight = %u\n"
        "# Two calibration points, each raw ADC x y then screen pixel x y.\n"
        "# The default maps pixel (x, y) to raw (16x, 16y).\n"
        "touch_calibration = %u %u %u %u %u %u %u %u\n"
        "# Seconds the owner set the clock ahead of the hardware (signed).\n"
        "rtc_offset = %lld\n"
        "# The console's wireless MAC address.\n"
        "mac = %02X:%02X:%02X:%02X:%02X:%02X\n"
        "# Bit n set = wireless channel n allowed.\n"
        "wireless_channels = 0x%04X\n",
        nick, msg, (unsigned)s->birthday_month, (unsigned)s->birthday_day,
        k_colors[s->favorite_color & 15], k_languages[s->language & 7],
        (unsigned)s->alarm_hour, (unsigned)s->alarm_minute,
        s->alarm_enabled ? "on" : "off", (unsigned)s->backlight,
        (unsigned)s->tp_raw_x1, (unsigned)s->tp_raw_y1,
        (unsigned)s->tp_x1, (unsigned)s->tp_y1,
        (unsigned)s->tp_raw_x2, (unsigned)s->tp_raw_y2,
        (unsigned)s->tp_x2, (unsigned)s->tp_y2,
        (long long)s->rtc_offset,
        (unsigned)s->mac[0], (unsigned)s->mac[1], (unsigned)s->mac[2],
        (unsigned)s->mac[3], (unsigned)s->mac[4], (unsigned)s->mac[5],
        (unsigned)s->channels);
    if (n < 0 || (size_t)n >= out_size)
        return -1;
    return n;
}

/* ------------------------------------------------------------------ */
/* The settings in force                                               */
/* ------------------------------------------------------------------ */

static void load_file(void)
{
    char path[1100];
    static char text[8192];
    long n;

    bw_firmware_defaults(&g_fw.current);
    g_fw.loaded = 1;
    if (bw_save_path(path, sizeof path, BW_FW_FILE_NAME) != 0)
        return;
    n = bw_save_read(path, (u8 *)text, sizeof text - 1);
    if (n < 0)
        return;
    if ((size_t)n > sizeof text - 1)
        n = (long)(sizeof text - 1);
    text[n] = '\0';
    {
        char err[160];
        int bad = bw_firmware_parse(&g_fw.current, text, err, sizeof err);

        if (bad != 0)
            fprintf(stderr, "eidos: %s: %d unusable line(s), first %s\n",
                    path, bad, err);
    }
}

const BwFirmwareSettings *bw_firmware_settings(void)
{
    if (!g_fw.overridden && !g_fw.loaded)
        load_file();
    return &g_fw.current;
}

void bw_firmware_set(const BwFirmwareSettings *s)
{
    if (s == NULL) {
        g_fw.overridden = 0;
        g_fw.loaded = 0;
        return;
    }
    g_fw.current = *s;
    g_fw.overridden = 1;
}

void bw_firmware_reload(void)
{
    if (!g_fw.overridden)
        g_fw.loaded = 0;
}

int bw_firmware_write_default(const char *path)
{
    char fallback[1100];
    char text[4096];
    BwFirmwareSettings s;
    FILE *f;
    int n;

    if (path == NULL) {
        if (bw_save_path(fallback, sizeof fallback, BW_FW_FILE_NAME) != 0)
            return -1;
        path = fallback;
    }
    f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
        return 1;
    }
    bw_firmware_defaults(&s);
    n = bw_firmware_format(&s, text, sizeof text);
    if (n < 0)
        return -1;
    return bw_save_write_atomic(path, text, (u32)n) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* The bytes                                                           */
/* ------------------------------------------------------------------ */

static void put16(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void put32(u8 *p, u32 v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

/* The 0x70 bytes the ARM7 copies to RAM: little-endian in the flash image,
 * in DS memory order (types.h, bw_mem_wr16) in the copy the game reads.    */
static void user_info_as(const BwFirmwareSettings *s, u8 out[BW_FW_USER_INFO_SIZE],
                         int ram)
{
#define put16(p, v) (ram ? bw_mem_wr16((p), (u16)(v)) : put16((p), (v)))
#define put32(p, v) (ram ? bw_mem_wr32((p), (u32)(v)) : put32((p), (v)))
    u32 i;
    u64 rtc = (u64)s->rtc_offset;

    memset(out, 0, BW_FW_USER_INFO_SIZE);
    out[0x00] = 5;                              /* settings version         */
    out[0x02] = (u8)(s->favorite_color & 0x0F);
    out[0x03] = s->birthday_month;
    out[0x04] = s->birthday_day;
    for (i = 0; i < BW_FW_NICKNAME_MAX; i++)
        put16(out + 0x06 + 2 * i, i < s->nickname_len ? s->nickname[i] : 0);
    put16(out + 0x1A, s->nickname_len);
    for (i = 0; i < BW_FW_MESSAGE_MAX; i++)
        put16(out + 0x1C + 2 * i, i < s->message_len ? s->message[i] : 0);
    put16(out + 0x50, s->message_len);
    out[0x52] = s->alarm_hour;
    out[0x53] = s->alarm_minute;
    out[0x56] = (u8)(s->alarm_enabled ? 1 : 0);
    put16(out + 0x58, s->tp_raw_x1);
    put16(out + 0x5A, s->tp_raw_y1);
    out[0x5C] = s->tp_x1;
    out[0x5D] = s->tp_y1;
    put16(out + 0x5E, s->tp_raw_x2);
    put16(out + 0x60, s->tp_raw_y2);
    out[0x62] = s->tp_x2;
    out[0x63] = s->tp_y2;
    put16(out + 0x64, (u32)(s->language & 7) | ((u32)(s->backlight & 3) << 4));
    out[0x66] = 0;                              /* year set: 2000           */
    if (ram) {
        memcpy(out + 0x68, &rtc, 8);        /* one s64 in DS memory order */
    } else {
        put32(out + 0x68, (u32)rtc);
        put32(out + 0x6C, (u32)(rtc >> 32));
    }
#undef put16
#undef put32
}

static void user_info(const BwFirmwareSettings *s, u8 out[BW_FW_USER_INFO_SIZE])
{
    user_info_as(s, out, 0);
}

u16 bw_firmware_crc16(u16 crc, const u8 *data, u32 size)
{
    u32 i;
    int b;

    for (i = 0; i < size; i++) {
        crc ^= data[i];
        for (b = 0; b < 8; b++)
            crc = (u16)((crc & 1) ? (crc >> 1) ^ 0xA001u : crc >> 1);
    }
    return crc;
}

void bw_firmware_user_block(const BwFirmwareSettings *s, u16 counter,
                            u8 out[0x100])
{
    memset(out, 0, 0x100);
    user_info(s, out);
    put16(out + 0x70, counter);
    put16(out + 0x72, bw_firmware_crc16(0xFFFF, out, BW_FW_USER_INFO_SIZE));
}

void bw_firmware_apply(void)
{
    const BwFirmwareSettings *s = bw_firmware_settings();
    u8 info[BW_FW_USER_INFO_SIZE];

#if defined(EIDOS_SSO)
    user_info_as(s, info, 0);          /* OSOwnerInfo is a game struct */
#else
    user_info_as(s, info, 1);
#endif
    memcpy(bw_ds_ptr(BW_FW_USER_INFO_ADDR), info, sizeof info);
    memcpy(bw_ds_ptr(BW_FW_MAC_ADDR), s->mac, 6);
    bw_mem_wr16((u8 *)bw_ds_ptr(BW_FW_CHANNELS_ADDR), s->channels);
}
