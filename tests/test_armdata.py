#!/usr/bin/env python3
"""
tests/test_armdata.py -- tools/armdata.py against a synthetic aletheia tree.

The test writes a small aletheia checkout of its own (layout.json,
data_objects.json, a reassembly listing with annotations, a few matched
sources and a port source) into a temporary directory, runs the generator
on it and checks what comes out: which names are bound and how, which words
are relocated to what, the exclusions, the forced-include header, the
overlay region, and that the build-tree paths of dsaddr copies are mapped
back to their originals.  There is no image anywhere in the tree, so the
generator demonstrably reads none.

    python tests/test_armdata.py --tool tools/armdata.py
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

FAILS = []
CHECKS = [0]


def check(label, cond, detail=""):
    CHECKS[0] += 1
    if not cond:
        FAILS.append(label)
        print("  FAIL %s %s" % (label, detail))


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def build_tree(root):
    layout = {
        "arm9": {
            "load_address": 0x02004000,
            "static": {"ram_start": 0x02004000, "ram_end": 0x02004400,
                       "bss_start": 0x02004400, "bss_end": 0x02005000},
            "autoloads": [
                {"index": 0, "kind": "ITCM", "ram_start": 0x01FF8000,
                 "ram_end": 0x01FF8040, "bss_start": 0x01FF8040,
                 "bss_end": 0x01FF8040, "src_offset": 0x400},
                {"index": 1, "kind": "MAIN_TWL", "ram_start": 0x02FE0000,
                 "ram_end": 0x02FE0020, "bss_start": 0x02FE0020,
                 "bss_end": 0x02FE0040, "src_offset": 0x440},
            ],
        },
        "overlays": [
            {"id": 5, "ram_start": 0x02100000, "ram_end": 0x02100800,
             "bss_start": 0x02100800, "bss_end": 0x02100900},
        ],
        "twl": {"ram_start": 0x02700000, "bss_end": 0x02710000},
    }
    write(os.path.join(root, "layout", "layout.json"), json.dumps(layout))
    objects = {
        "tool": "datareg.py", "version": 1,
        "regions": {"arm9": {
            "base": "0x02004000", "range": ["0x02004200", "0x02004400"],
            "objects": [
                # a table of three pointers: a string, null, a Thumb function
                {"addr": "0x02004200", "size": 12, "kind": "pointer_table",
                 "name": "g_02004200", "count": 3,
                 "targets": ["0x02004300", 0, "0x02004101"]},
                # a struct: u32, ptr (to .bss), u32, ptr (an ARM function
                # the build does not define)
                {"addr": "0x0200420C", "size": 16, "kind": "struct",
                 "name": "g_0200420C",
                 "fields": [["u32", 1], ["ptr", 1], ["u32", 1], ["ptr", 1]],
                 "targets": ["0x02004800", "0x02004180"]},
                # a pointer into the port's object
                {"addr": "0x0200421C", "size": 4, "kind": "pointer_table",
                 "name": "g_0200421C", "count": 1, "targets": ["0x02004A04"]},
                # a pointer to data the externs list says is data, in code
                {"addr": "0x02004220", "size": 4, "kind": "pointer_table",
                 "name": "g_02004220", "count": 1, "targets": ["0x02004150"]},
                {"addr": "0x02004224", "size": 0x1DC, "kind": "raw",
                 "name": "g_02004224"},
            ],
            "externs": [
                {"addr": "0x02004150", "name": "g_02004150", "kind": "data"},
                {"addr": "0x02004180", "name": "func_02004180", "kind": "func"},
            ],
        }},
    }
    write(os.path.join(root, "layout", "data_objects.json"), json.dumps(objects))
    write(os.path.join(root, "layout", "symbols_sdk.txt"),
          "# test\n02004180  SDK_Thing  lib(x.o)  arm9\n")
    # The listing: annotations only matter; word values are placeholders.
    write(os.path.join(root, "asm", "arm9", "main.s"), "\n".join([
        "\t.word\t0x00000000                  /* 02004010  -> func_02004100 (thumb) */",
        "\t.word\t0x00000000                  /* 02004300 */",
        "\t.word\t0x00000000                  /* 02004304  -> d_02004308 */",
        "\t.word\t0x00000000                  /* 02004308  -> arm9_main+0x110 */",
        "\t.word\t0x00000000                  /* 0200430C  -> mystery_sym */",
        "",
    ]))
    write(os.path.join(root, "asm", "arm9", "autoload1_main_twl.s"), "\n".join([
        "\t.word\t0x00000000                  /* 02FE0000  -> arm9_main+0x100 */",
        "\t.word\t0x00000000                  /* 02FE0010  -> arm9_main+0x180 */",
        "",
    ]))
    write(os.path.join(root, "src", "matched", "arm9", "user.c"), """\
/*
 * function: func_02004000
 * address:  0x02004000
 * symbols:
 *   g_02004200        0x02004200  data  a4
 *   tbl_alias         0x02004200  data
 *   g_02004800        0x02004800  data
 *   SDK_THING_SIZE    0x00000800  data
 *   SDK_THING_START   0x02FE0000  data
 *   g_port_alias      0x02004A00  data
 *   g_port            0x02004A00  data
 *   s_twl             0x02700010  data  twl
 *   g_member_clash    0x02004810  data
 *   func_02004100     0x02004100  thumb
 */
typedef unsigned int u32;
struct S { u32 a; u32 g_member_clash; };
extern u32 g_02004200[];
extern u32 tbl_alias[3];
extern struct S g_02004800;
extern char SDK_THING_SIZE[];
extern void SDK_THING_START(void);
extern u32 g_port_alias;
extern u32 g_port;
extern unsigned long long s_twl;
extern u32 g_member_clash;
extern void func_02004100(void (*cb)(int task), int done);
extern int (*g_fnptr_0200480C)(int);
extern u32 d_02004820;
void func_02004000(struct S *p)
{
    p->g_member_clash = g_02004200[0] + tbl_alias[1] + g_02004800.a
        + (u32)SDK_THING_SIZE + g_port_alias + g_port + (u32)s_twl + d_02004820;
    func_02004100(0, g_member_clash);
    (void)g_fnptr_0200480C;
    SDK_THING_START();
}
""")
    write(os.path.join(root, "src", "matched", "arm9", "func_02004100.c"), """\
/*
 * function: func_02004100
 * address:  0x02004100
 */
void func_02004100(void (*cb)(int task), int done)
{
    (void)cb; (void)done;
}
""")
    write(os.path.join(root, "port", "port.c"), """\
/* a port object, listed in the exclusions, and one that is not */
unsigned int g_port[4];
unsigned int d_02004820;
static int not_exported;
""")
    write(os.path.join(root, "exclusions.txt"), """\
# name address size owner
g_port   0x02004A00  0x10  port/port.c
""")


def run(tool, root, region="arm9", extra_sources=(), extra=(), expect_ok=True):
    build = root + "_build"
    out_c = os.path.join(build, "out", "%s.c" % region)
    out_h = os.path.join(build, "out", "%s.h" % region)
    os.makedirs(os.path.dirname(out_c), exist_ok=True)
    lst = os.path.join(build, "out", "sources.txt")
    sources = [
        # a dsaddr copy that does not exist yet: mapped to the original
        os.path.join(build, "dsaddr", "arm9", "user.c"),
        os.path.join(root, "src", "matched", "arm9", "func_02004100.c"),
        os.path.join(root, "port", "port.c"),
        # a generated file not made yet, and the generator's own output
        os.path.join(build, "overlay_table.c"),
        out_c,
    ] + list(extra_sources)
    write(lst, "\n".join(sources) + "\n")
    cmd = [sys.executable, tool, "--aletheia-root", root, "--sources", lst,
           "--region", region, "--exclusions", os.path.join(root, "exclusions.txt"),
           "--out-c", out_c, "--out-h", out_h, "--summary"] + list(extra)
    p = subprocess.run(cmd, capture_output=True, text=True)
    if not expect_ok:
        return p
    if p.returncode != 0:
        print(p.stdout, p.stderr)
    check("generator exit status", p.returncode == 0)
    with open(out_c, encoding="utf-8") as fh:
        c = fh.read()
    with open(out_h, encoding="utf-8") as fh:
        h = fh.read()
    return c, h, p.stdout


def names_in(c):
    """name -> (addr, bind, anchor) from the k_names table."""
    out = {}
    for m in re.finditer(r'\{ "(\w+)", 0x([0-9A-F]{8})u, (?:&bw_dsref_\w+|NULL), BW_DATA_BIND_(\w+), '
                         r'(\w+), \d+, 0x[0-9A-F]+u, (\w+), 0x([0-9A-F]{8})u \}', c):
        out[m.group(1)] = (int(m.group(2), 16), m.group(3), m.group(5))
    return out


def relocs_in(c):
    out = {}
    for m in re.finditer(r"\{ 0x([0-9A-F]{8})u, 0x([0-9A-F]{8})u, BW_DATA_RELOC_(\w+), (\d+) \}", c):
        out[int(m.group(1), 16)] = (int(m.group(2), 16), m.group(3), int(m.group(4)))
    return out


def funcs_in(c):
    out = []
    for m in re.finditer(r"\{ 0x([0-9A-F]{8})u, (\w+), (?:\"(\w+)\"|NULL) \}", c):
        out.append((int(m.group(1), 16), m.group(2), m.group(3)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    args = ap.parse_args()
    root = tempfile.mkdtemp(prefix="armdata_test_")
    try:
        build_tree(root)
        c, h, summary = run(args.tool, root)
        names = names_in(c)

        # --- names and bindings -------------------------------------------
        check("array name bound", names.get("g_02004200", (0, ""))[:2] == (0x02004200, "DS"),
              str(names.get("g_02004200")))
        check("alias bound to the same address",
              names.get("tbl_alias", (0, ""))[:2] == (0x02004200, "DS"))
        check("struct name in .bss", names.get("g_02004800", (0, ""))[:2] == (0x02004800, "DS"))
        check("linker constant", names.get("SDK_THING_SIZE", (0, ""))[:2] == (0x800, "CONST"))
        check("linker address declared as a function",
              names.get("SDK_THING_START", (0, ""))[:2] == (0x02FE0000, "DS"))
        check("alias of a port object bound to it",
              names.get("g_port_alias") == (0x02004A00, "HOST", "g_port"),
              str(names.get("g_port_alias")))
        check("twl name", names.get("s_twl", (0, ""))[:2] == (0x02700010, "DS"))
        check("function-pointer variable named by its address",
              names.get("g_fnptr_0200480C", (0, ""))[:2] == (0x0200480C, "DS"))
        check("port-defined name not bound", "g_port" not in names)
        check("port-defined unlisted name not bound", "d_02004820" not in names)
        check("member-name clash becomes a host object",
              names.get("g_member_clash", (0, ""))[:2] == (0x02004810, "OBJECT"),
              str(names.get("g_member_clash")))
        check("parameter names are not names",
              not any(n in names for n in ("cb", "task", "done")))
        check("functions are not names", "func_02004100" not in names)
        check("unlisted port definition warned",
              "d_02004820 is defined by port.c but not listed" in summary, summary)
        check("host object counted", "host objects     1" in summary, summary)
        check("missing generated file counted", "1 not generated yet" in summary, summary)
        check("unknown listing symbol warned", "mystery_sym" in summary)

        # --- the header ------------------------------------------------------
        check("header macro", "#define g_02004200 (*bw_dsref_g_02004200)" in h)
        check("header alias macro", "#define tbl_alias (*bw_dsref_tbl_alias)" in h)
        check("no macro for a port object", "#define g_port " not in h)
        check("no macro for a clash", "#define g_member_clash" not in h)
        check("host object defined", "BwDataCell g_member_clash[BW_DATA_CELLS(0x1F0u)];" in c, c[:0])
        check("host object listed", '{ 0x02004810u, 0x1F0u, g_member_clash, "g_member_clash" },' in c)
        check("variables defined", "void *bw_dsref_tbl_alias;" in c)

        # --- relocations -------------------------------------------------------
        rel = relocs_in(c)
        funcs = funcs_in(c)
        fidx = {a: i for i, (a, _f, _n) in enumerate(funcs)}
        check("table word 0: data", rel.get(0x02004200) == (0x02004300, "DATA", 0), str(rel.get(0x02004200)))
        check("table word 1: null skipped", 0x02004204 not in rel)
        check("table word 2: Thumb code",
              rel.get(0x02004208, (0, ""))[:2] == (0x02004101, "CODE"))
        check("Thumb target resolves to the build's function",
              funcs[fidx[0x02004101]][1:] == ("func_02004100", "func_02004100"),
              str(funcs))
        check("struct ptr field 1", rel.get(0x02004210, (0, ""))[:2] == (0x02004800, "DATA"))
        check("struct u32 fields not relocated", 0x0200420C not in rel and 0x02004214 not in rel)
        check("struct ptr field 2: code the build lacks",
              rel.get(0x02004218, (0, ""))[:2] == (0x02004180, "CODE"))
        check("trap for it", funcs[fidx[0x02004180]][1:] == ("bw_trap_02004180", None),
              str(funcs))
        check("trap defined", "static void bw_trap_02004180(void) { bw_data_trap(0x02004180u); }" in c)
        check("pointer into a port object",
              rel.get(0x0200421C, (0, ""))[:2] == (0x02004A04, "HOST"))
        check("pointer to data in the code region",
              rel.get(0x02004220, (0, ""))[:2] == (0x02004150, "DATA"))
        check("listing word in the data range", rel.get(0x02004304, (0, ""))[:2] == (0x02004308, "DATA"))
        check("listing section+offset", rel.get(0x02004308, (0, ""))[:2] == (0x02004110, "CODE"),
              str(rel.get(0x02004308)))
        check("listing word in code skipped", 0x02004010 not in rel)
        check("autoload word", rel.get(0x02FE0000, (0, ""))[:2] == (0x02004100, "CODE"))
        check("autoload code target the build lacks",
              rel.get(0x02FE0010, (0, ""))[:2] == (0x02004180, "CODE"))

        # --- ranges --------------------------------------------------------------
        check("static range", "{ 0x02004000u, 0x400u, 0x02004000u, 1 }" in c)
        check("autoload range reads at load + src_offset",
              "{ 0x02FE0000u, 0x20u, 0x02004440u, 1 }" in c)
        check(".bss range", "{ 0x02004400u, 0xC00u, 0, 0 }" in c)
        check("autoload .bss", "{ 0x02FE0020u, 0x20u, 0, 0 }" in c)
        check("image symbol", "const BwDataImage bw_arm9_data = {" in c)

        # --- exclusions: a pointer word inside a port object is the owner's ------
        objs_path = os.path.join(root, "layout", "data_objects.json")
        with open(objs_path, encoding="utf-8") as fh:
            objs = json.load(fh)
        write(os.path.join(root, "exclusions.txt"),
              "g_port 0x02004A00 0x10 port/port.c\n"
              "g_table_owned 0x02004200 0x0C port/port.c\n")
        write(os.path.join(root, "port", "port.c"),
              "unsigned int g_port[4];\nunsigned int d_02004820;\n"
              "unsigned int g_table_owned[3];\n")
        c2, _h2, _s2 = run(args.tool, root)
        rel2 = relocs_in(c2)
        check("words inside a port object skipped",
              not any(0x02004200 <= w < 0x0200420C for w in rel2), str(sorted(rel2)))
        check("names at a port object bound to it",
              names_in(c2).get("g_02004200", (0, "", ""))[1:] == ("HOST", "g_table_owned"))
        with open(objs_path, "w", encoding="utf-8") as fh:
            json.dump(objs, fh)

        # --- an overlay region ---------------------------------------------------
        c3, _h3, _s3 = run(args.tool, root, region="ov005")
        check("overlay image symbol", "const BwDataImage bw_ov005_data = {" in c3)
        check("overlay header", '"ov005", 5,' in c3)
        check("overlay copy range", "{ 0x02100000u, 0x800u, 0x00000000u, 1 }" in c3)
        check("overlay .bss", "{ 0x02100800u, 0x100u, 0, 0 }" in c3)

        # --- every declarator of one extern declaration is typed ----------
        sys.path.insert(0, os.path.dirname(os.path.abspath(args.tool)))
        import armdata as _ad
        got = [(d.group(2), d.group(1).strip()) for d in _ad._extern_decls(
            "extern const struct BgTemplate d_A, d_B,d_C;\n"
            "extern char *p, q[4];\n")]
        check("each declarator of a multi-name extern",
              got == [("d_A", "const struct BgTemplate"), ("d_B", "const struct BgTemplate"),
                      ("d_C", "const struct BgTemplate"), ("p", "char *"), ("q", "char")],
              str(got))

        # --- a wrong path into aletheia's tree is an error, not "generated" --
        p = run(args.tool, root, extra_sources=[
            os.path.join(root, "src", "matched", "arm9", "no_such_file.c")], expect_ok=False)
        check("missing aletheia source refused",
              p.returncode != 0 and "no such source" in p.stderr, p.stderr)
    finally:
        shutil.rmtree(root, ignore_errors=True)
        shutil.rmtree(root + "_build", ignore_errors=True)
    print("test_armdata.py: %d checks, %d failures" % (CHECKS[0], len(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
