"""Run-time-loaded modules: reading a made-up RSO and REL, preparing their code, and the C they become."""
import io
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).parent.parent
sys.path.insert(0, str(ROOT / "recomp/translate"))

import emit
import modules
from decode import Program
from functions import add_starts

LIS_R3, ADDI_R3, BL, BLR, NOP = 0x3c600000, 0x38630000, 0x48000001, 0x4e800020, 0x60000000
CODE_AT, CODE_WORDS = 0x100, 6


def made_up_rso():
    """Two functions: the first calls the second and the program's `imported`, and loads
    the address of something in section 2. Branches and fields are left for the linker."""
    code = [BL, BL, LIS_R3, ADDI_R3, BLR, BLR]  # Function at 0 ends at 0x10; function at 0x14.
    data = bytearray(0x400)
    struct.pack_into(f">{CODE_WORDS}I", data, CODE_AT, *code)
    struct.pack_into(">I", data, 0x8, 3)        # Sections.
    struct.pack_into(">I", data, 0xc, 0x58)     # Section table.
    sections = [(0, 0), (CODE_AT, CODE_WORDS * 4), (0x200, 0x20)]
    for number, (offset, size) in enumerate(sections):
        struct.pack_into(">II", data, 0x58 + number * 8, offset, size)

    def table(at, entries):
        for index, (offset, symbol, kind, addend) in enumerate(entries):
            struct.pack_into(">III", data, at + index * 12, offset, symbol << 8 | kind, addend)
        return at, len(entries) * 12

    internal = table(0x280, [(CODE_AT, 1, modules.REL24, 0x14), (CODE_AT + 10, 2, modules.HA, 0x10),
                             (CODE_AT + 14, 2, modules.LO, 0x10)])
    external = table(0x2c0, [(CODE_AT + 4, 0, modules.REL24, 0)])
    struct.pack_into(">IIII", data, 0x300, 0, 0x14, 1, 0)  # Export: the second function.
    data[0x310:0x318] = b"second\0\0"
    struct.pack_into(">I", data, 0x320, 0)  # Import 0's name.
    data[0x330:0x33a] = b"imported\0\0"
    struct.pack_into(">10I", data, 0x30, *internal, *external, 0x300, 16, 0x310, 0x320, 12, 0x330)
    return bytes(data)


class Modules(unittest.TestCase):
    def setUp(self):
        self.module = modules.parse_rso("test", made_up_rso())
        self.prepared = modules.prepare(self.module, 0xc0000000, 0xc1000000)

    def test_reads_tables(self):
        self.assertEqual(self.module.imports, ["imported"])
        self.assertEqual(self.module.exports, [("second", 1, 0x14)])
        self.assertEqual(self.module.code_section(), 1)

    def test_branches_reach_their_targets(self):
        code = self.prepared.code
        first, second = struct.unpack_from(">II", code, 0)
        self.assertEqual((0xc0000000 + (first & 0x3fffffc)) & 0xffffffff, 0xc0000014)
        self.assertEqual(0xc0000004 + (second & 0x3fffffc), 0xc1000000)  # The import's stand-in.
        self.assertIn(0xc0000014, self.prepared.starts)
        self.assertEqual(self.prepared.import_sites, {0: ("branch", 4)})
        self.assertEqual(self.prepared.relocated_words, {0, 4, 8, 12})

    def test_import_out_of_reach_stops(self):
        with self.assertRaises(SystemExit):
            modules.prepare(self.module, 0xc0000000, 0xb0000000)

    def test_generated_c(self):
        prepared = self.prepared
        header = bytearray(0x100)
        struct.pack_into(">I", header, 0, 0x100)
        struct.pack_into(">I", header, 0x48, 0xc0000000)
        struct.pack_into(">I", header, 0x90, len(prepared.code))
        program = Program(bytes(header) + prepared.code)
        functions = {}
        add_starts(program, functions, {0xc0000000, 0xc0000014})
        emit.MODULE = {"prefix": "m0", "base": 0xc0000000, "size": len(prepared.code), "imports": 0xc1000000,
                       "import_count": 1, "fields": prepared.fields}
        try:
            text, _, unknown = emit.emit_function(program, 0xc0000000, functions[0xc0000000], set(functions))
        finally:
            emit.MODULE = None
        self.assertFalse(unknown)
        self.assertIn("cpu.lr = (m0_code + 0x4u); f_c0000014();", text)
        self.assertIn("cpu.lr = (m0_code + 0x8u); dispatch(m0_import[0]);", text)
        self.assertIn("cpu.r[3] = (((((m0_section[2] + 0x10u)) + 0x8000u) >> 16) << 16);", text)
        self.assertIn("cpu.r[3] = cpu.r[3] + (uint32_t)((int32_t)(int16_t)((m0_section[2] + 0x10u) & 0xffffu));", text)


PROGRAM_FUNCTION, PROGRAM_DATA = 0x80004000, 0x80451234
REL_CODE_AT = 0x100


def made_up_rel():
    """A REL, number 5, as the SDK's makerel leaves it. Its code calls a program function,
    loads the address of its own data, then of program data; its data points at its second
    function. A prolog is the first function."""
    code = [BL, LIS_R3, ADDI_R3, LIS_R3, ADDI_R3, BLR, BLR]  # Functions at 0 and 0x18.
    data = bytearray(0x400)
    struct.pack_into(">IIIIIIII", data, 0, 5, 0, 0, 4, 0x4c, 0, 0, 3)  # Number, links, sections, table, name, version.
    struct.pack_into(">IIII", data, 0x20, 0x40, 0x300, 0x2e0, 16)       # Bss, relocations, imports.
    data[0x30:0x34] = bytes([1, 1, 1, 0])
    struct.pack_into(">III", data, 0x34, 0, 0x14, 0x14)               # Prolog, epilog, unresolved.
    struct.pack_into(">III", data, 0x40, 32, 32, 0x300)
    for number, (offset, size) in enumerate([(0, 0), (REL_CODE_AT | 1, len(code) * 4), (0x200, 0x20), (0, 0x40)]):
        struct.pack_into(">II", data, 0x4c + number * 8, offset, size)
    struct.pack_into(f">{len(code)}I", data, REL_CODE_AT, *code)
    struct.pack_into(">IIII", data, 0x2e0, 5, 0x300, 0, 0x340)  # Its own relocations, then the program's.

    def relocations(at, entries):
        for index, (step, kind, section, addend) in enumerate(entries):
            struct.pack_into(">HBBI", data, at + index * 8, step, kind, section, addend)

    relocations(0x300, [(0, modules.REL_SECTION, 1, 0), (6, modules.HA, 2, 0x10), (4, modules.LO, 2, 0x10),
                        (0, modules.REL_SECTION, 2, 0), (4, modules.ADDR32, 1, 0x18), (0, modules.REL_END, 0, 0)])
    relocations(0x340, [(0, modules.REL_SECTION, 1, 0), (0, modules.REL24, 0, PROGRAM_FUNCTION),
                        (14, modules.HA, 0, PROGRAM_DATA), (4, modules.LO, 0, PROGRAM_DATA), (0, modules.REL_END, 0, 0)])
    return bytes(data)


class RelModules(unittest.TestCase):
    def setUp(self):
        self.module = modules.parse_module("rel", made_up_rel())
        self.prepared = modules.prepare(self.module, 0xc0000000, 0xc1000000)

    def test_reads_lists(self):
        module = self.module
        self.assertEqual((module.kind, module.module_id, module.code_section(), module.bss_bytes), ("rel", 5, 1, 0x40))
        self.assertIsNone(modules.parse_rso("rel", made_up_rel()))
        self.assertEqual([(r.offset, r.kind, r.symbol) for r in module.internal],
                         [(REL_CODE_AT + 6, modules.HA, 2), (REL_CODE_AT + 10, modules.LO, 2), (0x204, modules.ADDR32, 1)])
        self.assertEqual([(r.offset, r.kind, r.addend) for r in module.fixed],
                         [(REL_CODE_AT, modules.REL24, PROGRAM_FUNCTION), (REL_CODE_AT + 14, modules.HA, PROGRAM_DATA),
                          (REL_CODE_AT + 18, modules.LO, PROGRAM_DATA)])
        self.assertEqual(module.exports, [("_prolog", 1, 0), ("_epilog", 1, 0x14), ("_unresolved", 1, 0x14)])

    def test_program_addresses_are_settled_now(self):
        prepared = self.prepared
        words = struct.unpack_from(">7I", prepared.code)
        self.assertEqual(words[3] & 0xffff, 0x8045)  # The high half, rounded for the signed low half.
        self.assertEqual(words[4] & 0xffff, 0x1234)
        self.assertEqual(0xc0000000 + (words[0] & 0x3fffffc), 0xc1000000)  # The program call's stand-in.
        self.assertEqual(prepared.fixed_calls, {0: PROGRAM_FUNCTION})
        self.assertEqual(prepared.import_sites, {0: ("fixed", 0, PROGRAM_FUNCTION)})
        self.assertEqual(prepared.relocated_words, {0, 4, 8, 12, 16})
        self.assertIn(0xc0000018, prepared.starts)  # From the pointer in its data.

    def test_generated_c_calls_the_program_directly(self):
        prepared = self.prepared
        header = bytearray(0x100)
        struct.pack_into(">I", header, 0, 0x100)
        struct.pack_into(">I", header, 0x48, 0xc0000000)
        struct.pack_into(">I", header, 0x90, len(prepared.code))
        program = Program(bytes(header) + prepared.code)
        functions = {}
        add_starts(program, functions, {0xc0000000})
        emit.MODULE = {"prefix": "m1", "base": 0xc0000000, "size": len(prepared.code), "imports": 0xc1000000,
                       "import_count": 1, "fields": prepared.fields, "fixed": {0xc1000000: PROGRAM_FUNCTION},
                       "program": {PROGRAM_FUNCTION}}
        try:
            text, _, unknown = emit.emit_function(program, 0xc0000000, functions[0xc0000000], set(functions))
        finally:
            emit.MODULE = None
        self.assertFalse(unknown)
        self.assertIn("cpu.lr = (m1_code + 0x4u); f_80004000();", text)
        self.assertIn("m1_section[2] + 0x10u", text)
        self.assertIn("0x80450000", text)

    def test_links_to_other_modules_are_read_where_the_linker_put_them(self):
        data = bytearray(made_up_rel())
        struct.pack_into(">I", data, 0x2e8, 9)  # The program's list now points into module 9.
        module = modules.parse_module("rel", bytes(data))
        self.assertTrue(module.external)
        prepared = modules.prepare(module, 0xc0000000, 0xc1000000)
        # The call at 0 into module 9 gets a stand-in whose target is read from the linked
        # branch as it runs; the address halves at 0xc and 0x10, from the immediate the
        # linker wrote. All three are left out of the identity check.
        self.assertEqual(prepared.linked_calls, {len(module.imports): 0})
        self.assertEqual(sorted(at for kind, _, _, at in prepared.fields.values() if kind == "linked"), [12, 16])
        self.assertLessEqual({0, 12, 16}, prepared.relocated_words)
        self.assertEqual(prepared.fixed_calls, {})

    def test_links_between_modules_are_function_starts(self):
        # Module 9's call into module 5's code, at 0x18: a function there even though
        # nothing inside module 5 leads to it. Its address halves point at data, not code.
        data = bytearray(made_up_rel())
        struct.pack_into(">I", data, 0, 9)                                     # Module 9,
        struct.pack_into(">II", data, 0x2e0, 9, 0x300)                         # with its own list,
        struct.pack_into(">I", data, 0x2e8, 5)                                 # and its other one into module 5.
        struct.pack_into(">HBBI", data, 0x348, 0, modules.REL24, 1, 0x18)
        found = [("five.rel", self.module), ("nine.rel", modules.parse_module("rel", bytes(data)))]
        self.assertEqual(emit.links_between(found), {5: {0x18}})

    def test_yaz0(self):
        # "abc" as literals, then a copy of 6 from 3 back: "abcabcabc".
        packed = b"Yaz0" + struct.pack(">I", 9) + bytes(8) + bytes([0xe0]) + b"abc" + bytes([0x40, 0x02])
        self.assertEqual(modules.yaz0_decompress(packed), b"abcabcabc")
        self.assertEqual(modules.decompress(b"plain"), b"plain")


class ModulePlacement(unittest.TestCase):
    """Where write_modules puts each module's code and import stand-ins, one after another."""

    def place(self, found):
        placed = []

        def emit_module(module, number, base, imports, *rest):
            placed.append((number, base, imports))
            return f"m{number}"

        with tempfile.TemporaryDirectory() as out, mock.patch.object(emit, "emit_module", emit_module), \
                redirect_stdout(io.StringIO()) as said:
            count = emit.write_modules(found, Path(out), {}, set())
            written = (Path(out) / "modules.c").read_text()
        return count, placed, written, said.getvalue()

    def test_modules_follow_one_another(self):
        first, second = modules.parse_module("rel", made_up_rel()), modules.parse_module("rel", made_up_rel())
        count, placed, written, _ = self.place([("a.rel", first), ("b.rel", second)])
        self.assertEqual(count, 2)
        (_, base0, imports0), (_, base1, imports1) = placed
        self.assertEqual(base0, emit.MODULE_BASE)
        code_bytes = first.sections[first.code_section()][1]
        self.assertEqual(imports0, emit.align_up(base0 + code_bytes, emit.MODULE_ALIGN))  # Just after its code.
        stand_ins = len(first.imports) + len(first.fixed) + len(first.external)
        self.assertGreaterEqual(base1, imports0 + 4 * stand_ins)  # Past every stand-in the first may need.
        self.assertEqual(base1 % emit.MODULE_ALIGN, 0)
        self.assertIn("&m0_module, &m1_module", written)

    def test_a_module_that_cant_be_placed_is_left_to_the_interpreter(self):
        # Code in two sections: there's no telling which the branches are in. The build
        # goes on, and the next module still gets a place of its own.
        data = bytearray(made_up_rel())
        struct.pack_into(">I", data, 0x4c + 2 * 8, 0x200 | 1)  # Section 2 executable too.
        unplaceable = modules.parse_module("rel", bytes(data))
        with self.assertRaises(modules.Unsupported):
            unplaceable.code_section()
        count, placed, written, said = self.place([("two.rel", unplaceable), ("one.rel", modules.parse_module("rel", made_up_rel()))])
        self.assertEqual(count, 1)
        self.assertEqual([number for number, _, _ in placed], [1])
        self.assertGreater(placed[0][1], emit.MODULE_BASE)
        self.assertIn("left to the interpreter", said)
        self.assertIn("&m1_module", written)
        self.assertNotIn("m0_module", written)


def lh_table(bits, entries, groups):
    """One of LH's Huffman trees as lh_table reads it: a count of four-byte groups less one
    (two bytes for 9-bit entries, one for 5-bit), then the entries packed from the high bit."""
    count = struct.pack("<H", groups) if bits > 8 else bytes([groups])
    room = (groups + 1) * 4 - len(count)
    packed = 0
    for entry in entries:
        packed = packed << bits | entry
    packed <<= room * 8 - bits * len(entries)
    return count + packed.to_bytes(room, "big")


def lh_stream(size, lengths, distances, bits):
    return bytes([0x40]) + struct.pack("<I", size)[:3] + lh_table(9, lengths, 1) + lh_table(5, distances, 0) + bits


# The root's two children are both leaves (0x100 for a 0 bit, 0x80 for a 1), at entries 2 and 3.
LITERALS_A_B = [0x180, ord("a"), ord("b"), 0, 0]
A_OR_COPY_OF_3 = [0x180, ord("a"), 0x100, 0, 0]  # A symbol of 0x100 or more copies (low byte + 3) bytes.
DISTANCE_ONE = [0x18, 0, 0, 0]                    # Width 0 either way: one back.


class LhCompression(unittest.TestCase):
    """Nintendo's LZ with Huffman codes (type 0x40), as Mario Sports Mix's modules are kept."""

    def test_literals(self):
        packed = lh_stream(4, LITERALS_A_B, DISTANCE_ONE, bytes([0b01100000]))  # a b b a
        self.assertEqual(modules.lh_decompress(packed), b"abba")
        self.assertEqual(modules.decompress(packed), b"abba")

    def test_a_copy_from_one_back(self):
        packed = lh_stream(4, A_OR_COPY_OF_3, DISTANCE_ONE, bytes([0b01000000]))  # a, then three more from one back
        self.assertEqual(modules.lh_decompress(packed), b"aaaa")

    def test_what_isnt_one_is_refused(self):
        good = lh_stream(4, LITERALS_A_B, DISTANCE_ONE, bytes([0b01100000]))
        self.assertIsNone(modules.lh_decompress(b"\x10" + good[1:]))                        # Another type.
        self.assertIsNone(modules.lh_decompress(good[:8]))                                  # Too short.
        self.assertIsNone(modules.lh_decompress(b"\x40\0\0\0" + struct.pack("<I", 65 << 20) + good[4:]))  # Over the cap.
        self.assertIsNone(modules.lh_decompress(lh_stream(4, A_OR_COPY_OF_3, DISTANCE_ONE, bytes([0b10000000]))))  # A copy with nothing before it.
        self.assertIsNone(modules.lh_decompress(lh_stream(64, LITERALS_A_B, DISTANCE_ONE, bytes([0]))))  # Runs out.
        self.assertEqual(modules.decompress(b"@ not compressed"), b"@ not compressed")


if __name__ == "__main__":
    unittest.main()
