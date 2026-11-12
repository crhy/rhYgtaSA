"""tools/laa.py — Large Address Aware on a 32-bit exe, tested without any native
code: the fake exes below are hand-built PE headers (MZ + e_lfanew + COFF +
optional header magic), which is all the tool reads and all it writes.

Why this is worth testing at all: the RTX copy's gta-sa.exe shipped with
Characteristics 0x010f (no IMAGE_FILE_LARGE_ADDRESS_AWARE), i.e. ~2 GB of
address space for a process that also carries the Remix bridge client and a
1900 MB streaming cap - docs/VEGETATION.md cause 2. apply.sh now sets the bit,
so a wrong mask or a wrong header offset would silently ship a 2 GB process.
The two offsets that must be right are e_lfanew (0x3C in the DOS header) and
Characteristics (e_lfanew+22, i.e. after SizeOfOptionalHeader at +20).

Covers: set / check / clear, idempotence, the one-time backup, refusal of a
non-PE file and of a 64-bit PE, and the CLI exit codes deploy.sh relies on
(--check 0 when set, 1 when not).

Run: python3 -m pytest -q tests/test_laa.py   (plain unittest works too)
"""
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import laa  # noqa: E402

LAA = laa.LAA
TMPROOT = os.path.join(REPO, "build", "tmp")


def fake_pe(path, machine=0x14C, magic=0x10B, chars=0x10F, e_lfanew=0x80, size=0x400):
    """Minimal but header-correct PE: only the fields laa.py reads are real."""
    buf = bytearray(size)
    buf[0:2] = b"MZ"
    struct.pack_into("<I", buf, 0x3C, e_lfanew)
    if e_lfanew + 24 + 224 > size:      # deliberately broken: header points past the end
        with open(path, "wb") as f:
            f.write(buf)
        return path
    buf[e_lfanew:e_lfanew + 4] = b"PE\0\0"
    # COFF file header: machine, nsec, timestamp, symtab, nsyms, optsize, chars
    struct.pack_into("<HHIIIHH", buf, e_lfanew + 4, machine, 2, 0x5F000000, 0, 0, 224, chars)
    # optional header: just the magic plus padding, 224 bytes total
    struct.pack_into("<H", buf, e_lfanew + 24, magic)
    for i in range(e_lfanew + 26, min(size, e_lfanew + 24 + 224)):
        buf[i] = 0x5A                      # "optional header body", must stay untouched
    buf[size - 4:size] = b"TAIL"           # and so must the tail of the file
    with open(path, "wb") as f:
        f.write(buf)
    return path


def chars_of(path):
    with open(path, "rb") as f:
        return laa.Pe(f.read()).chars


class LaaCase(unittest.TestCase):
    def setUp(self):
        os.makedirs(TMPROOT, exist_ok=True)
        self.dir = tempfile.mkdtemp(prefix="laa-test-", dir=TMPROOT)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def exe(self, name="gta-sa.exe", **kw):
        return fake_pe(os.path.join(self.dir, name), **kw)

    def run_cli(self, *args):
        p = subprocess.run([sys.executable, os.path.join(REPO, "tools", "laa.py"), *args],
                           capture_output=True, text=True)
        return p.returncode, p.stdout + p.stderr


class TestSetClearCheck(LaaCase):
    def test_set_sets_only_the_laa_bit(self):
        exe = self.exe(chars=0x10F)
        before = open(exe, "rb").read()
        laa.patch(pathlib.Path(exe), True)
        after = open(exe, "rb").read()
        self.assertEqual(len(before), len(after))
        pe = laa.Pe(before)
        diff = [i for i in range(len(before)) if before[i] != after[i]]
        self.assertTrue(diff, "nothing was written")
        self.assertTrue(set(diff) <= {pe.chars_off, pe.chars_off + 1},
                        "bytes outside the Characteristics u16 changed: %s" % diff)
        self.assertEqual(chars_of(exe), 0x12F)          # 0x10f | 0x20, other bits kept
        self.assertTrue(laa.Pe(after).laa)

    def test_check_reports_and_exits_by_the_flag(self):
        off, on = self.exe(name="off.exe", chars=0x10F), self.exe(name="on.exe", chars=0x12F)
        rc, out = self.run_cli(off, "--check")
        self.assertEqual(rc, 1)
        self.assertIn("NOT set", out)
        rc, out = self.run_cli(on, "--check")
        self.assertEqual(rc, 0)
        self.assertIn("SET", out)
        self.assertEqual(self.run_cli(on)[0], 0)        # --check is the default

    def test_set_then_clear_roundtrip(self):
        exe = pathlib.Path(self.exe(chars=0x10F))
        self.assertEqual(laa.patch(exe, True)[1], True)
        self.assertEqual(chars_of(exe), 0x12F)
        self.assertEqual(laa.patch(exe, False)[1], True)
        self.assertEqual(chars_of(exe), 0x10F)
        self.assertFalse(laa.Pe(exe.read_bytes()).laa)

    def test_idempotent(self):
        exe = pathlib.Path(self.exe(chars=0x10F))
        laa.patch(exe, True)
        pe, changed, backup = laa.patch(exe, True)
        self.assertFalse(changed, "a second --set must not write again")
        self.assertIsNone(backup, "a second --set must not make another backup")
        self.assertEqual(chars_of(exe), 0x12F)
        laa.patch(exe, False)
        pe, changed, backup = laa.patch(exe, False)
        self.assertFalse(changed, "a second --clear must not write again")
        self.assertEqual(chars_of(exe), 0x10F)

    def test_backup_is_made_once_and_holds_the_pre_laa_bytes(self):
        exe = pathlib.Path(self.exe(chars=0x10F))
        _, _, backup = laa.patch(exe, True)
        self.assertIsNotNone(backup)
        self.assertEqual(backup.name, "gta-sa.exe.noLAA-backup")
        self.assertEqual(chars_of(backup), 0x10F, "the backup must be the pre-LAA exe")
        # a later run must not replace the pristine backup with a patched copy
        shutil.copy2(exe, backup)
        self.assertEqual(chars_of(backup), 0x12F)
        laa.patch(exe, True)
        laa.patch(exe, False)
        laa.patch(exe, True)
        self.assertEqual(chars_of(backup), 0x12F, "the existing backup was overwritten")

    def test_backup_keeps_the_executable_mode(self):
        exe = pathlib.Path(self.exe(chars=0x10F))
        os.chmod(exe, 0o755)
        _, _, backup = laa.patch(exe, True)
        self.assertEqual(os.stat(backup).st_mode & 0o777, 0o755, "an exe backup that is not +x is a trap")

    def test_cli_set_clear_check_sequence(self):
        exe = self.exe(chars=0x10F)
        self.assertEqual(self.run_cli(exe, "--check")[0], 1)
        rc, out = self.run_cli(exe, "--set")
        self.assertEqual(rc, 0)
        self.assertIn("0x0010f -> 0x0012f", out)
        self.assertIn("noLAA-backup", out)
        self.assertEqual(self.run_cli(exe, "--check")[0], 0)
        rc, out = self.run_cli(exe, "--set")
        self.assertEqual(rc, 0)
        self.assertIn("already", out)
        self.assertEqual(self.run_cli(exe, "--clear")[0], 0)
        self.assertEqual(self.run_cli(exe, "--check")[0], 1)


class TestRefusals(LaaCase):
    def test_refuses_a_non_pe_file(self):
        p = os.path.join(self.dir, "not-an-exe.txt")
        with open(p, "wb") as f:
            f.write(b"echo hello\n" * 40)
        before = open(p, "rb").read()
        rc, out = self.run_cli(p, "--set")
        self.assertEqual(rc, 2)
        self.assertIn("refusing", out)
        self.assertEqual(open(p, "rb").read(), before, "a refused file must be untouched")
        self.assertFalse(os.path.exists(p + laa.BACKUP_SUFFIX), "a refusal must not leave a backup")

    def test_refuses_a_64_bit_pe(self):
        exe = self.exe(machine=0x8664, magic=0x20B, chars=0x12F)
        before = open(exe, "rb").read()
        rc, out = self.run_cli(exe, "--set")
        self.assertEqual(rc, 2)
        self.assertIn("machine 0x8664", out)
        self.assertEqual(open(exe, "rb").read(), before)
        self.assertEqual(self.run_cli(exe, "--check")[0], 2, "--check must refuse too, not report 'not set'")

    def test_refuses_a_pe32plus_optional_header_on_an_i386_machine(self):
        exe = self.exe(machine=0x14C, magic=0x20B)
        rc, out = self.run_cli(exe, "--set")
        self.assertEqual(rc, 2)
        self.assertIn("PE32", out)

    def test_refuses_a_bogus_e_lfanew_and_a_missing_file(self):
        exe = self.exe(e_lfanew=0x7FFF0000)
        rc, out = self.run_cli(exe, "--set")
        self.assertEqual(rc, 2)
        self.assertIn("e_lfanew", out)
        rc, out = self.run_cli(os.path.join(self.dir, "nope.exe"), "--check")
        self.assertEqual(rc, 2)
        self.assertIn("no such file", out)

    def test_refuses_a_truncated_header(self):
        p = os.path.join(self.dir, "truncated.exe")
        with open(p, "wb") as f:
            f.write(b"MZ" + b"\0" * 0x3E + struct.pack("<I", 0x80))
        self.assertEqual(self.run_cli(p, "--set")[0], 2)

    def test_a_read_only_exe_fails_without_leaving_a_backup(self):
        exe = pathlib.Path(self.exe(chars=0x10F))
        os.chmod(exe, 0o444)
        rc, out = self.run_cli(str(exe), "--set")
        self.assertEqual(rc, 2)
        self.assertIn("writable", out)
        self.assertEqual(chars_of(exe), 0x10F)
        self.assertFalse(os.path.exists(str(exe) + laa.BACKUP_SUFFIX),
                         "a run that cannot write must not leave a backup behind")

    def test_bad_usage(self):
        exe = self.exe()
        rc, out = self.run_cli(exe, "--set", "--clear")
        self.assertEqual(rc, 2)
        self.assertIn("usage", out)
        self.assertEqual(chars_of(exe), 0x10F)


if __name__ == "__main__":
    unittest.main()
