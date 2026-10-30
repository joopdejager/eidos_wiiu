/*
 * engine/audio/sseq.c -- the SSEQ bytecode: the event decoder and the
 * per-track interpreter.
 *
 * The opcode table is aletheia's tools/assets/sseq.py, established by
 * disassembling this ROM's own SEQ_BGM_DUMMY and anchored by the shape
 * every track 0 has: 0xFE AllocTrack followed by a run of 0x93 OpenTrack
 * commands, one per additional track, each naming an offset into the DATA
 * block's payload (audio.h, "Where a sequence's bytes come from").
 *
 * The interpreter is tools/assets/sseqsynth.py's run_track(), which is the
 * oracle tests/test_audio.c compares against, and it is a real interpreter
 * rather than a linear disassembly:
 *
 *   - Note records (tick, note+transpose clamped to 0..127, velocity, the
 *     current program/volume/expression/pan) and then advances the track's
 *     own tick by the note's duration.  This format's note duration gates
 *     the next event as well as sounding the note; there is no separate
 *     note-off event.
 *   - a forward Jump is taken unconditionally (monotonic, cannot loop); a
 *     backward Jump, which is this format's loop idiom, is taken once and
 *     refused the second time it is reached, so a looping track renders as
 *     its body twice and then ends.
 *   - Call pushes the address after the call and Return pops it, with a
 *     depth limit.
 *   - 0xA0 (Random), 0xA1 (GetVariable) and 0xA2 (If) are genuinely
 *     undocumented in this project: their argument shapes were never
 *     re-derived, so they are skipped by the commonly cited widths, 5, 2
 *     and 1 bytes.  That is a labelled approximation, not a decode, and it
 *     is the same one the oracle makes, so the two agree by construction.
 *     Any other unrecognised opcode ends the track's render there, which is
 *     the honest fallback sseq.py itself uses.
 *
 * One deliberate difference from the oracle, in its favour: a truncated
 * multi-byte argument at the very end of the buffer makes this decoder
 * report an unrecognised event (ending the track) where CPython's
 * struct.unpack_from would raise struct.error, which run_track does not
 * catch and which would abort the whole render.  No sequence in this ROM
 * reaches it; it is the difference between stopping and crashing.
 */

#include "audio.h"

#include <stdlib.h>
#include <string.h>

int bw_sseq_open(BwSseq *q, BwBlob blob)
{
    memset(q, 0, sizeof(*q));
    if (blob.size < 0x1C || memcmp(blob.data, "SSEQ", 4) != 0) return -1;
    if (memcmp(blob.data + 0x10, "DATA", 4) != 0) return -1;
    q->data = blob.data;
    q->size = blob.size;
    /* In the data root's order (sdk/sdat_order.h); the bytecode is bytes. */
    q->data_offset = bw_dr_rd32(blob.data + 0x18);
    return 0;
}

/* MIDI-style variable-length quantity, 7 bits per byte, MSB first. */
static int read_varint(const u8 *data, size_t size, size_t pos, u32 *val, size_t *consumed)
{
    u32 v = 0;
    size_t c = 0;
    for (;;) {
        u8 b;
        if (pos + c >= size) return -1;
        b = data[pos + c];
        c++;
        v = (v << 7) | (u32)(b & 0x7F);
        if ((b & 0x80) == 0) break;
        if (c > 5) return -1;
    }
    *val = v;
    *consumed = c;
    return 0;
}

/* The single-u8-argument commands sseq.py names.  Only Pan, Volume,
 * Expression and Transpose change anything the renderer can see; the rest
 * are decoded so the bytecode stays in step and then ignored, which is the
 * scope line the oracle draws too (portamento, modulation, the ADSR
 * set-commands, Tie/Mono, LoopStart/LoopEnd and the pitch wheel). */
static int u8_arg_op(u8 op)
{
    return (op >= 0xC0 && op <= 0xD6)
           && op != 0xC3 && op != 0xC4;   /* those two are s8 */
}

int bw_sseq_decode_event(const u8 *data, size_t size, size_t pos, BwSseqEvent *ev)
{
    u8 op;

    memset(ev, 0, sizeof(*ev));
    if (pos >= size) return -1;
    op = data[pos];
    ev->start = pos;

    if (op < 0x80) {
        u32 dur;
        size_t consumed;
        if (pos + 1 >= size) return -1;
        ev->op = BW_OP_NOTE;
        ev->note = op;
        ev->velocity = data[pos + 1];
        if (read_varint(data, size, pos + 2, &dur, &consumed) != 0) return -1;
        ev->duration = dur;
        ev->end = pos + 2 + consumed;
        return 0;
    }
    if (op == 0x80 || op == 0x81) {
        u32 v;
        size_t consumed;
        if (read_varint(data, size, pos + 1, &v, &consumed) != 0) return -1;
        ev->op = (op == 0x80) ? BW_OP_REST : BW_OP_PROGRAM_CHANGE;
        ev->duration = v;
        ev->value = (int)v;      /* ProgramChange's patch number */
        ev->end = pos + 1 + consumed;
        return 0;
    }
    if (op == 0x93) {
        if (pos + 5 > size) return -1;
        ev->op = BW_OP_OPEN_TRACK;
        ev->track_no = data[pos + 1];
        ev->offset = (u32)data[pos + 2] | ((u32)data[pos + 3] << 8)
                   | ((u32)data[pos + 4] << 16);
        ev->end = pos + 5;
        return 0;
    }
    if (op == 0x94 || op == 0x95) {
        if (pos + 4 > size) return -1;
        ev->op = (op == 0x94) ? BW_OP_JUMP : BW_OP_CALL;
        ev->offset = (u32)data[pos + 1] | ((u32)data[pos + 2] << 8)
                   | ((u32)data[pos + 3] << 16);
        ev->end = pos + 4;
        return 0;
    }
    if (op == 0xFE) {
        if (pos + 3 > size) return -1;
        ev->op = BW_OP_ALLOC_TRACK;
        ev->mask = (u32)data[pos + 1] | ((u32)data[pos + 2] << 8);
        ev->end = pos + 3;
        return 0;
    }
    if (op == 0xFC || op == 0xFD || op == 0xFF) {
        ev->op = (op == 0xFC) ? BW_OP_LOOP_END
               : (op == 0xFD) ? BW_OP_RETURN : BW_OP_TRACK_END;
        ev->end = pos + 1;
        return 0;
    }
    if (u8_arg_op(op)) {
        if (pos + 2 > size) return -1;
        ev->value = data[pos + 1];
        ev->end = pos + 2;
        switch (op) {
        case 0xC0: ev->op = BW_OP_PAN; break;
        case 0xC1: ev->op = BW_OP_VOLUME; break;
        case 0xD5: ev->op = BW_OP_EXPRESSION; break;
        default:   ev->op = BW_OP_OTHER; break;
        }
        return 0;
    }
    if (op == 0xC3 || op == 0xC4) {               /* Transpose, PitchBend */
        if (pos + 2 > size) return -1;
        ev->value = (int)(s8)data[pos + 1];
        ev->end = pos + 2;
        ev->op = (op == 0xC3) ? BW_OP_TRANSPOSE : BW_OP_OTHER;
        return 0;
    }
    if (op == 0xE0 || op == 0xE1) {               /* ModDelay, Tempo (u16) */
        if (pos + 3 > size) return -1;
        ev->value = (int)((u32)data[pos + 1] | ((u32)data[pos + 2] << 8));
        ev->end = pos + 3;
        ev->op = (op == 0xE1) ? BW_OP_TEMPO : BW_OP_OTHER;
        return 0;
    }
    if (op == 0xE3) {                             /* SweepPitch (s16) */
        if (pos + 3 > size) return -1;
        ev->value = (int)(s16)((u32)data[pos + 1] | ((u32)data[pos + 2] << 8));
        ev->end = pos + 3;
        ev->op = BW_OP_OTHER;
        return 0;
    }
    if ((op >= 0xB0 && op <= 0xB6) || (op >= 0xB8 && op <= 0xBD)) {
        if (pos + 4 > size) return -1;            /* u8 var_no, s16 value */
        ev->value = (int)(s16)((u32)data[pos + 2] | ((u32)data[pos + 3] << 8));
        ev->end = pos + 4;
        ev->op = BW_OP_OTHER;
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Growable lists                                                      */
/* ------------------------------------------------------------------ */

static int note_push(BwSeqNoteList *l, const BwSeqNoteEvent *e)
{
    if (l == NULL) return 0;
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 64;
        BwSeqNoteEvent *v = (BwSeqNoteEvent *)realloc(l->v, cap * sizeof(*v));
        if (v == NULL) return -1;
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = *e;
    return 0;
}

static int tempo_push(BwTempoList *l, u32 tick, int bpm)
{
    if (l == NULL) return 0;
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        BwTempoEvent *v = (BwTempoEvent *)realloc(l->v, cap * sizeof(*v));
        if (v == NULL) return -1;
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n].tick = tick;
    l->v[l->n].bpm = bpm;
    l->n++;
    return 0;
}

void bw_seq_note_list_free(BwSeqNoteList *l) { free(l->v); l->v = NULL; l->n = l->cap = 0; }
void bw_tempo_list_free(BwTempoList *l)      { free(l->v); l->v = NULL; l->n = l->cap = 0; }

/* ------------------------------------------------------------------ */
/* Track starts                                                        */
/* ------------------------------------------------------------------ */

int bw_sseq_track_starts(const BwSseq *q, BwSseqTrackStart *starts, int cap)
{
    size_t pos = q->data_offset, header_end = q->data_offset;
    int n = 0, i;

    for (;;) {
        BwSseqEvent ev;
        if (bw_sseq_decode_event(q->data, q->size, pos, &ev) != 0) break;
        if (ev.op == BW_OP_ALLOC_TRACK) {
            header_end = ev.end;
            pos = ev.end;
            continue;
        }
        if (ev.op == BW_OP_OPEN_TRACK) {
            int found = -1;
            for (i = 0; i < n; i++) if (starts[i].track_no == ev.track_no) found = i;
            if (found >= 0) {
                starts[found].start = ev.offset;    /* dict update keeps the slot */
            } else if (n < cap) {
                starts[n].track_no = ev.track_no;
                starts[n].start = ev.offset;
                n++;
            }
            header_end = ev.end;
            pos = ev.end;
            continue;
        }
        break;
    }

    /* open_tracks[0] = header_end: a plain dict assignment, so an existing
     * key 0 keeps its position and only its value changes. */
    for (i = 0; i < n; i++) {
        if (starts[i].track_no == 0) {
            starts[i].start = (u32)header_end;
            return n;
        }
    }
    if (n < cap) {
        starts[n].track_no = 0;
        starts[n].start = (u32)header_end;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* The interpreter                                                     */
/* ------------------------------------------------------------------ */

int bw_sseq_run_track(const u8 *data, size_t size, size_t start_pc, int track_index,
                      BwSeqNoteList *notes, BwTempoList *tempos, u32 *end_tick)
{
    return bw_sseq_run_track_ex(data, size, start_pc, track_index, notes, tempos,
                                end_tick, NULL);
}

int bw_sseq_run_track_ex(const u8 *data, size_t size, size_t start_pc, int track_index,
                         BwSeqNoteList *notes, BwTempoList *tempos, u32 *end_tick,
                         int *looped)
{
    size_t pc = start_pc;
    size_t call_stack[BW_AUDIO_MAX_CALL_DEPTH];
    int call_depth = 0;
    u32 tick = 0;
    int program = 0;
    int volume = 127, expression = 127, pan = 64, transpose = 0;
    long steps = 0;
    /* One visit counter per backward-jump site.  A track has very few of
     * them; a linear table is both simpler and faster than a map here. */
    struct { size_t pc; int visits; } jumps[256];
    int jump_count = 0;

    while (steps < BW_AUDIO_MAX_STEPS_PER_TRACK) {
        BwSseqEvent ev;
        steps++;
        if (pc >= size) break;
        if (bw_sseq_decode_event(data, size, pc, &ev) != 0) {
            u8 b = data[pc];
            if (b == 0xA0) { pc += 5; continue; }   /* Random:      s16, s16 */
            if (b == 0xA1) { pc += 2; continue; }   /* GetVariable: u8 var   */
            if (b == 0xA2) { pc += 1; continue; }   /* If:          bare      */
            break;
        }
        switch (ev.op) {
        case BW_OP_NOTE: {
            BwSeqNoteEvent n;
            int v = ev.note + transpose;
            if (v < 0) v = 0;
            if (v > 127) v = 127;
            n.tick = tick;
            n.dur = ev.duration;
            n.note = v;
            n.velocity = ev.velocity;
            n.program = program;
            n.volume = volume;
            n.expression = expression;
            n.pan = pan;
            n.track = track_index;
            if (note_push(notes, &n) != 0) return -1;
            tick += ev.duration;
            pc = ev.end;
            break;
        }
        case BW_OP_REST:
            tick += ev.duration;
            pc = ev.end;
            break;
        case BW_OP_PROGRAM_CHANGE: program = ev.value;  pc = ev.end; break;
        case BW_OP_PAN:            pan = ev.value;      pc = ev.end; break;
        case BW_OP_VOLUME:         volume = ev.value;   pc = ev.end; break;
        case BW_OP_EXPRESSION:     expression = ev.value; pc = ev.end; break;
        case BW_OP_TRANSPOSE:      transpose = ev.value; pc = ev.end; break;
        case BW_OP_TEMPO:
            if (tempo_push(tempos, tick, ev.value) != 0) return -1;
            pc = ev.end;
            break;
        case BW_OP_JUMP:
            if ((size_t)ev.offset > pc) {
                pc = ev.offset;
            } else {
                int i, slot = -1;
                for (i = 0; i < jump_count; i++) if (jumps[i].pc == pc) slot = i;
                if (slot < 0) {
                    if (jump_count >= (int)(sizeof(jumps) / sizeof(jumps[0]))) goto done;
                    slot = jump_count++;
                    jumps[slot].pc = pc;
                    jumps[slot].visits = 0;
                }
                if (jumps[slot].visits >= BW_AUDIO_MAX_JUMP_REPEATS) {
                    if (looped != NULL) *looped = 1;
                    goto done;
                }
                jumps[slot].visits++;
                pc = ev.offset;
            }
            break;
        case BW_OP_CALL:
            if (call_depth >= BW_AUDIO_MAX_CALL_DEPTH) goto done;
            call_stack[call_depth++] = ev.end;
            pc = ev.offset;
            break;
        case BW_OP_RETURN:
            if (call_depth == 0) goto done;
            pc = call_stack[--call_depth];
            break;
        case BW_OP_TRACK_END:
            goto done;
        default:
            pc = ev.end;
            break;
        }
    }
done:
    if (end_tick != NULL) *end_tick = tick;
    return 0;
}
