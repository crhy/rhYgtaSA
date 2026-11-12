#!/usr/bin/env bash
# s17: self-test for tools/characters.sh - runs entirely against a FAKE game tree under
# build/tmp, never a real install. The real game folder is only ever read (and the sandbox
# guard is tested to prove this script could not write there).
#
#   bash scripts/characters-selftest.sh [--keep]
#
# It reuses scripts/installer-selftest.sh's fake-tree builder (build/tmp/fakegame), adds the
# files the character switch touches (modloader/modloader.ini with CRLF line endings, exactly
# like ModLoader writes it, and a modloader/Ped Spec/ folder), stages a synthetic copy of the
# six "characters" packs from deps/manifest.json under build/tmp/char-selftest/stage, and runs
#
#   status -> off -> on (all, --drop-ped-spec) -> on again -> off
#   -> on --pack emergency-peds-vary (PedFuncs pulled in) -> off
#   -> missing-stage refusal -> --partial -> archive staging -> guards -> off
#
# checking the documented order, the priority lines landing INSIDE [Profiles.Default.Priority],
# the backups, and finally that the tree hashes back to the pre-"on" manifest: sha256 of every
# file, byte-identical, no state dir, no leftovers. Ends with "ALL PASS" or "N FAILED".
set -uo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${RHYS_GAME_DIR:-/media/rhy/data/Gamez/GTASA-RTX}"
STAGE_REAL="/media/rhy/data/Gamez/GTAmods/x/characters/stage"
FAKE="$REPO/build/tmp/fakegame"
WORK="$REPO/build/tmp/char-selftest"
CH="$REPO/tools/characters.sh"
KEEP=0
for a in "$@"; do case "$a" in --keep) KEEP=1 ;; *) echo "usage: $0 [--keep]"; exit 2 ;; esac; done

pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS  $*"; }
bad() { fail=$((fail+1)); echo "FAIL  $*"; }
chk() { local rc=$1; shift; if [ "$rc" = 0 ]; then ok "$*"; else bad "$*"; fi; }
res() { local rc=$?; chk "$rc" "$*"; }   # report the exit status of the test list before me
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
  printf '[System]\nStreamMemory = 1900\n' > "$FAKE/modloader/MixSets/MixSets.ini"
fi
[ -d "$FAKE/modloader" ] || { echo "FATAL: no fake modloader dir at $FAKE"; exit 1; }

# modloader.ini: the real one when we can read it (it is CRLF, which the ini writer has to
# survive), otherwise a fixture with the same shape and the same CRLF.
INI="$FAKE/modloader/modloader.ini"
if [ ! -f "$INI" ] && [ -f "$GAME/modloader/modloader.ini" ]; then
  cp -p "$GAME/modloader/modloader.ini" "$INI" && echo "   + modloader/modloader.ini (copied from the real install, read-only)"
fi
if [ ! -f "$INI" ]; then
  printf '[Folder.Config]\r\nPriorityLimit = 100\r\n\r\n[Profiles.Default.Priority]\r\n; default is 50\r\n;MyMod=50\r\n\r\n[Profiles.Default.IgnoreMods]\r\nVehFuncs\r\n_ignore\r\n' > "$INI"
  echo "   + modloader/modloader.ini (fixture, CRLF)"
fi
case "$(file "$INI")" in *CRLF*) CRLF=1 ;; *) CRLF=0 ;; esac
echo "   modloader.ini: $(wc -l < "$INI") lines, CRLF=$CRLF"
mkdir -p "$FAKE/modloader/Ped Spec"
printf 'fake ped_spec.asi\n' > "$FAKE/modloader/Ped Spec/ped_spec.asi"
printf 'fSpecColor 0.15\niSpecPower 40\n' > "$FAKE/modloader/Ped Spec/PedSpec.ini"
echo "   + modloader/Ped Spec/ (the one mod 'on --drop-ped-spec' moves out)"

manifest() { (cd "$FAKE" && find . -type f -print0 | sort -z | xargs -0 sha256sum); }
# read a key the way ModLoader would: only inside the given section
inikey() { awk -v k="$2" -v s="${3:-}" 'function t(x){sub(/\r+$/,"",x);sub(/^[ \t]+/,"",x);sub(/[ \t]+$/,"",x);return x}
           /^[ \t]*\[/{sec=t($0);next} {if(s!=""&&sec!="["s"]")next; i=index($0,"="); if(i&&t(substr($0,1,i-1))==k){v=substr($0,i+1); sub(/[ \t]*[;#].*$/,"",v); print t(v); exit}}' "$FAKE/$1"; }
packfiles() { local d n=0
  for d in "Proper Characters Retex HD" "Proper Player Retex" "PedFuncs" "Emergency Peds Vary" "Criminal Peds Vary" "Restored Unused Peds"; do
    [ -d "$FAKE/modloader/$d" ] && n=$((n + $(find "$FAKE/modloader/$d" -type f | wc -l)))
  done
  printf '%s\n' "$n"; }
# lines ending in CR vs total lines: the real modloader.ini is already mixed (its
# "VehFuncs" line got edited on Linux), so the check is a delta, not an absolute
inistats() { printf '%s %s\n' "$(grep -c $'\r$' "$INI" || true)" "$(wc -l < "$INI")"; }

# ------------------------------------------------------- the synthetic stage --
STAGE="$WORK/stage"
mkdir -p "$STAGE"
while IFS='|' read -r name files; do
  [ -n "$name" ] || continue
  for f in $files; do
    mkdir -p "$STAGE/$name/$(dirname "$f")"
    printf 'fake %s :: %s\n' "$name" "$f" > "$STAGE/$name/$f"
  done
done <<'EOF'
Proper Characters Retex HD|gta3.img/male01.dff gta3.img/male01.txd
Proper Player Retex|player.img/player_face.txd player.img/player_torso.txd player.img/player_legs.txd
PedFuncs|PedFuncs.asi
Emergency Peds Vary|data/peds/cped.ide gta3.img/cop_paramedic.dff gta3.img/dsher.txd
Criminal Peds Vary|gta3.img/gang_ballas.dff
Restored Unused Peds|gta3.img/unused_beach_bum.dff
EOF
NPACKFILES=$(find "$STAGE" -type f | wc -l)
echo "== synthetic stage: 6 packs, $NPACKFILES files ($STAGE)"

# snapshot the stage and the real game folder: this test must not change either
stagesnap() { find "$STAGE" -printf '%p %y %s\n' | sort; }
stagesnap > "$WORK/stage-before.txt"
gamesnap() { [ -d "$GAME" ] && find "$GAME" -maxdepth 2 -printf '%p %y %s\n' 2>/dev/null | sort; }
gamesnap > "$WORK/game-before.txt"

echo "== manifest before"
manifest > "$WORK/m-before.txt"; echo "   $(wc -l < "$WORK/m-before.txt") files hashed"

echo "== status and off before anything was installed"
run status0 "$CH" status "$FAKE" --stage "$STAGE"; chk $? "status on a clean tree exits 0"
grep -qE ' no *$' "$WORK/status0.log"; chk $? "status reports nothing installed"
grep -q 'proper-player-retex' "$WORK/status0.log"; chk $? "status lists the manifest packs by id"
run off0 "$CH" off "$FAKE"; chk $? "off on a clean tree is a no-op that exits 0"
manifest > "$WORK/m-clean.txt"; cmp -s "$WORK/m-before.txt" "$WORK/m-clean.txt"
chk $? "status + off changed no bytes ($(wc -l < "$WORK/m-clean.txt") files)"

echo "== on (the whole documented set, --drop-ped-spec)"
read -r cr1 ln1 <<< "$(inistats)"; echo "   modloader.ini before: $ln1 lines, $cr1 CR-terminated"
run on1 "$CH" on "$FAKE" --stage "$STAGE" --drop-ped-spec; chk $? "on exits 0"
mapfile -t ORDERED < <(grep '^   modloader/' "$WORK/on1.log" | sed 's/^   modloader\///; s/ (priority [0-9]*)$//')
printf '   install order seen: %s\n' "${ORDERED[*]:-<none>}"
{ [ "${#ORDERED[@]}" = 6 ] && [ "${ORDERED[0]}" = "Proper Characters Retex HD" ] \
  && [ "${ORDERED[1]}" = "Proper Player Retex" ] && [ "${ORDERED[2]}" = "PedFuncs" ] \
  && [ "${ORDERED[3]}" = "Emergency Peds Vary" ] && [ "${ORDERED[4]}" = "Criminal Peds Vary" ] \
  && [ "${ORDERED[5]}" = "Restored Unused Peds" ]; }
res "the packs go in the documented order (docs/CHARACTERS.md 5: NPC retex -> CJ retex -> PedFuncs -> Peds Vary -> extras)"
{ [ -f "$FAKE/modloader/Proper Player Retex/player.img/player_face.txd" ] \
  && [ -f "$FAKE/modloader/Emergency Peds Vary/data/peds/cped.ide" ] \
  && [ -f "$FAKE/modloader/Proper Characters Retex HD/gta3.img/male01.txd" ] \
  && [ "$(packfiles)" = "$NPACKFILES" ]; }
res "every staged file landed in its own modloader folder, pack layout preserved ($(packfiles) of $NPACKFILES files)"
{ [ ! -d "$FAKE/modloader/Ped Spec" ] && [ -d "$FAKE/modloader/.rhys-characters/moved/modloader/Ped Spec" ]; }
res "--drop-ped-spec moved Ped Spec into the state dir instead of deleting it"
{ grep -q 'mat_0AB982F62FEF3381' "$WORK/on1.log" && grep -q '8645663DE7AFBB81' "$WORK/on1.log" \
  && grep -q 'mat_E6E920AB99F5FC23' "$WORK/on1.log"; }
res "on prints which Remix material overrides stop matching (CJ's hashes + the 2 pedestrian ones)"

echo "== priorities"
NPRI=5   # packs with a priority in the manifest: 40, 60, 50, 50, 45
{ [ "$(inikey modloader/modloader.ini 'Proper Player Retex' 'Profiles.Default.Priority')" = 60 ] &&
  [ "$(inikey modloader/modloader.ini 'Proper Characters Retex HD' 'Profiles.Default.Priority')" = 40 ] &&
  [ "$(inikey modloader/modloader.ini 'Emergency Peds Vary' 'Profiles.Default.Priority')" = 50 ] &&
  [ "$(inikey modloader/modloader.ini 'Restored Unused Peds' 'Profiles.Default.Priority')" = 45 ]; }
res "priorities 40/60/50/45 are readable inside [Profiles.Default.Priority]"
awk '{gsub(/\r/,"")} /^\[/{insec=($0=="[Profiles.Default.Priority]")} insec && /^Proper Player Retex=/{c++} END{exit !c}' "$INI"
res "the priority line sits INSIDE the section, not appended at EOF"
{ grep -q 'VehFuncs' "$INI" && grep -q '_ignore' "$INI"; }
res "[Profiles.Default.IgnoreMods] (VehFuncs, _ignore) untouched"
grep -q ';MyMod=50' "$INI"; res "the section's own comment lines survived the edit"
read -r cr2 ln2 <<< "$(inistats)"
[ "$cr2" = "$((cr1 + NPRI))" ] && [ "$ln2" = "$((ln1 + NPRI))" ]
res "line endings survive: CR-terminated lines $cr1 -> $cr2, lines $ln1 -> $ln2 (exactly the $NPRI new priority lines, the file's own mixed CRLF/LF untouched)"
BACKUP="$FAKE/modloader/.rhys-characters/backups/modloader/modloader.ini"
[ -f "$BACKUP" ]; res "modloader.ini was backed up pristine in the state dir"
if grep -q 'Proper Player Retex=60' "$BACKUP"; then bad "the backup holds the EDITED ini, not the pristine one"
else ok "the backup holds the pristine ini (no priority lines in it)"; fi
{ [ "$(wc -l < "$FAKE/modloader/.rhys-characters/folders.txt")" = 6 ] \
  && [ "$(wc -l < "$FAKE/modloader/.rhys-characters/added.txt")" = "$NPACKFILES" ]; }
res "state dir records 6 pack folders and $NPACKFILES added files"

echo "== re-run: converges, and never overwrites the pristine backup"
run on2 "$CH" on "$FAKE" --stage "$STAGE" --drop-ped-spec; chk $? "a second on exits 0"
grep -q "0 file(s) copied, $NPACKFILES already identical" "$WORK/on2.log"
chk $? "the second on copied nothing (already identical)"
grep -q 'Proper Player Retex = 60 (already)' "$WORK/on2.log"
chk $? "the second on left the priority alone"
! grep -q 'Proper Player Retex=60' "$BACKUP"; chk $? "the second on did NOT record the edited ini as the backup"

echo "== off"
run off1 "$CH" off "$FAKE"; chk $? "off exits 0"
left=0
for d in "Proper Characters Retex HD" "Proper Player Retex" "PedFuncs" "Emergency Peds Vary" "Criminal Peds Vary" "Restored Unused Peds"; do
  [ -d "$FAKE/modloader/$d" ] && { left=1; bad "modloader/$d still there"; }
done
[ "$left" = 0 ] && ok "all six pack folders are gone"
{ [ -d "$FAKE/modloader/Ped Spec" ] && [ ! -d "$FAKE/modloader/.rhys-characters" ]; }
res "Ped Spec is back and the state dir is gone"
[ "$(inikey modloader/modloader.ini 'Proper Player Retex' 'Profiles.Default.Priority')" = "" ]
res "the priority lines are gone from modloader.ini"
run off2 "$CH" off "$FAKE"; chk $? "a second off is a no-op that exits 0"

echo "== a subset: --pack emergency-peds-vary pulls PedFuncs in by itself"
run onsub "$CH" on "$FAKE" --stage "$STAGE" --pack emergency-peds-vary; chk $? "on --pack emergency-peds-vary exits 0"
{ [ -d "$FAKE/modloader/PedFuncs" ] && [ -d "$FAKE/modloader/Emergency Peds Vary" ] \
  && [ ! -d "$FAKE/modloader/Proper Player Retex" ]; }
res "only the requested pack and its required plugin were installed"
grep -q 'pulled in: pedfuncs is required by emergency-peds-vary' "$WORK/onsub.log"
chk $? "the log says why PedFuncs came along (deps/manifest.json 'requires')"
run offsub "$CH" off "$FAKE"; chk $? "off removes the subset too"

echo "== nothing staged: refuse rather than install half a set"
mkdir -p "$WORK/stage-empty"
run onnone "$CH" on "$FAKE" --stage "$WORK/stage-empty"; chk $(( ! $? )) "on refuses when nothing is staged"
{ grep -q 'NOT STAGED' "$WORK/onnone.log" && grep -q 'libertycity.net\|mixmods.com.br' "$WORK/onnone.log"; }
res "the refusal names each pack, where to stage it and the author's page"
manifest > "$WORK/m-refuse.txt"; cmp -s "$WORK/m-before.txt" "$WORK/m-refuse.txt"
chk $? "the refusal changed no bytes"
mkdir -p "$WORK/stage-half" && cp -p -r "$STAGE/Emergency Peds Vary" "$WORK/stage-half/"
run onhalf "$CH" on "$FAKE" --stage "$WORK/stage-half" --partial; chk $? "--partial installs what is staged"
{ [ -d "$FAKE/modloader/Emergency Peds Vary" ] && [ ! -d "$FAKE/modloader/Proper Player Retex" ]; }
res "--partial installed only the staged pack"
grep -q 'requires pedfuncs, which is not staged' "$WORK/onhalf.log"
chk $? "--partial warns that the pack's dependency is missing (it needs PedFuncs to work)"
run offhalf "$CH" off "$FAKE"; chk $? "off undoes the partial install"

echo "== a staged archive instead of a folder"
if command -v 7z >/dev/null 2>&1; then
  mkdir -p "$WORK/stage-zip"
  ( cd "$STAGE" && 7z a -tzip -y "$WORK/stage-zip/proper-player-retex-3.2-pc.zip" "Proper Player Retex" >/dev/null )
  run onzip "$CH" on "$FAKE" --stage "$WORK/stage-zip" --pack proper-player-retex; chk $? "on unpacks an archive named like the manifest entry"
  [ -f "$FAKE/modloader/Proper Player Retex/player.img/player_face.txd" ]
  res "the archive's contents landed in modloader/Proper Player Retex (one top level flattened)"
  run offzip "$CH" off "$FAKE"; chk $? "off removes the unpacked pack"
else
  echo "   7z not installed - archive staging not tested"
fi

echo "== the guards"
mkdir -p "$FAKE/modloader/Proper Player Retex"
printf 'hand installed\n' > "$FAKE/modloader/Proper Player Retex/hand.txd"
run onhand "$CH" on "$FAKE" --stage "$STAGE"; chk $(( ! $? )) "on refuses a pack folder it did not create"
run offhand "$CH" off "$FAKE"; chk $(( ! $? )) "off refuses to delete a hand-installed pack folder"
[ -f "$FAKE/modloader/Proper Player Retex/hand.txd" ]; res "the hand-installed folder is still there, untouched"
rm -rf "$FAKE/modloader/Proper Player Retex"
mkdir -p "$WORK/nomarker/modloader"
run onnomarker env RHYREMIX_INSTALLER_SANDBOX=1 "$CH" on "$WORK/nomarker" --stage "$STAGE"
chk $(( ! $? )) "sandbox mode refuses a folder without .fake-install-marker"
if [ -d "$GAME" ] && [ ! -f "$GAME/.fake-install-marker" ]; then
  run onreal env RHYREMIX_INSTALLER_SANDBOX=1 "$CH" on "$GAME" --stage "$STAGE"
  chk $(( ! $? )) "sandbox mode refuses the REAL game folder (this test cannot write there)"
else
  echo "   skipped the real-game refusal check ($GAME $([ -d "$GAME" ] && echo 'has a fake marker' || echo 'not present'))"
fi
manifest > "$WORK/m-guards.txt"; cmp -s "$WORK/m-before.txt" "$WORK/m-guards.txt"
chk $? "the guard tests changed no bytes either"

echo "== the real staging area, reported honestly (read-only)"
run statusreal "$CH" status "$FAKE"; chk $? "status with the default stage dirs exits 0"
if [ -d "$STAGE_REAL" ]; then
  echo "   $STAGE_REAL exists"
else
  grep -q 'GTAmods/x/characters/stage' "$WORK/statusreal.log"
  chk $? "status prints where packs have to be staged (that folder does not exist yet)"
fi

echo "== nothing outside the fake tree moved"
stagesnap > "$WORK/stage-after.txt"; cmp -s "$WORK/stage-before.txt" "$WORK/stage-after.txt"
chk $? "the synthetic stage dir is unchanged (packs are copied, not moved)"
gamesnap > "$WORK/game-after.txt"; cmp -s "$WORK/game-before.txt" "$WORK/game-after.txt"
chk $? "$GAME unchanged (names, types and sizes identical)"

echo "== the final round-trip"
manifest > "$WORK/m-after.txt"
if cmp -s "$WORK/m-before.txt" "$WORK/m-after.txt"; then
  ok "on/off round-trip is byte-identical over the whole run ($(wc -l < "$WORK/m-after.txt") files)"
else
  bad "the fake tree did NOT come back byte-identical"
  diff "$WORK/m-before.txt" "$WORK/m-after.txt" | head -20 | sed 's/^/       /'
fi
found=$(find "$FAKE" -name '.rhys-characters' -o -name '*.bak' -o -name '*.orig' | head -3)
[ -z "$found" ]; chk $? "no state / backup / .orig leftovers anywhere in the tree"

[ "$KEEP" = 1 ] || { rm -rf "$WORK"; rm -rf "$FAKE"; }
echo
echo "$pass checks passed, $fail failed"
[ "$fail" = 0 ] && echo "ALL PASS" || echo "$fail FAILED"
exit $((fail > 0))
