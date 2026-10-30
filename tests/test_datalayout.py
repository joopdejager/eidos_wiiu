#!/usr/bin/env python3
"""tests/test_datalayout.py -- tools/datalayout.py and armdata.py's use of it.

The struct layouts a big-endian host's swap runs come from: offsets with
mwcc's alignment, the multi-byte fields as runs, the types it refuses
(unions, bit-fields, packed), the block stand-ins it treats as opaque, the
type it infers from a cast of a copied local, and two declarations of one
object combined (armdata._combined_layout).  Written to a temporary tree;
no game data.
"""

import argparse
import os
import sys
import tempfile

FAILS = []
CHECKS = [0]


def check(label, got, want):
    CHECKS[0] += 1
    if got != want:
        FAILS.append(label)
        print("  FAIL %-44s got %r, want %r" % (label, got, want))


HEADER = """
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned char u8;
struct Inner { u8 a; u16 b; };
typedef struct Tmpl { u32 x, y; u8 s[4]; struct Inner in; u32 *p; } Tmpl;
typedef struct { u32 w[8]; } Block;
union U { u32 a; u16 b; };
struct Bits { u32 a : 3; u32 b : 29; };
struct Packed { u8 a; u32 b; } __attribute__((packed));
struct Fn { void (*cb[2])(int); u16 n; };
typedef Tmpl TmplAlias;
"""

SOURCE = """
#include "defs.h"
extern const struct Tmpl d_A;
extern TmplAlias d_B[];
extern Block d_C;
extern union U d_D;
extern struct Bits d_E;
extern struct Packed d_F;
extern struct Fn d_G;
extern const unsigned char d_H[];
extern u16 d_I[];
void f(void)
{
    Block local;
    local = d_C;
    use((const struct Tmpl *)(&local));
    use((struct Tmpl *)d_H);
}
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tools", required=True)
    args = ap.parse_args()
    sys.path.insert(0, args.tools)
    import armdata
    import datalayout

    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "defs.h"), "w") as fh:
            fh.write(HEADER)
        src = os.path.join(d, "unit.c")
        with open(src, "w") as fh:
            fh.write(SOURCE)
        sf = armdata.SourceFile(src, SOURCE, True)
        names = {n: {"addr": 0x02100000 + 0x100 * i}
                 for i, n in enumerate(sorted(sf.decl_types))}
        lays = datalayout.declared_layouts(sf, names)

        # Tmpl: x@0 y@4 s@8 (4 bytes) in@12 {a@12, b@14} p@16; size 20.
        want = (20, [(0, 8, 4), (14, 2, 2), (16, 4, 4)])
        check("struct: offsets, padding, nesting, pointer",
              lays.get("d_A") and (lays["d_A"].size, lays["d_A"].runs), want)
        check("typedef alias of a struct", lays.get("d_B") and lays["d_B"].key(),
              lays.get("d_A") and lays["d_A"].key())
        check("block stand-in: the type its copy is cast to",
              lays.get("d_C") and lays["d_C"].key(), lays.get("d_A") and lays["d_A"].key())
        check("bytes cast to a struct take its layout",
              lays.get("d_H") and lays["d_H"].key(), lays.get("d_A") and lays["d_A"].key())
        check("a union is unknown", "d_D" in lays, False)
        check("a bit-field is unknown", "d_E" in lays, False)
        check("a packed struct is unknown", "d_F" in lays, False)
        check("function pointers are words",
              lays.get("d_G") and (lays["d_G"].size, lays["d_G"].runs), (12, [(0, 8, 4), (8, 2, 2)]))
        check("a scalar declaration is not a layout", "d_I" in lays, False)

        defs = datalayout.file_defs(src)
        check("block body is opaque", datalayout.opaque(defs, "Block"), True)
        check("a real struct is not opaque", datalayout.opaque(defs, "struct Tmpl"), False)

        one = datalayout.Layout(12, 2, [(0, 2, 2)])
        two = datalayout.Layout(12, 4, [(0, 4, 2), (4, 8, 4)])
        clash = datalayout.Layout(12, 4, [(0, 4, 4)])
        check("compatible layouts combine",
              armdata._combined_layout([one, two]).runs, [(0, 4, 2), (4, 8, 4)])
        check("a byte at two widths gives none", armdata._combined_layout([two, clash]), None)
        check("two sizes give none",
              armdata._combined_layout([one, datalayout.Layout(8, 2, [(0, 2, 2)])]), None)

    print("test_datalayout: %d checks, %d failures" % (CHECKS[0], len(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
