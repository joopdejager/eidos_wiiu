/*
 * engine/audio/sdat.c -- the SDAT container: SYMB, INFO, FAT and FILE.
 *
 * The layout is aletheia's tools/assets/sdat.py, which established it
 * against this ROM's own wb_sound_data.sdat and documents every field in
 * its module docstring.  The three record decodes this engine needs are
 * that module's seq_record(), bank_record() and wavarc_fileid(), including
 * the correction that the SEQ record's bytes +4:+6 are one little-endian
 * u16 holding a *direct* BANK-table index (940 of 940 populated SEQ records
 * resolve to a real BANK record that way, and the resolved BANK's SYMB name
 * matches the SEQ's own name); there is no PLAYER indirection.  PLAYER is
 * decoded here too, because the port will need its channel bitmask and heap
 * size later, and because decoding it is what settled that question.
 */

#include "audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* The archive's numbers, in the data root's order: a big-endian host's
 * extractor turns the whole structure (sdk/sdat_order.h), so these read it
 * as the matched code does, and the cartridge's order on a little-endian
 * host. */
/* ------------------------------------------------------------------ */

static u16 rd16(const u8 *p) { return bw_dr_rd16(p); }
static u32 rd32(const u8 *p) { return bw_dr_rd32(p); }

static int section(const u8 *data, size_t size, u32 pos, const char *magic,
                   const u8 **body, size_t *body_size)
{
    u32 sz;
    if ((size_t)pos + 8 > size) {
        fprintf(stderr, "sdat: section %s at 0x%X runs past the end\n", magic, pos);
        return -1;
    }
    if (memcmp(data + pos, magic, 4) != 0) {
        fprintf(stderr, "sdat: expected %s at 0x%X, found %.4s\n", magic, pos,
                (const char *)(data + pos));
        return -1;
    }
    sz = rd32(data + pos + 4);
    if ((size_t)pos + sz > size) {
        fprintf(stderr, "sdat: section %s claims 0x%X bytes, past the end\n", magic, sz);
        return -1;
    }
    *body = data + pos;
    *body_size = sz;
    return 0;
}

int bw_sdat_open(BwSdat *s, const void *data, size_t size)
{
    const u8 *d = (const u8 *)data;
    u32 symb_off, symb_size, info_off, info_size, fat_off, fat_size, file_off;
    const u8 *body;
    size_t body_size;
    int i;

    memset(s, 0, sizeof(*s));
    if (size < 0x40 || memcmp(d, "SDAT", 4) != 0) {
        fprintf(stderr, "sdat: not an SDAT image\n");
        return -1;
    }
    s->data = d;
    s->size = size;

    symb_off  = rd32(d + 0x10);
    symb_size = rd32(d + 0x14);
    info_off  = rd32(d + 0x18);
    info_size = rd32(d + 0x1C);
    fat_off   = rd32(d + 0x20);
    fat_size  = rd32(d + 0x24);
    file_off  = rd32(d + 0x28);
    (void)info_size;
    (void)fat_size;

    if (section(d, size, info_off, "INFO", &body, &body_size) != 0) return -1;
    s->info = body;
    s->info_size = body_size;

    if (symb_size != 0) {
        if (section(d, size, symb_off, "SYMB", &body, &body_size) != 0) return -1;
        s->symb = body;
        s->symb_size = body_size;
    }

    if (section(d, size, fat_off, "FAT ", &body, &body_size) != 0) return -1;
    s->fat = body;
    s->fat_count = rd32(body + 8);
    if (0xC + (size_t)s->fat_count * 16 > body_size) {
        fprintf(stderr, "sdat: FAT declares %u entries, past its own block\n",
                s->fat_count);
        return -1;
    }

    /* The FILE block's own sub-headers are redundant with FAT for reading
     * purposes (sdat.py's docstring says so and cross-checks the size); it
     * is validated for presence and then not walked. */
    if (section(d, size, file_off, "FILE", &body, &body_size) != 0) return -1;

    for (i = 0; i < BW_SDAT_KIND_COUNT; i++) {
        u32 off = rd32(s->info + 8 + (u32)i * 4);
        s->rec_table[i] = off;
        if (off != 0 && (size_t)off + 4 <= s->info_size) {
            u32 count = rd32(s->info + off);
            if ((size_t)off + 4 + (size_t)count * 4 <= s->info_size) {
                s->count[i] = count;
            } else {
                s->rec_table[i] = 0;
            }
        } else {
            s->rec_table[i] = 0;
        }
        if (s->symb != NULL) {
            u32 soff = rd32(s->symb + 8 + (u32)i * 4);
            if (soff != 0 && (size_t)soff + 4 <= s->symb_size) {
                u32 scount = rd32(s->symb + soff);
                if ((size_t)soff + 4 + (size_t)scount * 4 <= s->symb_size
                    && scount == s->count[i]) {
                    s->symb_table[i] = soff;
                }
            }
        }
    }
    return 0;
}

int bw_sdat_load(BwSdat *s, const char *path)
{
    FILE *fh = fopen(path, "rb");
    long n;
    u8 *buf;

    memset(s, 0, sizeof(*s));
    if (fh == NULL) {
        fprintf(stderr, "sdat: cannot open %s\n", path);
        return -1;
    }
    if (fseek(fh, 0, SEEK_END) != 0 || (n = ftell(fh)) < 0) {
        fclose(fh);
        fprintf(stderr, "sdat: cannot size %s\n", path);
        return -1;
    }
    rewind(fh);
    buf = (u8 *)malloc((size_t)n);
    if (buf == NULL || fread(buf, 1, (size_t)n, fh) != (size_t)n) {
        free(buf);
        fclose(fh);
        fprintf(stderr, "sdat: cannot read %s\n", path);
        return -1;
    }
    fclose(fh);
    if (bw_sdat_open(s, buf, (size_t)n) != 0) {
        free(buf);
        return -1;
    }
    s->owned = buf;
    return 0;
}

void bw_sdat_close(BwSdat *s)
{
    if (s == NULL) return;
    free(s->owned);
    memset(s, 0, sizeof(*s));
}

u32 bw_sdat_count(const BwSdat *s, BwSdatKind kind)
{
    if (s == NULL || (int)kind < 0 || kind >= BW_SDAT_KIND_COUNT) return 0;
    return s->count[kind];
}

const char *bw_sdat_name(const BwSdat *s, BwSdatKind kind, u32 index)
{
    u32 table, str_off;
    if (s == NULL || s->symb == NULL) return NULL;
    if ((int)kind < 0 || kind >= BW_SDAT_KIND_COUNT) return NULL;
    table = s->symb_table[kind];
    if (table == 0 || index >= s->count[kind]) return NULL;
    str_off = rd32(s->symb + table + 4 + index * 4);
    if (str_off == 0 || (size_t)str_off >= s->symb_size) return NULL;
    /* The block is NUL-terminated data inside a bounded buffer; a missing
     * terminator would run off the end, so it is checked. */
    if (memchr(s->symb + str_off, 0, s->symb_size - str_off) == NULL) return NULL;
    return (const char *)(s->symb + str_off);
}

long bw_sdat_find(const BwSdat *s, BwSdatKind kind, const char *name)
{
    u32 i, n = bw_sdat_count(s, kind);
    for (i = 0; i < n; i++) {
        const char *have = bw_sdat_name(s, kind, i);
        if (have != NULL && strcmp(have, name) == 0) return (long)i;
    }
    return -1;
}

/* The INFO record pointer for one slot, or 0 when the slot is absent. */
static u32 info_ptr(const BwSdat *s, BwSdatKind kind, u32 index, size_t need)
{
    u32 table, ptr;
    if (s == NULL || (int)kind < 0 || kind >= BW_SDAT_KIND_COUNT) return 0;
    table = s->rec_table[kind];
    if (table == 0 || index >= s->count[kind]) return 0;
    ptr = rd32(s->info + table + 4 + index * 4);
    if (ptr == 0 || (size_t)ptr + need > s->info_size) return 0;
    return ptr;
}

int bw_sdat_seq_record(const BwSdat *s, u32 index, BwSdatSeqRecord *out)
{
    u32 p = info_ptr(s, BW_SDAT_SEQ, index, 12);
    if (p == 0) return -1;
    out->file_id          = rd32(s->info + p);
    out->bank             = rd16(s->info + p + 4);
    out->volume           = s->info[p + 6];
    out->channel_pressure = s->info[p + 7];
    out->player_priority  = s->info[p + 8];
    out->player_no        = s->info[p + 9];
    return 0;
}

int bw_sdat_bank_record(const BwSdat *s, u32 index, BwSdatBankRecord *out)
{
    u32 p = info_ptr(s, BW_SDAT_BANK, index, 12);
    int i;
    if (p == 0) return -1;
    out->file_id = rd32(s->info + p);
    for (i = 0; i < 4; i++) out->wave_arc[i] = rd16(s->info + p + 4 + i * 2);
    return 0;
}

int bw_sdat_player_record(const BwSdat *s, u32 index, BwSdatPlayerRecord *out)
{
    u32 p = info_ptr(s, BW_SDAT_PLAYER, index, 8);
    if (p == 0) return -1;
    out->seq_max         = s->info[p];
    out->channel_bitmask = rd16(s->info + p + 2);
    out->heap_size       = rd32(s->info + p + 4);
    return 0;
}

int bw_sdat_wavarc_fileid(const BwSdat *s, u32 index, u32 *out)
{
    u32 p = info_ptr(s, BW_SDAT_WAVARC, index, 4);
    if (p == 0) return -1;
    *out = rd32(s->info + p);
    return 0;
}

int bw_sdat_raw_file(const BwSdat *s, u32 file_id, BwBlob *out)
{
    u32 off, size;
    if (s == NULL || file_id >= s->fat_count) return -1;
    off  = rd32(s->fat + 0xC + file_id * 16);
    size = rd32(s->fat + 0xC + file_id * 16 + 4);
    if ((size_t)off + size > s->size) return -1;
    out->data = s->data + off;
    out->size = size;
    return 0;
}
