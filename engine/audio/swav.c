/*
 * engine/audio/swav.c -- SWAR wave archives, SWAV waves, and the three
 * sample encodings the DS channel hardware reads.
 *
 * Layout: aletheia's tools/assets/swav.py.  A SWAR is the generic NitroSDK
 * section shape with a wave count at 0x38 and one absolute file offset per
 * wave at 0x3C; a wave's byte length is not stored, it is the distance to
 * the next wave's offset (or to the end of the archive for the last).  A
 * SWAV record is u8 wave_type, u8 loop_flag, u16 sample_rate, u16
 * timer_val, u16 loop_offset (in 4-byte words), u32 nonloop_length (in
 * 4-byte words), then (loop_offset + nonloop_length) * 4 bytes of sample
 * data.
 *
 * PCM8 is signed 8-bit, decoded to s16 by multiplying by 256 (which is
 * swav.py's decode and round-trips exactly).  PCM16 is signed 16-bit
 * little-endian; it was never observed in this ROM's archives, so the path
 * is implemented from the record layout and not exercised by real data.
 * IMA-ADPCM is a 4-byte seed header (s16 initial predictor, u8 initial step
 * index, u8 reserved) then standard IMA nibbles, low nibble first: the
 * step and index tables here are the public 1992 IMA reference tables, the
 * same ones tools/assets/adpcm.py carries, and the decoder is that module's
 * decode_block() -- the header's predictor is the loop-in value and is not
 * itself a decoded sample.
 */

#include "audio.h"

#include <string.h>

/* The archive's table and each wave's header, in the data root's order
 * (sdk/sdat_order.h: a big-endian host's extractor turns them, as the
 * matched code reads them). */
static u16 rd16(const u8 *p) { return bw_dr_rd16(p); }
static u32 rd32(const u8 *p) { return bw_dr_rd32(p); }

/* The samples, which nothing turns: the DS's little-endian. */
static u16 sample16(const u8 *p) { return bw_rd16(p); }

static const s32 bw_ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
    2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767
};

static const s32 bw_ima_index[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

int bw_swar_open(BwSwar *a, BwBlob blob)
{
    memset(a, 0, sizeof(*a));
    if (blob.size < 0x3C || memcmp(blob.data, "SWAR", 4) != 0) return -1;
    if (memcmp(blob.data + 0x10, "DATA", 4) != 0) return -1;
    a->data = blob.data;
    a->size = blob.size;
    a->wave_count = rd32(blob.data + 0x38);
    if (0x3C + (size_t)a->wave_count * 4 > blob.size) {
        a->wave_count = 0;
        return -1;
    }
    return 0;
}

int bw_swar_wave(const BwSwar *a, u32 index, BwSwav *out)
{
    size_t off, end, data_start, total;

    memset(out, 0, sizeof(*out));
    if (a == NULL || index >= a->wave_count) return -1;
    off = rd32(a->data + 0x3C + index * 4);
    end = (index + 1 < a->wave_count)
          ? (size_t)rd32(a->data + 0x3C + (index + 1) * 4)
          : a->size;
    if (off + 12 > a->size || end > a->size) return -1;

    out->wave_type            = a->data[off];
    out->loop_flag            = a->data[off + 1];
    out->sample_rate          = rd16(a->data + off + 2);
    out->timer_val            = rd16(a->data + off + 4);
    out->loop_offset_words    = rd16(a->data + off + 6);
    out->nonloop_length_words = rd32(a->data + off + 8);

    data_start = off + 12;
    total = ((size_t)out->loop_offset_words + out->nonloop_length_words) * 4;
    if (data_start + total > end) return -1;
    out->sample_data  = a->data + data_start;
    out->sample_bytes = total;
    return 0;
}

size_t bw_swav_decoded_count(const BwSwav *w)
{
    switch (w->wave_type) {
    case BW_WAVE_PCM8:  return w->sample_bytes;
    case BW_WAVE_PCM16: return w->sample_bytes / 2;
    case BW_WAVE_ADPCM: return w->sample_bytes >= 4 ? (w->sample_bytes - 4) * 2 : 0;
    default:            return 0;
    }
}

static s32 clamp_s32(s32 v, s32 lo, s32 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

size_t bw_swav_decode(const BwSwav *w, s16 *out, size_t cap)
{
    size_t n = bw_swav_decoded_count(w);
    size_t i;

    if (n > cap) n = cap;
    switch (w->wave_type) {
    case BW_WAVE_PCM8:
        for (i = 0; i < n; i++) out[i] = (s16)((s8)w->sample_data[i] * 256);
        return n;
    case BW_WAVE_PCM16:
        for (i = 0; i < n; i++) out[i] = (s16)sample16(w->sample_data + i * 2);
        return n;
    case BW_WAVE_ADPCM: {
        s32 predictor = (s16)sample16(w->sample_data);
        s32 index = clamp_s32(w->sample_data[2], 0, 88);
        const u8 *nib = w->sample_data + 4;
        for (i = 0; i < n; i++) {
            u8 byte = nib[i >> 1];
            int nibble = (i & 1) ? ((byte >> 4) & 0xF) : (byte & 0xF);
            s32 step = bw_ima_step[index];
            s32 diffq = step >> 3;
            if (nibble & 4) diffq += step;
            if (nibble & 2) diffq += step >> 1;
            if (nibble & 1) diffq += step >> 2;
            if (nibble & 8) predictor -= diffq; else predictor += diffq;
            predictor = clamp_s32(predictor, -32768, 32767);
            index = clamp_s32(index + bw_ima_index[nibble], 0, 88);
            out[i] = (s16)predictor;
        }
        return n;
    }
    default:
        return 0;
    }
}

size_t bw_swav_loop_start(const BwSwav *w, size_t decoded_count)
{
    size_t loop_start;
    switch (w->wave_type) {
    case BW_WAVE_PCM8:  loop_start = (size_t)w->loop_offset_words * 4; break;
    case BW_WAVE_PCM16: loop_start = (size_t)w->loop_offset_words * 2; break;
    default:
        /* ADPCM: 8 decoded samples per word, minus the one-word seed
         * header, which is not itself a decoded sample. */
        loop_start = w->loop_offset_words > 0
                     ? ((size_t)w->loop_offset_words - 1) * 8 : 0;
        break;
    }
    if (loop_start > decoded_count) loop_start = decoded_count;
    return loop_start;
}
