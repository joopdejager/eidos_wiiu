# Audio: the engine and the ARM7 sound driver

## The audio engine, and the synth oracle it is tested against

`engine/audio/` is the second host-side reimplementation of hardware the DS
had and a PC does not: the ARM7 sound core. It is pure C99 with no SDL, no
third-party code and nothing outside the standard library (`math.h` for
`pow`, `fmod`, `ceil`, `round` and `fabs`, and nothing else). The platform
layer's audio callback calls into it; `tests/test_audio.c` drives it
headless.

| file | what it does |
| --- | --- |
| `engine/audio/audio.h` | the whole API, and the provenance of every rule it implements |
| `engine/audio/sdat.c` | the SDAT container: SYMB, INFO, FAT and FILE, and the SEQ / BANK / WAVARC / PLAYER records |
| `engine/audio/sbnk.c` | SBNK instrument tables: types 0, 1, 2, 3, 16 and 17, and region selection |
| `engine/audio/swav.c` | SWAR archives and SWAV waves: PCM8, PCM16 and IMA-ADPCM, and the loop point |
| `engine/audio/sseq.c` | the SSEQ opcode decoder and the per-track bytecode interpreter |
| `engine/audio/mixer.c` | one voice: the resample, the ADSR, the pan, and the two PSG generators |
| `engine/audio/player.c` | SEQ to BANK to WAVARC resolution, the tempo map, the render and the streaming player |

**The formats.** The SEQ INFO record's bytes +4:+6 are one little-endian
u16 holding a **direct** BANK-table index, not two u8 fields and not a
PLAYER indirection; aletheia's asset lane settled that against the data
(940 of 940 populated SEQ records resolve to a real BANK record that way,
and the resolved BANK's name matches the SEQ's own in every case sampled)
and `bw_sdat_seq_record` is that reading. A BANK record carries a file id
and four wave-archive slots; a region's `wave_arc` is a *local* 0-3 index
into those, which gives the global WAVARC index, whose INFO record is a
bare u32 file id for the SWAR archive, in which `wave_id` selects the
SWAV. The SBNK region record is **10 bytes**, `wave_id` first, and a type
16 or 17 region entry is a u16 type prefix in front of it, so 12 bytes per
region: reading it 12 bytes wide is what used to make every region past
the first of a drum kit decode as garbage. Types 2 and 3 are PSG and carry
no wave at all; type 2's leading u16 is a duty selector.

**The sequencer** is a real interpreter, not a linear disassembly. Each
track runs its own bytecode with its own tick, program, volume, expression,
pan and transpose; a note's duration gates the next event as well as
sounding the note, because this format has no note-off; a forward `Jump` is
taken unconditionally, a backward one (this format's loop idiom) once, so a
looping track renders as its body twice and then ends; `Call` and `Return`
use a real return stack. The three opcodes `0xA0` (Random), `0xA1`
(GetVariable) and `0xA2` (If) are genuinely undocumented in this project:
their argument shapes were never re-derived, and they are skipped by the
commonly cited 5, 2 and 1 byte widths. That is a labelled approximation
rather than a decode, it is the same one the oracle makes, and any *other*
unrecognised opcode ends that track's render where it stands.

**The mixer** is 16 channels at 32768 Hz, which is the DS mixer's own
output rate and the rate the host device is opened at, so nothing resamples
twice. A voice resamples its source by the semitone ratio
`2^((played - base) / 12) * native_rate / output_rate` with linear
interpolation, wraps into `[loop_start, total_len)` past the loop point,
plays a one-shot out to its own natural length whatever the note's gate
says (which is what makes a cry, whose gate is zero ticks, sound at all),
shapes it with a linear ADSR, pans it linearly about 64 and clips to 16
bits. PSG square is a bipolar rectangle with the hardware's eight duty
settings; PSG noise is the 15-bit Galois LFSR clocked at the *channel timer*
rate the SDK's SND timer formula derives from the note's frequency
(`timer = -(clock / frequency)`, clock = 33513982/2 Hz) and sample-and-held
to the output rate, because an LFSR bit is a held digital level between
clocks rather than something to interpolate through.

**A voice is chunked, and that is load bearing.** All the state a note
carries (the source position, the square's phase, the noise LFSR and its
cursor, the sample counter) lives in the voice, so rendering a note in
pieces is bit-identical to rendering it in one call. That is what lets the
offline render and the real-time callback be the *same* mixer rather than
two implementations that drift; `tests/test_audio.c` checks it directly, on
a PCM voice in 37-sample chunks and a PSG voice in 101-sample chunks.

**The oracle, and why the comparison is exact.** `tests/test_audio.c` does
not compare against hand-written expected values. `tools/audio_oracle.py`
imports aletheia's `tools/assets/sseqsynth.py` (out of the sibling
checkout; nothing is copied here and aletheia is not modified), renders the
same sequences in Python and writes the `.wav` that tool itself produces
plus a note dump, into the build directory. The C engine renders the same
sequences and the test checks two things:

- **the note event list**, diffed line for line. A difference there is a
  *sequencer* difference: a mis-decoded opcode, a jump taken a different
  number of times, a tempo segment off by a tick, a region resolved to the
  wrong instrument. Every float in the dump is written as its exact
  IEEE-754 bit pattern, so the diff means "the same double" rather than
  "the same 17 digits", which two printf implementations need not agree on.
- **the sample buffers**, sample for sample, with the peak, the RMS and the
  largest difference reported per sequence.

The tolerance is **zero**: the two renders are required to be identical,
not merely close. That is reachable, and it is reachable on purpose.
sseqsynth.py is a Python program and Python floats are IEEE-754 binary64,
so `engine/audio` computes the same quantities in the same order in
`double` and gets the same bits. Three things had to be got right for that
and each is written down where it lives:

- **Python's `round()` is round-half-to-even**, not C `round()`'s
  half-away-from-zero, and the oracle uses it for the note start sample,
  the note length, the timer period and the final quantisation. Getting it
  wrong shifts whole notes by a sample. `bw_audio_py_round` is CPython's
  own `float.__round__`.
- **the order of the mix accumulation is observable**, because floating
  point addition is not associative. The offline render sums the notes in
  the oracle's order, which is a Python dict's insertion order over the
  tracks (the `OpenTrack` order with track 0 appended last) and then note
  order within a track, each note's whole span at once.
- **x87 is not binary64.** On 32-bit x86 the default floating point unit
  evaluates `double` expressions in 80-bit registers and rounds to 64 bits
  only on a spill, which made every ILP32 render differ from the oracle's
  in the last ulp of a gain or a note-off time. `CMakeLists.txt` compiles
  the six audio sources with `-msse2 -mfpmath=sse` on such a target. That
  is the whole of the difference; the other 32-bit results were already
  integer arithmetic and never cared.

Nothing about the engine *requires* double. `BwAudioVoice` is the whole of
the per-note arithmetic and is the piece a later fixed-point pass would
replace, with this test standing as the reference it then has to stay near
rather than equal.

**Measured**, three sequences over the same 10-second window, checked by
ctest:

| SEQ | name | what it covers | notes | render | difference |
| --- | --- | --- | --- | --- | --- |
| 1062 | `SEQ_BGM_GYM` | PCM waves out of a real SWAR | 2510 events, 176 rendered, 9 tracks, 116 BPM | peak 32000, RMS 6549.031 | **0 of 655360 samples** |
| 1146 | `SEQ_BGM_BATTLEPINCH` | PSG square, with PCM alongside | 1110 events, 97 rendered, 6 tracks, 180 BPM | peak 27449, RMS 5658.633 | **0 of 655360 samples** |
| 1379 | `SEQ_SE_KOUKA_H` | PSG noise | 24 events, 24 rendered, 2 tracks, 89 BPM | peak 30081, RMS 2514.572 | **0 of 275032 samples** |

All three note event lists are identical to the oracle's, and the peak and
RMS agree exactly because the buffers do. "Rendered" counts the notes that
start inside the window; the rest of the list lies past it. The busiest of
the three has **19 voices at once**: this model gives every note its own
voice, where the DS shares 16 channels by priority, which is what the ARM7
driver below does instead.

**SSEQ offsets.** `OpenTrack`, `Jump` and `Call` count their 24-bit offsets
from the DATA block's payload (the file's `data_offset`, 0x1C), not from
the file: on this ROM's SDAT all 2,016 `OpenTrack` targets in 940
sequences follow a track end or a return under that reading (9 under the
file-absolute one), and all 9,945 `Jump` and `Call` targets are command
boundaries (4,710 under the file-absolute one). Both `engine/audio` and the
ARM7 driver read them that way. Before the fix the three sequences above
had 296, 173 and 17 note events. aletheia's `sseqsynth.py` reads
`OpenTrack` this way since its commit 1609c2263 but still takes `Jump` and
`Call` targets as file offsets, so `tools/audio_oracle.py` probes the
imported oracle with a nine-byte sequence and, only while the probe shows
the file-absolute reading, adds `data_offset` to those two targets around
its `run_track`; with the fix in aletheia the probe turns that off.

**What is reproduced rather than improved, deliberately.** sseqsynth.py
documents its own approximations, and the engine makes the same ones,
because the point of an oracle comparison is that two independent
implementations of the *same* model agree. The ADSR is linear with the
byte used directly as a fraction of each stage's maximum time (the asset
lane picked that direction empirically, not from a spec); volume,
expression and velocity multiply linearly rather than in dB; pan is a
linear crossfade rather than equal power; portamento, modulation, sweep and
the pitch wheel are no-ops. One of them looks like a bug and is not:
**the noise LFSR's all-ones reset value is a fixed point of its own
update** (right-shifting all ones always shifts out a 1, so the tap is
always XORed back in), so every noise note is a constant level shaped by
the envelope rather than noise. aletheia's tool found that, proved it (its
clock-rate fix changed the rendered file not at all, byte for byte) and
deliberately left it. The engine reproduces it exactly, so that whoever
fixes the seed fixes it in the tool first and this test then *measures* the
change instead of hiding it.

## The ARM7 sound driver

The game does not call `engine/audio`. It calls the SDK's SND library,
which sends command lists to the ARM7 over PXI tag 7; `sdk/arm7/snd.c` is
the transport and `sdk/arm7/snd_driver.c` executes them, on two pieces of
its own:

| file | what it does |
| --- | --- |
| `sdk/arm7/snd_seq.c` | the sequencer: 16 players, a pool of 32 tracks, 16 extended channels allocated by priority, the SSEQ interpreter in real time with its prefix and variable commands, envelopes, the modulation LFO, sweeps and portamento |
| `sdk/arm7/snd_hw.c` | the sound hardware: 16 channels (PCM8, PCM16, IMA-ADPCM with its loop state, PSG square on 8-13, noise on 14-15), two captures, SOUNDCNT's master volume and output selector, mixed in integers |

The shapes are the SDK's own: `SNDWork`, `SNDPlayer`, `SNDTrack`,
`SNDExChannel`, `SNDLfoParam` and the bank structures are read from the
DWARF of `libsnd.TWL.HYB.a` with aletheia's `tools/sdkobj.py`, the player
and track parameters live in byte images at those offsets (so
`PLAYER_PARAM` and `TRACK_PARAM` are the stores the ARM9 means them to be),
and `SND_ReadDriverInfo` gets `SNDWork` back in the DS layout. The
arithmetic is `snd_util.o`'s: `SND_CalcTimer` from its listing, and the
pitch, decibel-square and volume tables generated from formulas that
reproduce the SDK's tables exactly (the test holds them to the SDK's
sums). The ARM7 behaviour around them (the driver frame of 64 * 2728 bus
cycles, the tempo counter's 240 per tick, the envelope in 1/128 dB from
-92544, the SBNK envelope byte conversions, the decibel sum for a
channel's level) is public NDS sound driver documentation; the hardware is
GBATEK's. Where neither says, the source says "Choice:". Nothing is taken
from ARM7 code.

What that gives the game: a looping sequence loops forever from its loop
point; the tempo ratio, track pitch, pan and volume, pitch bend,
portamento, Tie and the LFO change the sound as it plays; the 16 hardware
channels are shared between all players, a note taking the lowest-priority
channel (a releasing note first) and being dropped when every channel
outranks it; `SKIP_SEQ`, `ALLOCATABLE_CHANNEL`, mutes and the sequence
variables (in the shared work, as the ARM9 reads them) work; and the
direct channel commands, captures, master pan and the output selector drive
the hardware model, which is how the game's own stream object
(`func_020738C0`: a locked channel looping over a ring buffer, refilled on
an alarm started with the channel by one `START_TIMER`) plays.
`tests/test_snd.c` checks each of those, and with the data root plays
`SEQ_SE_DECIDE2` and `SEQ_BGM_GYM` from main RAM and streams one of GYM's
own waves through a ring buffer, which must equal the wave played in one
piece sample for sample. The SDAT has no STRM entries.

## Byte order on a big-endian host

The matched sound code reads the archive's structure with native loads:
`NNS_SndArcInit` the SDAT header, the `NNS_SndArcGet*Info` accessors the
INFO tables and records, `NNS_SndArcGetFileSize` and the loaders the FAT,
the players an SSEQ's data offset, `SND_GetNextInstData` an SBNK's
instrument table and records, `SND_GetWaveDataAddress` a SWAR's wave
table, and `PM_VOICE_LoadWave` (the cries) a SWAV's rate and timer. So on a
big-endian host (the Wii U) the extractor turns every number of that
structure into the host's order (`sdk/sdat_order.c`; the full list is in
`sdk/sdat_order.h`), and the mods' sound merge turns the files it appends
the same way. What stays bytes: the tags and strings, the sequence
bytecode (read a byte at a time by both sequencers), an instrument entry's
type byte, and the samples, which only the sound hardware model reads, in
the DS's little-endian order (`sdk/arm7/snd_hw.c`).

The port's readers follow: `engine/audio` and the ARM7 driver read the
turned numbers with `bw_dr_rd16` / `bw_dr_rd32` (`sdk/types.h`), and the
samples with `bw_rd16`. A bank's `waveArcLink` slots are not file data at
all: `SND_AssignWaveArc` stores the wave archives' addresses there at run
time with native stores, so the driver reads them with `bw_mem_rd32`.

One record needs a decomp change: `NNSSndArcWaveArcInfo` is a word with
the bit-fields `fileId : 24`, `isIndividual : 1`, `reserved : 7`, which a
big-endian host's compiler fills from the high bit. Under that host
`src/game/arm9/nns_snd_arc_types.h` declares them the other way round, so
`fileId` is the word's low 24 bits there too (archived in aletheia's
`docs/ORIGINAL_BUGS.tsv`).
