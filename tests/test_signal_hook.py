"""The s01 corona hook is a hand-written i386 detour, so it cannot be unit
tested by calling it. What can be tested is what it compiles to: the stub is a
few instructions whose every offset has to be right, and the two bugs it has
had are both wrong offsets.

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

Run against the built asi: build/gate/rhyremix.asi (what scripts/check.sh
builds) or build/rhyremix.asi, or point RHYREMIX_ASI at one.
"""

import os
import re
import shutil
import subprocess
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CALL_SITE = 0x49DD4C
RESUME_AT = CALL_SITE + 5          # the instruction the patched call returns to
CORONA_FN = 0x6FC580               # the corona-type thunk the stub jumps to
MASK16 = 0xFFFFFFFF


def find_asi():
    env = os.environ.get("RHYREMIX_ASI")
    cands = [env] if env else []
    cands += ["build/gate/rhyremix.asi", "build/rhyremix.asi"]
    for c in cands:
        p = c if os.path.isabs(c) else os.path.join(REPO, c)
        if os.path.exists(p):
            return p
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
    eax_at_call = None
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
            eax_at_call = eax
    return pushes, esp_at_call, tail, args_ptr


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

    def test_source_checks_the_callee_before_patching(self):
        path = os.path.join(REPO, "plugin", "src", "signalhook.cpp")
        with open(path, encoding="utf-8") as fh:
            src = fh.read()
        self.assertIn("kCalleeBytes", src)
        self.assertIn("kResumeAt", src)
        # The offsets the stub hard-codes must match the documented layout.
        self.assertIn("kArgPos = 6", src)
        self.assertIn("leal 36(%%ebp)", src)
        # ... and the realignment must precede the argument pushes.
        self.assertLess(src.index("andl $-16, %%esp"), src.index("pushl %%ebx"))
        # A bad pointer must never be dereferenced inside the render pass.
        self.assertIn("rhyReadable(args", src)
        self.assertIn("rhyRangePlausible", src)


if __name__ == "__main__":
    unittest.main()
