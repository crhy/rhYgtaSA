#!/usr/bin/env bash
# s16: self-test for tools/vegetation.sh - runs entirely against a FAKE game tree under
# build/tmp, never a real install, never the game folder (which it only reads).
#
#   bash scripts/vegetation-selftest.sh [--keep] [--no-real-pack]
#
#     --keep          keep build/tmp/veg-selftest and build/tmp/fakegame afterwards
#     --no-real-pack  skip the round-trip with the real 192 MB staged pack (fast, synthetic pack only)
#
# It reuses scripts/installer-selftest.sh's fake-tree builder (build/tmp/fakegame, rebuilt
# here and removed again unless --keep), adds the
# files the vegetation fixes touch (ImprovedStreaming.ini, the Remix_fix copies, the rtx.conf
# opacity-micromap keys), then hashes every file in the tree and runs
#
#     status -> off -> on --step 3 -> on --step 4 -> on --step 2 -> off -> off
#
# checking the fixes really land, and finally that the tree hashes back to the pre-"on"
# manifest: sha256 of every file, byte-identical, no state dir, no leftovers. It also checks
# the two guards (a hand-installed pack folder, and the RHYREMIX_INSTALLER_SANDBOX marker).
# Ends with "ALL PASS" or "N FAILED".
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${RHYS_GAME_DIR:-/media/rhy/data/Gamez/GTASA-RTX}"
REAL_PACK="${RHYS_VEG_PACK:-/media/rhy/data/Gamez/GTAmods/removed-from-game/rtx-copy/V Vegetation Mix}"
FAKE="$REPO/build/tmp/fakegame"
WORK="$REPO/build/tmp/veg-selftest"
VEG="$REPO/tools/vegetation.sh"
KEEP=0; REALPACK=1
for a in "$@"; do case "$a" in --keep) KEEP=1 ;; --no-real-pack) REALPACK=0 ;; *) echo "usage: $0 [--keep] [--no-real-pack]"; exit 2 ;; esac; done

pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS  $*"; }
bad() { fail=$((fail+1)); echo "FAIL  $*"; }
chk() { local rc=$1; shift; if [ "$rc" = 0 ]; then ok "$*"; else bad "$*"; fi; }
run() { local label="$1"; shift; echo "-- $label"; "$@" > "$WORK/$label.log" 2>&1; local rc=$?
        echo "   rc=$rc (log: ${WORK#"$REPO"/}/$label.log)"; return $rc; }

rm -rf "$WORK"; mkdir -p "$WORK"
export RHYREMIX_INSTALLER_SANDBOX=1

# ------------------------------------------------- the fake game tree + extras --
echo "== fake game tree (scripts/installer-selftest.sh --fake-only)"
if [ -d "$GAME/rtx-remix/mods/gameReadyAssets" ]; then
  run fakegame bash "$REPO/scripts/installer-selftest.sh" --fake-only
  chk $? "fake tree built by the installer self-test"
else
  echo "   no real game folder at $GAME - building a minimal fake tree instead"
  rm -rf "$FAKE"
  mkdir -p "$FAKE/modloader/MixSets" "$FAKE/moonloader" "$FAKE/rtx-remix/mods/gameReadyAssets"
  printf 'fake install - scratch, never a real game folder\n' > "$FAKE/.fake-install-marker"
  printf 'rtx.enableCulling = False\n' > "$FAKE/rtx.conf"
  printf '[System]\nStreamMemory = 1900\n' > "$FAKE/modloader/MixSets/MixSets.ini"
fi

# The vegetation fixes reach files the installer self-test does not copy. Add them: the
# real ones when we can read them, a fixture otherwise. Then seed two known states so
# both code paths get tested: rtx.conf WITH the opacity-micromap keys (replace) and
# Remix_fix/ImprovedStreaming.ini WITHOUT RemoveUnusedWhenPercent (append).
add_ini() { # <rel> - copy from the real install if it has one, else write a fixture
  local rel="$1"
  mkdir -p "$FAKE/$(dirname "$rel")"
  if [ -f "$GAME/$rel" ]; then cp -p "$GAME/$rel" "$FAKE/$rel"; echo "   + $rel (copied from the real install, read-only)"
  else cp -p "$WORK/fixture.ini" "$FAKE/$rel"; echo "   + $rel (fixture)"; fi
}
cat > "$WORK/fixture.ini" <<'EOF'
[Settings]
StreamMemoryForced = 1024
RemoveUnusedWhenPercent = 80
[Range4]
Enabled = 1
[Range5]
Enabled = 1
EOF
for rel in "modloader/Improved Streaming/ImprovedStreaming.ini" \
           "modloader/Remix_fix/ImprovedStreaming.ini" \
           "modloader/Remix_fix/MixSets.ini"; do add_ini "$rel"; done
[ -f "$FAKE/modloader/MixSets/MixSets.ini" ] || { mkdir -p "$FAKE/modloader/MixSets"; cp -p "$WORK/fixture.ini" "$FAKE/modloader/MixSets/MixSets.ini"; }
printf 'rtx.opacityMicromap.cache.minFreeVidmemMBToNotAllocate = 1536\nrtx.opacityMicromap.cache.maxBudgetSizeMB = 1024\n' >> "$FAKE/rtx.conf"
grep -v '^RemoveUnusedWhenPercent' "$FAKE/modloader/Remix_fix/ImprovedStreaming.ini" > "$WORK/no-key.ini" \
  && cat "$WORK/no-key.ini" > "$FAKE/modloader/Remix_fix/ImprovedStreaming.ini"
echo "   seeded rtx.conf with the live opacity-micromap values; dropped RemoveUnusedWhenPercent from the Remix_fix copy"

manifest() { (cd "$FAKE" && find . -type f -print0 | sort -z | xargs -0 sha256sum); }
val() { awk -v k="$2" -v s="${3:-}" 'function t(x){sub(/^[ \t]+/,"",x);sub(/[ \t]+$/,"",x);return x}
        /^[ \t]*\[/{sec=t($0);next} {if(s!=""&&sec!="["s"]")next; i=index($0,"="); if(i&&t(substr($0,1,i-1))==k){v=substr($0,i+1); sub(/[ \t]*[;#].*$/,"",v); print t(v); exit}}' "$FAKE/$1"; }

# ------------------------------------------------------------- the fake pack --
PACK="$WORK/pack/V Vegetation Mix"
mkpack() {
  rm -rf "$WORK/pack"; mkdir -p "$PACK/gta3.img" "$PACK/models/grass" "$PACK/data/maps/generic"
  local n
  for n in tree_a.dff tree_b.dff bush_a.dff other.txd insanity_pines.txd gta_tree_boak.txd; do
    printf 'fake %s, 1 byte of story\n' "$n" > "$PACK/gta3.img/$n"
  done
  printf 'fake plant1\n' > "$PACK/models/grass/plant1.txd"
  printf 'deftype 1000 tree_a 1\n' > "$PACK/data/maps/generic/procobj.ide"
  printf 'deftype 1001 tree_b 1\n' > "$PACK/data/maps/generic/vegepart.ide"
}
mkpack

# the staged pack and the game folder are read-only for this test: snapshot them
packsnap() { [ -d "$REAL_PACK" ] && find "$REAL_PACK" -printf '%p %y %s\n' | sort; }
packsnap > "$WORK/pack-before.txt"

echo "== manifest before"
manifest > "$WORK/m-before.txt"; echo "   $(wc -l < "$WORK/m-before.txt") files hashed"

echo "== status and off before anything was installed"
run status0 "$VEG" status "$FAKE" --pack "$PACK"; chk $? "status on a clean tree exits 0"
grep -q 'installed: no' "$WORK/status0.log"; chk $? "status says 'installed: no'"
run off0 "$VEG" off "$FAKE"; chk $? "off on a clean tree is a no-op that exits 0"
manifest > "$WORK/m-clean.txt"; cmp -s "$WORK/m-before.txt" "$WORK/m-clean.txt"; chk $? "status + off changed no bytes ($(wc -l < "$WORK/m-clean.txt") files)"

echo "== on --step 3 (the three ranked fixes)"
run on3 "$VEG" on "$FAKE" --pack "$PACK" --step 3; chk $? "on --step 3 exits 0"
D="$FAKE/modloader/V Vegetation Mix"
[ -f "$D/gta3.img/tree_a.dff" ] && [ -f "$D/gta3.img/other.txd" ] && [ -f "$D/models/grass/plant1.txd" ]
chk $? "the pack's models were copied into modloader/'V Vegetation Mix'"
[ ! -f "$D/data/maps/generic/procobj.ide" ] && [ ! -f "$D/data/maps/generic/vegepart.ide" ]
chk $? "step 1: the pack's procobj.ide / vegepart.ide were left out"
[ ! -d "$D/data/maps/generic" ] && chk $? "step 1: no empty data/maps/generic left behind"
[ "$(val 'modloader/MixSets/MixSets.ini' StreamMemory)" = 1024 ]; chk $? "step 2: MixSets StreamMemory 1900 -> 1024"
[ "$(val 'modloader/Remix_fix/MixSets.ini' StreamMemory)" = 1024 ]; chk $? "step 2: the Remix_fix MixSets.ini too"
[ "$(val 'modloader/Improved Streaming/ImprovedStreaming.ini' Enabled Range4)" = 0 ] &&
[ "$(val 'modloader/Improved Streaming/ImprovedStreaming.ini' Enabled Range5)" = 0 ]
chk $? "step 2: [Range4]/[Range5] Enabled = 0 (the pinned vegetation ranges)"
[ "$(val 'modloader/Improved Streaming/ImprovedStreaming.ini' Enabled Range3)" = 1 ]
chk $? "step 2: [Range3] (peds) was NOT touched - the edit is section-scoped"
[ "$(val 'modloader/Improved Streaming/ImprovedStreaming.ini' RemoveUnusedWhenPercent)" = 70 ]
chk $? "step 2: RemoveUnusedWhenPercent 80 -> 70"
grep -q 'RemoveUnusedWhenPercent = 70' "$FAKE/modloader/Improved Streaming/ImprovedStreaming.ini"
chk $? "the edited line is the real ini line"
[ "$(val 'modloader/Remix_fix/ImprovedStreaming.ini' RemoveUnusedWhenPercent)" = 70 ]
chk $? "step 2: a key the file never had is appended (Remix_fix copy)"
[ "$(val rtx.conf rtx.opacityMicromap.cache.maxBudgetSizeMB)" = 256 ] &&
[ "$(val rtx.conf rtx.opacityMicromap.cache.minFreeVidmemMBToNotAllocate)" = 3072 ]
chk $? "step 3: rtx.conf opacity-micromap budget 1024/1536 -> 256/3072"
[ -f "$FAKE/modloader/.rhys-vegetation/step" ] && [ "$(cat "$FAKE/modloader/.rhys-vegetation/step")" = 3 ]
chk $? "state dir records step 3"
[ -f "$FAKE/modloader/.rhys-vegetation/backups/modloader/MixSets/MixSets.ini" ]
chk $? "the pre-fix MixSets.ini is backed up inside the state dir"
grep -q 'StreamMemory = 1900' "$FAKE/modloader/.rhys-vegetation/backups/modloader/MixSets/MixSets.ini"
chk $? "the backup holds the pristine 1900, not the fixed value"
run status3 "$VEG" status "$FAKE" --pack "$PACK"; chk $? "status after on exits 0"
grep -q 'by this tool at --step 3' "$WORK/status3.log"; chk $? "status reports step 3"
grep -q 'procobj.ide' "$WORK/status3.log"; chk $? "status lists the withheld IDE files"

echo "== re-run with a different step (delta only, backups kept)"
run on4 "$VEG" on "$FAKE" --pack "$PACK" --step 4; chk $? "on --step 4 exits 0"
[ ! -f "$D/gta3.img/insanity_pines.txd" ] && [ ! -f "$D/gta3.img/gta_tree_boak.txd" ]
chk $? "step 4: the two heaviest dictionaries are out of the install"
[ -f "$D/gta3.img/tree_a.dff" ]; chk $? "step 4: the other files are still there (no re-copy storm)"
grep -q 'StreamMemory = 1900' "$FAKE/modloader/.rhys-vegetation/backups/modloader/MixSets/MixSets.ini"
chk $? "the second run did not overwrite the pristine backup with the fixed file"
run on2 "$VEG" on "$FAKE" --pack "$PACK" --step 2; chk $? "on --step 2 (a lower step) exits 0"
[ -f "$D/gta3.img/insanity_pines.txd" ]; chk $? "step 2 again: the withheld .txd came back"
[ ! -f "$D/data/maps/generic/procobj.ide" ]; chk $? "step 2 still leaves the IDEs out (step 1 is part of 1..2)"

echo "== off"
run off1 "$VEG" off "$FAKE"; chk $? "off exits 0"
[ ! -d "$D" ]; chk $? "the pack folder is gone"
[ ! -d "$FAKE/modloader/.rhys-vegetation" ]; chk $? "the state dir is gone"
[ "$(val 'modloader/MixSets/MixSets.ini' StreamMemory)" = 1900 ]; chk $? "MixSets.ini restored to 1900"
[ "$(val 'modloader/Improved Streaming/ImprovedStreaming.ini' RemoveUnusedWhenPercent)" = 80 ]
chk $? "ImprovedStreaming.ini restored to 80"
[ "$(val 'modloader/Remix_fix/ImprovedStreaming.ini' RemoveUnusedWhenPercent)" = "" ]
chk $? "the appended key is gone from the Remix_fix copy"
[ "$(val rtx.conf rtx.opacityMicromap.cache.maxBudgetSizeMB)" = 1024 ]; chk $? "rtx.conf restored to 1024"
run off2 "$VEG" off "$FAKE"; chk $? "a second off is a no-op that exits 0"

echo "== the round-trip is byte-identical"
manifest > "$WORK/m-after.txt"
if cmp -s "$WORK/m-before.txt" "$WORK/m-after.txt"; then
  ok "every file in the fake tree hashes back to before 'on' ($(wc -l < "$WORK/m-after.txt") files)"
else
  bad "the fake tree did NOT come back byte-identical"
  diff "$WORK/m-before.txt" "$WORK/m-after.txt" | head -20 | sed 's/^/       /'
fi
find "$FAKE" -name '.rhys-vegetation' -o -name '*.bak' -o -name '*.orig' | grep -q .
chk $(( ! $? )) "no state / backup / .orig leftovers anywhere in the tree"

echo "== the guards"
mkdir -p "$D/gta3.img"; printf 'hand installed\n' > "$D/gta3.img/hand.dff"
run onhand "$VEG" on "$FAKE" --pack "$PACK" --step 3; chk $(( ! $? )) "on refuses a pack folder it did not create"
run offhand "$VEG" off "$FAKE"; chk $(( ! $? )) "off refuses to delete a hand-installed pack folder"
rm -rf "$D"
mkdir -p "$WORK/nomarker/modloader"
run onnomarker env RHYREMIX_INSTALLER_SANDBOX=1 "$VEG" on "$WORK/nomarker" --step 1
chk $(( ! $? )) "sandbox mode refuses a folder without .fake-install-marker"
manifest > "$WORK/m-guards.txt"; cmp -s "$WORK/m-before.txt" "$WORK/m-guards.txt"
chk $? "the guard tests changed no bytes either"

# ------------------------------------------------- the real 192 MB staged pack --
if [ "$REALPACK" = 1 ] && [ -d "$REAL_PACK" ]; then
  echo "== the real staged pack ($(find "$REAL_PACK" -type f | wc -l) files, $(du -sh "$REAL_PACK" | cut -f1))"
  run realon "$VEG" on "$FAKE" --pack "$REAL_PACK" --step 3; chk $? "on with the real pack exits 0"
  n=$(find "$D" -type f | wc -l)
  [ "$n" = "$(($(find "$REAL_PACK" -type f | wc -l) - 2))" ]
  chk $? "all but the 2 IDE files installed ($n of $(find "$REAL_PACK" -type f | wc -l))"
  [ ! -f "$D/data/maps/generic/procobj.ide" ]
  [ -f "$D/gta3.img/bsor_tree.txd" ] && [ -f "$D/models/grass/plant1.txd" ]
  chk $? "the big dictionaries and models/grass/plant1.txd are in place"
  run realstatus "$VEG" status "$FAKE"; chk $? "status with the real pack exits 0"
  run realoff "$VEG" off "$FAKE"; chk $? "off with the real pack exits 0"
  manifest > "$WORK/m-real.txt"; cmp -s "$WORK/m-before.txt" "$WORK/m-real.txt"
  chk $? "the real-pack round-trip is byte-identical too"
  cmp -s "$WORK/pack-before.txt" <(packsnap)
  chk $? "$REAL_PACK untouched (names, types and sizes identical)"
else
  echo "== real pack skipped ($REAL_PACK $([ "$REALPACK" = 1 ] && echo missing || echo '--no-real-pack'))"
fi

[ "$KEEP" = 1 ] || { rm -rf "$WORK"; rm -rf "$FAKE"; }
echo
echo "$pass checks passed, $fail failed"
[ "$fail" = 0 ] && echo "ALL PASS" || echo "$fail FAILED"
exit $((fail > 0))
