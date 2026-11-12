#!/usr/bin/env bash
# s11 gate: the user guide (README.md) has to be followable by someone who is not on this
# machine and not a developer of this repo. Three checks, all mechanical:
#
#   1. every repo path README names - in a code block or in `inline code` - exists here,
#      so the guide can never send a reader to a script that was renamed or deleted;
#   2. every http(s) link answers 200 or 3xx to `curl -sI`, and every local markdown link
#      resolves to a file in this repo;
#   3. no personal paths (/home/rhy, /media/rhy) anywhere in README: a guide with this
#      machine's paths in it is not a guide.
#
# Usage: bash scripts/check-readme.sh [README.md]
# Ends with "All checks passed." like scripts/check.sh, which this gate does not touch.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
README="${1:-README.md}"
mkdir -p build/tmp
fail=0
ok(){ echo "ok   $*"; }; bad(){ echo "FAIL $*"; fail=1; }
done_(){ [ $fail = 0 ] && echo "All checks passed." || echo "GATE FAILED"; exit $fail; }

[ -f "$README" ] || { bad "$README does not exist"; done_; }

# ---------------------------------------------------------------- 0. sections the guide must have
for h in "Requirements" "Step 1" "Step 2" "Play" "Troubleshooting" "Uninstall" "Credits"; do
  grep -qiE "^#{1,3} .*$h" "$README" && ok "section: $h" || bad "section '$h' is missing from $README"
done

# ------------------------------------------------- 1. repo paths named in code blocks / inline code
# Placeholders (<game>, <steam>, <name>), generated or staging trees (build/, vendor/) and
# anything belonging to the game folder are not repo paths and are not checked here.
paths=$(python3 - "$README" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
blocks = re.findall(r"```[^\n]*\n(.*?)```", src, re.S)
prose  = re.sub(r"```.*?```", "", src, flags=re.S)
tokens = []
for b in blocks:
    for line in b.splitlines():
        line = re.sub(r"(?<!\\)#.*$", "", line)
        tokens += line.split()
tokens += re.findall(r"`([^`\n]+)`", prose)
TOP = ("apply.sh", "deploy.sh", "README.md", "THIRD_PARTY.md",
       "scripts/", "tools/", "docs/", "config/", "plugin/", "tests/", "launcher/", "deps/", "data/")
seen, out = set(), []
for t in tokens:
    t = t.strip("`\"'(),;")
    if not t or "<" in t or "*" in t or "://" in t:
        continue
    p = t[2:] if t.startswith("./") else t
    if p.startswith("~") or p.startswith("/"):
        continue
    if p.split("/")[0] in ("build", "vendor", "OLD"):
        continue
    if p.startswith(TOP) and p not in seen:
        seen.add(p); out.append(p)
print("\n".join(out))
PY
)
n=0
while IFS= read -r p; do
  [ -n "$p" ] || continue
  n=$((n+1))
  [ -e "$p" ] && ok "$README -> $p exists" || bad "$README points at '$p', which is not in the repo"
done <<< "$paths"
[ "$n" -gt 0 ] && ok "$n repo paths checked" || bad "no repo paths found in $README - did the extraction break?"

# ---------------------------------------------------------------- 2a. local markdown links
locals=$(python3 - "$README" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
out = []
for target in re.findall(r"\[[^\]]*\]\(([^)\s]+)\)", src):
    if "://" in target or target.startswith("#") or target.startswith("mailto:"):
        continue
    out.append(target.split("#")[0])
print("\n".join(x for x in out if x))
PY
)
nl=0
while IFS= read -r l; do
  [ -n "$l" ] || continue
  nl=$((nl+1))
  [ -e "$l" ] && ok "local link -> $l" || bad "local link '$l' does not resolve"
done <<< "$locals"
ok "$nl local link(s) checked"

# ---------------------------------------------------------------- 2b. every http(s) link: curl -sI 200/3xx
urls=$(python3 - "$README" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
urls = re.findall(r"https?://[^\s<>()\"'`]+", src)
clean = []
for u in urls:
    u = u.rstrip(".,;:!?'\"`")
    if u not in clean:
        clean.append(u)
print("\n".join(clean))
PY
)
nu=0; nbad=0
while IFS= read -r u; do
  [ -n "$u" ] || continue
  nu=$((nu+1))
  code=$(timeout 60 curl -sI -o /dev/null -w '%{http_code}' "$u" 2>/dev/null)
  case "$code" in
    2??) ok "HTTP $code  $u" ;;
    3??) ok "HTTP $code  $u (redirect; follows to $(timeout 60 curl -sIL -o /dev/null -w '%{http_code}' "$u" 2>/dev/null))" ;;
    *)   bad "HTTP ${code:-none} $u"; nbad=$((nbad+1)) ;;
  esac
done <<< "$urls"
[ "$nu" -gt 0 ] && ok "$nu link(s) checked, $nbad bad" || bad "no links found in $README - did the extraction break?"

# ---------------------------------------------------------------- 3. no personal paths
if grep -n '/home/rhy\|/media/rhy' "$README" >build/tmp/readme-personal-paths.txt 2>/dev/null && [ -s build/tmp/readme-personal-paths.txt ]; then
  bad "personal paths in $README:"; sed 's/^/       /' build/tmp/readme-personal-paths.txt
else
  ok "no personal paths (/home/rhy, /media/rhy) in $README"
fi

done_
