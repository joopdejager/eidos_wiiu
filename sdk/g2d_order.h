/*
 * sdk/g2d_order.h -- NITRO 2D graphics files in the host's byte order.
 *
 * The game reads the NNS G2D formats it loads (NCLR palettes, NCGR
 * characters, NSCR screens, NCER cells, NANR animations, NMCR and NMAR
 * multi-cells) with native loads: NNS_G2dFindBinaryBlock walks a file's
 * blocks comparing each block's tag as a u32 against constants spelled in
 * the DS's order, and the unpack functions and their callers read every
 * header, table and record field as numbers.  Many of these files are
 * LZ-compressed members, which no extractor can turn, so a big-endian host
 * turns each file when the game first looks into it: the port's
 * NNS_G2dFindBinaryBlock (sdk/g2d_order.c) is every reader's way in.
 *
 * A file says which order it is in: its byte-order mark at +4 reads 0xFEFF
 * natively once turned, 0xFFFE before, so turning is done once however
 * often the game looks.  Turned: the file header (its tag too), every
 * block's tag and size, and the blocks' headers, tables and records, the
 * colours and screen entries included.  Left as bytes: character data
 * (4- and 8-bit pixels), 8-bit affine screen entries, the label strings.
 * A little-endian host turns nothing.
 */

#ifndef BW_G2D_ORDER_H
#define BW_G2D_ORDER_H

#include "types.h"

/* Turn one file in place: 1 turned now, 0 already in the host's order or
 * not a NITRO file, -1 malformed (left partly turned; the game will not
 * find its blocks). */
int bw_g2d_to_host(void *file);

#endif
