/*
 * sdk/types.h -- the scalar vocabulary the matched game C is written in,
 * defined for a host compiler.
 *
 * This is our own file.  It is not copied from any SDK header; the names and
 * widths are the ones the matched sources under src/matched/ already spell
 * out for themselves (every one of those files opens with its own
 * `typedef unsigned char u8;` block, because a matched translation unit must
 * compile standalone under mwccarm with no includes at all).
 *
 * Consequence, and it is deliberate: **matched sources never include this
 * header.**  They carry their own typedefs and a second file-scope typedef of
 * the same name in the same translation unit is a constraint violation in
 * C99.  This header exists for the port's own code: the SDK shims, the stubs
 * and the tests.
 *
 * Widths are taken from stdint so that they are exact on every host, not from
 * the ARM ABI's `int`.  The DS is a 32-bit little-endian LP32 target; the
 * host may be LP64 or LLP64.  What survives the move unchanged is every
 * fixed-width scalar; what does not is anything whose size depends on a
 * pointer.  See docs/building.md, "Portability rules".
 */

#ifndef BW_PORT_TYPES_H
#define BW_PORT_TYPES_H

#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

/*
 * Plain char is signed, as it is under the pinned DS compiler (`-char
 * signed`).  The build asks for it on every compiler (docs/building.md, "Portability
 * rules", rule 6); this fails to compile, with a negative array size, on a
 * configuration that did not get it.
 */
typedef char bw_plain_char_is_signed[((char)-1 < 0) ? 1 : -1];

typedef volatile uint8_t  vu8;
typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;

typedef volatile int8_t   vs8;
typedef volatile int16_t  vs16;
typedef volatile int32_t  vs32;

/*
 * BOOL is the SDK's plain 32-bit truth value, not C99 _Bool: the matched code
 * returns it out of functions whose callers compare it against 0 and 1, and
 * it must stay four bytes wide in a struct.
 */
typedef int32_t BOOL;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

/*
 * Fixed point.  The DS's three formats, kept exactly as the hardware and the
 * matched code use them; no float appears anywhere in the conversion, because
 * the game's numbers are decided by the integer arithmetic and rounding a
 * float would introduce a different answer.
 *
 *   fx32   1.19.12   (12 fractional bits)  the common one
 *   fx16   1. 3.12   (12 fractional bits)  packed vectors
 *   fx64c  1.31.32   (32 fractional bits)  the matrix accumulator
 */
typedef int32_t fx32;
typedef int16_t fx16;
typedef int64_t fx64;
typedef int64_t fx64c;

#define FX32_SHIFT      12
#define FX16_SHIFT      12
#define FX64C_SHIFT     32

#define FX32_INT_SIZE   19
#define FX32_DEC_SIZE   12

#define FX32_ONE        ((fx32)(1 << FX32_SHIFT))
#define FX16_ONE        ((fx16)(1 << FX16_SHIFT))
#define FX64C_ONE       ((fx64c)((int64_t)1 << FX64C_SHIFT))

#define FX32_HALF       ((fx32)(1 << (FX32_SHIFT - 1)))

/* Integer <-> fixed.  FX32_CONST is the compile-time literal form. */
#define FX32_CONST(x)   ((fx32)(((x) > 0) ? ((x) * (1 << FX32_SHIFT) + 0.5) \
                                          : ((x) * (1 << FX32_SHIFT) - 0.5)))
#define FX16_CONST(x)   ((fx16)(((x) > 0) ? ((x) * (1 << FX16_SHIFT) + 0.5) \
                                          : ((x) * (1 << FX16_SHIFT) - 0.5)))

#define FX32_FROM_INT(n)  ((fx32)((s32)(n) << FX32_SHIFT))
#define FX32_TO_INT(x)    ((s32)((x) >> FX32_SHIFT))            /* floor */
#define FX32_TO_INT_ROUND(x) ((s32)(((x) + FX32_HALF) >> FX32_SHIFT))

#define FX16_FROM_INT(n)  ((fx16)((s32)(n) << FX16_SHIFT))
#define FX16_TO_INT(x)    ((s32)((x) >> FX16_SHIFT))

/*
 * Multiply and divide.  The intermediate is 64-bit on purpose: fx32 * fx32
 * overflows 32 bits for operands past about 512.0, and the DS code relies on
 * the wide intermediate (it calls the runtime's _ll_mul helper, see
 * docs/idioms/random-below-n-is-a-64-bit-multiply-high.md).
 */
#define FX_MUL(a, b)    ((fx32)(((s64)(a) * (s64)(b)) >> FX32_SHIFT))
#define FX_DIV(a, b)    ((fx32)(((s64)(a) << FX32_SHIFT) / (s64)(b)))

/*
 * Endian-clean little-endian reads.  Every byte the port reads out of an
 * extracted file is little-endian, whatever the host is; never cast a
 * `const u8 *` to a `const u32 *` and dereference it, because that is both a
 * big-endian bug and an unaligned-access bug at once.
 */
static inline u16 bw_rd16(const u8 *p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static inline u32 bw_rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static inline void bw_wr16(u8 *p, u16 v)
{
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)(v >> 8);
}

static inline void bw_wr32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF);
    p[3] = (u8)((v >> 24) & 0xFF);
}

/*
 * DS memory reads and writes.  The emulated address space (main RAM, the
 * register file, palette, VRAM, OAM, the shared work areas) holds what the
 * matched code's own stores leave there, and those are host-order stores
 * through bw_ds_ptr.  Port code that reads or writes a value the game also
 * reads or writes uses these, so both sides agree on any host.  On a
 * little-endian host they are bw_rd16..bw_wr32; on a big-endian host they
 * are not, and bytes copied in from an extracted file are little-endian
 * until something converts them.
 */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define BW_HOST_BIG_ENDIAN 1
#else
#define BW_HOST_BIG_ENDIAN 0
#endif

static inline u16 bw_mem_rd16(const u8 *p)
{
#if BW_HOST_BIG_ENDIAN
    return (u16)(((u16)p[0] << 8) | (u16)p[1]);
#else
    return bw_rd16(p);
#endif
}

static inline u32 bw_mem_rd32(const u8 *p)
{
#if BW_HOST_BIG_ENDIAN
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
#else
    return bw_rd32(p);
#endif
}

static inline void bw_mem_wr16(u8 *p, u16 v)
{
#if BW_HOST_BIG_ENDIAN
    p[0] = (u8)(v >> 8);
    p[1] = (u8)(v & 0xFF);
#else
    bw_wr16(p, v);
#endif
}

static inline void bw_mem_wr32(u8 *p, u32 v)
{
#if BW_HOST_BIG_ENDIAN
    p[0] = (u8)(v >> 24);
    p[1] = (u8)((v >> 16) & 0xFF);
    p[2] = (u8)((v >> 8) & 0xFF);
    p[3] = (u8)(v & 0xFF);
#else
    bw_wr32(p, v);
#endif
}

/*
 * A number in a NARC's framing (header, BTAF, BTNF) in the data root.  The
 * extractor turns that framing into the host's order on a big-endian host
 * (install/byteorder.c), because the matched code reads it with native
 * loads, so the port reads and writes it in the host's order too.  The
 * four-byte tags stay bytes (bw_rd32 against a FOURCC), and the members'
 * own formats are still the cartridge's little-endian.
 */
#define bw_dr_rd16 bw_mem_rd16
#define bw_dr_rd32 bw_mem_rd32
#define bw_dr_wr16 bw_mem_wr16
#define bw_dr_wr32 bw_mem_wr32

/*
 * A field of one of the game types tools/sso_types.txt lists (the bitfield
 * and union views of hardware and firmware words).  With EIDOS_SSO those
 * types keep the DS byte and bitfield order, so such a field is
 * little-endian in memory; otherwise it is in host order like any other
 * game store.  Every other game struct is host order either way: use
 * bw_mem_rd16..bw_mem_wr32.
 */
#if defined(EIDOS_SSO)
#define bw_gs_rd16 bw_rd16
#define bw_gs_rd32 bw_rd32
#define bw_gs_wr16 bw_wr16
#define bw_gs_wr32 bw_wr32
#else
#define bw_gs_rd16 bw_mem_rd16
#define bw_gs_rd32 bw_mem_rd32
#define bw_gs_wr16 bw_mem_wr16
#define bw_gs_wr32 bw_mem_wr32
#endif

#endif /* BW_PORT_TYPES_H */
