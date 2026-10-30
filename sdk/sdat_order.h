/*
 * sdk/sdat_order.h -- the sound archive (SDAT) in the host's byte order.
 *
 * The matched sound code reads the archive's structure with native loads:
 * NNS_SndArcInit the header, the NNS_SndArcGet*Info accessors the INFO
 * block and its records, NNS_SndArcGetFileSize and the loaders the FAT, the
 * players the SSEQ data offset, SND_GetNextInstData the SBNK instrument
 * table and records, SND_GetWaveDataAddress the SWAR wave table, and
 * PM_VOICE_LoadWave a SWAV's header.  On a big-endian host those numbers
 * must be in its order, so the extractor turns them (install/byteorder.c)
 * and the mods' sound merge turns the files it appends (sdk/vfs_sdat.c).
 *
 * Turned: every number of the archive's structure -- the header, SYMB,
 * INFO and FAT blocks, and in each file its header, its block's size and
 * the numbers of its tables and records (SSEQ: the data offset; SSAR: the
 * data offset, the count and the records; SBNK: the instrument count and
 * table, and each instrument's wave and wave archive numbers; SWAR: the
 * wave count and table, and each SWAV's rate, timer, loop start and loop
 * length).  Left as bytes: the tags, the strings, the sequence bytecode
 * (read byte by byte), an instrument entry's type byte, and the samples,
 * which only the sound hardware model reads (sdk/arm7/snd_hw.c, in the
 * DS's little-endian order).  The port's readers read the turned numbers
 * with bw_dr_rd16 / bw_dr_rd32 (sdk/types.h).
 *
 * Bytes two records, files or waves share are turned once.  A little-endian
 * host turns nothing, but the functions still check the structure.
 */

#ifndef BW_SDAT_ORDER_H
#define BW_SDAT_ORDER_H

#include "types.h"

/* The whole archive, in place: 1 turned, 0 not an SDAT, -1 malformed. */
int bw_sdat_to_host(u8 *p, u32 n);

/* One of its files (SSEQ, SSAR, SBNK or SWAR), in place: 1 turned, -1
 * malformed or another kind. */
int bw_sdat_file_to_host(u8 *p, u32 n);

#endif
