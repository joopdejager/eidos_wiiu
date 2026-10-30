/*
 * engine/audio/sbnk.c -- the SBNK instrument bank.
 *
 * Layout: aletheia's tools/assets/sbnk.py, verified there across all 1497
 * real SBNKs in this ROM (30046 instrument-table entries).  The two facts
 * that are not the obvious reading, and that the asset lane had to correct,
 * are both honoured here:
 *
 *   - the shared region record is **10 bytes**, not 12: u16 wave_id, u16
 *     wave_arc, u8 base_note, u8 attack, u8 decay, u8 sustain, u8 release,
 *     u8 pan.  wave_id comes *first*; it is the second u16 that is the
 *     small 0-3 in-bank archive slot.  Reading it 12 bytes wide bleeds into
 *     the next structure and only the first region of a type 16/17
 *     instrument survives (4.1% of later regions were plausible before the
 *     fix, 100% after).
 *   - a type 16/17 region entry is a u16 region_type prefix followed by
 *     that 10-byte record, so 12 bytes per region.
 *
 * Types 2 and 3 are PSG: the same 10-byte shape, but type 2's leading u16
 * is a duty selector (0-7) rather than a wave id and type 3 carries no wave
 * parameter at all.  Neither ever resolves a wave archive.
 *
 * Region selection (sseqsynth.py's resolve_instrument_region): type 16 is a
 * drum kit with one region per note in [low_note, high_note], indexed
 * directly by played_note - low_note and clamped; type 17 is a key split
 * whose ascending nonzero split points pick the first region whose split
 * point is >= the played note, falling back to the last region.  Types 0,
 * 4 and 5 have no playable region here, which is a documented gap rather
 * than an error.
 */

#include "audio.h"

#include <string.h>

/* The bank's numbers, in the data root's order (sdk/sdat_order.h: a
 * big-endian host's extractor turns them, as the matched code reads them). */
static u16 rd16(const u8 *p) { return bw_dr_rd16(p); }
static u32 rd32(const u8 *p) { return bw_dr_rd32(p); }

int bw_sbnk_open(BwSbnk *b, BwBlob blob)
{
    memset(b, 0, sizeof(*b));
    if (blob.size < 0x3C || memcmp(blob.data, "SBNK", 4) != 0) return -1;
    if (memcmp(blob.data + 0x10, "DATA", 4) != 0) return -1;
    b->data = blob.data;
    b->size = blob.size;
    b->instrument_count = rd32(blob.data + 0x38);
    if (0x3C + (size_t)b->instrument_count * 4 > blob.size) {
        b->instrument_count = 0;
        return -1;
    }
    return 0;
}

int bw_sbnk_instrument_type(const BwSbnk *b, u32 program)
{
    if (b == NULL || program >= b->instrument_count) return -1;
    return (int)(rd32(b->data + 0x3C + program * 4) & 0xFF);
}

/* The 10-byte record shared by types 1-5 and by every type 16/17 region. */
static int parse_record(const BwSbnk *b, size_t off, BwRegion *out)
{
    if (off + 10 > b->size) return -1;
    out->kind      = BW_REGION_WAVE;
    out->wave_id   = rd16(b->data + off);
    out->wave_arc  = rd16(b->data + off + 2);
    out->duty      = 0;
    out->base_note = b->data[off + 4];
    out->attack    = b->data[off + 5];
    out->decay     = b->data[off + 6];
    out->sustain   = b->data[off + 7];
    out->release   = b->data[off + 8];
    out->pan       = b->data[off + 9];
    return 0;
}

int bw_sbnk_resolve(const BwSbnk *b, int program, int played_note, BwRegion *out)
{
    u32 entry;
    int rec_type;
    size_t off;

    memset(out, 0, sizeof(*out));
    if (b == NULL || program < 0 || (u32)program >= b->instrument_count) return -1;
    entry = rd32(b->data + 0x3C + (u32)program * 4);
    rec_type = (int)(entry & 0xFF);
    off = (size_t)((entry >> 8) & 0xFFFF);

    if (rec_type == 1) {
        return parse_record(b, off, out);
    }
    if (rec_type == 2) {
        if (parse_record(b, off, out) != 0) return -1;
        out->duty = out->wave_id;
        out->wave_id = 0;
        out->wave_arc = 0;
        out->kind = BW_REGION_PSG_SQUARE;
        return 0;
    }
    if (rec_type == 3) {
        if (parse_record(b, off, out) != 0) return -1;
        out->duty = 0;
        out->wave_id = 0;
        out->wave_arc = 0;
        out->kind = BW_REGION_PSG_NOISE;
        return 0;
    }
    if (rec_type == 16) {
        int low, high, count, idx;
        if (off + 2 > b->size) return -1;
        low  = b->data[off];
        high = b->data[off + 1];
        count = high - low + 1;
        if (count <= 0) return -1;
        idx = played_note - low;
        if (idx < 0) idx = 0;
        if (idx > count - 1) idx = count - 1;
        /* region entry = u16 region_type + the 10-byte record */
        return parse_record(b, off + 2 + (size_t)idx * 12 + 2, out);
    }
    if (rec_type == 17) {
        u8 splits[8];
        int i, count = 0, chosen = -1, seen = 0;
        if (off + 8 > b->size) return -1;
        memcpy(splits, b->data + off, 8);
        for (i = 0; i < 8; i++) if (splits[i] != 0) count++;
        if (count == 0) return -1;
        /* zip(nonzero split points, regions): the n-th nonzero split point
         * belongs to the n-th region. */
        for (i = 0; i < 8 && seen < count; i++) {
            if (splits[i] == 0) continue;
            if (played_note <= (int)splits[i]) { chosen = seen; break; }
            seen++;
        }
        if (chosen < 0) chosen = count - 1;
        return parse_record(b, off + 8 + (size_t)chosen * 12 + 2, out);
    }
    return -1;   /* type 0 (empty), and types 4/5 which carry no region here */
}
