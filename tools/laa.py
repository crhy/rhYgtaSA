#!/usr/bin/env python3
"""Set, clear or check Large Address Aware (LAA) on a 32-bit GTA SA exe.

usage: laa.py EXE [--check|--set|--clear]      (default: --check, read-only)

Why: a 32-bit process without IMAGE_FILE_LARGE_ADDRESS_AWARE (0x0020 in the COFF
Characteristics) gets ~2 GB of address space from Wine/Proton instead of ~4 GB.
In the RTX build that same process carries the Remix bridge client (the
`d3d9.dll` in the game folder), ModLoader, MixSets' 1900 MB streaming cap and
the vegetation pack's 151 MB of textures - see docs/VEGETATION.md, cause 2. The
downgrader ships the exe without the flag (0x010f), so apply.sh sets it.

What it touches: the two Characteristics bytes at e_lfanew+22 and nothing else.
`--set` first copies the exe to EXE.noLAA-backup, once - a later run never
overwrites that backup, so re-running the installer cannot replace the pristine
pre-LAA copy with an already-patched file.

Refuses anything that is not a 32-bit PE (no MZ, no PE signature, machine not
0x14c i386, or a PE32+ optional header): setting this bit on a 64-bit image is
a no-op at best and a corrupt header at worst.

Exit codes: --check 0 = flag set, 1 = flag not set; --set/--clear 0 = done
(including "already in the wanted state"); 2 = refused (not a 32-bit PE) or the
write could not be done.
"""
import os
import shutil
import struct
import sys
from pathlib import Path

LAA = 0x0020                      # IMAGE_FILE_LARGE_ADDRESS_AWARE
MACHINE_I386 = 0x14C
PE32_MAGIC = 0x10B                # 0x20B would be PE32+ (64-bit)
E_LFANEW = 0x3C                   # u32 offset of the "PE\0\0" signature
CHARS_OFF = 22                    # COFF Characteristics, relative to the signature
OPT_OFF = 24                      # optional header, relative to the signature
BACKUP_SUFFIX = ".noLAA-backup"

EXIT_OK, EXIT_NOT_SET, EXIT_REFUSED = 0, 1, 2


class PeError(Exception):
    """Not a 32-bit PE image - refuse to touch it."""


class Pe:
    """The few COFF header fields this tool cares about."""

    def __init__(self, data: bytes):
        if len(data) < E_LFANEW + 4 or data[:2] != b"MZ":
            raise PeError("no MZ at offset 0 - not a PE file")
        (self.lfanew,) = struct.unpack_from("<I", data, E_LFANEW)
        if self.lfanew < 0x40 or self.lfanew + OPT_OFF + 2 > len(data):
            raise PeError(f"e_lfanew {self.lfanew:#x} points outside the file")
        if data[self.lfanew:self.lfanew + 4] != b"PE\0\0":
            raise PeError(f"no PE signature at {self.lfanew:#x}")
        # COFF file header after the 4-byte signature: machine +4, nsec +6,
        # TimeDateStamp +8, symtab +12, nsyms +16, SizeOfOptionalHeader +20,
        # Characteristics +22 (all u16 unless stated, relative to "PE\0\0").
        self.machine, nsec = struct.unpack_from("<HH", data, self.lfanew + 4)
        (opt_size,) = struct.unpack_from("<H", data, self.lfanew + 4 + 16)
        (self.chars,) = struct.unpack_from("<H", data, self.lfanew + CHARS_OFF)
        self.chars_off = self.lfanew + CHARS_OFF
        if self.lfanew + OPT_OFF + opt_size > len(data):
            raise PeError(f"optional header ({opt_size} bytes) runs past the end of the file")
        if self.machine != MACHINE_I386:
            raise PeError(f"machine {self.machine:#x} is not i386 ({MACHINE_I386:#x}) - not a 32-bit PE")
        (magic,) = struct.unpack_from("<H", data, self.lfanew + OPT_OFF)
        if magic != PE32_MAGIC:
            raise PeError(f"optional header magic {magic:#x} is not PE32 ({PE32_MAGIC:#x}) - not a 32-bit PE")
        self.nsec = nsec

    @property
    def laa(self) -> bool:
        return bool(self.chars & LAA)


def backup_path(exe: Path) -> Path:
    return exe.with_name(exe.name + BACKUP_SUFFIX)


def patch(exe: Path, want: bool) -> tuple:
    """Set (want=True) or clear the LAA bit. Returns (pe, changed, backup)."""
    data = exe.read_bytes()
    pe = Pe(data)                       # raises PeError before anything is written
    new = (pe.chars | LAA) if want else (pe.chars & ~LAA)
    if new == pe.chars:
        return pe, False, None
    if not os.access(exe, os.W_OK):     # check before the backup, so a failed run leaves nothing behind
        raise OSError(f"{exe} is not writable")
    backup = None
    if want:                            # one-time: never overwrite an existing backup
        b = backup_path(exe)
        if not b.exists():
            shutil.copy2(exe, b)        # copy2 keeps the mode, so the backup stays executable
            backup = b
    with open(exe, "r+b") as f:        # two bytes in place: no temp copy of a 5 MB exe
        f.seek(pe.chars_off)
        f.write(struct.pack("<H", new))
        f.flush()
        os.fsync(f.fileno())
    return pe, True, backup


def state(pe: Pe) -> str:
    return f"characteristics {pe.chars:#07x}: Large Address Aware {'SET' if pe.laa else 'NOT set'}"


def main(argv: list) -> int:
    args = [a for a in argv[1:] if not a.startswith("--")]
    flags = [a for a in argv[1:] if a.startswith("--")]
    if len(args) != 1 or len(flags) > 1 or (set(flags) - {"--check", "--set", "--clear", "--help", "-h"}):
        print("usage: laa.py EXE [--check|--set|--clear]   (default --check, read-only)",
              file=sys.stderr)
        return EXIT_REFUSED
    if flags and flags[0] in ("--help", "-h"):
        print(__doc__.strip())
        return EXIT_OK
    exe = Path(args[0])
    mode = flags[0] if flags else "--check"
    if not exe.is_file():
        print(f"laa: {exe}: no such file", file=sys.stderr)
        return EXIT_REFUSED
    try:
        if mode == "--check":
            pe = Pe(exe.read_bytes())
            note = ("~4 GB address space" if pe.laa
                    else "~2 GB address space - run: laa.py " + str(exe) + " --set (see docs/VEGETATION.md)")
            print(f"laa: {exe}: {state(pe)} ({note})")
            return EXIT_OK if pe.laa else EXIT_NOT_SET
        pe, changed, backup = patch(exe, mode == "--set")
    except PeError as e:
        print(f"laa: {exe}: refusing - {e}", file=sys.stderr)
        return EXIT_REFUSED
    except OSError as e:
        print(f"laa: {exe}: {e}", file=sys.stderr)   # e.g. read-only file: could not be done
        return EXIT_REFUSED
    if not changed:
        print(f"laa: {exe}: {state(pe)} already, nothing to do")
        return EXIT_OK
    after = pe.chars | LAA if mode == "--set" else pe.chars & ~LAA
    extra = f"; one-time backup {backup.name}" if backup else ""
    print(f"laa: {exe}: characteristics {pe.chars:#07x} -> {after:#07x} "
          f"(Large Address Aware {'set' if mode == '--set' else 'cleared'}){extra}")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main(sys.argv))
