/*
 * sdk/g3d_order.h -- NITRO 3D files in the host's byte order.
 *
 * The game reads the NNS G3D formats with native loads: models and their
 * textures (BMD0, BTX0) through the NNS_G3dGet* lookups, the model's node,
 * material and shape records, and the display lists, which
 * NNS_G3dDraw sends to the geometry engine a word at a time
 * (engine/g3d/hw3d.c reads each FIFO word natively); and the animations
 * (BCA0 joints, BMA0 material colours, BTA0 texture SRT, BTP0 texture
 * patterns, BVA0 visibility) through the samplers in
 * src/game/arm9/nns_g3d_anm_calc.c.  None of the game's 3D files is
 * compressed, so the extractor turns them (install/byteorder.c): every
 * archive member that is one, and every one a member holds at an offset
 * (the map chunks of a/0/0/8, the building packs of a/2/2/9 and a/2/3/0,
 * a/0/7/0).
 *
 * Turned: the file header (its tag too), every block's tag and size, the
 * dictionaries' sizes, offsets and entries, the model's header, info,
 * node records, materials, shapes, display lists and envelope matrices,
 * the texture block's headers and dictionaries, and every animation's
 * header, records, track words and keys.  Left as bytes: texel data, the
 * 4x4 textures' palette indices and the palettes (which only the port's
 * texture decoder reads, as the DS's little-endian bytes:
 * engine/g3d/texture.c), the render command stream, names, the material
 * animations' alpha keys and the texture patterns' texture and palette
 * indices.
 *
 * A file says which order it is in by its byte-order mark at +4: the bytes
 * FF FE as on the cartridge, FE FF once turned on a big-endian host.  The
 * port's own readers of the cartridge's layout (engine/g3d/nsbmd.c,
 * engine/world/) turn what they load back into the DS's order.  A
 * little-endian host turns nothing.
 */

#ifndef BW_G3D_ORDER_H
#define BW_G3D_ORDER_H

#include "types.h"

/* One 3D file of `n` bytes at `p`, in place: 1 turned, 0 not a 3D file in
 * the DS's order, -1 malformed (left as it was). */
int bw_g3d_to_host(u8 *p, u32 n);

/* Back: 1 turned, 0 not a 3D file in the host's order, -1 malformed. */
int bw_g3d_to_ds(u8 *p, u32 n);

/* Every 3D file in an archive member, at its start or at a word-aligned
 * offset inside it: the number turned, or -1 when the member is a 3D file
 * and malformed.  An embedded candidate that does not walk is left alone.
 * Nothing on a little-endian host. */
int bw_g3d_member_to_host(u8 *p, u32 n);
int bw_g3d_member_to_ds(u8 *p, u32 n);

#endif
