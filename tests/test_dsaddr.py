#!/usr/bin/env python3
"""
test_dsaddr.py -- tools/dsaddr.py, the fixed-address rewrite.

Three parts:

  - unit cases, one per cast form the corpus uses, one per thing the
    rewrite must leave alone (comments, strings, literals that are not
    the operand of a pointer cast, function pointers, constant
    initializers), one per way a name stands for a DS address (a
    file-local constant, a register macro's parameter, a CALLEES
    function's parameter and its callers), and the residue
    classification and report;
  - with a compiler (--cc): the rewritten fixture parses, the
    declaration the prelude spells agrees with sdk/dsmem.h's, and every
    matched file in the current build (--sources) parses after its
    rewrite, with no diagnostic its original did not have;
  - with an aletheia checkout (--aletheia-root): a dry run over every
    matched source, asserting the rewrite is idempotent and keeps every
    line where it was, and printing how many files and casts it touches;
    and the residue report over the same corpus.

    python tests/test_dsaddr.py [--aletheia-root DIR] [--sources FILE]
                                [--cc CC [--cc-id GNU|Clang|MSVC]
                                 [--cflags "FLAGS"]] [unittest args]

ctest runs it with everything the build knows.  Standard library only.
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.dont_write_bytecode = True     # no __pycache__ in the source tree
sys.path.insert(0, os.path.join(ROOT, "tools"))
import dsaddr  # noqa: E402

CFG = argparse.Namespace(aletheia_root=None, sources=None, cc=None,
                         cc_id="GNU", cflags="")


def rw(src: str) -> str:
    """The rewritten body (the prelude stripped), asserting the prelude is
    there exactly when something was rewritten."""
    out, sites, _ = dsaddr.rewrite(src, "x.c")
    if not sites:
        assert out == src
        return out
    head = out.split("\n", 3)
    assert head[0].startswith("/* tools/dsaddr.py"), head[0]
    assert head[1] == dsaddr.PRELUDE_DECL, head[1]
    assert head[2] == '#line 1 "x.c"', head[2]
    return head[3]


BODY = "void t(void)\n{\n%s\n}\n"


def kinds(src: str, wrap: bool = True):
    """The residue kinds of `src`, as statements in a function unless
    `wrap` is false."""
    return sorted(s.kind for s in dsaddr.analyse(BODY % src if wrap else src)[1])


class Forms(unittest.TestCase):
    """Each cast form in the corpus, rewritten."""

    def check(self, src, want):
        # statements, so inside a function (at file scope a cast in an
        # initializer is left alone)
        src, want = BODY % src, BODY % want
        self.assertEqual(rw(src), want)
        # idempotent, and lines stay where they were
        again, sites, _ = dsaddr.rewrite(dsaddr.rewrite(src, "x.c")[0], "x.c")
        self.assertEqual(sites, [])
        self.assertEqual(rw(src).count("\n"), src.count("\n"))

    def test_deref_volatile_typedef(self):
        self.check("*(vu16 *)0x04000208 = 1;",
                   "*((vu16 *)bw_ds_ptr(0x04000208u)) = 1;")

    def test_parenthesised_literal(self):
        self.check("x = *(volatile u32 *)(0x04000000);",
                   "x = *((volatile u32 *)bw_ds_ptr(0x04000000u));")
        self.check("x = *(volatile u32 *)( ( 0x04000000 ) );",
                   "x = *((volatile u32 *)bw_ds_ptr(0x04000000u));")

    def test_ioreg_addr(self):
        # aletheia's sdk_ioreg.h: IOREG_ADDR(a) is the plain number on the DS
        self.check("x = *(volatile u16 *)IOREG_ADDR(0x04000280);",
                   "x = *((volatile u16 *)bw_ds_ptr(0x04000280u));")

    def test_suffixes(self):
        self.check("p = (u8 *)0x05000000u;", "p = ((u8 *)bw_ds_ptr(0x05000000u));")
        self.check("p = (u8 *)0x05000000UL;", "p = ((u8 *)bw_ds_ptr(0x05000000u));")
        self.check("p = (u8 *)0X6000000;", "p = ((u8 *)bw_ds_ptr(0x06000000u));")
        self.check("p = (u8 *)33554432;", "p = ((u8 *)bw_ds_ptr(0x02000000u));")

    def test_qualifiers_struct_and_stars(self):
        self.check("q = (const struct Work *)0x027FFC00;",
                   "q = ((const struct Work *)bw_ds_ptr(0x027FFC00u));")
        self.check("q = (u32 volatile * const *)0x02FFFC80;",
                   "q = ((u32 volatile * const *)bw_ds_ptr(0x02FFFC80u));")
        self.check("s = *(char **)0x02000000;",
                   "s = *((char **)bw_ds_ptr(0x02000000u));")
        self.check("v = *(volatile unsigned short*)0x04000006;",
                   "v = *((volatile unsigned short*)bw_ds_ptr(0x04000006u));")

    def test_regions(self):
        for addr in (0x02000000, 0x03000000, 0x04000000, 0x05000000,
                     0x06000000, 0x07000000, 0x07FFFFFF):
            self.check("p = (u8 *)0x%08X;" % addr,
                       "p = ((u8 *)bw_ds_ptr(0x%08Xu));" % addr)

    def test_inside_a_macro(self):
        self.check("#define REG_IME (*(vu16 *)0x04000208)\n",
                   "#define REG_IME (*((vu16 *)bw_ds_ptr(0x04000208u)))\n")
        self.check("#define R(x) \\\n    (*(vu32 *)0x04000000 = (x))\n",
                   "#define R(x) \\\n    (*((vu32 *)bw_ds_ptr(0x04000000u)) = (x))\n")

    def test_split_over_lines(self):
        self.check("a = *(vu16 *)\n    0x04000208;\nb = 1;\n",
                   "a = *((vu16 *)bw_ds_ptr(0x04000208u))\n;\nb = 1;\n")
        self.check("#define R (*(vu16 *) \\\n  0x04000208)\nint b;\n",
                   "#define R (*((vu16 *)bw_ds_ptr(0x04000208u))\\\n)\nint b;\n")
        self.check("#define R (*(vu16 *) \\\r\n  0x04000208)\r\nint b;\r\n",
                   "#define R (*((vu16 *)bw_ds_ptr(0x04000208u))\\\n)\r\nint b;\r\n")

    def test_address_expression(self):
        self.check("GX_Load((void *)(0x05000000 + offset), n);",
                   "GX_Load(((void *)bw_ds_ptr(0x05000000 + offset)), n);")
        self.check("r = (vu32 *)(0x040000B0 + dmaNo * 12);",
                   "r = ((vu32 *)bw_ds_ptr(0x040000B0 + dmaNo * 12));")

    def test_pointer_typedef_without_star(self):
        self.check("typedef volatile u16 *REGP;\nv = *(REGP)0x04000004;",
                   "typedef volatile u16 *REGP;\nv = *((REGP)bw_ds_ptr(0x04000004u));")

    def test_pointer_to_function_pointer_is_data(self):
        self.check("(*(void (**)(u32))0x020A8144)(x);",
                   "(*((void (**)(u32))bw_ds_ptr(0x020A8144u)))(x);")
        self.check("a = (u8 (*)[4])0x06000000;",
                   "a = ((u8 (*)[4])bw_ds_ptr(0x06000000u));")

    def test_after_keyword_and_in_cast_chain(self):
        self.check("return (u8 *)0x02000000;",
                   "return ((u8 *)bw_ds_ptr(0x02000000u));")
        self.check("n = (u32)(u8 *)0x02000000;",
                   "n = (u32)((u8 *)bw_ds_ptr(0x02000000u));")

    def test_inside_function_body_braces(self):
        src = ("void f(void)\n{\n    if (x) {\n        *(vu16 *)0x04000208 = 0;\n    }\n}\n")
        self.assertIn("bw_ds_ptr(0x04000208u)", rw(src))

    def test_prelude_names_the_original(self):
        out, _, _ = dsaddr.rewrite(BODY % "p = (u8 *)0x02000000;",
                                   "C:\\a\\b \"c\".c")
        self.assertIn('#line 1 "C:/a/b \\"c\\".c"\n', out)


class LeftAlone(unittest.TestCase):
    """What the rewrite must not touch."""

    def same(self, src, wrap=True):
        if wrap:
            src = BODY % src
        out, sites, _ = dsaddr.rewrite(src, "x.c")
        self.assertEqual(sites, [])
        self.assertEqual(out, src)

    def test_comments_and_strings(self):
        self.same("/* (vu16 *)0x04000208 */ x = 1;\n")
        self.same("// *(vu16 *)0x04000208 = 1;\nx = 1;\n")
        self.same("// continued \\\n *(vu16 *)0x04000208 = 1;\nx = 1;\n")
        self.same("// continued \\\r\n *(vu16 *)0x04000208 = 1;\r\nx = 1;\r\n")
        self.same('s = "(vu16 *)0x04000208";\n')
        self.same('s = "\\"(vu16 *)0x04000208";\n')
        self.same("c = '\\''; s = \"(u8 *)0x02000000\";\n")

    def test_literals_not_cast_to_a_pointer(self):
        self.same("x = 0x04000000;\n")
        self.same("n = (u32)0x04000000;\n")
        self.same("n = (unsigned long)0x02000000;\n")
        self.same("if (p == 0x02000000) f(0x04000000);\n")
        self.same("#define REG_BASE 0x04000000\n", wrap=False)
        self.same("#if ADDR == 0x02000000\n#endif\n")
        self.same("y = f(a) (0x04000000);\n")
        self.same("y = (a) + 0x04000000;\n")
        self.same("y = sizeof(u8 *) + 0x04000000;\n")
        self.same("y = (u8 *)(base + 0x06000000);\n")

    def test_outside_the_ds_regions(self):
        self.same("p = (u8 *)0x01FF8000;\n")
        self.same("p = (u8 *)0x08000000;\n")
        self.same("p = (u8 *)0x01FFFFFF;\n")
        self.same("p = (void *)0x45585048;\n")
        self.same("p = (u8 *)0x204000000;\n")
        self.same("x = f_0x04000000;\n")

    def test_function_pointer(self):
        self.same("((void (*)(void))0x02005124)();\n")
        self.assertEqual(kinds("((void (*)(void))0x02005124)();\n"), ["funcptr-cast"])

    def test_constant_initializers(self):
        src = "static vu16 *const r = (vu16 *)0x04000208;\n"
        self.same(src, wrap=False)
        self.assertEqual(kinds(src, wrap=False), ["static-init"])
        src = "void *tab[] = { (void *)0x02000000, 0 };\n"
        self.same(src, wrap=False)
        self.assertEqual(kinds(src, wrap=False), ["static-init"])
        src = "void f(void) {\n    static u8 *p = (u8 *)0x02000000;\n}\n"
        self.same(src, wrap=False)
        self.assertEqual(kinds(src, wrap=False), ["static-init"])
        # a non-static local is an ordinary assignment
        src = "void f(void) {\n    u8 *p = (u8 *)0x02000000;\n}\n"
        self.assertIn("bw_ds_ptr", rw(src))
        src = ("void f(void) {\n    static const struct { void *p; } t[] = {\n"
               "        { (void *)0x02000000 },\n    };\n}\n")
        self.same(src, wrap=False)
        self.assertEqual(kinds(src, wrap=False), ["static-init"])
        src = "void f(void) {\n    static int n;\n    n = *(int *)0x02000000;\n}\n"
        self.assertIn("bw_ds_ptr", rw(src))

    def test_residue_kinds(self):
        self.assertEqual(kinds("#define REG_BASE 0x04000000\n"), ["define"])
        self.assertEqual(kinds("x = 0x04000000 + 8;\n"), ["bare"])
        self.assertEqual(kinds("n = (int)0x020A7174;\n"), ["int-cast"])
        self.assertEqual(kinds("x = 0x01000000;\n"), [])


def check_rewrite(test, src, want):
    """`src` rewrites to `want` (bodies, prelude stripped), the rewrite is
    idempotent and keeps every line where it was."""
    test.assertEqual(rw(src), want)
    out = dsaddr.rewrite(src, "x.c")[0]
    again, sites, _ = dsaddr.rewrite(out, "x.c")
    test.assertEqual(sites, [])
    test.assertEqual(again, out)
    test.assertEqual(rw(src).count("\n"), src.count("\n"))


def residue_kinds(src):
    return [s.kind for s in dsaddr.analyse(src)[1]]


class Constants(unittest.TestCase):
    """A file-local #define standing for a DS address, used as a cast
    operand: rewritten at the use site."""

    def test_plain_use(self):
        src = ("#define REG_KEYINPUT 0x04000130\n"
               "void f(void) { k = *(vu16 *)REG_KEYINPUT; }\n")
        check_rewrite(self, src,
                      "#define REG_KEYINPUT 0x04000130\n"
                      "void f(void) { k = *((vu16 *)bw_ds_ptr(REG_KEYINPUT)); }\n")
        self.assertEqual(residue_kinds(src), ["define-resolved"])

    def test_expression_operand_and_parens(self):
        src = ("#define REG_DMA0SAD_ADDR 0x040000B0\n"
               "void f(int n) { p = (vu32 *)(REG_DMA0SAD_ADDR + n * 12);\n"
               "  q = (vu32 *)(REG_DMA0SAD_ADDR); }\n")
        check_rewrite(self, src,
                      "#define REG_DMA0SAD_ADDR 0x040000B0\n"
                      "void f(int n) { p = ((vu32 *)bw_ds_ptr(REG_DMA0SAD_ADDR + n * 12));\n"
                      "  q = ((vu32 *)bw_ds_ptr(REG_DMA0SAD_ADDR)); }\n")

    def test_through_another_constant(self):
        src = ("#define HW_REG_BASE 0x04000000\n"
               "#define REG_IME_ADDR (HW_REG_BASE + 0x208)\n"
               "void f(void) { *(vu16 *)REG_IME_ADDR = 1; }\n")
        check_rewrite(self, src,
                      "#define HW_REG_BASE 0x04000000\n"
                      "#define REG_IME_ADDR (HW_REG_BASE + 0x208)\n"
                      "void f(void) { *((vu16 *)bw_ds_ptr(REG_IME_ADDR)) = 1; }\n")

    def test_in_another_macro_body(self):
        src = ("#define HW_BG_VRAM 0x06000000\n"
               "#define BG(n) ((void *)(HW_BG_VRAM + (n) * 0x800))\n")
        check_rewrite(self, src,
                      "#define HW_BG_VRAM 0x06000000\n"
                      "#define BG(n) (((void *)bw_ds_ptr(HW_BG_VRAM + (n) * 0x800)))\n")

    def test_left_alone(self):
        same = [
            # not a DS value
            "#define OFS 0x208\nvoid f(void) { p = (u8 *)OFS; }\n",
            # used as a number
            "#define BASE 0x04000000\nvoid f(void) { n = BASE + 8; }\n",
            # postfix binds tighter than the cast: the operand is TAB[2]
            "#define TAB 0x02000000\nvoid f(void) { p = (u8 *)TAB[2]; }\n",
            # a function-like macro call is not the constant
            "#define ADDR(x) (0x04000000 + (x))\nvoid f(void) { p = (u8 *)ADDR(2); }\n",
            # defined twice: which one a use sees depends on where it is
            "#define B 0x04000000\n#undef B\n#define B 8\nvoid f(void) { p = (u8 *)B; }\n",
        ]
        for src in same:
            out, sites, _ = dsaddr.rewrite(src, "x.c")
            self.assertEqual(sites, [], src)
            self.assertEqual(out, src)

    def test_constant_initializer(self):
        src = ("#define REG 0x04000000\n"
               "static vu32 *const r = (vu32 *)REG;\n")
        out, sites, residue = dsaddr.rewrite(src, "x.c")
        self.assertEqual((sites, out), ([], src))
        self.assertIn("static-init", [s.kind for s in residue])

    def test_other_uses_keep_it_residue(self):
        src = ("#define REG 0x04000000\n"
               "void f(void) { p = (u8 *)REG; n = REG + 4; }\n")
        self.assertIn("bw_ds_ptr(REG)", rw(src))
        r = [s for s in dsaddr.analyse(src)[1] if s.kind == "define"]
        self.assertEqual(len(r), 1)
        self.assertEqual(r[0].uses, {"cast": 1, "other": 1})


REG16 = "#define REG16(a) (*(volatile u16 *)(a))\n"


class Macros(unittest.TestCase):
    """A function-like macro that casts its parameter: its body is
    rewritten when every invocation passes a DS address."""

    def test_register_macro(self):
        src = REG16 + ("void f(void) { REG16(0x04000050) = 0;\n"
                       "  REG16(0x0400000A) = REG16(0x0400000A) & 0x43; }\n")
        check_rewrite(self, src,
                      "#define REG16(a) (*((volatile u16 *)bw_ds_ptr(a)))\n"
                      "void f(void) { REG16(0x04000050) = 0;\n"
                      "  REG16(0x0400000A) = REG16(0x0400000A) & 0x43; }\n")
        self.assertEqual(residue_kinds(src), ["macro-arg"] * 3)

    def test_argument_through_constant_or_offset(self):
        src = ("#define BASE 0x04000000\n" + REG16 +
               "void f(int i) { REG16(BASE + 8) = 1; REG16((0x04001000 + i)) = 2; }\n")
        self.assertIn("(*((volatile u16 *)bw_ds_ptr(a)))", rw(src))

    def test_spliced_multi_line_body(self):
        src = ("#define LO(a) \\\n"
               "    do { \\\n"
               "        int v = *(vu16 *)(a) & ~0x3f; \\\n"
               "        *(vu16 *)(a) = v; \\\n"
               "    } while (0)\n"
               "void f(void) { LO(0x04000048); LO(0x04001048); }\n")
        check_rewrite(self, src,
                      "#define LO(a) \\\n"
                      "    do { \\\n"
                      "        int v = *((vu16 *)bw_ds_ptr(a)) & ~0x3f; \\\n"
                      "        *((vu16 *)bw_ds_ptr(a)) = v; \\\n"
                      "    } while (0)\n"
                      "void f(void) { LO(0x04000048); LO(0x04001048); }\n")

    def test_second_parameter(self):
        src = ("#define SET(v, a) (*(vu32 *)(a) = (v))\n"
               "void f(void) { SET(1, 0x04000000); }\n")
        self.assertIn("(*((vu32 *)bw_ds_ptr(a)) = (v))", rw(src))

    def test_mixed_invocations_leave_it_alone(self):
        src = REG16 + "void f(u32 x) { REG16(0x04000050) = 0; REG16(x) = 1; }\n"
        out, sites, residue = dsaddr.rewrite(src, "x.c")
        self.assertEqual((sites, out), ([], src))
        self.assertEqual([s.kind for s in residue], ["macro-arg-mixed"])
        self.assertEqual(residue[0].note, "REG16")

    def test_left_alone(self):
        same = [
            # no invocation: nothing shows what the parameter holds
            REG16,
            # a struct-field accessor: the parameter is not the operand
            "#define W(o) (*(u32 *)(p + (o)))\nvoid f(void) { W(0x04000000) = 1; }\n",
            # used without arguments (passed on to another macro)
            REG16 + "#define AP(m) m(0x04000000)\nvoid f(void) { REG16(0x04000000) = AP(REG16); }\n",
            # an invocation in a constant initializer
            REG16 + "static u16 *p = &REG16(0x04000000);\n",
        ]
        for src in same:
            out, sites, _ = dsaddr.rewrite(src, "x.c")
            self.assertEqual(sites, [], src)
            self.assertEqual(out, src)


CALLEE = ("void func_0207D658(u32 addr, int plane, int brightness)\n{\n"
          "    *(vu16 *)addr = (u16)(plane | 0x80);\n"
          "    *(vu16 *)(addr + 4) = (u16)brightness;\n}\n")


class Callees(unittest.TestCase):
    """Functions in CALLEES translate their address parameter; callers
    hand them the DS address."""

    def test_every_entry_documented(self):
        for name, ent in dsaddr.CALLEES.items():
            self.assertTrue(ent["params"], name)
            self.assertEqual(dsaddr.CALLEE_NAMES[name], name)
            for a in ent["aliases"]:
                self.assertEqual(dsaddr.CALLEE_NAMES[a], name)
        with open(os.path.join(ROOT, "tools", "dsaddr.py")) as f:
            tool = f.read()
        for name in dsaddr.CALLEES:
            # each entry is preceded by a comment giving the evidence
            self.assertRegex(tool, r"#[^\n]*\n(?:\s*#[^\n]*\n)*\s*\"%s\":" % name)

    def test_callee_body(self):
        check_rewrite(self, CALLEE, CALLEE
                      .replace("*(vu16 *)addr", "*((vu16 *)bw_ds_ptr(addr))")
                      .replace("*(vu16 *)(addr + 4)", "*((vu16 *)bw_ds_ptr(addr + 4))"))

    def test_only_listed_functions_and_parameters(self):
        same = [
            CALLEE.replace("func_0207D658", "func_0207D659"),
            CALLEE.replace("addr", "base"),
            # a prototype is not the definition
            "void func_0207D658(u32 addr, int p, int b);\n"
            "void g(u32 addr) { *(vu16 *)addr = 0; }\n",
        ]
        for src in same:
            out, sites, _ = dsaddr.rewrite(src, "x.c")
            self.assertEqual(sites, [], src)
            self.assertEqual(out, src)

    def test_callers(self):
        src = ("void f(void) {\n"
               "    func_0207D658(0x04000050, 1, 2);\n"
               "    G2x_SetBlendBrightness_((void *)0x04001050, 1, 2);\n"
               "    func_0207D658(0x04000050 + 0x1000, 1, 2);\n"
               "    other((void *)0x04000050, 1, 2);\n"
               "    func_0207D658((u32)p, (int)(u8 *)0x02000000, 2);\n}\n")
        out, sites, residue = dsaddr.rewrite(src, "x.c")
        # the pointer cast standing as the address argument stays a DS
        # address; one passed elsewhere is rewritten
        self.assertIn("G2x_SetBlendBrightness_((void *)0x04001050, 1, 2);", out)
        self.assertIn("other(((void *)bw_ds_ptr(0x04000050u)), 1, 2);", out)
        self.assertIn("(int)((u8 *)bw_ds_ptr(0x02000000u))", out)
        self.assertEqual([s.kind for s in residue], ["callee-arg"] * 3)
        self.assertEqual({s.note for s in residue}, {"func_0207D658"})


class Residue(unittest.TestCase):
    """What the analysis records about a literal it leaves alone, and the
    report category it gets."""

    def one(self, body, wrap=True):
        src = BODY % body if wrap else body
        r = dsaddr.analyse(src)[1]
        self.assertEqual(len(r), 1, r)
        s = r[0]
        line = src.split("\n")[s.line - 1].strip()
        return s, dsaddr.classify(s, line, src[s.start:s.end], {}, "x.c")

    def test_operators_and_calls(self):
        s, cat = self.one("x = (v & 0x07000000) >> 24;")
        self.assertEqual((s.prev_op, s.next_op, cat), ("&", ")", "mask"))
        s, cat = self.one("if (a >= 0x02000000 && a < 8) {}")
        self.assertEqual((s.prev_op, cat), (">=", "range"))
        s, cat = self.one("if ((u32)ptr < 0x02400000) {}")
        self.assertEqual(cat, "range-host")
        s, cat = self.one("g(1, h(0x021F55B4, 2));")
        self.assertEqual((s.call, cat), (("h", 0, True), "call-image"))
        s, cat = self.one("fill(0, 0x06800000, 0x20000);")
        self.assertEqual((s.call, cat), (("fill", 1, True), "call-unknown"))
        s, cat = self.one("PXI_SendWordByFifo(4, 0x03002000, 0);")
        self.assertEqual(cat, "pxi")
        s, cat = self.one("e->c = 0x021BA521;")
        self.assertEqual(cat, "held-image")
        s, cat = self.one("u32 ptr = 0x06400000;")
        self.assertEqual(cat, "held-hw")
        s, cat = self.one("n = v / 100000000;")
        self.assertEqual(cat, "decimal")

    def test_callee_parameter_types(self):
        src = BODY % "set(0x0400006C, 0); seti(0x0400006C, 0);"
        res = dsaddr.analyse(src)[1]
        idx = {"set": ("a.c", [True, False]), "seti": ("b.c", [False, False])}
        cats = [dsaddr.classify(s, "", "0x0400006C", idx, "x.c") for s in res]
        self.assertEqual(cats, ["pointer-param", "call-int"])

    def test_asm_body(self):
        src = ("asm void f(void)\n{\n    ldr r0, =0x027FFC00\n"
               "    dcd 0x02000000\n    bx lr\n}\n")
        res = dsaddr.analyse(src)[1]
        self.assertEqual([s.asm for s in res], [True, True])
        self.assertEqual({dsaddr.classify(s, "", "0x0", {}) for s in res}, {"asm"})

    def test_every_kind_has_a_category(self):
        for k in ("macro-arg", "callee-arg", "define-resolved", "directive",
                  "static-init", "macro-arg-mixed"):
            self.assertIn(k, dsaddr.CATEGORIES)


class Report(unittest.TestCase):

    def test_generated_and_stable(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "arm9"))
            files = {
                "arm9/a.c": REG16 + "void a(void) { REG16(0x04000050) = 0; x = (v & 0x07000000); }\n",
                "arm9/b.c": "void b(void) { func_0207D658(0x04000050, 1, 2); e->c = 0x021BA521; }\n",
                "arm9/c.c": "void c(void) { func_0207D658(p, 1, 2); }\n",
            }
            for name, text in files.items():
                with open(os.path.join(d, name), "w", newline="") as f:
                    f.write(text)
            one = dsaddr.build_report(d)
            self.assertEqual(one, dsaddr.build_report(d))
            for want in ("## Register macro arguments (resolved, 1)",
                         "## Bit patterns (harmless, 1)",
                         "## Address arguments of translating callees (resolved, 1)",
                         "## Game code or data addresses held as integers (port, 1)",
                         "| func_0207D658 | arm9/c.c:1 | `p` | check |",
                         "| func_0207D658 | arm9/b.c:1 | `0x04000050` | yes |",
                         "| `REG16(a)` | arm9/a.c:1 |"):
                self.assertIn(want, one)
            self.assertNotIn(d, one)
            for c in one:
                self.assertLess(ord(c), 128)     # plain ASCII: no em dashes


# ---------------------------------------------------------------------------
# With a compiler
# ---------------------------------------------------------------------------

FIXTURE = r"""
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef volatile unsigned short vu16;
typedef volatile unsigned int vu32;
typedef volatile u16 *REGP;
struct Work { u32 a, b; };
#define REG_IME (*(vu16 *)0x04000208)
#define REG_DISPCNT (*(volatile u32 *) \
    (0x04000000))
#define REG16(a) (*(vu16 *)(a))
#define HW_BG_VRAM 0x06000000
static u32 static_base = 0x04000000;   /* not a cast: left alone */
static vu16 *const static_reg = (vu16 *)0x04000208;   /* constant: left alone */
u32 fixture(u32 offset, int dmaNo);
void func_0207D658(u32 addr, int plane, int brightness);
void func_0207D658(u32 addr, int plane, int brightness)
{
    *(vu16 *)addr = (u16)(plane | 0x80);
    *(vu16 *)(addr + 4) = (u16)brightness;
}
u32 fixture(u32 offset, int dmaNo)
{
    u32 r = 0;
    const struct Work *w = (const struct Work *)0x027FFC00;
    u8 (*rows)[4] = (u8 (*)[4])0x06000000;
    void (**slot)(u32) = (void (**)(u32))0x020A8144;
    REG_IME = 1;
    REG_DISPCNT |= 0x10000;
    r += *(vu16 *)0x04000006;
    r += *(volatile unsigned short*)(0x04000130);
    r += *(REGP)0x04000004;
    r += w->a + rows[1][2] + (slot != 0);
    r += *(vu32 *)(0x040000B0 + dmaNo * 12);
    r += ((u8 *)0x05000000u)[offset];
    r += *(u8 *)(0x07000000 + offset);
    r += (const void *)(u8 *)0x02000000 != 0;
    r += static_base + *static_reg;
    REG16(0x04000050) = 1;
    r += *(u8 *)(HW_BG_VRAM + offset);
    func_0207D658(0x04000050, 1, 2);
    return r; /* (vu16 *)0x04000208 in a comment */
}
"""


def compile_only(path, include_dirs=()):
    """Syntax-check `path`; returns (ok, diagnostics)."""
    flags = shlex.split(CFG.cflags or "")
    if CFG.cc_id == "MSVC":
        cmd = [CFG.cc, "/nologo", "/Zs", "/W3"] + flags
        cmd += ["/I" + d for d in include_dirs] + [path]
    else:
        cmd = [CFG.cc, "-std=c99", "-fsyntax-only", "-Wall", "-Wextra",
               "-Wno-unused-parameter", "-Wno-unused-variable",
               "-Wno-missing-prototypes", "-fsigned-char"] + flags
        cmd += ["-I" + d for d in include_dirs] + [path]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True, errors="replace")
    out = p.stdout
    if CFG.cc_id == "MSVC":
        # cl echoes the source name; that is not a diagnostic
        out = "\n".join(l for l in out.splitlines()
                        if l.strip() and l.strip() != os.path.basename(path))
    return p.returncode == 0, out.strip()


def _diag_lines(text):
    """Diagnostics with the file name dropped (the copy and the original
    name the same file and line through #line)."""
    out = []
    for l in text.splitlines():
        m = re.search(r":(\d+)(?::\d+)?: ?(warning|error)\b.*$", l)
        if m:
            out.append(m.group(0))
    return sorted(out)


class Compiles(unittest.TestCase):

    def setUp(self):
        if not CFG.cc:
            self.skipTest("no --cc given")
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        if hasattr(self, "tmp"):
            self.tmp.cleanup()

    def write(self, name, text):
        p = os.path.join(self.tmp.name, name)
        with open(p, "w", newline="") as f:
            f.write(text)
        return p

    def test_fixture_parses_warning_free(self):
        orig = self.write("fixture.c", FIXTURE)
        out, sites, residue = dsaddr.rewrite(FIXTURE, orig)
        self.assertEqual(len(sites), 16)
        self.assertEqual(sorted(s.kind for s in residue),
                         ["bare", "callee-arg", "define-resolved",
                          "macro-arg", "static-init"])
        p = self.write("fixture_rw.c", out)
        ok, diag = compile_only(p)
        self.assertTrue(ok, diag)
        self.assertEqual(diag, "")

    def test_prelude_agrees_with_dsmem_h(self):
        # A conflicting redeclaration is a hard error in C, so this is the
        # proof that `unsigned int` is u32 on this compiler and target.
        p = self.write("agree.c", dsaddr.PRELUDE_DECL + "\n"
                       '#include "dsmem.h"\n'
                       "void *agree(void);\n"
                       "void *agree(void) { return bw_ds_ptr(0x04000208u); }\n")
        ok, diag = compile_only(p, [os.path.join(ROOT, "sdk")])
        self.assertTrue(ok, diag)
        self.assertEqual(diag, "")

    def test_build_sources_parse_after_rewrite(self):
        if not CFG.sources:
            self.skipTest("no --sources given")
        with open(CFG.sources) as f:
            srcs = [l.strip() for l in f if l.strip()]
        self.assertTrue(srcs)
        changed = []
        for i, src in enumerate(srcs):
            text = dsaddr._read(src)
            out, sites, _ = dsaddr.rewrite(text, src)
            if not sites:
                self.assertEqual(out, text)
                continue
            changed.append((src, len(sites)))
            p = self.write("b%d.c" % i, out)
            inc = [os.path.dirname(src)]
            ok0, d0 = compile_only(src, inc)
            ok1, d1 = compile_only(p, inc)
            self.assertTrue(ok1 or not ok0, "%s: %s" % (src, d1))
            # nothing new; a rewrite may remove a diagnostic (an integer
            # cast to a pointer of another size becomes a call)
            new = sorted(set(_diag_lines(d1)) - set(_diag_lines(d0)))
            self.assertEqual(new, [], src)
        print("\n  build sources: %d, rewritten: %d%s" % (
            len(srcs), len(changed),
            "".join("\n    %s (%d casts)" % c for c in changed)),
            file=sys.stderr)


class Corpus(unittest.TestCase):

    def test_every_matched_file(self):
        if not CFG.aletheia_root:
            self.skipTest("no --aletheia-root given")
        root = os.path.join(CFG.aletheia_root, "src", "matched")
        self.assertTrue(os.path.isdir(root), root)
        files = casts = 0
        changed = 0
        for d, _, names in os.walk(root):
            for nm in names:
                if not nm.endswith(".c"):
                    continue
                path = os.path.join(d, nm)
                src = dsaddr._read(path)
                out, sites, _ = dsaddr.rewrite(src, path)
                files += 1
                if sites:
                    changed += 1
                    casts += len(sites)
                    # the prelude adds three lines and nothing else moves
                    self.assertEqual(out.count("\n"), src.count("\n") + 3, path)
                    again, more, _ = dsaddr.rewrite(out, path)
                    self.assertEqual(more, [], path)
                    self.assertEqual(again, out, path)
                else:
                    self.assertEqual(out, src, path)
        print("\n  corpus: %d files, %d rewritten, %d casts" % (files, changed, casts),
              file=sys.stderr)
        self.assertGreater(casts, 2000)

    def test_report(self):
        # every residue site gets a category (an unknown one would raise),
        # and the register macros, constants and callees each rewrite some
        if not CFG.aletheia_root:
            self.skipTest("no --aletheia-root given")
        text = dsaddr.build_report(os.path.join(CFG.aletheia_root, "src", "matched"))
        # (Since aletheia's wave 20 no src/matched file reaches a DS address
        # through a callee, so that count may be absent.)
        m = re.search(r"(\d+) through a define, (\d+) through a macro", text)
        self.assertTrue(m, text[:2000])
        self.assertTrue(all(int(g) > 0 for g in m.groups()), m.group(0))
        print("\n  report: %s" % m.group(0), file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--aletheia-root")
    ap.add_argument("--sources")
    ap.add_argument("--cc")
    ap.add_argument("--cc-id", default="GNU")
    ap.add_argument("--cflags", default="")
    a, rest = ap.parse_known_args()
    CFG.__dict__.update(vars(a))
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)



class Width(unittest.TestCase):
    """--width (a big-endian target): a register cast passes its access width
    to bw_ds_ptr_w; a `void *` block of registers passes the word's, not GNU
    C's sizeof (void) of 1, which would map it at the byte rule."""

    def rewrite(self, src):
        old = dsaddr.WIDTH
        dsaddr.WIDTH = True
        try:
            return dsaddr.rewrite(BODY % src, "x.c")[0]
        finally:
            dsaddr.WIDTH = old

    def test_typed_access(self):
        self.assertIn("bw_ds_ptr_w(0x04000280u, sizeof(*(volatile u16 *)0))",
                      self.rewrite("x = *(volatile u16 *)0x04000280;"))

    def test_void_block(self):
        self.assertIn("bw_ds_ptr_w(0x04000640u, 4)",
                      self.rewrite("MI_Copy64B((const void *)0x04000640, m);"))

    def test_void_argument_takes_the_parameter_width(self):
        # no prototype: a word
        self.assertIn("bw_ds_ptr_w(0x0400006Cu, 4)",
                      self.rewrite("Set(0, (void *)0x0400006C);"))
        old = dsaddr.WIDTH
        dsaddr.WIDTH = True
        try:
            out = dsaddr.rewrite("void Set(int b, volatile u16 *reg);\n"
                                 "void Other(struct Task *t);\n"
                                 + BODY % "Set(0, (void *)0x0400006C);", "x.c")[0]
        finally:
            dsaddr.WIDTH = old
        self.assertIn("bw_ds_ptr_w(0x0400006Cu, sizeof(*(volatile u16 *)0))", out)
        # a struct parameter (maybe incomplete in the unit): a word
        old = dsaddr.WIDTH
        dsaddr.WIDTH = True
        try:
            out = dsaddr.rewrite("void Run(struct Task *t);\n"
                                 + BODY % "Run((void *)0x04000100);", "x.c")[0]
        finally:
            dsaddr.WIDTH = old
        self.assertIn("bw_ds_ptr_w(0x04000100u, 4)", out)


if __name__ == "__main__":
    main()
