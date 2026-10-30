/*
 * sdk/vfs_msg.c -- parse `.msg` files and merge their lines into a bank.
 * The grammar and the merge rules are in vfs_msg.h; the bank format and the
 * cipher are in docs/research/limits/text.md.
 *
 * Bank layout used here (one language section, as in every real bank):
 *
 *   0x00 u16 langCount (1)   0x02 u16 count   0x04 u32 size   0x08 u32 0
 *   0x0C u32 langOff[0] (16)
 *   section: u32 size, then count * { u32 offset, u16 length, u16 flags },
 *   then the strings back to back.  `offset` is from the section start and
 *   `length` counts units including the 0xFFFF end code.
 *
 * Untouched lines are copied byte for byte (their ciphertext depends on
 * their index, not on where they sit), which is why a merge never disturbs
 * a line no mod mentions.  A new line copies the flags word of the first
 * line of the bank, because the game never reads it and a bank uses one
 * value throughout.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "vfs_msg.h"

#define MSG_END     0xFFFFu
#define MSG_NEWLINE 0xFFFEu
#define MSG_CONTROL 0xF000u

/* A bank's numbers and units are in the data root's order (sdk/types.h,
 * bw_dr_rd16): a big-endian host's extractor turned them. */
static u32 rd32(const u8 *p)
{
    return bw_dr_rd32(p);
}

static u32 rd16(const u8 *p)
{
    return bw_dr_rd16(p);
}

static void wr32(u8 *p, u32 v)
{
    bw_dr_wr32(p, v);
}

static void wr16(u8 *p, u32 v)
{
    bw_dr_wr16(p, (u16)v);
}

void bw_msg_cipher(u16 *units, u32 count, u32 index)
{
    u16 key = (u16)((index + 3u) * 0x2983u);
    u32 i;

    for (i = 0; i < count; i++) {
        units[i] = (u16)(units[i] ^ key);
        key = (u16)((key << 3) | (key >> 13));
    }
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

typedef struct Units {
    u16 *p;
    u32  n;
    u32  cap;
} Units;

static int units_push(Units *u, u32 v)
{
    if (u->n == u->cap) {
        u32 cap = u->cap == 0 ? 32 : u->cap * 2;
        u16 *g = (u16 *)realloc(u->p, cap * sizeof(u16));

        if (g == NULL) {
            return -1;
        }
        u->p = g;
        u->cap = cap;
    }
    u->p[u->n++] = (u16)v;
    return 0;
}

static void set_err(char *err, size_t size, u32 line, const char *what)
{
    if (err != NULL && size > 0) {
        snprintf(err, size, "line %u: %s", (unsigned)line, what);
    }
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* One to four hex digits in [s, e).  Returns the value or -1. */
static long hex_field(const char *s, const char *e)
{
    long v = 0;

    if (s >= e || e - s > 4) {
        return -1;
    }
    for (; s < e; s++) {
        int d = hex_digit(*s);

        if (d < 0) {
            return -1;
        }
        v = v * 16 + d;
    }
    return v;
}

/* Decode one UTF-8 sequence at *pp (before end).  Returns the code point or -1. */
static long utf8_next(const unsigned char **pp, const unsigned char *end)
{
    const unsigned char *p = *pp;
    unsigned c = *p;
    long cp;
    int extra;
    int i;

    if (c < 0x80) {
        *pp = p + 1;
        return (long)c;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        extra = 1;
        cp = (long)(c & 0x1F);
    } else if (c >= 0xE0 && c <= 0xEF) {
        extra = 2;
        cp = (long)(c & 0x0F);
    } else if (c >= 0xF0 && c <= 0xF4) {
        extra = 3;
        cp = (long)(c & 0x07);
    } else {
        return -1;
    }
    if (end - p <= extra) {
        return -1;
    }
    for (i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            return -1;
        }
        cp = (cp << 6) | (long)(p[i] & 0x3F);
    }
    if ((extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return -1;
    }
    *pp = p + extra + 1;
    return cp;
}

#define NOMEM "out of memory"

/*
 * Encode the text [s, e) of one entry.  Returns NULL on success, else a
 * static message.
 */
static const char *parse_text(const char *s, const char *e, Units *u)
{
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *end = (const unsigned char *)e;

    while (p < end) {
        unsigned c = *p;

        if (c == '\\') {
            if (p + 1 >= end) {
                return "unknown escape at the end of the line";
            }
            if (p[1] == 'n') {
                if (units_push(u, MSG_NEWLINE) != 0) return NOMEM;
            } else if (p[1] == '\\' || p[1] == '{' || p[1] == '}') {
                if (units_push(u, p[1]) != 0) return NOMEM;
            } else {
                return "unknown escape";
            }
            p += 2;
        } else if (c == '{') {
            const unsigned char *close = p + 1;
            const char *body;
            const char *bend;
            const char *colon;
            long kind;
            u32 argc = 0;
            u32 argpos;

            while (close < end && *close != '}') {
                close++;
            }
            if (close >= end) {
                return "control code without a closing brace";
            }
            body = (const char *)p + 1;
            bend = (const char *)close;
            if (bend - body >= 2 && (body[0] == 'U' || body[0] == 'u') && body[1] == '+') {
                long v = hex_field(body + 2, bend);

                if (v < 0) return "bad {U+XXXX} unit";
                if (v == (long)MSG_END) return "{U+FFFF} would end the string";
                if (units_push(u, (u32)v) != 0) return NOMEM;
                p = close + 1;
                continue;
            }
            colon = (const char *)memchr(body, ':', (size_t)(bend - body));
            kind = hex_field(body, colon != NULL ? colon : bend);
            if (kind < 0) return "bad control code kind";
            if (units_push(u, MSG_CONTROL) != 0 || units_push(u, (u32)kind) != 0 ||
                units_push(u, 0) != 0) {
                return NOMEM;
            }
            argpos = u->n - 1;
            if (colon != NULL && colon + 1 < bend) {
                const char *a = colon + 1;

                for (;;) {
                    const char *comma = (const char *)memchr(a, ',', (size_t)(bend - a));
                    const char *ae = comma != NULL ? comma : bend;
                    long v = hex_field(a, ae);

                    if (v < 0) return "bad control code argument";
                    if (units_push(u, (u32)v) != 0) return NOMEM;
                    argc++;
                    if (comma == NULL) break;
                    a = comma + 1;
                }
            }
            u->p[argpos] = (u16)argc;
            p = close + 1;
        } else {
            long cp = utf8_next(&p, end);

            if (cp < 0) {
                return "invalid UTF-8";
            }
            if (cp >= 0x10000) {
                cp -= 0x10000;
                if (units_push(u, (u32)(0xD800 + (cp >> 10))) != 0 ||
                    units_push(u, (u32)(0xDC00 + (cp & 0x3FF))) != 0) {
                    return NOMEM;
                }
            } else if (cp == (long)MSG_END) {
                return "U+FFFF would end the string";
            } else if (units_push(u, (u32)cp) != 0) {
                return NOMEM;
            }
        }
    }
    return NULL;
}

void bw_msg_lines_free(BwMsgLines *lines)
{
    u32 i;

    if (lines == NULL) {
        return;
    }
    for (i = 0; i < lines->count; i++) {
        free(lines->lines[i].units);
    }
    free(lines->lines);
    memset(lines, 0, sizeof(*lines));
}

static int lines_add(BwMsgLines *out, u32 index, Units *u)
{
    if (out->count == out->cap) {
        u32 cap = out->cap == 0 ? 16 : out->cap * 2;
        BwMsgLine *g = (BwMsgLine *)realloc(out->lines, cap * sizeof(*g));

        if (g == NULL) {
            return -1;
        }
        out->lines = g;
        out->cap = cap;
    }
    out->lines[out->count].index = index;
    out->lines[out->count].units = u->p;
    out->lines[out->count].len = u->n;
    out->count++;
    u->p = NULL;
    u->n = u->cap = 0;
    return 0;
}

int bw_msg_parse(const char *text, size_t len, BwMsgLines *out,
                 char *err, size_t err_size)
{
    const char *p = text;
    const char *end = text + len;
    u32 lineno = 0;

    memset(out, 0, sizeof(*out));
    if (err != NULL && err_size > 0) {
        err[0] = '\0';
    }
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) {
        p += 3;
    }
    while (p < end) {
        const char *eol = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *le = eol != NULL ? eol : end;
        const char *next = eol != NULL ? eol + 1 : end;
        const char *q = p;
        unsigned long idx = 0;
        int digits = 0;
        Units u;
        const char *msg;

        lineno++;
        if (le > p && le[-1] == '\r') {
            le--;
        }
        while (q < le && (*q == ' ' || *q == '\t')) {
            q++;
        }
        if (q == le || *p == '#') {
            p = next;
            continue;
        }
        q = p;
        while (q < le && *q >= '0' && *q <= '9') {
            if (idx <= BW_MSG_MAX_INDEX) {
                idx = idx * 10 + (unsigned long)(*q - '0');
            }
            q++;
            digits++;
        }
        if (digits == 0) {
            set_err(err, err_size, lineno, "an entry must start with a decimal index");
            goto fail;
        }
        if (idx > BW_MSG_MAX_INDEX) {
            set_err(err, err_size, lineno, "index past 65534");
            goto fail;
        }
        while (q < le && (*q == ' ' || *q == '\t')) {
            q++;
        }
        if (q >= le || *q != '=') {
            set_err(err, err_size, lineno, "expected '=' after the index");
            goto fail;
        }
        q++;
        if (q < le && *q == ' ') {
            q++;
        }
        if ((size_t)(le - q) >= 8 && memcmp(q, "{packed}", 8) == 0) {
            set_err(err, err_size, lineno, "{packed} lines are not supported in this version");
            goto fail;
        }
        memset(&u, 0, sizeof(u));
        msg = parse_text(q, le, &u);
        if (msg == NULL && u.n > 65534u) {
            msg = "text longer than 65534 units";
        }
        if (msg == NULL && lines_add(out, (u32)idx, &u) != 0) {
            msg = NOMEM;
        }
        if (msg != NULL) {
            free(u.p);
            set_err(err, err_size, lineno, msg);
            goto fail;
        }
        p = next;
    }
    return 0;

fail:
    bw_msg_lines_free(out);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Merge                                                               */
/* ------------------------------------------------------------------ */

void bw_msg_free(u8 *image)
{
    free(image);
}

int bw_msg_merge(const u8 *bank, u32 bank_size,
                 const BwMsgLines *const *mods, u32 mod_count,
                 u8 **out, u32 *out_size)
{
    u32 count;
    u32 sect;
    u32 new_count;
    u32 flags = 0;
    const BwMsgLine **pick;
    u8 *img;
    unsigned long long total;
    u32 i, m, k;
    u32 off;

    *out = NULL;
    *out_size = 0;
    if (bank == NULL || bank_size < 20 || rd16(bank) != 1) {
        return -1;
    }
    count = rd16(bank + 2);
    sect = rd32(bank + 12);
    if (sect < 16 || (unsigned long long)sect + 4 + 8ull * count > bank_size) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        const u8 *e = bank + sect + 4 + 8 * i;
        unsigned long long s = (unsigned long long)sect + rd32(e);
        unsigned long long t = s + 2ull * rd16(e + 4);

        if (rd16(e + 4) == 0 || t > bank_size) {
            return -1;
        }
    }
    if (count > 0) {
        flags = rd16(bank + sect + 4 + 6);
    }

    new_count = count;
    for (m = 0; m < mod_count; m++) {
        for (k = 0; mods[m] != NULL && k < mods[m]->count; k++) {
            if (mods[m]->lines[k].index > BW_MSG_MAX_INDEX) {
                return -1;
            }
            if (mods[m]->lines[k].index + 1 > new_count) {
                new_count = mods[m]->lines[k].index + 1;
            }
        }
    }
    pick = (const BwMsgLine **)calloc(new_count == 0 ? 1 : new_count, sizeof(*pick));
    if (pick == NULL) {
        return -1;
    }
    for (m = 0; m < mod_count; m++) {
        for (k = 0; mods[m] != NULL && k < mods[m]->count; k++) {
            pick[mods[m]->lines[k].index] = &mods[m]->lines[k];
        }
    }

    total = 16ull + 4 + 8ull * new_count;
    for (i = 0; i < new_count; i++) {
        if (pick[i] != NULL) {
            total += 2ull * (pick[i]->len + 1);
        } else if (i < count) {
            total += 2ull * rd16(bank + sect + 4 + 8 * i + 4);
        } else {
            total += 2;
        }
    }
    if (total > 0xFFFFFFF0ull) {
        free(pick);
        return -1;
    }
    img = (u8 *)calloc((size_t)total, 1);
    if (img == NULL) {
        free(pick);
        return -1;
    }
    wr16(img, 1);
    wr16(img + 2, new_count);
    wr32(img + 4, (u32)total - 16);
    wr32(img + 8, 0);
    wr32(img + 12, 16);
    wr32(img + 16, (u32)total - 16);
    off = 4 + 8 * new_count;
    for (i = 0; i < new_count; i++) {
        u8 *ent = img + 16 + 4 + 8 * i;
        u8 *dst = img + 16 + off;
        u32 units;
        u32 eflags = flags;

        if (pick[i] != NULL) {
            u32 j;
            u32 n = pick[i]->len;
            u16 *tmp = (u16 *)malloc((n + 1) * sizeof(u16));

            if (tmp == NULL) {
                free(img);
                free(pick);
                return -1;
            }
            if (n > 0) {
                memcpy(tmp, pick[i]->units, n * sizeof(u16));
            }
            tmp[n] = MSG_END;
            bw_msg_cipher(tmp, n + 1, i);
            for (j = 0; j <= n; j++) {
                wr16(dst + 2 * j, tmp[j]);
            }
            free(tmp);
            units = n + 1;
            if (i < count) {
                eflags = rd16(bank + sect + 4 + 8 * i + 6);
            }
        } else if (i < count) {
            const u8 *e = bank + sect + 4 + 8 * i;

            units = rd16(e + 4);
            eflags = rd16(e + 6);
            memcpy(dst, bank + sect + rd32(e), 2ull * units);
        } else {
            u16 t = MSG_END;

            bw_msg_cipher(&t, 1, i);
            wr16(dst, t);
            units = 1;
        }
        wr32(ent, off);
        wr16(ent + 4, units);
        wr16(ent + 6, eflags);
        off += 2 * units;
    }
    free(pick);
    *out = img;
    *out_size = (u32)total;
    return 0;
}
