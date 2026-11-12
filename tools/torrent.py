#!/usr/bin/env python3
"""Make, verify and print the magnet of a .torrent - stdlib only (BEP 3, v1 info dict).

Why this exists at all: mktorrent is installed on the dev box, but the release
bundle is only trustworthy if the info-hash can be *re-derived from the bytes on
disk* by something that does not depend on the tool that made the file. So the
same module that can build a torrent also checks one, and the check is what
tools/make-release.sh runs before it prints a magnet.

    torrent.py make TARGET [-o OUT.torrent] [-T trackers.txt] [--piece-length N]
                          [--name NAME] [--comment C] [--source S] [--private]
                          [--no-date] [--json]
    torrent.py check TORRENT [--dir DIR] [--expect HASH] [-T trackers.txt] [--json]
    torrent.py magnet TORRENT [-T trackers.txt]
    torrent.py show TORRENT [--json]

`check` recomputes the info-hash two independent ways and fails (exit 1) unless
all of it agrees:

  * SHA1 over the canonical bencode of the info dict, compared with the raw
    bytes of the info dict inside the file (a non-canonical file would give
    every client a different hash);
  * piece-by-piece SHA1 over the actual files on disk, in the order the info
    dict lists them, compared with the `pieces` string;
  * every file's length, and its per-file sha256 when the torrent carries one
    (BEP 47);
  * with --expect, the whole thing against a hash written down elsewhere
    (bundle.json), so two builds of the same content must agree.

A torrent is a manifest of hashes, exactly like deps/manifest.json: the only
difference is that nobody is going to read the pieces by eye, so the verifier
has to be automatic.

exit: 0 ok, 1 verification failed, 2 usage / IO error.
"""
import argparse
import base64
import hashlib
import json
import os
import sys
import time
from urllib.parse import quote

CREATED_BY = "rhygtaSA tools/torrent.py"
MAX_PIECES = 1200          # aim: a pieces string that stays small enough to fit in a magnet-era .torrent
# 32 KiB is the floor: mktorrent refuses anything below 2^15, and so do
# a number of clients; 4 MiB is the ceiling a home mirror ever needs.
MIN_PIECE, MAX_PIECE = 1 << 15, 1 << 22


# ------------------------------------------------------------------ bencode ---
def bencode(obj):
    """Canonical bencode: dict keys sorted by their raw byte string."""
    if isinstance(obj, bool):
        raise TypeError("bencode: bool is not a torrent type (use int)")
    if isinstance(obj, int):
        return b"i%de" % obj
    if isinstance(obj, bytes):
        return b"%d:%s" % (len(obj), obj)
    if isinstance(obj, str):
        return bencode(obj.encode("utf-8"))
    if isinstance(obj, (list, tuple)):
        return b"l" + b"".join(bencode(v) for v in obj) + b"e"
    if isinstance(obj, dict):
        items = []
        for k, v in obj.items():
            kb = k.encode("utf-8") if isinstance(k, str) else k
            if not isinstance(kb, bytes) or not kb:
                raise TypeError("bencode: dict keys must be non-empty str/bytes")
            items.append((kb, v))
        items.sort(key=lambda kv: kv[0])
        return b"d" + b"".join(bencode(kb) + bencode(v) for kb, v in items) + b"e"
    raise TypeError("bencode: cannot encode %r" % type(obj))


def bdecode(data, pos=0):
    """Strict decoder. Returns (value, next_pos); strings stay bytes."""
    if pos >= len(data):
        raise ValueError("bencode: truncated input")
    c = data[pos:pos + 1]
    if c == b"i":
        end = data.index(b"e", pos)
        text = data[pos + 1:end]
        if not text or not text.lstrip(b"-").isdigit():
            raise ValueError("bencode: bad integer %r" % text)
        if text != b"0" and text != b"-0" and text.lstrip(b"-").startswith(b"0"):
            raise ValueError("bencode: integer leading zero %r" % text)
        return int(text), end + 1
    if c == b"l":
        out, pos = [], pos + 1
        while data[pos:pos + 1] != b"e":
            v, pos = bdecode(data, pos)
            out.append(v)
        return out, pos + 1
    if c == b"d":
        out, pos, prev = {}, pos + 1, None
        while data[pos:pos + 1] != b"e":
            k, pos = bdecode(data, pos)
            if not isinstance(k, bytes):
                raise ValueError("bencode: dict key is not a string")
            if prev is not None and k <= prev:
                raise ValueError("bencode: dict keys not sorted/unique (%r)" % k)
            prev = k
            out[k], pos = bdecode(data, pos)
        return out, pos + 1
    if c.isdigit():
        colon = data.index(b":", pos)
        n = int(data[pos:colon])
        start = colon + 1
        if start + n > len(data):
            raise ValueError("bencode: string length runs past the end")
        return data[start:start + n], start + n
    raise ValueError("bencode: unexpected %r at %d" % (c, pos))


def decode_span(data, key=b"info"):
    """Return (top_level_dict, raw_bytes_of_key_value). The raw span is what a
    client hashes, so it is compared against our own canonical re-encode. The
    walk records the exact byte range instead of searching for the key text,
    which could also appear inside a comment or a piece hash."""
    obj, end = bdecode(data)
    if not isinstance(obj, dict) or key not in obj:
        raise ValueError("torrent has no %r dict" % key)
    pos = 1
    while pos < end - 1:
        k, pos = bdecode(data, pos)
        vstart = pos
        _v, pos = bdecode(data, pos)
        if k == key:
            return obj, data[vstart:pos]
    raise ValueError("cannot locate %r in the file" % key)


# ------------------------------------------------------------------- pieces ---
def piece_length_for(total):
    n = MIN_PIECE
    while total // n + 1 > MAX_PIECES and n < MAX_PIECE:
        n *= 2
    return n


def walk_files(target):
    """Sorted (path_parts, size, abspath) for a directory; deterministic order."""
    out = []
    for root, dirs, files in os.walk(target):
        dirs.sort()
        for f in sorted(files):
            ap = os.path.join(root, f)
            if os.path.islink(ap):
                raise SystemExit("torrent: symlink inside the bundle, refusing: %s" % ap)
            rel = os.path.relpath(ap, target)
            out.append((rel.split(os.sep), os.path.getsize(ap), ap))
    out.sort(key=lambda t: [p.encode("utf-8", "surrogateescape") for p in t[0]])
    return out


def hash_pieces(entries, piece_len):
    """(pieces_bytes, per-file sha256, total bytes) streaming through the files."""
    h, pieces, n = None, bytearray(), 0
    sha256s = {}
    for parts, _size, ap in entries:
        fsha = hashlib.sha256()
        with open(ap, "rb") as f:
            while True:
                want = piece_len - n
                chunk = f.read(min(want, 1 << 20)) if want > 0 else b""
                if want <= 0:
                    pieces += h.digest()
                    h, n = None, 0
                    continue
                if not chunk:
                    break
                if h is None:
                    h = hashlib.sha1()
                h.update(chunk)
                fsha.update(chunk)
                n += len(chunk)
        sha256s["/".join(parts)] = fsha.hexdigest()
    if n:
        pieces += h.digest()
    return bytes(pieces), sha256s


# -------------------------------------------------------------- subcommands ---
def read_trackers(path):
    out = []
    if not path:
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                if any(c.isspace() for c in line):
                    raise SystemExit("trackers: %r has whitespace (one announce URL per line)" % line)
                out.append(line)
    return out


def magnet_link(info_hash_hex, name, trackers):
    link = "magnet:?xt=urn:btih:%s&xt=urn:btih:%s&dn=%s" % (
        info_hash_hex, base64.b32encode(bytes.fromhex(info_hash_hex)).decode(), quote(name, safe=""))
    for t in trackers:
        link += "&tr=%s" % quote(t, safe="")
    return link


def cmd_make(args):
    target = args.target
    if not os.path.exists(target):
        raise SystemExit("torrent: no such target: %s" % target)
    target = os.path.abspath(target)
    trackers = read_trackers(args.trackers)
    if args.piece_length:
        plen = args.piece_length
        if plen & (plen - 1) or plen < MIN_PIECE or plen > MAX_PIECE:
            raise SystemExit("torrent: --piece-length must be a power of two between %d and %d"
                             % (MIN_PIECE, MAX_PIECE))
    else:
        total = sum(s for _p, s, _a in walk_files(target)) if os.path.isdir(target) \
            else os.path.getsize(target)
        plen = piece_length_for(total)

    if os.path.isdir(target):
        entries = walk_files(target)
        if not entries:
            raise SystemExit("torrent: %s is empty - a torrent of nothing serves nobody" % target)
        name = args.name or os.path.basename(target.rstrip("/"))
    else:
        entries = [(os.path.basename(target), os.path.getsize(target), target)]
        name = args.name or os.path.basename(target)

    pieces, sha256s = hash_pieces(entries, plen)
    files = []
    for parts, size, _ap in entries:
        e = {"length": size, "path": list(parts)}
        if args.per_file_sha256:
            e["sha256"] = sha256s["/".join(parts)]
        files.append(e)
    info = {"name": name, "piece length": plen, "pieces": pieces}
    if os.path.isdir(target):
        info["files"] = files
    if args.private:
        info["private"] = 1
    if args.source:
        info["source"] = args.source

    top = {"announce": trackers[0] if trackers else None, "info": info,
           "encoding": b"UTF-8", "created by": CREATED_BY}
    if len(trackers) > 1:
        top["announce-list"] = [[t] for t in trackers]
    if not args.no_date:
        top["creation date"] = int(time.time())
    if args.comment:
        top["comment"] = args.comment
    top = {k: v for k, v in top.items() if v is not None}

    data = bencode(top)
    out = args.output or (name + ".torrent")
    with open(out, "wb") as f:
        f.write(data)
    info_hash = hashlib.sha1(bencode(info)).hexdigest()
    res = {"torrent": out, "info_hash": info_hash, "name": name, "piece_length": plen,
           "pieces": len(pieces) // 20, "files": len(files),
           "bytes": sum(s for _p, s, _a in entries), "trackers": trackers,
           "magnet": magnet_link(info_hash, name, trackers)}
    if args.json:
        print(json.dumps(res, indent=1))
    else:
        print("INFOHASH %s" % info_hash)
        print("MADE     %s (%d pieces of %d bytes, %d files, %d bytes total)"
              % (out, res["pieces"], plen, res["files"], res["bytes"]))
        for t in trackers:
            print("TRACKER  %s" % t)
        print("MAGNET   %s" % res["magnet"])
    return 0


def cmd_check(args):
    with open(args.torrent, "rb") as f:
        raw = f.read()
    try:
        top, info_raw = decode_span(raw)
    except (ValueError, IndexError) as e:
        print("FAIL   %s: not a bencoded torrent: %s" % (args.torrent, e))
        return 1
    info = top.get(b"info")
    if not isinstance(info, dict):
        print("FAIL   %s: no info dict" % args.torrent)
        return 1

    def s(key, default=None):
        v = info.get(key.encode(), default)
        if isinstance(v, bytes):
            return v.decode("utf-8", "replace")
        return v

    plen = info.get(b"piece length")
    pieces = info.get(b"pieces")
    if not isinstance(plen, int) or plen <= 0:
        print("FAIL   %s: bad piece length %r" % (args.torrent, plen))
        return 1
    if not isinstance(pieces, bytes) or len(pieces) % 20:
        print("FAIL   %s: pieces must be a whole number of 20-byte SHA1s" % args.torrent)
        return 1

    # 1. the hash a client would compute, two ways
    hash_canon = hashlib.sha1(bencode(info)).hexdigest()
    hash_raw = hashlib.sha1(info_raw).hexdigest()
    if hash_canon != hash_raw:
        print("FAIL   %s: info dict is not canonically bencoded (client hash %s, ours %s) - "
              "every client would get a different torrent" % (args.torrent, hash_raw, hash_canon))
        return 1

    # 2. the files on disk
    base = args.dir
    if base is None:
        guess = os.path.join(os.path.dirname(os.path.abspath(args.torrent)), s("name") or "")
        base = guess if os.path.isdir(guess) else os.getcwd()
    if b"files" in info:
        entries = []
        for e in info[b"files"]:
            rel = [p.decode("utf-8", "surrogateescape") for p in e.get(b"path", [])]
            if not rel or ".." in rel or os.path.isabs(rel[0]):
                print("FAIL   %s: file path %r escapes the bundle" % (args.torrent, rel))
                return 1
            entries.append((rel, e.get(b"length"), os.path.join(base, *rel)))
    else:
        entries = [([s("name") or ""], info.get(b"length"), os.path.join(base, s("name") or ""))]
    missing = [e[2] for e in entries if not os.path.isfile(e[2])]
    if missing:
        print("FAIL   %s: %d file(s) of the bundle are not under %s, e.g. %s"
              % (args.torrent, len(missing), base, missing[0]))
        return 1
    wrong = ["%s: torrent says %s, disk says %s" % ("/".join(p), want, os.path.getsize(ap))
             for p, want, ap in entries if want != os.path.getsize(ap)]
    if wrong:
        print("FAIL   %s: %s" % (args.torrent, wrong[0]))
        return 1

    got, sha256s = hash_pieces(entries, plen)
    if got != pieces:
        n = min(len(got), len(pieces))
        bad = next((i for i in range(0, n, 20) if got[i:i + 20] != pieces[i:i + 20]), n // 20)
        print("FAIL   %s: piece %d of %s does not match the bytes on disk (%s)"
              % (args.torrent, bad // 20, os.path.basename(args.torrent),
                 "/".join(entries[0][0])))
        return 1
    for e in info.get(b"files", []):
        want = e.get(b"sha256")
        if want:
            rel = "/".join(p.decode("utf-8", "surrogateescape") for p in e[b"path"])
            if sha256s.get(rel) != want.decode():
                print("FAIL   %s: %s sha256 %s, the torrent says %s"
                      % (args.torrent, rel, sha256s.get(rel), want.decode()))
                return 1

    # 3. trackers, if we were told which ones the release uses
    trackers = [t.decode() for grp in top.get(b"announce-list", []) for t in grp] or \
               ([top[b"announce"].decode()] if b"announce" in top else [])
    want_trackers = read_trackers(args.trackers) if args.trackers else []
    bad_tr = [t for t in want_trackers if t not in trackers]

    if args.expect and args.expect.lower() != hash_canon:
        print("FAIL   %s: info hash is %s, expected %s - the content changed"
              % (args.torrent, hash_canon, args.expect.lower()))
        return 1
    if bad_tr:
        print("FAIL   %s: trackers missing from the torrent: %s" % (args.torrent, " ".join(bad_tr)))
        return 1

    print("OK     %s: info hash %s recomputes from %d file(s) / %d piece(s) of %d bytes"
          % (os.path.basename(args.torrent), hash_canon, len(entries), len(pieces) // 20, plen))
    print("MAGNET %s" % magnet_link(hash_canon, s("name") or "", trackers))
    if args.json:
        print(json.dumps({"info_hash": hash_canon, "name": s("name"), "piece_length": plen,
                          "pieces": len(pieces) // 20, "files": len(entries),
                          "trackers": trackers, "bytes": sum(e[1] for e in entries)}, indent=1))
    return 0


def cmd_magnet(args):
    with open(args.torrent, "rb") as f:
        top, _ = decode_span(f.read())
    info = top[b"info"]
    h = hashlib.sha1(bencode(info)).hexdigest()
    trackers = [t.decode() for grp in top.get(b"announce-list", []) for t in grp] or \
               ([top[b"announce"].decode()] if b"announce" in top else [])
    if args.trackers:
        trackers = read_trackers(args.trackers)
    print(magnet_link(h, info[b"name"].decode("utf-8", "replace"), trackers))
    return 0


def cmd_show(args):
    with open(args.torrent, "rb") as f:
        top, info_raw = decode_span(f.read())
    info = top[b"info"]
    out = {"torrent": args.torrent,
           "info_hash": hashlib.sha1(info_raw).hexdigest(),
           "announce": (top.get(b"announce") or b"").decode(),
           "announce-list": [[t.decode() for t in g] for g in top.get(b"announce-list", [])],
           "comment": (top.get(b"comment") or b"").decode(),
           "created by": (top.get(b"created by") or b"").decode(),
           "creation date": top.get(b"creation date"),
           "name": info[b"name"].decode("utf-8", "replace"),
           "piece length": info[b"piece length"],
           "pieces": len(info[b"pieces"]) // 20,
           "private": info.get(b"private", 0),
           "files": [("/".join(p.decode("utf-8", "replace") for p in e[b"path"]), e[b"length"])
                     for e in info.get(b"files", [])] or
                    [(info[b"name"].decode("utf-8", "replace"), info.get(b"length"))]}
    if args.json:
        print(json.dumps(out, indent=1))
    else:
        for k, v in out.items():
            print("%-14s %s" % (k, v))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="torrent.py", description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("make", help="build a .torrent from a file or directory")
    m.add_argument("target")
    m.add_argument("-o", "--output")
    m.add_argument("-T", "--trackers", help="announce URLs, one per line")
    m.add_argument("--piece-length", type=int)
    m.add_argument("--name")
    m.add_argument("--comment")
    m.add_argument("--source", help="BEP 3 source tag (changes the info hash)")
    m.add_argument("--private", action="store_true")
    m.add_argument("--no-date", action="store_true")
    m.add_argument("--per-file-sha256", action="store_true", help="add BEP 47 sha256 per file")
    m.add_argument("--json", action="store_true")
    m.set_defaults(func=cmd_make)

    c = sub.add_parser("check", help="recompute the info hash from the files on disk")
    c.add_argument("torrent")
    c.add_argument("--dir", help="where the bundle lives (default: next to the .torrent)")
    c.add_argument("--expect", help="fail unless the info hash equals this")
    c.add_argument("-T", "--trackers", help="fail unless every tracker here is in the torrent")
    c.add_argument("--json", action="store_true")
    c.set_defaults(func=cmd_check)

    g = sub.add_parser("magnet", help="print the magnet link of a .torrent")
    g.add_argument("torrent")
    g.add_argument("-T", "--trackers")
    g.set_defaults(func=cmd_magnet)

    s = sub.add_parser("show", help="dump a .torrent as readable metadata")
    s.add_argument("torrent")
    s.add_argument("--json", action="store_true")
    s.set_defaults(func=cmd_show)

    args = ap.parse_args(argv)
    try:
        return args.func(args)
    except OSError as e:
        sys.stderr.write("torrent: %s\n" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
