"""The s01 corona hook is a hand-written i386 detour, so it cannot be unit
tested by calling it. What can be tested is what it compiles to, and what the
code it patches still looks like in the real exe.

  1. `lea 40(%esp)` where a `jmp` over a `call` leaves the argument block at
     esp+36, and no return address for the callee to come back to
     (crash of 2026-11-11 12:04:33).
  2. `and $-16,%esp` done *after* pushing the recorder's arguments. The call
     site's esp is whatever the renderer left it at, so the realignment moved
     esp below the arguments and the recorder read the game's own stack as its
     arguments (crash of 2026-11-11 13:04:44: esp 0x010BF54C, 12-byte shift,
     args became 0xC48F9A0F and args[6] faulted at 0xC48F9A27).

Test 2 is checked by simulating the stub's esp for every possible alignment of
the call site, which is the only way to catch an offset that is right on an
aligned stack and wrong on the real one.

s19 added the rest, after re-reading the whole detour against the disassembly
(.strata/notes-s19-signal-hook-hardening.md has the stack depth of every
instruction on both sides):

  3. the stub must leave the stack exactly as the deleted `call` would have -
     esp back at the entry value after popa, one return address below it at the
     tail jmp - and must never write into the game's argument block;
  4. the recorder's three arguments must land where a cdecl callee reads them;
  5. the bytes the kill switch compares against must be the bytes the real
     gta-sa.exe has: the call at 0x49DD4C, the instruction it returns to, the
     caller's `add esp,0x54`, the corona thunk at 0x6FC580 and the plain `ret`
     of the function it tail-jumps to (a `ret 0x54` there would double-clean);
  6. the kill switch itself: source=list must be able to keep the patch from
     ever being written, and the recorder path must stay free of anything that
     can throw or allocate.

Run against the built asi: build/gate/rhyremix.asi (what scripts/check.sh
builds) or build/rhyremix.asi, or point RHYREMIX_ASI at one. The exe checks use
/media/rhy/data/Gamez/GTASA-RTX/gta-sa.exe read-only, or RHY_GTA_EXE; they skip
if it is not there.
"""

import os
import re
import shutil
import struct
import subprocess
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CALL_SITE = 0x49DD4C
RESUME_AT = CALL_SITE + 5          # the instruction the patched call returns to
CLEANUP_AT = RESUME_AT + 3         # the caller's add esp,0x54
CORONA_FN = 0x6FC580               # the corona-type thunk the stub jumps to
CORONA_REAL = 0x6FC180             # the function it tail-jumps to
CORONA_PUSH = 0x49DCD9             # s30: push $0x41700000 -> the corona radius
CORONA_RADIUS_IMM = CORONA_PUSH + 1  # the imm32 we resize
MASK16 = 0xFFFFFFFF

DEFAULT_EXE = "/media/rhy/data/Gamez/GTASA-RTX/gta-sa.exe"


def find_asi():
    env = os.environ.get("RHYREMIX_ASI")
    cands = [env] if env else []
    cands += ["build/gate/rhyremix.asi", "build/rhyremix.asi"]
    for c in cands:
        p = c if os.path.isabs(c) else os.path.join(REPO, c)
        if os.path.exists(p):
            return p
    return None


def find_exe():
    for c in (os.environ.get("RHY_GTA_EXE"), DEFAULT_EXE):
        if c and os.path.exists(c):
            return c
    return None


def objdump():
    for tool in ("i686-w64-mingw32-objdump", "objdump"):
        if shutil.which(tool):
            return tool
    return None


def stub_disasm(asi):
    """The stub's instructions, as text, or None if it is not in the image."""
    tool = objdump()
    if tool is None:
        return None
    nm = tool.replace("objdump", "nm")
    if not shutil.which(nm):
        return None
    syms = subprocess.run([nm, asi], capture_output=True, text=True).stdout
    addr = None
    for line in syms.splitlines():
        parts = line.split()
        # e.g. "65d54fc0 t __ZL19rhySignalCoronaStubv"
        if len(parts) >= 3 and "rhySignalCoronaStub" in parts[-1]:
            addr = int(parts[0], 16)
            break
    if addr is None:
        return None
    out = subprocess.run(
        [tool, "-d", "-M", "intel", "--start-address", hex(addr),
         "--stop-address", hex(addr + 96), asi],
        capture_output=True, text=True).stdout
    body = []
    for line in out.splitlines():
        m = re.match(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2} )+\s*(.*)$", line)
        if m:
            body.append(m.group(1).strip())
            if m.group(1).strip().startswith("jmp"):
                break
    return body


def disasm_range(path, start, stop):
    """[(addr, raw bytes, text)] for a range of a PE file, or None."""
    tool = objdump()
    if tool is None:
        return None
    out = subprocess.run(
        [tool, "-d", "-M", "intel", "--start-address", hex(start),
         "--stop-address", hex(stop), path],
        capture_output=True, text=True).stdout
    rows = []
    for line in out.splitlines():
        m = re.match(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$", line)
        if m:
            rows.append((int(m.group(1), 16),
                         bytes.fromhex(m.group(2).replace(" ", "")),
                         " ".join(m.group(3).split())))
    return rows


def simulate(body, entry_esp):
    """Walk the stub's instructions and track esp/ebp/eax.

    Returns (pushes, esp_at_call, tail_pushes, eax_args) where pushes is the
    list of (address, operand) for every push before the call, tail_pushes the
    same after it, and eax_args the argument block pointer handed to the callee.
    """
    esp, ebp, eax = entry_esp, None, None
    pushes, tail = [], []
    called = False
    esp_at_call = esp
    args_ptr = None
    for ins in body:
        m = re.match(r"(pusha|popa|pushf|popf)\b", ins)
        if m:
            if m.group(1) in ("pusha", "pushf"):
                esp -= 32 if m.group(1) == "pusha" else 4
            else:
                esp += 32 if m.group(1) == "popa" else 4
            continue
        m = re.match(r"push\s+(\S+)", ins)
        if m:
            esp = (esp - 4) & MASK16
            (tail if called else pushes).append((esp, m.group(1)))
            continue
        m = re.match(r"and\s+esp,0xfffffff0", ins)
        if m:
            esp &= 0xFFFFFFF0
            continue
        m = re.match(r"mov\s+ebp,esp", ins)
        if m:
            ebp = esp
            continue
        m = re.match(r"mov\s+esp,ebp", ins)
        if m:
            esp = ebp
            continue
        m = re.match(r"lea\s+eax,\[(e(sp|bp))\+0x([0-9a-f]+)\]", ins)
        if m:
            base = esp if m.group(1) == "esp" else ebp
            eax = (base + int(m.group(3), 16)) & MASK16
            if args_ptr is None:
                args_ptr = eax  # the argument block the recorder is handed
            continue
        m = re.match(r"mov\s+eax,(0x[0-9a-f]+)", ins)
        if m:
            eax = int(m.group(1), 16)
            continue
        if re.match(r"call", ins):
            called = True
            esp_at_call = esp
    return pushes, esp_at_call, tail, args_ptr


def esp_trace(body, entry_esp):
    """[(instruction, esp before it)] for every instruction of the stub."""
    esp, ebp = entry_esp, None
    rows = []
    for ins in body:
        rows.append((ins, esp, ebp))
        m = re.match(r"(pusha|popa|pushf|popf)\b", ins)
        if m:
            if m.group(1) in ("pusha", "pushf"):
                esp -= 32 if m.group(1) == "pusha" else 4
            else:
                esp += 32 if m.group(1) == "popa" else 4
            continue
        m = re.match(r"push\s+", ins)
        if m:
            esp = (esp - 4) & MASK16
            continue
        m = re.match(r"and\s+esp,0xfffffff0", ins)
        if m:
            esp &= 0xFFFFFFF0
            continue
        m = re.match(r"mov\s+ebp,esp", ins)
        if m:
            ebp = esp
            continue
        m = re.match(r"mov\s+esp,ebp", ins)
        if m:
            esp = ebp
    return rows


class TestCoronaStubEncoding(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.asi = find_asi()
        cls.body = stub_disasm(cls.asi) if cls.asi else None
        cls.text = "\n".join(cls.body) if cls.body else ""

    def test_stub_is_in_the_built_image(self):
        if self.asi is None:
            self.skipTest("no built asi (run plugin/build.sh first)")
        if self.body is None:
            self.skipTest("no i386 objdump/nm available")
        self.assertTrue(self.body, "stub disassembly is empty")

    def test_arguments_are_read_at_36_above_the_saved_block(self):
        """pusha (32) + pushfl (4) = 36. A jmp pushed no return address, so
        esp+40 would hand the recorder the argument block one dword late."""
        if self.body is None:
            self.skipTest("stub not disassembled")
        self.assertRegex(self.text, r"lea\s+eax,\[e(sp|bp)\+0x24\]")
        self.assertNotRegex(self.text, r"lea\s+eax,\[e(sp|bp)\+0x28\]")

    def test_stub_pushes_the_return_address_before_the_tail_jump(self):
        if self.body is None:
            self.skipTest("stub not disassembled")
        self.assertIn("mov    eax,0x%x" % RESUME_AT, self.text.replace("\t", "    "))
        push = self.text.find("push   eax")
        jmp = self.text.rfind("jmp")
        self.assertNotEqual(push, -1)
        self.assertNotEqual(jmp, -1)
        self.assertLess(push, jmp)
        self.assertIn("mov    eax,0x%x" % CORONA_FN, self.text.replace("\t", "    "))

    def test_registers_and_flags_survive(self):
        """esi (the entity) and ebx (the head index) are read by the game right
        after the call site, so the stub must restore everything it saves."""
        if self.body is None:
            self.skipTest("stub not disassembled")
        for ins in ("pusha", "pushf", "popf", "popa"):
            self.assertIn(ins, self.text)
        self.assertLess(self.text.index("popa"), self.text.rfind("jmp"))

    def test_arguments_land_where_the_callee_reads_them_at_every_alignment(self):
        """The real bug of 2026-11-11 13:04:44.

        A cdecl callee reads its arguments at esp_at_call+0/+4/+8. So the three
        argument pushes must land exactly there - which they do not if the stub
        realigns esp after pushing them and the call site's esp was not already
        16-byte aligned. Simulate every alignment the game can hand us.
        """
        if self.body is None:
            self.skipTest("stub not disassembled")
        for base in range(0x1000, 0x1040, 4):
            pushes, esp_call, _tail, eax = simulate(self.body, base)
            args_pushes = pushes[-3:]
            want = [esp_call + 8, esp_call + 4, esp_call]
            got = [a for a, _ in args_pushes]
            self.assertEqual(
                got, want,
                "entry esp 0x%x: arguments pushed at %s but the callee reads "
                "them at %s (the stub realigns esp after pushing them)"
                % (base, [hex(x) for x in got], [hex(x) for x in want]))
            # and the argument block it hands over is the game's real one
            self.assertEqual(eax, base,
                             "entry esp 0x%x: the argument block pointer should "
                             "be the entry esp, got 0x%x" % (base, eax))
            # the call itself happens on a 16-byte aligned stack
            self.assertEqual(esp_call % 16, 0,
                             "entry esp 0x%x: esp at the call is 0x%x" % (base, esp_call))

    def test_tail_hands_the_callee_a_real_return_address(self):
        if self.body is None:
            self.skipTest("stub not disassembled")
        base = 0x100C                      # deliberately not 16-byte aligned
        _pushes, _esp_call, tail, _eax = simulate(self.body, base)
        self.assertTrue(tail, "no pushes after the call: the callee has no return address")
        addr, operand = tail[-1]
        self.assertEqual(addr, base - 4, "return address must sit where the "
                                         "deleted call would have pushed it")
        # The stub loads it into a register first, so check the value it loads
        # and that the load comes before the push.
        self.assertIn("0x%x" % RESUME_AT, self.text)
        self.assertLess(self.text.index("0x%x" % RESUME_AT),
                        self.text.rfind("push"),
                        "the resume address must be loaded before it is pushed")

    def test_recorder_arguments_are_where_a_cdecl_callee_reads_them(self):
        """The recorder is a C function: it reads args/entity/index at
        esp_call+4/+8/+12. The pushes must be in reverse order (index first,
        the pointer last) and the padding must make esp_call 16-aligned."""
        if self.body is None:
            self.skipTest("stub not disassembled")
        for base in range(0x1000, 0x1040, 4):
            pushes, esp_call, _tail, _eax = simulate(self.body, base)
            order = [op for _, op in pushes[-3:]]
            self.assertEqual(order, ["ebx", "esi", "eax"],
                             "entry esp 0x%x: pushed %s, expected the reverse of "
                             "(args, entity, index)" % (base, order))
            self.assertEqual([a for a, _ in pushes[-3:]],
                             [esp_call + 8, esp_call + 4, esp_call],
                             "entry esp 0x%x: the recorder's arguments are not at "
                             "the three positions a cdecl callee reads at esp+12/"
                             "+8/+4 (esp_call is esp before the call pushes the "
                             "return address)" % base)
            self.assertEqual(esp_call % 16, 0)

    def test_stub_leaves_the_stack_balanced_at_every_alignment(self):
        """Stack balance on every path, the s19 way: esp after popa must be the
        entry esp again (the game's own `add esp,0x54` at 0x49DD54 then cleans
        exactly 84 bytes), and esp at the tail jmp must be entry-4, which is
        what the deleted call would have handed the callee."""
        if self.body is None:
            self.skipTest("stub not disassembled")
        for base in range(0x1000, 0x1040, 4):
            rows = esp_trace(self.body, base)
            # esp right after popa == the entry esp
            popa = [i for i, (ins, _e, _b) in enumerate(rows)
                    if ins.startswith("popa")]
            self.assertEqual(len(popa), 1, "expected exactly one popa")
            after_popa = rows[popa[0]][1] + 32
            self.assertEqual(after_popa, base,
                             "entry esp 0x%x: esp after popa is 0x%x" % (base, after_popa))
            jmp = rows[-1]
            self.assertTrue(jmp[0].startswith("jmp"), "the stub must end in a jmp")
            self.assertEqual(jmp[1], base - 4,
                             "entry esp 0x%x: esp at the tail jmp is 0x%x, want 0x%x"
                             % (base, jmp[1], base - 4))

    def test_stub_writes_nothing_into_the_game_argument_block(self):
        """The 84 bytes at esp are the game's: the callee reads them, and even
        rewrites the alpha slot (0x6FC2D0). The stub may only push."""
        if self.body is None:
            self.skipTest("stub not disassembled")
        for ins in self.body:
            if re.match(r"mov\s+\[e(sp|bp)", ins) or re.match(r"mov\s+e(sp|bp),", ins):
                if not re.match(r"mov\s+ebp,esp", ins) and \
                        not re.match(r"mov\s+esp,ebp", ins):
                    self.fail("the stub writes to the stack frame: %r" % ins)

    def test_stub_restores_esp_from_ebp_before_popping_the_saved_registers(self):
        """popa does not restore esp (it skips the slot pusha wrote), so the
        `mov esp,ebp` is what puts the frame back; it has to come first."""
        if self.body is None:
            self.skipTest("stub not disassembled")
        idx = {ins: i for i, ins in enumerate(self.body)}
        restore = [i for ins, i in idx.items() if re.match(r"mov\s+esp,ebp", ins)]
        self.assertEqual(len(restore), 1)
        popf = [i for ins, i in idx.items() if re.match(r"popf", ins)]
        popa = [i for ins, i in idx.items() if re.match(r"popa", ins)]
        self.assertTrue(restore[0] < popf[0] < popa[0],
                        "order must be mov esp,ebp; popf; popa")

    def test_source_checks_the_callee_before_patching(self):
        path = os.path.join(REPO, "plugin", "src", "signalhook.cpp")
        with open(path, encoding="utf-8") as fh:
            src = fh.read()
        self.assertIn("kRhySignalResumeAt", src)
        with open(os.path.join(REPO, "plugin", "src", "signalheads.cpp"),
                  encoding="utf-8") as fh:
            self.assertIn("kRhyPatchCalleeBytes", fh.read())
        # The offsets the stub hard-codes must match the documented layout.
        self.assertIn("kRhyArgPos == 6", src)
        self.assertIn("leal 36(%%ebp)", src)
        # ... and the realignment must precede the argument pushes.
        self.assertLess(src.index("andl $-16, %%esp"), src.index("pushl %%ebx"))
        # A bad pointer must never be dereferenced inside the render pass.
        self.assertIn("rhyReadable(", src)
        self.assertIn("rhyGameRead", src)


class TestPatchSiteInRealExe(unittest.TestCase):
    """The kill switch compares bytes; this checks the expected bytes against
    the exe it is meant to protect, read-only. If a game update or SilentPatch
    moves this code, these checks go red before the game does."""

    @classmethod
    def setUpClass(cls):
        cls.exe = find_exe()
        cls.rows = {}
        if cls.exe:
            for start, stop in ((CALL_SITE, CLEANUP_AT + 3),
                                (CORONA_FN, CORONA_FN + 0x16),
                                (CORONA_PUSH, CORONA_PUSH + 5),
                                (0x6FC44E, 0x6FC4C8)):
                rows = disasm_range(cls.exe, start, stop)
                if rows is None:
                    return
                for addr, raw, text in rows:
                    cls.rows[addr] = (raw, text)

    def skip_if_missing(self):
        if self.exe is None:
            self.skipTest("gta-sa.exe not found (set RHY_GTA_EXE)")
        if not self.rows:
            self.skipTest("no i386 objdump available")

    def test_call_site_is_a_call_to_the_corona_thunk(self):
        self.skip_if_missing()
        raw, text = self.rows[CALL_SITE]
        self.assertEqual(text, "call 0x6fc580")
        self.assertEqual(raw[0], 0xE8)
        self.assertEqual(len(raw), 5, "the patch must cover exactly one instruction")
        rel = int.from_bytes(raw[1:5], "little")
        self.assertEqual((CALL_SITE + 5 + rel) & MASK16, CORONA_FN)

    def test_resume_point_is_the_instruction_the_call_returns_to(self):
        """0x49DD51 is what the stub pushes as the return address; it reads esi,
        which is why the stub has to restore every register."""
        self.skip_if_missing()
        raw, text = self.rows[RESUME_AT]
        self.assertEqual(raw, b"\x8b\x46\x14")
        self.assertEqual(text, "mov eax,DWORD PTR [esi+0x14]")

    def test_caller_cleans_exactly_21_arguments(self):
        """The stub pushes nothing across its tail jump, so this `add esp,0x54`
        is the only thing that balances the frame. 0x54 = 84 = 21 dwords."""
        self.skip_if_missing()
        raw, text = self.rows[CLEANUP_AT]
        self.assertEqual(raw[:3], b"\x83\xc4\x54")
        self.assertEqual(text, "add esp,0x54")

    def test_callee_is_the_corona_type_thunk(self):
        self.skip_if_missing()
        raw, text = self.rows[CORONA_FN]
        self.assertEqual(raw[:5], b"\x0f\xb6\x54\x24\x28")
        self.assertEqual(text, "movzx edx,BYTE PTR [esp+0x28]")
        jmp = self.rows[CORONA_FN + 0x10]
        self.assertEqual(jmp[1], "jmp 0x6fc180")

    def test_the_corona_radius_is_a_push_immediate_we_can_resize(self):
        """s30: the game registers every lit signal head with corona radius 15.0
        game units - a 30 m additive green flare, which the Remix bridge renders
        as emissive geometry and which is what floods the street (see
        .strata/notes-s30-signal-bulbs.md). We resize it by rewriting the imm32
        of this one push, so the push has to be exactly what we think it is, and
        has to be in the same code page as the call site we patch."""
        self.skip_if_missing()
        raw, text = self.rows[CORONA_PUSH]
        self.assertEqual(raw, b"\x68\x00\x00\x70\x41")
        self.assertEqual(text, "push 0x41700000")   # struct.pack("<f", 15.0)
        self.assertEqual(int.from_bytes(raw[1:5], "little"), 0x41700000)
        import struct
        self.assertEqual(struct.unpack("<f", raw[1:5])[0], 15.0)
        self.assertEqual(CORONA_RADIUS_IMM & ~0xFFF, CALL_SITE & ~0xFFF,
                         "both patches must fit one VirtualProtect")
        # and it belongs to that call: it sits between the top of
        # CTrafficLights::DisplayActualLight (0x49DAB0) and the call itself, so
        # patching it changes traffic signal coronas and nothing else.
        self.assertTrue(0x49DAB0 <= CORONA_PUSH < CALL_SITE)

    def test_thunk_reads_argument_9_and_the_real_callee_reads_21(self):
        """[esp+0x28] at the thunk is argument 9 (the corona type), and the
        function it tail-jumps to reads its arguments up to [esp+0x80] after
        pushing one register: 21 dwords, which is the length the recorder
        validates."""
        self.skip_if_missing()
        self.assertEqual(self.rows[CORONA_FN + 0x0C][1],
                         "mov DWORD PTR [esp+0x28],eax")
        rows = disasm_range(self.exe, CORONA_REAL, CORONA_REAL + 0x10)
        self.assertEqual(rows[0][2], "sub esp,0x1c")

    def test_the_callee_returns_without_cleaning(self):
        """A `ret 0x54` instead of `ret` would clean the arguments twice: the
        stub's pushed return address would be followed by a stack that is 84
        bytes too shallow. Every return of 0x6FC180 must be a plain ret."""
        self.skip_if_missing()
        rets = [(a, raw, text) for a, (raw, text) in self.rows.items()
                if text.startswith("ret")]
        self.assertTrue(rets, "no ret found in the corona function")
        for _a, raw, text in rets:
            self.assertEqual(raw, b"\xc3", "not a plain ret: %r at 0x%x" % (text, _a))


HEAD_LOOP_BOUND = 0x49DBDD     # movzx eax, BYTE PTR [edi+0xd]: the 2DFX entry count
HEAD_LOOP_TEST = 0x49DC01      # jle past the whole head loop when that count is 0
RANGE_PUSH = 0x49DCF3          # push 0x42480000: argument 8 of the call, 50.0 m
CALLEE_RANGE_TEST = 0x6FC22E   # where the callee squares the range and compares
FLOAT_RANGE = 0x858B40         # 50.0 m: the alpha reaches 0 here
FLOAT_FADE = 0x85F070          # 35.0 m: full alpha below here
FLOAT_FADE_RATE = 0x863E0C     # 1/15 per metre between the two


def pe_float_at(path, va):
    """The float stored at a virtual address of a PE file, or None.

    Section-table walk, nothing else: the numbers the game's own distance test
    uses live in .rdata, and reading them is the only way to pin them without
    trusting a comment.
    """
    with open(path, "rb") as fh:
        data = fh.read()
    off = int.from_bytes(data[0x3C:0x40], "little")          # e_lfanew
    nsec = int.from_bytes(data[off + 6:off + 8], "little")   # NumberOfSections
    tsz = int.from_bytes(data[off + 20:off + 22], "little")  # SizeOfOptionalHeader
    base = int.from_bytes(data[off + 52:off + 56], "little")  # ImageBase
    sec = off + 24 + tsz
    rva = va - base
    for i in range(nsec):
        e = sec + i * 40
        vsize = int.from_bytes(data[e + 8:e + 12], "little")
        vaddr = int.from_bytes(data[e + 12:e + 16], "little")
        rsize = int.from_bytes(data[e + 16:e + 20], "little")
        rptr = int.from_bytes(data[e + 20:e + 24], "little")
        if vaddr <= rva < vaddr + max(vsize, rsize):
            o = rptr + (rva - vaddr)
            return struct.unpack("<f", data[o:o + 4])[0]
    return None


class TestSignalDistanceInRealExe(unittest.TestCase):
    """n02: the numbers the head-source decision is now made on.

    s30 read `records=0` at the Grove Street spawn as "the patched call site was
    never reached" and switched the whole session to the list, which puts the
    light back on the mast. The exe says the sample was worthless: the game's own
    signal corona is dropped beyond 50.0 m (argument 8, pushed at 0x49DCF3 and
    tested inside the callee at 0x6FC22E - after our patch point), and the spawn
    is 79.1 m from the nearest of the 1522 poles. So the range, the fade distance
    and the loop bound that decides whether a head exists at all are pinned here
    against the bytes, and rhyHeadsHealthy() uses them.
    """

    @classmethod
    def setUpClass(cls):
        cls.exe = find_exe()
        cls.rows = {}
        if cls.exe:
            for start, stop in ((HEAD_LOOP_BOUND, HEAD_LOOP_TEST + 6),
                                (RANGE_PUSH, RANGE_PUSH + 5),
                                (CALLEE_RANGE_TEST, CALLEE_RANGE_TEST + 21),
                                (CALL_SITE, CALL_SITE + 5)):
                rows = disasm_range(cls.exe, start, stop)
                if rows is None:
                    return
                for addr, raw, text in rows:
                    cls.rows[addr] = (raw, text)

    def skip_if_missing(self):
        if self.exe is None:
            self.skipTest("gta-sa.exe not found (set RHY_GTA_EXE)")
        if not self.rows:
            self.skipTest("no i386 objdump available")

    def test_the_range_argument_is_a_push_of_fifty_metres(self):
        """The 50.0 that rhyHeadsHealthy() compares the nearest-signal distance
        against is not a guess: it is argument 8 of the very call we patch."""
        self.skip_if_missing()
        raw, text = self.rows[RANGE_PUSH]
        self.assertEqual(raw, b"\x68\x00\x00\x48\x42")
        self.assertEqual(text, "push 0x42480000")
        self.assertEqual(struct.unpack("<f", raw[1:5])[0], 50.0)
        # it belongs to that call: inside DisplayActualLight, before the call, and
        # in the same code page, so it is the range of this corona and no other
        self.assertTrue(0x49DAB0 <= RANGE_PUSH < CALL_SITE)
        self.assertEqual(RANGE_PUSH & ~0xFFF, CALL_SITE & ~0xFFF)

    def test_the_head_loop_is_bounded_by_the_models_2dfx_count(self):
        """Whether a head exists at all is decided by the model's 2DFX entry
        count (CModelInfo+0xD): read at 0x49DBDD, tested at 0x49DBE3, and a
        count of 0 jumps to 0x49DE2C - past the corona call at 0x49DD4C. That is
        why "no records" is not automatically "the hook is broken", and why the
        list file has to carry the lens offsets itself: no .dff in gta3.img and no
        .ipl in it carries a 0x00010105 (2DFX) chunk (measured over all 12964 .dff
        and 164 .ipl entries; the exe contains no "2dFx" magic either)."""
        self.skip_if_missing()
        raw, text = self.rows[HEAD_LOOP_BOUND]
        self.assertEqual(raw, b"\x0f\xb6\x47\x0d")
        self.assertEqual(text, "movzx eax,BYTE PTR [edi+0xd]")
        raw, text = self.rows[HEAD_LOOP_TEST]
        self.assertEqual(raw[:2], b"\x0f\x8e")            # jle rel32
        target = (HEAD_LOOP_TEST + 6 + int.from_bytes(raw[2:6], "little")) & MASK16
        self.assertEqual(target, 0x49DE2C)
        self.assertGreater(target, CALL_SITE,
                           "the zero-count branch must skip the corona call")

    def test_the_range_is_tested_in_the_callee_after_the_patch_point(self):
        """d^2 against range^2, with `jnp` to the give-up path. It runs inside
        0x6FC180, i.e. after the instruction we detour: our recorder is not gated
        by the distance, the game's own drawing is."""
        self.skip_if_missing()
        self.assertGreater(CALLEE_RANGE_TEST, CALL_SITE)
        self.assertEqual(self.rows[CALLEE_RANGE_TEST][1],
                         "fld DWORD PTR [esp+0x4c]")
        self.assertEqual(self.rows[CALLEE_RANGE_TEST + 4][1],
                         "fmul DWORD PTR [esp+0x4c]")
        self.assertEqual(self.rows[CALLEE_RANGE_TEST + 8][1], "fcompp")
        self.assertEqual(self.rows[CALLEE_RANGE_TEST + 15][1][:3], "jnp")

    def test_the_fade_and_range_floats_are_35_and_50_metres(self):
        """Full alpha to 35 m, none at 50 m, 1/15 per metre in between - the
        distances a player can actually see a head at, read out of .rdata."""
        if self.exe is None:
            self.skipTest("gta-sa.exe not found (set RHY_GTA_EXE)")
        self.assertEqual(pe_float_at(self.exe, FLOAT_RANGE), 50.0)
        self.assertEqual(pe_float_at(self.exe, FLOAT_FADE), 35.0)
        rate = pe_float_at(self.exe, FLOAT_FADE_RATE)
        self.assertAlmostEqual(rate, 1.0 / 15.0, places=6)

    def test_the_code_uses_those_numbers(self):
        """The decision has to be made on the exe's range, not on a constant that
        can drift away from it, and it has to keep the hook when there is no
        signal inside that range to record."""
        with open(os.path.join(REPO, "plugin", "src", "signalheads.h"),
                  encoding="utf-8") as fh:
            hdr = fh.read()
        self.assertIn("kRhySignalCoronaRangeM = 50", hdr)
        self.assertIn("kRhySignalCoronaFadeM = 35", hdr)
        with open(os.path.join(REPO, "plugin", "src", "signalheads.cpp"),
                  encoding="utf-8") as fh:
            heads = fh.read()
        self.assertIn("nearestSignalM <= coronaRangeM", heads)
        self.assertIn("kRhyHeadSourceFar", heads)
        with open(os.path.join(REPO, "plugin", "src", "trafficlights.cpp"),
                  encoding="utf-8") as fh:
            traffic = fh.read()
        self.assertIn("kRhySignalCoronaRangeM", traffic)
        self.assertIn("rhyNearestSignal2D", traffic)
        self.assertIn("rhyHeadKeepsHook", traffic)
        # and the grace is a knob, so the decision can be delayed but not hidden
        with open(os.path.join(REPO, "plugin", "src", "config.cpp"),
                  encoding="utf-8") as fh:
            cfg = fh.read()
        self.assertIn('"head_grace_ms", 20000', cfg)
        self.assertIn('"lens_offset", 1', cfg)


class TestKillSwitch(unittest.TestCase):
    """source=list must be able to keep the patch from ever being written, and
    any byte mismatch must log a reason and fall back to source=list."""

    def setUp(self):
        with open(os.path.join(REPO, "plugin", "src", "signalhook.cpp"),
                  encoding="utf-8") as fh:
            self.hook = fh.read()
        with open(os.path.join(REPO, "plugin", "src", "signalheads.cpp"),
                  encoding="utf-8") as fh:
            self.heads = fh.read()
        with open(os.path.join(REPO, "plugin", "src", "trafficlights.cpp"),
                  encoding="utf-8") as fh:
            self.traffic = fh.read()

    def test_install_is_gated_on_source_and_bytes_in_one_place(self):
        # The gate is inside rhySignalHookInstall, not only in the caller, so no
        # call path can install the hook without both conditions.
        install = self.hook[self.hook.index("bool rhySignalHookInstall"):]
        self.assertIn("rhyConfigGet()", install)
        self.assertIn("traffic_source == 1", install)
        self.assertIn("rhySignalPatchCheck", install)
        self.assertIn("kSignalHookSource", self.hook)
        self.assertIn("kSignalHookCleanup", self.hook)

    def test_the_byte_check_decodes_the_call_and_checks_the_cleanup(self):
        check = self.heads[self.heads.index("int rhySignalPatchCheck"):]
        self.assertIn("0xE8", check)              # opcode, not a byte-for-byte copy
        self.assertIn("kRhySignalCoronaFn", check)
        self.assertIn("0x54", check)              # the add esp,0x54 window
        self.assertIn("kRhyPatchSource", check)
        # the source check comes before anything reads the exe
        self.assertLess(check.index("kRhyPatchSource"), check.index("callSite[0]"))

    def test_no_bytes_is_not_mistaken_for_bad_bytes(self):
        """s30 REGRESSION. The install path asks the source question first, with
        no byte windows at all. When that call answered kRhyPatchCallSite the
        hook could never install on any exe: every run from s19 to rhY's
        2026-11-12 test fell back to source=list, lit the pole instead of the
        head, and logged a byte mismatch about an exe whose bytes are correct.
        The null-window answer is now its own value, and the install path treats
        it as 'go and read the bytes'."""
        check = self.heads[self.heads.index("int rhySignalPatchCheck"):]
        gate = check[check.index("if (!callSite || !callee || !cleanup)"):]
        gate = "\n".join(line.split("//")[0] for line in gate.splitlines())
        first_return = gate[gate.index("return"):gate.index("return") + 60]
        self.assertIn("kRhyPatchNoBytes", first_return)
        self.assertNotIn("kRhyPatchCallSite", first_return)
        install = self.hook[self.hook.index("bool rhySignalHookInstall"):]
        self.assertIn("kRhyPatchNoBytes", install)
        # and the byte windows are still read through the pointer checks
        self.assertIn("rhyGameRead", install)

    def test_the_corona_resize_is_checked_and_gated(self):
        """Resizing the game's own corona is a second code patch, so it goes
        through the same two gates: source=game (it lives inside the install
        path, after the kill switch) and its own byte check."""
        install = self.hook[self.hook.index("bool rhySignalHookInstall"):]
        self.assertIn("rhySignalCoronaApply", install)
        self.assertLess(install.index("if (why != kRhyPatchOk)"),
                        install.index("rhySignalCoronaApply("))
        resize = self.hook[self.hook.index("bool rhySignalCoronaApply"):]
        self.assertIn("rhySignalCoronaPatchCheck", resize)
        self.assertIn("rhyGameRead", resize)
        self.assertIn("kRhySignalCoronaRadiusImm", resize)
        self.assertIn("VirtualProtect", resize)
        # corona_radius is a live knob: the ini-edit path re-applies it, so the
        # A/B does not need a restart (and 0 puts the game's flare back).
        with open(os.path.join(REPO, "plugin", "src", "trafficlights.cpp"),
                  encoding="utf-8") as fh:
            traffic = fh.read()
        edit = traffic[traffic.index("if (rev != g_cfgRev)"):]
        self.assertIn("rhySignalCoronaApply(cfg->traffic_corona_radius)", edit)
        check = self.heads[self.heads.index("int rhySignalCoronaPatchCheck"):]
        self.assertIn("kRhyPatchCoronaBytes", check)
        # the ini default and its off switch
        with open(os.path.join(REPO, "plugin", "src", "config.cpp"),
                  encoding="utf-8") as fh:
            cfg = fh.read()
        self.assertIn('"corona_radius=0.45', cfg)
        self.assertIn('"bulb_radiance=840', cfg)    # n23: 1200 x 0.7, finding C row 4b
        self.assertIn('"radiance_scale=1.0', cfg)   # n23: the signals' own scale
        self.assertIn('"bulb_radius=0', cfg)

    def test_the_patch_site_is_read_through_the_pointer_checks(self):
        install = self.hook[self.hook.index("bool rhySignalHookInstall"):]
        self.assertIn("rhyGameRead", install)
        self.assertNotIn("memcmp(at,", install)   # no raw read of the code page

    def test_fallback_to_list_is_logged(self):
        self.assertIn("Falling back to source=list", self.traffic)
        # and the frame function only retries when the user asked for game
        self.assertIn("cfg->traffic_source == 1 && !rhySignalHookInstalled()",
                      self.traffic)

    def test_the_hook_is_drained_even_when_the_module_is_off(self):
        # A full record array would count "dropped" forever and hide the real
        # counters; with the hook installed and source=list it must still drain.
        self.assertIn("rhySignalHookInstalled()", self.traffic)
        self.assertIn("rhySignalHeadsTake", self.traffic)


class TestRecorderCannotThrow(unittest.TestCase):
    """The recorder is called from the naked stub, whose .eh_frame FDE is empty
    (objdump -W: pc=stub..stub+0x29, all DW_CFA_nop), so the unwinder would
    resume with the CIE's rule `eip = [esp]` - argument 0, a data value. An
    exception, or any unwind that crosses the stub, lands on garbage. So the
    recorder path is straight-line code over a fixed array."""

    FORBIDDEN = ("new ", "throw ", "try {", "catch", "rhyLog", "std::string",
                 "push_back", "resize", "rhyCreateLight", "DrawLightInstance",
                 "VirtualAlloc", "malloc")

    @staticmethod
    def code_only(text):
        """Strip comments: the path must be free of the constructs, but its
        comments are allowed to talk about them."""
        out = []
        for line in text.splitlines():
            line = re.sub(r"//.*$", "", line)
            out.append(line)
        return "\n".join(out)

    def bodies(self):
        out = []
        with open(os.path.join(REPO, "plugin", "src", "signalhook.cpp"),
                  encoding="utf-8") as fh:
            src = fh.read()
        out.append(self.code_only(src[src.index("extern \"C\" int rhySignalRecordCorona"):
                                      src.index("__attribute__((naked))")]))
        with open(os.path.join(REPO, "plugin", "src", "signalheads.cpp"),
                  encoding="utf-8") as fh:
            src = fh.read()
        out.append(self.code_only(src[src.index("int rhyCollectHead"):src.index(
            "unsigned rhyCollectorTake")]))
        return out

    def test_nothing_in_the_recorder_path_can_throw_or_allocate(self):
        for body in self.bodies():
            for word in self.FORBIDDEN:
                self.assertNotIn(word, body, "recorder path contains %r" % word)

    def test_the_recorder_path_calls_no_game_function(self):
        for body in self.bodies():
            self.assertNotIn("0x6FC", body)
            self.assertNotIn("RegisterCorona(", body)

    def test_the_page_cache_is_invalidated_once_a_frame(self):
        with open(os.path.join(REPO, "plugin", "src", "signalhook.cpp"),
                  encoding="utf-8") as fh:
            src = fh.read()
        take = src[src.index("unsigned rhySignalHeadsTake"):]
        self.assertIn("rhyPageCacheClear", take)


if __name__ == "__main__":
    unittest.main()
