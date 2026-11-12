"""tools/make-release.sh - the self-hosted dependency bundle (task s14).

The gate, in code. Everything runs on a FAKE set under REPO/build/tmp: five small
random files staged in a temp directory and a small manifest. No real pack is
copied into the repo, no real host is contacted, no torrent is announced.

What has to hold, and why each test exists:

  * the torrent's info hash must recompute from the bytes on disk. The check here
    is deliberately NOT tools/torrent.py (that is the thing under test): bdecode,
    canonical re-encode, SHA1 of the info dict and piece-by-piece re-hashing are
    re-implemented below, so a bug in the builder cannot certify itself;
  * SHA256SUMS must verify with sha256sum -c, and must list the hosted archives
    (not the bundle's own README/manifest), because that file is what a user
    checks before installing;
  * tools/fetch-deps.sh must be able to install from the bundle over file:// and
    over a local http.server - the bundle is only useful if the fetcher that
    apply.sh runs can consume it;
  * licences decide what is collected: redistribute no/ask stay out unless the
    caller passes --include-no/--include-ask WITH --permission-note;
  * a staged file whose hash contradicts the manifest stops the release (exit 1)
    rather than publishing a bundle of the wrong thing;
  * the magnet written into deps/manifest.json must be the one that torrent.py
    check derived, and the https mirror slots must stay TBD (no host yet).

Run: python3 -m pytest -q tests/test_make_release.py   (plain unittest works too)
"""
import hashlib
import http.server
import json
import os
import shutil
import subprocess
import tempfile
import threading
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MKREL = os.path.join(REPO, "tools", "make-release.sh")
FETCH = os.path.join(REPO, "tools", "fetch-deps.sh")
TORRENT_PY = os.path.join(REPO, "tools", "torrent.py")
TRACKERS = os.path.join(REPO, "deps", "trackers.txt")
TMPROOT = os.path.join(REPO, "build", "tmp")


# --------------------------------------------- independent torrent reader ----
def bdecode(data, pos=0):
    c = data[pos:pos + 1]
    if c == b"i":
        end = data.index(b"e", pos)
        return int(data[pos + 1:end]), end + 1
    if c == b"l":
        out, pos = [], pos + 1
        while data[pos:pos + 1] != b"e":
            v, pos = bdecode(data, pos)
            out.append(v)
        return out, pos + 1
    if c == b"d":
        out, pos = {}, pos + 1
        while data[pos:pos + 1] != b"e":
            k, pos = bdecode(data, pos)
            out[k], pos = bdecode(data, pos)
        return out, pos + 1
    n = int(data[pos:data.index(b":", pos)])
    start = data.index(b":", pos) + 1
    return data[start:start + n], start + n


def bencode(obj):
    if isinstance(obj, int):
        return b"i%de" % obj
    if isinstance(obj, bytes):
        return b"%d:%s" % (len(obj), obj)
    if isinstance(obj, str):
        return bencode(obj.encode())
    if isinstance(obj, (list, tuple)):
        return b"l" + b"".join(bencode(v) for v in obj) + b"e"
    if isinstance(obj, dict):
        items = sorted(obj.items(), key=lambda kv: kv[0])
        return b"d" + b"".join(bencode(k) + bencode(v) for k, v in items) + b"e"
    raise TypeError(obj)


def torrent_info(path):
    top, _ = bdecode(open(path, "rb").read())
    return top, top[b"info"]


def info_hash(top, info):
    """SHA1 of the canonical bencode of the info dict - what every client uses."""
    return hashlib.sha1(bencode(info)).hexdigest()


def pieces_from_disk(info, base):
    """Re-hash the bundle: pieces span file boundaries, like every client does."""
    plen = info[b"piece length"]
    files = [(e[b"length"], os.path.join(base, *[p.decode() for p in e[b"path"]]))
             for e in info[b"files"]]
    out, buf, h = bytearray(), 0, None
    for length, path in files:
        with open(path, "rb") as f:
            left = length
            while left:
                want = min(plen - buf, left, 1 << 20)
                if want == 0:
                    out += h.digest()
                    h, buf = None, 0
                    continue
                chunk = f.read(want)
                if not chunk:
                    raise AssertionError("short file: %s" % path)
                h = h or hashlib.sha1()
                h.update(chunk)
                buf += len(chunk)
                left -= len(chunk)
    if buf:
        out += h.digest()
    return bytes(out), [(l, p) for l, p in files]


def read_trackers(path=TRACKERS):
    return [l.strip() for l in open(path, encoding="utf-8") if l.strip()]


# ------------------------------------------------------------ fake fixtures ---
def entry(eid, name, redist, **kw):
    e = {"id": eid, "group": "characters", "name": name, "upstream_name": kw.pop("up", "TBD"),
         "size": None, "sha256": kw.pop("sha", "TBD"),
         "source_page": "https://www.mixmods.com.br/2022/07/pedfuncs/",
         "author": "Test Author", "license": "test licence for " + eid,
         "redistribute": redist, "target": "modloader/" + eid, "unpack": "zip",
         "requires": [],
         "urls": [{"kind": "mirror", "url": "TBD"}, {"kind": "torrent", "url": "TBD"},
                  {"kind": "source", "url": "https://www.mixmods.com.br/2022/07/pedfuncs/"}]}
    e.update(kw)
    return e


class BundleCase(unittest.TestCase):
    def setUp(self):
        os.makedirs(TMPROOT, exist_ok=True)
        self.dir = tempfile.mkdtemp(prefix="make-release-test-", dir=TMPROOT)
        self.stage = os.path.join(self.dir, "stage")
        self.out = os.path.join(self.dir, "out")
        os.makedirs(self.stage)
        # five fake archives: two we may host, one forbidden, one "ask", one staged
        # under the author's own file name; plus an entry whose file is never staged
        self.sizes = {"alpha-pack.zip": 300000, "beta-pack.zip": 1500000,
                      "forbidden-pack.zip": 40000, "ask-pack.zip": 60000,
                      "odd-name.zip": 120000}
        self.hosted_sizes = {"alpha-pack.zip": 300000, "beta-pack.zip": 1500000,
                             "gamma-pack.zip": 120000}
        for name, size in self.sizes.items():
            with open(os.path.join(self.stage, name), "wb") as f:
                f.write(os.urandom(size))
        self.manifest = os.path.join(self.dir, "manifest.json")
        json.dump({"schema": 1, "updated": "0-test", "title": "fake set", "files": [
            entry("alpha-pack", "alpha-pack.zip", "yes"),
            entry("beta-pack", "beta-pack.zip", "yes", up="Beta_Pack_1.0.zip"),
            entry("forbidden-pack", "forbidden-pack.zip", "no"),
            entry("ask-pack", "ask-pack.zip", "ask"),
            entry("gamma-pack", "gamma-pack.zip", "yes",
                  local_file=os.path.join(self.stage, "odd-name.zip")),
            entry("missing-pack", "missing-pack.zip", "yes"),
        ]}, open(self.manifest, "w", encoding="utf-8"), indent=2)
        # the staged file for beta-pack carries the name the author used
        shutil.move(os.path.join(self.stage, "beta-pack.zip"),
                    os.path.join(self.stage, "Beta_Pack_1.0.zip"))

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    # helpers ------------------------------------------------------------
    def run_mkrel(self, *args, env=None):
        e = dict(os.environ)
        e.update(env or {})
        p = subprocess.run(["bash", MKREL, *[str(a) for a in args]], capture_output=True, text=True,
                           timeout=300, env=e, cwd=REPO)
        return p.returncode, p.stdout + p.stderr

    def build(self, *args, **kw):
        manifest = kw.pop("manifest", self.manifest)
        rc, log = self.run_mkrel(kw.pop("out", self.out), "--manifest", manifest,
                                 "--stage", self.stage, *args, **kw)
        return rc, log

    def bundle_dir(self, out=None):
        return os.path.join(out or self.out, "rhygtasa-deps-0-test")

    def torrent_path(self, out=None):
        return os.path.join(out or self.out, "rhygtasa-deps-0-test.torrent")

    def hosted(self):
        return ["alpha-pack.zip", "beta-pack.zip", "gamma-pack.zip"]

    def staged_sha(self, name):
        real = {"beta-pack.zip": "Beta_Pack_1.0.zip", "gamma-pack.zip": "odd-name.zip"}.get(name, name)
        return hashlib.sha256(open(os.path.join(self.stage, real), "rb").read()).hexdigest()


class TestBundleContents(BundleCase):
    def test_collects_only_staged_and_allowed_entries(self):
        rc, log = self.build()
        self.assertEqual(rc, 0, log)
        b = self.bundle_dir()
        for name in self.hosted():
            self.assertTrue(os.path.isfile(os.path.join(b, name)), name)
        for name in ("forbidden-pack.zip", "ask-pack.zip", "missing-pack.zip"):
            self.assertFalse(os.path.exists(os.path.join(b, name)), name)
        self.assertIn("redistribute: no", log)
        self.assertIn("not staged", log)
        # matched by the author's own file name, and by an explicit local_file
        self.assertEqual(hashlib.sha256(open(os.path.join(b, "beta-pack.zip"), "rb").read()).hexdigest(),
                         self.staged_sha("beta-pack.zip"))
        self.assertEqual(hashlib.sha256(open(os.path.join(b, "gamma-pack.zip"), "rb").read()).hexdigest(),
                         self.staged_sha("gamma-pack.zip"))

    def test_bundle_layout_and_extras_outside_the_hashed_folder(self):
        rc, log = self.build()
        self.assertEqual(rc, 0, log)
        b = self.bundle_dir()
        for extra in ("SHA256SUMS", "manifest.json", "README.txt"):
            self.assertTrue(os.path.isfile(os.path.join(b, extra)), extra)
        # the magnet cannot live inside the folder its own hash covers
        self.assertTrue(os.path.isfile(self.torrent_path()))
        self.assertTrue(os.path.isfile(os.path.join(self.out, "rhygtasa-deps-0-test.magnet.txt")))
        self.assertTrue(os.path.isfile(os.path.join(self.out, "rhygtasa-deps-0-test.bundle.json")))
        self.assertFalse(os.path.exists(os.path.join(b, "rhygtasa-deps-0-test.torrent")))
        meta = json.load(open(os.path.join(self.out, "rhygtasa-deps-0-test.bundle.json"), encoding="utf-8"))
        self.assertEqual([f["name"] for f in meta["files"]], self.hosted())
        for f in meta["files"]:
            self.assertEqual(f["sha256"], self.staged_sha(f["name"]), f["name"])
        self.assertNotIn("btih", open(os.path.join(b, "README.txt"), encoding="utf-8").read(),
                         "the README inside the bundle must not carry the magnet it covers")

    def test_sha256sums_verify_and_cover_the_archives(self):
        rc, log = self.build()
        self.assertEqual(rc, 0, log)
        b = self.bundle_dir()
        p = subprocess.run(["sha256sum", "-c", "SHA256SUMS"], cwd=b, capture_output=True,
                           text=True, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        lines = dict(reversed(l.split()) for l in
                     open(os.path.join(b, "SHA256SUMS"), encoding="utf-8").read().splitlines())
        self.assertEqual(sorted(lines), sorted(self.hosted()))
        for name, want in lines.items():
            got = hashlib.sha256(open(os.path.join(b, name), "rb").read()).hexdigest()
            self.assertEqual(got, want, name)

    def test_rebuilding_the_same_content_gives_the_same_info_hash(self):
        rc, log = self.build()
        self.assertEqual(rc, 0, log)
        out2 = os.path.join(self.dir, "out2")
        rc2, log2 = self.build("--torrent-tool", "python", out=out2)
        self.assertEqual(rc2, 0, log2)
        top, info = torrent_info(self.torrent_path())
        top2, info2 = torrent_info(os.path.join(out2, "rhygtasa-deps-0-test.torrent"))
        self.assertEqual(info_hash(top, info), info_hash(top2, info2),
                         "mktorrent and the python builder must agree on the info hash")
        self.assertEqual(info[b"piece length"], info2[b"piece length"])


class TestTorrentVerifies(BundleCase):
    def test_info_hash_recomputes_from_the_files_on_disk(self):
        rc, log = self.build()
        self.assertEqual(rc, 0, log)
        top, info = torrent_info(self.torrent_path())
        got, files = pieces_from_disk(info, self.bundle_dir())
        self.assertEqual(got, info[b"pieces"], "pieces do not match the bytes on disk")
        self.assertEqual(len(info[b"pieces"]) % 20, 0)
        h = info_hash(top, info)
        self.assertRegex(h, r"^[0-9a-f]{40}$")
        # the magnet the tool printed must be that hash, and carry every tracker
        magnet = open(os.path.join(self.out, "rhygtasa-deps-0-test.magnet.txt"),
                      encoding="utf-8").read().strip()
        self.assertIn("xt=urn:btih:" + h, magnet)
        for t in read_trackers():
            self.assertIn(t.replace(":", "%3A").replace("/", "%2F"), magnet)
        # and the .torrent must name the same trackers
        ann = [t.decode() for g in top.get(b"announce-list", []) for t in g]
        for t in read_trackers():
            self.assertIn(t, ann)

    def test_a_tampered_file_breaks_the_check(self):
        rc, log = self.build()
        self.assertEqual(rc, 0, log)
        victim = os.path.join(self.bundle_dir(), "alpha-pack.zip")
        with open(victim, "r+b") as f:
            f.seek(0)
            f.write(b"not what the torrent says")
        p = subprocess.run(["python3", TORRENT_PY, "check", self.torrent_path(),
                            "--dir", self.bundle_dir()], capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("does not match the bytes on disk", p.stdout + p.stderr)

    def test_check_fails_when_a_file_is_missing(self):
        rc, log = self.build()
        self.assertEqual(rc, 0, log)
        os.remove(os.path.join(self.bundle_dir(), "beta-pack.zip"))
        p = subprocess.run(["python3", TORRENT_PY, "check", self.torrent_path(),
                            "--dir", self.bundle_dir()], capture_output=True, text=True, timeout=120)
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)
        self.assertIn("not under", p.stdout + p.stderr)

    def test_trackers_file_is_comment_free(self):
        raw = open(TRACKERS, encoding="utf-8").read()
        self.assertNotIn("#", raw, "deps/trackers.txt is a comment-free config")
        for line in raw.splitlines():
            if line.strip():
                self.assertRegex(line.strip(), r"^(https?|udp)://[^ \t]+$")
        self.assertTrue(read_trackers(), "no trackers configured")


class TestLicenceGate(BundleCase):
    def test_no_reupload_mods_are_never_collected_by_default(self):
        rc, log = self.build()
        self.assertEqual(rc, 0, log)
        self.assertFalse(os.path.exists(os.path.join(self.bundle_dir(), "forbidden-pack.zip")))

    def test_widening_the_licence_needs_a_permission_note(self):
        rc, log = self.build("--include-no")
        self.assertEqual(rc, 2, log)
        self.assertIn("--permission-note", log)

    def test_permission_note_lets_the_ask_and_no_entries_in_and_records_it(self):
        rc, log = self.build("--include-no", "--include-ask",
                             "--permission-note", "e-mail from Test Author, 2026-11-11, bundle only")
        self.assertEqual(rc, 0, log)
        for name in ("forbidden-pack.zip", "ask-pack.zip"):
            self.assertTrue(os.path.isfile(os.path.join(self.bundle_dir(), name)), name)
        meta = json.load(open(os.path.join(self.out, "rhygtasa-deps-0-test.bundle.json"), encoding="utf-8"))
        self.assertIn("Test Author", meta["permission_note"])
        self.assertIn("Test Author", open(os.path.join(self.bundle_dir(), "README.txt"), encoding="utf-8").read())

    def test_a_staged_file_that_contradicts_the_manifest_stops_the_release(self):
        doc = json.load(open(self.manifest, encoding="utf-8"))
        for e in doc["files"]:
            if e["id"] == "alpha-pack":
                e["sha256"] = "0" * 64
        bad = os.path.join(self.dir, "bad-manifest.json")
        json.dump(doc, open(bad, "w", encoding="utf-8"), indent=2)
        rc, log = self.build(manifest=bad)
        self.assertEqual(rc, 1, log)
        self.assertIn("the manifest says", log)
        self.assertFalse(os.path.exists(self.torrent_path()), "a wrong bundle must not be published")

    def test_nothing_staged_is_not_silently_an_empty_bundle(self):
        # every entry points at a file that is nowhere: the release must stop, not
        # publish an empty folder with a torrent of nothing in it
        doc = json.load(open(self.manifest, encoding="utf-8"))
        for e in doc["files"]:
            e["name"] = "absent-" + e["id"] + ".zip"
            e.pop("local_file", None)
            e["upstream_name"] = "TBD"
        nowhere = os.path.join(self.dir, "nowhere.json")
        json.dump(doc, open(nowhere, "w", encoding="utf-8"), indent=2)
        rc, log = self.build(manifest=nowhere)
        self.assertEqual(rc, 1, log)
        self.assertIn("nothing to bundle", log)
        self.assertFalse(os.path.exists(self.torrent_path()))


class TestInstallFromBundle(BundleCase):
    """The point of the bundle: tools/fetch-deps.sh (what apply.sh runs) installs it."""

    def install_manifest(self, urls_for):
        doc = json.load(open(self.manifest, encoding="utf-8"))
        keep = []
        for e in doc["files"]:
            if e["name"] in self.hosted():
                e["sha256"] = self.staged_sha(e["name"])
                e["urls"] = [{"kind": "mirror", "url": urls_for(e["name"])}]
                keep.append(e)
        path = os.path.join(self.dir, "install.json")
        json.dump({"schema": 1, "updated": "0-test", "files": keep},
                  open(path, "w", encoding="utf-8"), indent=2)
        return path

    def run_fetch(self, manifest, dest):
        p = subprocess.run(["bash", FETCH, manifest, dest, "--quiet"], capture_output=True,
                           text=True, timeout=300, cwd=REPO, env=dict(os.environ, RHYS_ARIA2C="aria2c"))
        return p.returncode, p.stdout + p.stderr

    def check_installed(self, dest):
        for name in self.hosted():
            with open(os.path.join(dest, name), "rb") as f:
                self.assertEqual(hashlib.sha256(f.read()).hexdigest(), self.staged_sha(name), name)
        self.assertFalse([x for x in os.listdir(dest) if x.endswith((".part", ".bad-sha256"))])

    def test_fetch_deps_installs_from_the_bundle_over_file_urls(self):
        rc, log = self.build()
        self.assertEqual(rc, 0, log)
        b = os.path.abspath(self.bundle_dir())
        m = self.install_manifest(lambda n: "file://%s/%s" % (b, n))
        dest = os.path.join(self.dir, "installed-file")
        rc, log = self.run_fetch(m, dest)
        self.assertEqual(rc, 0, log)
        self.check_installed(dest)

    def test_fetch_deps_installs_from_the_bundle_over_http(self):
        rc, log = self.build()
        self.assertEqual(rc, 0, log)
        b = self.bundle_dir()

        class H(http.server.SimpleHTTPRequestHandler):
            def log_message(self, *a):
                pass

        srv = http.server.HTTPServer(("127.0.0.1", 0), H)
        port = srv.server_address[1]
        cwd = os.getcwd()
        os.chdir(b)
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        try:
            m = self.install_manifest(lambda n: "http://127.0.0.1:%d/%s" % (port, n))
            dest = os.path.join(self.dir, "installed-http")
            rc, log = self.run_fetch(m, dest)
            self.assertEqual(rc, 0, log)
            self.check_installed(dest)
        finally:
            srv.shutdown()
            srv.server_close()
            os.chdir(cwd)

    def test_the_bundle_hash_catches_a_corrupt_mirror(self):
        rc, log = self.build()
        self.assertEqual(rc, 0, log)
        b = os.path.abspath(self.bundle_dir())
        m = self.install_manifest(lambda n: "file://%s/%s" % (b, n))
        with open(os.path.join(b, "alpha-pack.zip"), "r+b") as f:
            f.write(b"corrupted")
        dest = os.path.join(self.dir, "installed-bad")
        rc, log = self.run_fetch(m, dest)
        self.assertEqual(rc, 1, log)
        self.assertIn("MISMATCH", log)
        self.assertFalse(os.path.exists(os.path.join(dest, "alpha-pack.zip")),
                         "a file whose hash does not match is never installed")


class TestManifestSlots(BundleCase):
    def test_update_fills_sha_size_magnet_and_keeps_the_mirror_tbd(self):
        rc, log = self.build("--update-manifest", self.manifest)
        self.assertEqual(rc, 0, log)
        doc = json.load(open(self.manifest, encoding="utf-8"))
        by = {e["id"]: e for e in doc["files"]}
        magnet = open(os.path.join(self.out, "rhygtasa-deps-0-test.magnet.txt"), encoding="utf-8").read().strip()
        top, info = torrent_info(self.torrent_path())
        self.assertIn("urn:btih:" + info_hash(top, info), magnet)
        for eid in ("alpha-pack", "beta-pack", "gamma-pack"):
            e = by[eid]
            self.assertEqual(e["sha256"], self.staged_sha(e["name"]), eid)
            self.assertEqual(e["size"], self.hosted_sizes[e["name"]], eid)
            urls = {u["kind"]: u["url"] for u in e["urls"]}
            self.assertEqual(urls.get("magnet"), magnet, eid)
            self.assertEqual(urls["mirror"], "TBD", "%s: the https mirror slot stays TBD" % eid)
        # what we may not host is untouched
        for eid in ("forbidden-pack", "ask-pack", "missing-pack"):
            self.assertEqual(by[eid]["sha256"], "TBD", eid)
            self.assertNotIn("magnet", {u["kind"] for u in by[eid]["urls"]}, eid)
        # and the manifest is still readable by the one reader
        p = subprocess.run(["bash", FETCH, self.manifest, "--dump"], capture_output=True,
                           text=True, timeout=60, cwd=REPO)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)

    def test_write_manifest_targets_the_manifest_that_was_passed(self):
        copy = os.path.join(self.dir, "copy.json")
        shutil.copy(self.manifest, copy)
        rc, log = self.run_mkrel(self.out, "--stage", self.stage, "--manifest", copy,
                                "--write-manifest", "--quiet")
        self.assertEqual(rc, 0, log)
        self.assertIn("MANIFEST", log)
        by = {e["id"]: e for e in json.load(open(copy, encoding="utf-8"))["files"]}
        self.assertEqual(by["alpha-pack"]["sha256"], self.staged_sha("alpha-pack.zip"))
        self.assertEqual({u["kind"]: u["url"] for u in by["alpha-pack"]["urls"]}["mirror"], "TBD")

    def test_update_refuses_to_write_invalid_json(self):
        # a manifest whose entry lost its urls array must be left alone, not half-edited
        doc = json.load(open(self.manifest, encoding="utf-8"))
        doc["files"][0].pop("urls", None)
        broken = os.path.join(self.dir, "no-urls.json")
        json.dump(doc, open(broken, "w", encoding="utf-8"), indent=2)
        before = open(broken, encoding="utf-8").read()
        rc, log = self.build("--update-manifest", broken)
        self.assertIn(rc, (1, 2), log)
        self.assertEqual(open(broken, encoding="utf-8").read(), before, "the manifest was half-edited")


class TestUsage(BundleCase):
    def test_outside_build_needs_force(self):
        victim = os.path.join(REPO, "deps", "bundle-test")
        try:
            rc, log = self.run_mkrel(victim, "--manifest", self.manifest, "--stage", self.stage)
            self.assertEqual(rc, 2, log)
            self.assertIn("not under build/", log)
            self.assertFalse(os.path.isdir(os.path.join(victim, "rhygtasa-deps-0-test")))
        finally:
            shutil.rmtree(victim, ignore_errors=True)

    def test_unknown_option_and_missing_manifest(self):
        rc, log = self.run_mkrel(self.out, "--nope")
        self.assertEqual(rc, 2, log)
        rc, log = self.run_mkrel(self.out, "--manifest", os.path.join(self.dir, "gone.json"))
        self.assertEqual(rc, 2, log)
        rc, log = self.run_mkrel()
        self.assertEqual(rc, 2, log)
        self.assertIn("usage", log)

    def test_piece_length_must_be_a_power_of_two(self):
        rc, log = self.build("--piece-length", 40000)
        self.assertEqual(rc, 2, log)
        self.assertIn("power of two", log)

    def test_a_named_piece_length_is_the_one_the_torrent_uses(self):
        rc, log = self.build("--piece-length", 65536)
        self.assertEqual(rc, 0, log)
        _top, info = torrent_info(self.torrent_path())
        self.assertEqual(info[b"piece length"], 65536)


class TestRealManifestShape(unittest.TestCase):
    """deps/manifest.json + deps/trackers.txt as shipped: the bundle slots exist and
    the licence rules still hold, so make-release.sh has something honest to fill."""

    def setUp(self):
        self.doc = json.load(open(os.path.join(REPO, "deps", "manifest.json"), encoding="utf-8"))

    def test_bundle_block_and_magnet_slots(self):
        self.assertIn("bundle", json.dumps(self.doc.get("hosts", {})))
        by = {e["id"]: e for e in self.doc["files"]}
        for e in self.doc["files"]:
            kinds = {u["kind"] for u in e["urls"]}
            if e["redistribute"] in ("yes", "ask"):
                self.assertIn("magnet", kinds, "%s is ours to host but has no magnet slot" % e["id"])
            else:
                self.assertNotIn("magnet", kinds, "%s forbids re-upload: no magnet slot" % e["id"])
            for u in e["urls"]:
                if u["kind"] in ("mirror", "torrent", "magnet"):
                    self.assertEqual(u["url"], "TBD", "%s: no host yet, so %s must stay TBD" % (e["id"], u["kind"]))
        self.assertIn("gtasa-remix-pack", by)

    def test_trackers_file_ships_verified_trackers(self):
        ts = read_trackers()
        self.assertGreaterEqual(len(ts), 1)
        for t in ts:
            self.assertTrue(t.endswith("/announce"), t)


if __name__ == "__main__":
    unittest.main(verbosity=2)
