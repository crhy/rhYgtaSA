# Self-hosted dependencies: the product rule

Written 2026-11-11, restated here on 2026-11-12 so the rule lives in the repo rather than in a
session note.

**The rule.** A player should have to do exactly two things: downgrade the game to 1.0 US, and run
one installer command. Anything in that path that today needs a hand download - a MixMods page
behind Cloudflare, a LibertyCity login, a sharemods link - is something **we** fetch and host
ourselves (HTTPS plus a torrent), with the author credited and the licence recorded.

**Why.** Half of this stack lives on sites that a script cannot fetch from. If those files are not
ours to serve, the two-step install is a lie: the installer stops at the first pack and the user is
told to go click around. Hosting them is what makes `apply.sh --with-deps` mean something.

**How it is implemented today.**

- [../../deps/manifest.json](../../deps/manifest.json) is the single list: every dependency with its
  upstream page, exact file name, sha256, size, version and licence (`redistribute: yes/no`).
- `tools/fetch-deps.sh` only ever installs a file whose sha256 matches the manifest, and prints the
  author's page instead of downloading when we do not host it yet. Nothing unverified reaches the
  game folder.
- `tools/make-release.sh` builds the folder we would serve - archives, `SHA256SUMS`, one torrent and
  its magnet, with the info hash re-derived from the bytes on disk - and collects **only** packs
  whose licence allows re-upload.
- [../CHARACTERS.md](../CHARACTERS.md) sections 7 and 12 record, pack by pack, what was fetched,
  what is blocked and by what, and the hand-download URL for a human to finish.

**The limit, stated honestly.** MixMods and LibertyCity files are free to use but not to
re-upload, so those stay linked rather than mirrored; the packs we can legally host are the ones
with a permissive licence. As of 2026-11-12 nothing is staged yet, so `--with-deps` prints pages
instead of fetching, and `tools/make-release.sh` reports every pack as not staged.
