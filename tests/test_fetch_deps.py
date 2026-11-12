"""tools/fetch-deps.sh — the fetcher behind "we host everything the user would
otherwise download by hand" (docs/CHARACTERS.md section 9, deps/manifest.json).

Everything here runs against a local http.server on 127.0.0.1 in
REPO/build/tmp: no real host is contacted, and the real manifest's TBD urls are
only ever *validated*, never fetched.

What must not regress, and why each case exists:
  * a file is only ever named after its sha256 matched - a half download is
    <name>.part, a wrong download is <name>.bad-sha256, and nothing that failed
    reaches the game folder (apply.sh unpacks only what --plan lists);
  * resume works, because these are 20-200 MB archives on a home connection;
  * a missing aria2c is a clear, actionable error for magnet/torrent urls and
    must not break the https path (aria2c is NOT installed on the dev box, so
    the curl fallback is the path actually exercised here);
  * a manifest typo (bad sha, missing key, duplicate id, absolute target) stops
    the run with exit 2 instead of downloading the wrong thing.

Run: python3 -m pytest -q tests/test_fetch_deps.py   (plain unittest works too)
"""
import hashlib
import http.server
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(REPO, "tools", "fetch-deps.sh")
MANIFEST = os.path.join(REPO, "deps", "manifest.json")
TMPROOT = os.path.join(REPO, "build", "tmp")
US = "\x1f"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# --------------------------------------------------------------- fake host ----
class Handler(http.server.BaseHTTPRequestHandler):
    """Serves Serve.files[path]; Serve.behaviour[path] = ok | flaky | norange."""
    files = {}
    behaviour = {}
    trunc_left = 0          # how many more responses are cut short
    hits = []               # (path, Range header or None)

    def log_message(self, *a):       # keep the test output readable
        pass

    def _send(self, body, start=0, partial=False):
        self.send_response(206 if partial else 200)
        if partial:
            self.send_header("Content-Range",
                             "bytes %d-%d/%d" % (start, len(body) - 1, len(body)))
        self.send_header("Content-Length", str(len(body) - start))
        self.end_headers()
        self.wfile.write(body[start:])

    def do_GET(self):
        path = self.path.lstrip("/")
        rng = self.headers.get("Range")
        Handler.hits.append((path, rng))
        if path not in Handler.files:
            self.send_error(404)
            return
        body = Handler.files[path]
        beh = Handler.behaviour.get(path, "ok")
        cut = Handler.trunc_left > 0
        if cut:
            Handler.trunc_left -= 1
        if rng and beh != "norange":
            start = int(rng.split("=", 1)[1].split("-")[0])
            if start >= len(body):
                self.send_error(416)
                return
            self.send_response(206)
            self.send_header("Content-Range",
                             "bytes %d-%d/%d" % (start, len(body) - 1, len(body)))
            self.send_header("Content-Length", str((len(body) - start) // 2 if cut else len(body) - start))
            self.end_headers()
            chunk = body[start:]
            self.wfile.write(chunk[:len(chunk) // 2] if cut else chunk)
            if cut:
                self.close_connection = True
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            return
        if rng and beh == "norange":      # refuse to resume: curl exits 33
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(200)
        if beh != "norange":
            self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body[:len(body) // 2] if cut else body)
        if cut:
            self.close_connection = True
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


class Server:
    def __enter__(self):
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:%d" % self.httpd.server_address[1]
        return self

    def __exit__(self, *a):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=5)


def entry(eid="pack", name="pack.zip", group="characters", target="modloader/Pack",
          sha="TBD", urls=(("mirror", "TBD"),), **extra):
    e = {
        "id": eid, "group": group, "name": name, "size": None, "sha256": sha,
        "source_page": "https://authors.example/%s" % eid, "author": "The Author",
        "license": "free to use, no re-upload", "redistribute": "no", "target": target,
        "unpack": "auto", "urls": [{"kind": k, "url": u} for k, u in urls],
    }
    e.update(extra)
    return e


class FetchCase(unittest.TestCase):
    def setUp(self):
        os.makedirs(TMPROOT, exist_ok=True)
        self.dir = tempfile.mkdtemp(prefix="fetch-deps-test-", dir=TMPROOT)
        self.dest = os.path.join(self.dir, "deps")
        Handler.files, Handler.behaviour, Handler.hits = {}, {}, []
        Handler.trunc_left = 0

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    # helpers ------------------------------------------------------------
    def manifest(self, entries, path=None):
        p = path or os.path.join(self.dir, "manifest.json")
        with open(p, "w", encoding="utf-8") as f:
            json.dump({"schema": 1, "files": entries}, f, indent=1)
        return p

    def broken_manifest(self, text):
        p = os.path.join(self.dir, "broken.json")
        with open(p, "w", encoding="utf-8") as f:
            f.write(text)
        return p

    def fetch(self, manifest, *args, env=None):
        return self.runscript(manifest, self.dest, *args, env=env)

    def runscript(self, *args, env=None):
        e = dict(os.environ)
        e.update(env or {})
        p = subprocess.run(["bash", SCRIPT, *args], capture_output=True, text=True,
                           timeout=120, env=e, cwd=REPO)
        return p.returncode, p.stdout, p.stderr

    def out(self, rc_out):
        return rc_out[1] + rc_out[2]

    def serve(self, name, data, behaviour="ok"):
        Handler.files[name] = data
        Handler.behaviour[name] = behaviour
        return name


class TestHappyPath(FetchCase):
    def test_fetches_verifies_and_names_the_file(self):
        data = b"the pack, 120 MB in real life\n" * 500
        with Server() as srv:
            m = self.manifest([entry(sha=sha256(data), urls=[("mirror", "%s/%s" % (srv.base, "pack.zip"))])])
            Handler.files["pack.zip"] = data
            rc, out, err = self.fetch(m)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("OK     pack", out)
        self.assertEqual(open(os.path.join(self.dest, "pack.zip"), "rb").read(), data)
        self.assertFalse(os.path.exists(os.path.join(self.dest, "pack.zip.part")))
        self.assertFalse(os.path.exists(os.path.join(self.dest, "pack.zip.bad-sha256")))

    def test_second_run_is_idempotent_and_offline(self):
        data = b"stable bytes"
        with Server() as srv:
            self.serve("pack.zip", data)
            m = self.manifest([entry(sha=sha256(data), urls=[("mirror", "%s/pack.zip" % srv.base)])])
            rc1, out1, _ = self.fetch(m)
            hits_after_first = len(Handler.hits)
            rc2, out2, err2 = self.fetch(m)
        self.assertEqual((rc1, rc2), (0, 0))
        self.assertIn("already in DEST", out2)
        self.assertEqual(len(Handler.hits), hits_after_first, "the second run touched the network")

    def test_plan_lists_only_verified_entries_with_a_target(self):
        data = b"pack bytes"
        with Server() as srv:
            self.serve("pack.zip", data)
            m = self.manifest([
                entry(eid="pack", sha=sha256(data), urls=[("mirror", "%s/pack.zip" % srv.base)]),
                entry(eid="loose", name="loose.bin", target="", sha=sha256(data),
                      urls=[("mirror", "%s/pack.zip" % srv.base)]),
            ])
            self.fetch(m)
            rc, out, err = self.fetch(m, "--verify-only", "--plan", "--quiet")
        self.assertEqual(rc, 0, out + err)
        lines = [l for l in out.splitlines() if l.strip()]
        self.assertEqual(lines, ["pack.zip%smodloader/Pack%sauto" % (US, US)],
                         "plan must carry the target for the installer and skip target-less entries")

    def test_one_failing_entry_does_not_stop_the_others(self):
        data = b"good pack"
        with Server() as srv:
            self.serve("good.zip", data)
            m = self.manifest([
                entry(eid="bad", name="bad.zip", sha=sha256(data), urls=[("mirror", "%s/nope.zip" % srv.base)]),
                entry(eid="good", name="good.zip", sha=sha256(data), urls=[("mirror", "%s/good.zip" % srv.base)]),
            ])
            rc, out, err = self.fetch(m)
        self.assertEqual(rc, 1)
        self.assertIn("FAILED: bad", err)
        self.assertTrue(os.path.exists(os.path.join(self.dest, "good.zip")))


class TestShaMismatch(FetchCase):
    def test_wrong_content_is_never_installed(self):
        served = b"mirror served the wrong build"
        with Server() as srv:
            self.serve("pack.zip", served)
            m = self.manifest([entry(sha=sha256(b"what we meant to host"),
                                     urls=[("mirror", "%s/pack.zip" % srv.base)])])
            rc, out, err = self.fetch(m)
        text = out + err
        self.assertEqual(rc, 1)
        self.assertIn("MISMATCH", text)
        self.assertFalse(os.path.exists(os.path.join(self.dest, "pack.zip")),
                         "a file whose sha256 does not match must not be named as the real one")
        bad = os.path.join(self.dest, "pack.zip.bad-sha256")
        self.assertTrue(os.path.exists(bad), "the wrong file should be kept for whoever runs the mirror")
        self.assertEqual(open(bad, "rb").read(), served)

    def test_a_stale_wrong_file_is_moved_aside_and_retried(self):
        good = b"the right bytes"
        with Server() as srv:
            self.serve("pack.zip", good)
            os.makedirs(self.dest)
            with open(os.path.join(self.dest, "pack.zip"), "wb") as f:
                f.write(b"an old download of something else")
            m = self.manifest([entry(sha=sha256(good), urls=[("mirror", "%s/pack.zip" % srv.base)])])
            rc, out, err = self.fetch(m)
        text = out + err
        self.assertEqual(rc, 0, text)
        self.assertIn("WRONG sha256", text)
        self.assertEqual(open(os.path.join(self.dest, "pack.zip"), "rb").read(), good)
        self.assertEqual(open(os.path.join(self.dest, "pack.zip.bad-sha256"), "rb").read(),
                         b"an old download of something else")

    def test_sha256_TBD_refuses_to_fetch_unverified_bytes(self):
        with Server() as srv:
            self.serve("pack.zip", b"unverified")
            m = self.manifest([entry(sha="TBD", urls=[("mirror", "%s/pack.zip" % srv.base)])])
            rc, out, err = self.fetch(m)
            self.assertEqual(rc, 1)
            self.assertIn("refusing to fetch unverified", out + err)
            self.assertEqual(Handler.hits, [], "an unverified entry must not even be requested")
            rc, out, err = self.fetch(m, "--insecure")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("UNVERIFIED", out + err)
        self.assertTrue(os.path.exists(os.path.join(self.dest, "pack.zip")))


class TestResume(FetchCase):
    def test_a_cut_connection_resumes_on_the_next_run(self):
        data = b"A" * 4096 + b"B" * 4096
        with Server() as srv:
            self.serve("pack.zip", data)
            Handler.trunc_left = 20                      # every response in run 1 is cut short
            m = self.manifest([entry(sha=sha256(data), urls=[("mirror", "%s/pack.zip" % srv.base)])])
            rc1, out1, err1 = self.fetch(m)
            part = os.path.join(self.dest, "pack.zip.part")
            self.assertEqual(rc1, 1, out1 + err1)
            self.assertTrue(os.path.exists(part), "the partial file must be kept for the resume")
            self.assertFalse(os.path.exists(os.path.join(self.dest, "pack.zip")))
            self.assertIn("resumes it", err1)
            Handler.trunc_left = 0                       # host behaves now
            rc2, out2, err2 = self.fetch(m)
        text = out2 + err2
        self.assertEqual(rc2, 0, text)
        self.assertEqual(open(os.path.join(self.dest, "pack.zip"), "rb").read(), data)
        self.assertTrue(any(r is not None for _, r in Handler.hits),
                        "nothing ever asked the server for a byte range - that was not a resume")
        self.assertFalse(os.path.exists(part))

    def test_a_server_that_refuses_ranges_starts_over(self):
        data = b"no range support here" * 300
        with Server() as srv:
            self.serve("pack.zip", data, behaviour="norange")
            os.makedirs(self.dest)
            with open(os.path.join(self.dest, "pack.zip.part"), "wb") as f:
                f.write(b"half of something")
            m = self.manifest([entry(sha=sha256(data), urls=[("mirror", "%s/pack.zip" % srv.base)])])
            rc, out, err = self.fetch(m)
        text = out + err
        self.assertEqual(rc, 0, text)
        self.assertIn("does not support resume", text)
        self.assertEqual(open(os.path.join(self.dest, "pack.zip"), "rb").read(), data)


class TestMissingTool(FetchCase):
    """aria2c is not installed on this box, so these are the real conditions."""

    def test_magnet_without_aria2c_fails_with_an_actionable_error(self):
        m = self.manifest([entry(sha=sha256(b"would-be the torrent"),
                                     urls=[("magnet", "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567")])])
        rc, out, err = self.fetch(m, env={"RHYS_ARIA2C": os.path.join(self.dir, "no-such-aria2c")})
        text = out + err
        self.assertEqual(rc, 1)
        self.assertIn("needs aria2c", text)
        self.assertIn("apt install aria2", text)
        self.assertFalse(os.path.exists(os.path.join(self.dest, "pack.zip")))

    def test_https_still_works_without_aria2c(self):
        data = b"curl fallback"
        with Server() as srv:
            self.serve("pack.zip", data)
            m = self.manifest([entry(sha=sha256(data), urls=[("mirror", "%s/pack.zip" % srv.base)])])
            rc, out, err = self.fetch(m, env={"RHYS_ARIA2C": os.path.join(self.dir, "no-such-aria2c")})
        self.assertEqual(rc, 0, out + err)
        self.assertIn("via curl", out)
        self.assertEqual(open(os.path.join(self.dest, "pack.zip"), "rb").read(), data)

    def test_a_broken_aria2c_falls_back_to_curl(self):
        data = b"aria2c said no"
        fake = os.path.join(self.dir, "fake-aria2c")
        with open(fake, "w") as f:
            f.write("#!/bin/sh\nexit 1\n")
        os.chmod(fake, 0o755)
        with Server() as srv:
            self.serve("pack.zip", data)
            m = self.manifest([entry(sha=sha256(data), urls=[("mirror", "%s/pack.zip" % srv.base)])])
            rc, out, err = self.fetch(m, env={"RHYS_ARIA2C": fake})
        text = out + err
        self.assertEqual(rc, 0, text)
        self.assertIn("falling back to curl", text)
        self.assertEqual(open(os.path.join(self.dest, "pack.zip"), "rb").read(), data)


class TestManualAndSelection(FetchCase):
    def test_no_host_yet_is_a_manual_line_not_a_failure(self):
        m = self.manifest([entry()])
        rc, out, err = self.fetch(m)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("MANUAL pack", out)
        self.assertIn("https://authors.example/pack", out)
        self.assertIn("nothing hosted yet", out)

    def test_source_pages_are_never_downloaded(self):
        # a "source" url is the author's page: fetching it would save HTML under
        # the name of an archive, which is the worst possible silent failure
        with Server() as srv:
            self.serve("page", b"<html>the author's page</html>")
            m = self.manifest([entry(sha=sha256(b"whatever"), urls=[("source", "%s/page" % srv.base)])])
            rc, out, err = self.fetch(m)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("MANUAL", out)
        self.assertEqual(Handler.hits, [])

    def test_no_torrent_skips_magnet_and_torrent_urls(self):
        m = self.manifest([entry(sha=sha256(b"x"), urls=[("torrent", "TBD"),
                                       ("magnet", "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567")])])
        rc, out, _ = self.fetch(m, "--no-torrent")
        self.assertEqual(rc, 0)
        self.assertIn("MANUAL", out)

    def test_only_and_group_select_entries(self):
        with Server() as srv:
            self.serve("a.zip", b"a")
            m = self.manifest([
                entry(eid="a", name="a.zip", group="characters", sha=sha256(b"a"),
                      urls=[("mirror", "%s/a.zip" % srv.base)]),
                entry(eid="b", name="b.zip", group="base", sha=sha256(b"b"),
                      urls=[("mirror", "%s/a.zip" % srv.base)]),
            ])
            rc, out, _ = self.fetch(m, "--only", "a")
            self.assertEqual(rc, 0)
            self.assertIn("not selected (", out)
            rc, out, err = self.fetch(m, "--group", "base")
        self.assertEqual(rc, 1, out + err)    # b is served a.zip's bytes: hash mismatch
        self.assertIn("FAILED: b", out + err)

    def test_verify_only_reports_without_network(self):
        data = b"already there"
        os.makedirs(self.dest)
        with open(os.path.join(self.dest, "pack.zip"), "wb") as f:
            f.write(data)
        m = self.manifest([entry(sha=sha256(data), urls=[("mirror", "http://127.0.0.1:1/nope")])])
        rc, out, err = self.fetch(m, "--verify-only")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("verified", out)
        self.assertEqual(Handler.hits, [])

    def test_list_prints_the_manifest(self):
        m = self.manifest([entry(eid="x", name="x.zip", sha="TBD")])
        rc, out, _ = self.fetch(m, "--list")
        self.assertEqual(rc, 0)
        self.assertIn("x\tcharacters\tx.zip\tsha256:TBD\tmodloader/Pack", out)


class TestManifestValidation(FetchCase):
    """A manifest typo must stop the run (exit 2), not download the wrong file."""

    def check(self, entries, want):
        rc, out, err = self.fetch(self.manifest(entries))
        self.assertEqual(rc, 2, out + err)
        self.assertIn(want, err)

    def test_bad_json(self):
        rc, _, err = self.fetch(self.broken_manifest('{"schema": 1, "files": ['))
        self.assertEqual(rc, 2)
        self.assertIn("not valid JSON", err)

    def test_missing_required_key(self):
        e = entry()
        del e["sha256"]
        self.check([e], "missing required key 'sha256'")

    def test_all_required_keys_are_named_in_the_reader(self):
        import re
        src = open(SCRIPT, encoding="utf-8").read()
        m = re.search(r'REQ = \[([^\]]+)\]', src)
        self.assertIsNotNone(m, "the reader lost its required-key list")
        keys = [k.strip().strip('"') for k in m.group(1).split(",")]
        for k in ("name", "size", "sha256", "source_page", "author", "license", "target", "urls"):
            self.assertIn(k, keys)

    def test_duplicate_id(self):
        self.check([entry(), entry()], "duplicate id")

    def test_short_sha256(self):
        self.check([entry(sha="deadbeef")], "64 hex")

    def test_whitespace_in_url(self):
        self.check([entry(urls=[("mirror", "http://x/Proper Player Retex.zip")])], "whitespace")

    def test_absolute_or_escaping_target(self):
        self.check([entry(target="/etc")], "relative")
        self.check([entry(target="modloader/../data")], "relative")

    def test_unknown_url_kind_and_schema(self):
        self.check([entry(urls=[("ftp", "ftp://x/y")])], "url kind")
        rc, _, err = self.fetch(self.broken_manifest(json.dumps({"schema": 2, "files": [entry()]})))
        self.assertEqual(rc, 2)
        self.assertIn("unsupported schema", err)

    def test_requires_unknown_id(self):
        self.check([entry(eid="a", requires=["nope"])], "requires unknown id")

    def test_usage_errors(self):
        rc, _, err = self.runscript()                       # no manifest, no DEST
        self.assertEqual(rc, 2)
        self.assertIn("usage", err)
        rc, _, err = self.runscript(MANIFEST, self.dest, "--nope")
        self.assertEqual(rc, 2)
        self.assertIn("unknown option", err)
        rc, _, err = self.runscript(os.path.join(self.dir, "missing.json"), self.dest)
        self.assertEqual(rc, 2)
        self.assertIn("no such manifest", err)


class TestRealManifest(unittest.TestCase):
    """deps/manifest.json is the thing apply.sh ships - it has to be valid JSON
    with every required key, and it has to cover what the docs say a user must
    download by hand (docs/CHARACTERS.md section 7, docs/VEGETATION.md)."""

    REQUIRED = ["id", "name", "size", "sha256", "source_page", "author", "license",
                "redistribute", "target", "urls"]

    def setUp(self):
        with open(MANIFEST, encoding="utf-8") as f:
            self.doc = json.load(f)

    def ids(self):
        return [e["id"] for e in self.doc["files"]]

    def test_schema_and_unique_ids(self):
        self.assertEqual(self.doc["schema"], 1)
        self.assertEqual(len(self.ids()), len(set(self.ids())))

    def test_required_keys_everywhere(self):
        for e in self.doc["files"]:
            for k in self.REQUIRED:
                self.assertIn(k, e, "%s is missing %s" % (e.get("id"), k))

    def test_field_shapes(self):
        for e in self.doc["files"]:
            self.assertRegex(e["id"], r"^[a-z0-9][a-z0-9._-]*$")
            self.assertTrue(e["name"])
            self.assertNotIn("/", e["name"])
            self.assertTrue(e["sha256"] == "TBD" or len(e["sha256"]) == 64, e["id"])
            self.assertTrue(e["source_page"] == "TBD" or e["source_page"].startswith("https://"), e["id"])
            self.assertTrue(e["author"].strip() and e["license"].strip(), e["id"])
            self.assertIn(e["redistribute"], ("yes", "no", "ask"), e["id"])
            self.assertFalse(e["target"].startswith("/"), e["id"])
            self.assertNotIn("..", e["target"].split("/"), e["id"])
            self.assertTrue(e["urls"], e["id"])
            for u in e["urls"]:
                self.assertIn(u["kind"], ("mirror", "torrent", "magnet", "direct", "source"), e["id"])
                self.assertFalse(any(c.isspace() for c in u["url"]), e["id"])
            self.assertTrue(e["size"] is None or (isinstance(e["size"], int) and e["size"] > 0), e["id"])

    def test_the_hand_download_list_is_covered(self):
        want = {"proper-player-retex", "proper-characters-retex-hd", "pedfuncs",
                "emergency-peds-vary", "criminal-peds-vary"}      # docs/CHARACTERS.md section 7
        self.assertTrue(want <= set(self.ids()), sorted(want - set(self.ids())))

    def test_the_vegetation_pack_and_its_requirements_are_covered(self):
        ids = set(self.ids())
        self.assertIn("v-vegetation-mix", ids)                     # docs/VEGETATION.md
        by = {e["id"]: e for e in self.doc["files"]}
        self.assertIn("essentials-pack", by["v-vegetation-mix"]["requires"])
        self.assertIn("proper-fixes", by["v-vegetation-mix"]["requires"])

    def test_mods_that_forbid_reupload_are_not_marked_mirrordable(self):
        for e in self.doc["files"]:
            if "MixMods" in e["license"] or "LibertyCity" in e["license"]:
                self.assertEqual(e["redistribute"], "no", e["id"])
                for u in e["urls"]:
                    if u["kind"] in ("mirror", "torrent", "magnet"):
                        self.assertEqual(u["url"], "TBD", "%s mirrors a no-reupload mod" % e["id"])

    def test_modloader_targets_are_one_folder_per_pack(self):
        targets = [e["target"] for e in self.doc["files"] if e["target"]]
        self.assertEqual(len(targets), len(set(targets)))
        for t in targets:
            self.assertTrue(t.startswith("modloader/"), t)

    def test_the_fetcher_accepts_it(self):
        p = subprocess.run(["bash", SCRIPT, MANIFEST, os.path.join(TMPROOT, "manifest-check"),
                            "--list", "--quiet"], capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)


if __name__ == "__main__":
    unittest.main()
