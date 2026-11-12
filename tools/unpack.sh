#!/bin/bash
# unpack.sh HOW ARCHIVE DESTDIR - put ONE archive's contents into ONE modloader folder.
#
#   HOW      unzip | unrar | 7z | copy   (copy = the file is not an archive, just place it)
#   ARCHIVE  the file tools/fetch-deps.sh verified by sha256
#   DESTDIR  modloader/<pack name>/ in the game folder
#
# Why a separate tool: ModLoader replaces game files by name at load time and two
# packs merged into one folder silently win/lose by priority (docs/CHARACTERS.md
# section 8.1), so "one archive, one folder" is a rule worth having in one place -
# and archives like to carry their own top-level folder, which would bury the pack
# one level too deep. This flattens exactly that one level, nothing else.
#
# exit: 0 unpacked, 1 the unpacker failed, 2 usage / unpacker not installed.
set -uo pipefail

usage() { echo "usage: unpack.sh unzip|unrar|7z|copy ARCHIVE DESTDIR" >&2; }

[ $# = 3 ] || { usage; exit 2; }
HOW="$1" ARCHIVE="$2" DEST="$3"
[ -f "$ARCHIVE" ] || { echo "unpack: no such archive: $ARCHIVE" >&2; exit 2; }
case "$HOW" in unzip|unrar|7z|copy) ;; *) usage; exit 2 ;; esac

need() { command -v "$1" >/dev/null 2>&1 || { echo "unpack: $1 is not installed (Debian/Ubuntu: sudo apt install $2)" >&2; exit 2; }; }

mkdir -p "$DEST" || { echo "unpack: cannot create $DEST" >&2; exit 1; }

if [ "$HOW" = copy ]; then
  cp -p "$ARCHIVE" "$DEST/" || { echo "unpack: could not copy $ARCHIVE into $DEST" >&2; exit 1; }
  echo "unpack: $(basename "$ARCHIVE") -> $DEST"
  exit 0
fi

# unpack into a scratch folder inside DEST, then move the contents up, so a pack
# that ships "My Pack/" as its only top level still ends up as modloader/My Pack/files
TMP="$DEST/.unpack-tmp"
rm -rf "$TMP"; mkdir -p "$TMP" || { echo "unpack: cannot create $TMP" >&2; exit 1; }
case "$HOW" in
  unzip) need unzip unzip; unzip -qq -o "$ARCHIVE" -d "$TMP" ;;
  unrar) need unrar unrar-free; unrar x -y -idq "$ARCHIVE" "$TMP/" ;;
  7z)    need 7z p7zip-full;   7z x -y -bso0 -bsp0 -o"$TMP" "$ARCHIVE" ;;
esac
rc=$?
if [ "$rc" != 0 ]; then
  echo "unpack: $HOW failed on $ARCHIVE (exit $rc) - $DEST left as it was apart from an empty scratch dir" >&2
  rm -rf "$TMP"; exit 1
fi

top="$(find "$TMP" -mindepth 1 -maxdepth 1)"
src="$TMP"
if [ "$(printf '%s\n' "$top" | wc -l)" = 1 ] && [ -d "$top" ]; then
  echo "unpack: $(basename "$ARCHIVE") has one top-level folder '$(basename "$top")' - flattening it"
  src="$top"
fi
find "$src" -mindepth 1 -maxdepth 1 -exec mv -f -t "$DEST" {} + || { echo "unpack: could not move $src into $DEST" >&2; exit 1; }
rm -rf "$TMP"
echo "unpack: $(basename "$ARCHIVE") -> $DEST ($(find "$DEST" -type f | wc -l) file(s))"
