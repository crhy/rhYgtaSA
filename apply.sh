#!/bin/bash
# Apply the rhYgtaSA Linux/Proton fixes to a GTA SA RTX install that already has GTASA-Remix (Hemry81) + RTX Remix 1.6.1 in it.
# Usage: ./apply.sh /path/to/GTASA-RTX [--with-deps [--deps-group characters]] [--water remix-ref]
#   --with-deps     also fetch the mods a user would otherwise download by hand, from OUR hosts
#                   (deps/manifest.json -> tools/fetch-deps.sh), and unpack the ones that verify into
#                   <game>/modloader/<pack>/. Off by default until the hosts exist; flip WITH_DEPS=1
#                   here once they do. --no-deps forces it off.
#   --deps-group    which manifest group to install with --with-deps (default: characters)
#   --water V       which water material to install (remix-ref|soft|realistic|calm|pack,
#                   default remix-ref - the n07 research variant). --no-water leaves the pack's
#                   material exactly as it is. n13: this used to be a step that did not exist,
#                   which is why nobody ever got the new water.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
R=""; WITH_DEPS=0; DEPS_GROUP=characters; WATER=remix-ref; WATER_SET=0
while [ $# -gt 0 ]; do
  case "$1" in
    --with-deps)  WITH_DEPS=1; shift ;;
    --no-deps)    WITH_DEPS=0; shift ;;
    --water)      [ $# -ge 2 ] || { echo "usage: apply.sh /path/to/GTASA-RTX [--water remix-ref|soft|realistic|calm|pack|--no-water]"; exit 1; }
                  WATER="$2"; WATER_SET=1; shift 2 ;;
    --no-water)   WATER=none; WATER_SET=1; shift ;;
    --deps-group) [ $# -ge 2 ] || { echo "usage: apply.sh /path/to/GTASA-RTX [--with-deps [--deps-group characters]]"; exit 1; }
                  DEPS_GROUP="$2"; shift 2 ;;
    -h|--help)    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)            R="$1"; shift ;;
  esac
done
[ -n "$R" ] || { echo "usage: apply.sh /path/to/GTASA-RTX [--with-deps [--deps-group characters]] [--water remix-ref|--no-water]"; exit 1; }
case " $WATER " in
  " remix-ref "|" soft "|" realistic "|" calm "|" pack "|" none ") ;;
  *) echo "apply.sh: --water must be remix-ref|soft|realistic|calm|pack (or --no-water), got '$WATER'"; exit 1 ;;
esac
[ -f "$R/rtx.conf" ] && [ -d "$R/rtx-remix/mods/gameReadyAssets" ] && [ -d "$R/moonloader" ] || { echo "not a GTASA-Remix install: $R"; exit 1; }
# Selftest guard: scripts/installer-selftest.sh exports RHYREMIX_INSTALLER_SANDBOX=1
# and puts a marker in its scratch install, so a bug in that test can never point
# the installer at a real game folder.
if [ "${RHYREMIX_INSTALLER_SANDBOX:-}" = 1 ] && [ ! -f "$R/.fake-install-marker" ]; then
  echo "refusing to install into $R: sandbox mode needs $R/.fake-install-marker"; exit 1
fi
# One backup folder, filled once: re-running apply.sh must not overwrite the
# pristine pre-patch copy with the file it just patched (a timestamped folder per
# run did exactly that - the second run's "backup" was already-patched content).
B="$R/rhYgtaSA-backup"
echo "== backing up files the patch touches to $B (once; an existing backup is kept)"
# if/then, not `[ -f x ] && cp`: inside `grep ... | while`, a failing last command
# is the pipeline's status and `set -e` would abort the install over one absent
# optional file. Same reason the loop reads a process substitution, not a pipe.
while IFS= read -r f; do
  if [ -f "$R/$f" ] && [ ! -f "$B/$f" ]; then mkdir -p "$B/$(dirname "$f")"; cp -p "$R/$f" "$B/$f"; fi
done < <(grep '^+++ ' "$HERE/patches/gtasa-remix-linux.patch" | sed -E 's/^\+\+\+ "?b\///; s/"?\t.*$//; s/"$//')
echo "== patching GTASA-Remix files (scripts, light USDs, rtx.conf)"
# File by file, and dry-run first in both directions. One big `patch --forward`
# run cannot tell the three cases apart: it aborts the install (set -e) and
# leaves .rej files behind when a file has drifted - a user who edited rtx.conf,
# or who re-runs apply.sh after tools/strip_pack_lamp_lights.py took the pack's
# lamp lights out, gets a half-installed folder and no explanation. So: applies
# cleanly -> apply it; already applied -> skip (that is what makes a re-run a
# no-op); matches neither -> leave the file alone and say which one.
TMP="$HERE/build/tmp/apply-patch-chunks"; rm -rf "$TMP"; mkdir -p "$TMP"
awk -v out="$TMP" '/^diff -ruN /{n++; f=sprintf("%s/%03d.patch", out, n)} {if (f) print > f}' "$HERE/patches/gtasa-remix-linux.patch"
n_applied=0; n_already=0; n_diverged=0; n_missing=0
for c in "$TMP"/*.patch; do
  f=$(grep -m1 '^+++ ' "$c" | sed -E 's/^\+\+\+ "?b\///; s/"?\t.*$//; s/"$//')
  if [ ! -f "$R/$f" ]; then echo "   WARN: $f is not in $R - skipped"; n_missing=$((n_missing+1)); continue; fi
  if patch -p1 -d "$R" --dry-run --forward --batch --binary < "$c" >/dev/null 2>&1; then
    if patch -p1 -d "$R" --forward --batch --binary < "$c" >/dev/null 2>&1; then
      n_applied=$((n_applied+1))
    else
      echo "   WARN: $f should have patched but did not - left alone"; n_diverged=$((n_diverged+1))
    fi
  elif patch -p1 -d "$R" --dry-run --reverse --batch --binary < "$c" >/dev/null 2>&1; then
    n_already=$((n_already+1))
  else
    echo "   WARN: $f matches neither the pre- nor the post-patch text - left alone (see patches/gtasa-remix-linux.patch)"
    n_diverged=$((n_diverged+1))
  fi
done
rm -rf "$TMP"
echo "   $n_applied patched, $n_already already applied, $n_diverged diverged, $n_missing missing"
echo "== Large Address Aware on gta-sa.exe (the downgrader's exe is not LAA)"
# A 32-bit process without IMAGE_FILE_LARGE_ADDRESS_AWARE gets ~2 GB of address
# space from Wine/Proton, and in this build that one process also carries the
# Remix bridge client (d3d9.dll in the game folder) plus a 1900 MB streaming cap
# - docs/VEGETATION.md cause 2. laa.py keeps a one-time gta-sa.exe.noLAA-backup.
# A failure is a warning, not an abort: the rest of the install is still valid.
if [ -f "$R/gta-sa.exe" ]; then
  python3 "$HERE/tools/laa.py" "$R/gta-sa.exe" --set \
    || echo "WARN: could not set Large Address Aware on $R/gta-sa.exe - do it by hand: python3 $HERE/tools/laa.py $R/gta-sa.exe --set"
else
  echo "WARN: no $R/gta-sa.exe in $R - skipped the Large Address Aware step"
fi
echo "== street lamps: rhyremix.asi lights them, so the pack's duplicate lamp lights go"
python3 "$HERE/tools/strip_pack_lamp_lights.py" "$R"
echo "== water: the n07 research variant by default (n13); --water <variant> picks another, --no-water leaves the pack's"
# Why this step did not exist until n13 (rhY: "make sure we get the new water by default"): n07 built
# remix-ref and s32 built soft, but NOTHING installed either one - apply.sh never called water.sh,
# the launcher only called it when RHYS_WATER was set, and water.sh printed the rtx.conf lines that
# go with a variant instead of writing them. Measured on the live install: `soft` water, and an
# rtx.conf with none of the animated-water keys, so the runtime's own water path never ran.
# `default` is the same path launch-rtx.sh takes: install remix-ref, apply its rtx.conf keys (with
# backups in <game>/.rhys-water/), and never overwrite a variant recorded in <game>/.rhys-water/state
# as an explicit choice - so a re-run after you picked soft or pack on purpose changes nothing.
if [ "$WATER" = none ]; then
  echo "   skipped (--no-water): the pack's own water material stays as it is."
  echo "   later: bash $HERE/tools/water.sh default \"$R\""
elif [ ! -d "$R/rtx-remix/mods/gameReadyAssets/texture/Water" ]; then
  echo "   WARN: no $R/rtx-remix/mods/gameReadyAssets/texture/Water - the pack's water material is not in this install; skipped"
elif [ "$WATER_SET" = 1 ]; then
  RHYS_WHY="apply.sh --water $WATER" bash "$HERE/tools/water.sh" "$WATER" "$R" \
    || echo "WARN: water variant '$WATER' not installed - the rest of the install is valid; run: bash $HERE/tools/water.sh $WATER \"$R\""
else
  RHYS_WHY="apply.sh (n13 default)" bash "$HERE/tools/water.sh" default "$R" \
    || echo "WARN: the default water variant was not installed - the rest of the install is valid; run: bash $HERE/tools/water.sh default \"$R\""
fi
echo "== new helper modules"
mkdir -p "$R/moonloader/lib"; cp "$HERE"/moonloader/lib/*.lua "$R/moonloader/lib/"
echo "== stale upstream script (SARemiv_VTK.lua uses pre-cloud model ids) -> backup"
if [ -f "$R/moonloader/SARemiv_VTK.lua" ]; then
  mkdir -p "$B/moonloader"; mv "$R/moonloader/SARemiv_VTK.lua" "$B/moonloader/"
fi
echo "== Proton bridge setting"; cp "$HERE/config/bridge.conf" "$R/.trex/bridge.conf"
echo "== MixSets: static ped shadows (real-time shadow = gray square under CJ in Remix)"
[ -f "$R/modloader/MixSets/MixSets.ini" ] && sed -i -E 's/^(UseHighPedShadows\s*=\s*)-1/\10/' "$R/modloader/MixSets/MixSets.ini"
echo "== launcher"; cp "$HERE/launcher/launch-rtx.sh" "$R/"; chmod +x "$R/launch-rtx.sh"
# The installed launcher has to find this checkout to rebuild the plugin on every start. It
# cannot guess that path and it must stay byte-identical to launcher/launch-rtx.sh (see
# scripts/installer-selftest.sh), so the path goes in its own file: launch-rtx.sh reads
# <game>/rhys-asi-repo when RHYS_ASI_REPO is unset and the default is not a repo.
echo "$HERE" > "$R/rhys-asi-repo"
echo "== dependencies from our hosts (deps/manifest.json)"
# rhY's rule: nothing a user would have to download by hand. The manifest lists every such file with
# its sha256, licence and target folder; tools/fetch-deps.sh fetches it (aria2c for torrent/magnet,
# curl for https) and only ever names a file whose sha256 matched. Off until our hosts exist - and a
# failed fetch is a WARN, not an abort: the plugin install above is still valid without the packs.
if [ "$WITH_DEPS" = 1 ]; then
  DEPS="$HERE/build/deps"; mkdir -p "$DEPS"
  if timeout 600 bash "$HERE/tools/fetch-deps.sh" "$HERE/deps/manifest.json" "$DEPS" --group "$DEPS_GROUP"; then
    echo "== unpacking what verified into $R/modloader/<pack>/ (one folder per pack, never merged)"
    UNPACKED=0
    while IFS=$'\x1f' read -r f target how; do
      [ -n "$f" ] || continue
      dest="$R/$target"; mkdir -p "$dest"
      case "$how" in                      # the manifest's own hint wins; else the extension
        zip) tool=unzip ;; rar) tool=unrar ;; 7z) tool=7z ;; none) tool=copy ;;
        *) case "$f" in
             *.zip) tool=unzip ;; *.rar) tool=unrar ;; *.7z) tool=7z ;; *) tool=copy ;;
           esac ;;
      esac
      "$HERE/tools/unpack.sh" "$tool" "$DEPS/$f" "$dest" && UNPACKED=$((UNPACKED+1))
    done < <(timeout 300 bash "$HERE/tools/fetch-deps.sh" "$HERE/deps/manifest.json" "$DEPS" --group "$DEPS_GROUP" --verify-only --plan --quiet || true)
    echo "   $UNPACKED pack(s) installed from $DEPS_GROUP. Rollback: delete those modloader folders (docs/CHARACTERS.md section 10)."
  else
    echo "WARN: dependency fetch failed - the install is still valid, the packs below were simply not added:"
    timeout 120 bash "$HERE/tools/fetch-deps.sh" "$HERE/deps/manifest.json" "$DEPS" --group "$DEPS_GROUP" --list --quiet || true
    echo "      (docs/CHARACTERS.md section 7 lists the pages; drop the files in $DEPS and re-run)"
  fi
else
  echo "skipped: --with-deps fetches them from our hosts (hosts are TBD, so it is off for now - see deps/manifest.json)"
fi
echo "done. Start with: $R/launch-rtx.sh"
