#!/usr/bin/env bash
# tools/winedbg-hang.sh — dump every thread's backtrace of a HUNG GTA SA (RTX/Proton) run.
#
# Usage:
#   tools/winedbg-hang.sh                 # find the game's winedbg pid itself
#   tools/winedbg-hang.sh <wpid>          # wpid = winedbg's own pid, the 4-hex number it prints
#                                         # ("WineDbg attached to pid 0148" in
#                                         #  GTAmods/.strata/winedbg-deadlock-bt.txt)
#   tools/winedbg-hang.sh --dry-run       # show what would run, touch nothing
#
# Run it while the game is frozen (black screen, logs silent). It writes
#   REPO/build/hang-<YYYYmmdd-HHMMSS>.txt
# with, in order:
#   1. host-side snapshot of the game process: threads, VmSize/VmPeak, biggest mappings.
#      That is the direct test of the "the 32-bit process ran out of address space" cause
#      in docs/VEGETATION.md (gta-sa.exe is not large-address-aware: ~2 GB under Wine).
#   2. tails of the game logs (modloader, Improved Streaming, MixSets, rhyremix, remix-dxvk).
#   3. winedbg: "info threads", "bt all" and the module list ("info proc") of the game.
#
# Modelled on GTAmods/.strata/winedbg-deadlock-bt.txt: that file is winedbg console output -
# "WineDbg attached to pid 0148" then "Backtracing for thread NNNN in process 0148" blocks,
# i.e. the "bt all" command. The same file records the failure mode to avoid ("WineDbg can't
# debug several processes at once", "warning: could not attach to 0020"), so this script
# attaches to exactly ONE process, dumps all its threads, and detaches.
#
# It never writes to the game folder, never needs sudo, and only reads /proc and the logs.
# It does stop the game's threads for the few seconds the dump takes - the game is already hung.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${GTASA_RTX_DIR:-/media/rhy/data/Gamez/GTASA-RTX}"
STEAM="${STEAM_ROOT:-$HOME/.local/share/Steam}"
PROTON="${PROTON:-$STEAM/steamapps/common/Proton - Experimental/proton}"
PFX_BASE="${STEAM_COMPAT_DATA_PATH:-$HOME/.local/share/gtasa-rtx-pfx}"
WINEPREFIX="$PFX_BASE/pfx"
OUT="$REPO/build/hang-$(date +%Y%m%d-%H%M%S).txt"
TMP="$REPO/build/tmp"
DRY=0
WPID=""

for a in "$@"; do
  case "$a" in
    --dry-run) DRY=1 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) WPID="$a" ;;
  esac
done

say() { printf '\n== %s ==\n' "$1" | tee -a "$OUT"; }
run() {   # log the command line, run it (unless --dry-run), append its output to $OUT
  printf '$ %s\n' "$*" >>"$OUT"
  if [ "$DRY" = 1 ]; then echo "  (dry run)" >>"$OUT"; return 0; fi
  "$@" >>"$OUT" 2>&1
}

mkdir -p "$TMP" "$REPO/build"
{
  echo "# winedbg-hang.sh $(date -Is)"
  echo "# game=$GAME"
  echo "# prefix=$WINEPREFIX"
  echo "# proton=$PROTON"
} >"$OUT"

# ---------------------------------------------------------------- host side
HPID="$(pgrep -x 'gta-sa.exe' 2>/dev/null | head -1)"
[ -n "$HPID" ] || HPID="$(pgrep -f 'gta-sa\.exe' 2>/dev/null | head -1)"
say "host process"
if [ -n "$HPID" ]; then
  echo "host pid $HPID: $(tr '\0' ' ' <"/proc/$HPID/cmdline")" >>"$OUT"
  run grep -E '^(State|Threads|VmSize|VmPeak|VmRSS|VmSwap|VmLargestFree)' "/proc/$HPID/status"
  say "biggest mappings of the game process (address-space check: a non-LAA 32-bit exe gets ~2 GB)"
  if [ "$DRY" != 1 ] && command -v python3 >/dev/null; then
    python3 - "$HPID" >>"$OUT" 2>&1 <<'PY'
import sys
pid = sys.argv[1]
regs = []
for ln in open(f"/proc/{pid}/maps"):
    parts = ln.split(None, 5)
    start, end = (int(x, 16) for x in parts[0].split("-"))
    regs.append((end - start, parts[0], parts[1], parts[5].strip() if len(parts) > 5 else ""))
regs.sort(reverse=True)
print(f"total mapped: {sum(r[0] for r in regs)/2**30:.2f} GB in {len(regs)} mappings")
print("largest 15 mappings:")
for size, rng, perms, name in regs[:15]:
    print(f"  {size/2**20:8.1f} MB  {rng:26s} {perms:5s} {name}")
free = [r[0] for r in regs if r[3] == "[heap]"]
print("(a 'free' gap big enough for the next big allocation is what the game needs;")
print(" if VmSize sits just under 2 GB and the freeze is in a streaming/alloc path, the cap is the cause)")
PY
  fi
else
  echo "no running gta-sa.exe host process found (pgrep -x gta-sa.exe)" >>"$OUT"
fi

# ---------------------------------------------------------------- log tails
say "log tails (the last thing each log said before the silence)"
for f in "modloader/modloader.log" "modloader/Improved Streaming/ImprovedStreaming.log" \
         "modloader/MixSets/MixSets.log" "scripts/rhyremix.log" \
         "rtx-remix/logs/remix-dxvk.log" "moonloader/moonloader.log"; do
  if [ -f "$GAME/$f" ]; then
    printf -- '-- %s (last modified %s) --\n' "$f" "$(date -r "$GAME/$f" '+%F %T')" >>"$OUT"
    tail -n 25 "$GAME/$f" >>"$OUT" 2>&1
  else
    printf -- '-- %s: missing --\n' "$f" >>"$OUT"
  fi
done

# ---------------------------------------------------------------- winedbg
WINEDBG=()
if [ -x "$PROTON" ]; then
  # Proton ships winedbg only as a Windows binary (files/lib/wine/*/winedbg.exe, symlinked into
  # the prefix's system32/syswow64); "proton runinprefix <prog>" runs "wine <prog>" with this
  # prefix's environment, so the debugger matches the running wineserver. Do NOT use the
  # distro winedbg against a Proton prefix: a version mismatch cannot attach usefully.
  WINEDBG=("$PROTON" runinprefix winedbg)
elif command -v winedbg >/dev/null; then
  echo "NOTE: no Proton script at $PROTON - falling back to the system winedbg." >>"$OUT"
  echo "      It must match the Wine version of the running prefix." >>"$OUT"
  WINEDBG=(env WINEPREFIX="$WINEPREFIX" winedbg)
else
  say "no winedbg found"
  echo "looked for $PROTON and winedbg in \$PATH" >>"$OUT"
  echo "output: $OUT"; exit 1
fi

if [ -z "$WPID" ]; then
  say "winedbg process list (looking for the game's wpid)"
  # man winedbg: --command runs the string and exits, and its own example for finding a pid
  # is: winedbg --command "info proc"
  # the output went to $OUT; re-extract just the process-list section for parsing
  PROCLIST="$TMP/winedbg-proc.txt"
  run "${WINEDBG[@]}" --command "info proc"
  sed -n '/info proc/,$p' "$OUT" >"$PROCLIST"
  WPID="$(awk 'tolower($0) ~ /gta-sa/ {
             for (i = 1; i <= NF; i++)
               if ($i ~ /^[0-9a-fA-F][0-9a-fA-F]?[0-9a-fA-F]?[0-9a-fA-F]?[0-9a-fA-F]?[0-9a-fA-F]?[0-9a-fA-F]?[0-9a-fA-F]?$/) { print $i; exit }
           }' "$PROCLIST")"
  echo "parsed wpid: '${WPID:-none}' (raw list above; pass it as an argument if this is wrong)" >>"$OUT"
fi

if [ -z "$WPID" ]; then
  say "could not identify the game's wpid"
  echo "Read it off winedbg yourself, then re-run with it as the argument:" >>"$OUT"
  echo "  STEAM_COMPAT_DATA_PATH=$PFX_BASE \"$PROTON\" runinprefix winedbg" >>"$OUT"
  echo "  Wine-dbg> info proc        (the number in front of gta-sa.exe)" >>"$OUT"
  echo "  Wine-dbg> bt all" >>"$OUT"
  echo "then: tools/winedbg-hang.sh <that-pid>" >>"$OUT"
  echo "output: $OUT"; exit 1
fi

say "winedbg backtraces (wpid $WPID)"
CMDS="$TMP/winedbg-hang-cmds.txt"
cat >"$CMDS" <<'EOF'
info threads
bt all
info proc
detach
EOF
# --file executes the command list and exits; the trailing number is the process to attach to.
run "${WINEDBG[@]}" --file "$CMDS" "$WPID"

# Fallback if this winedbg build has no --file: one command string, same attach.
if [ "$DRY" != 1 ] && ! grep -qi "Backtracing for thread" "$OUT"; then
  say "no backtrace from --file, retrying with --command"
  run "${WINEDBG[@]}" --command "bt all" "$WPID"
fi

if grep -qi "can't debug several processes\|could not attach" "$OUT"; then
  say "attach problem"
  echo "Another winedbg already holds the process (the old trace hit this too). Kill it:" >>"$OUT"
  echo "  pkill -f winedbg" >>"$OUT"
  echo "then re-run this script. Attach to ONE process only - 'bt all' covers every thread." >>"$OUT"
fi

echo "output: $OUT"
