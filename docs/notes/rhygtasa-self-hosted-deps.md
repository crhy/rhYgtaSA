---
name: rhygtasa-self-hosted-deps
description: rhYgtaSA must install for everyone in two easy steps; anything rhY has to download by hand we host ourselves (torrent or similar).
metadata:
  type: project
---
rhY (2026-11-11): every dependency that needs a manual download (Cloudflare/captcha sites like mixmods, sharemods) must be hosted by
us - a torrent or similar - so the installer fetches everything itself. End users: downgrade to 1.0, run our installer, done.

**Why:** rhY wants everyone to be able to install and run the mod very easily.
**How to apply:** in research/staging tasks (e.g. s06), list hand-download files as "to self-host", note license/credit; design the
installer around one fetch from our host. Related: [[gtasa-realism-modding]], [[keep-strata-queue-deep]].
