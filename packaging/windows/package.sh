#!/usr/bin/env sh
# Makes dist/OmaZettel-<version>-windows-x64/ and a .zip of it: OmaZettel
# with everything it needs to run on a Windows computer without MSYS2.
# Run it in MSYS2's UCRT64 shell after bin/build.
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
BUILD="$ROOT/build"
VERSION="$(sed -n 's/^#define OMAZETTEL_VERSION "\(.*\)"/\1/p' "$ROOT/src/main.cpp")"
NAME="OmaZettel-$VERSION-windows-x64"
OUT="$ROOT/dist/$NAME"
PREFIX="${MINGW_PREFIX:-/ucrt64}"

for tool in windeployqt6 windeployqt-qt6 windeployqt; do
  if command -v "$tool" >/dev/null 2>&1; then DEPLOY="$tool"; break; fi
done
if [ -z "${DEPLOY:-}" ]; then
  echo "windeployqt is required (pacman -S mingw-w64-ucrt-x86_64-qt6-tools)." >&2
  exit 1
fi

rm -rf "$OUT" "$ROOT/dist/$NAME.zip"
mkdir -p "$OUT"
cp "$BUILD/omazettel.exe" "$BUILD/omazettel.com" "$OUT/"
if [ -f "$BUILD/omazettel-pdf.dll" ]; then cp "$BUILD/omazettel-pdf.dll" "$OUT/"; fi
cp "$ROOT/LICENSE" "$ROOT/THIRD-PARTY-NOTICES.md" "$OUT/"
mkdir -p "$OUT/licenses" "$OUT/skills/omazettel"
cp "$ROOT/LICENSES/"* "$OUT/licenses/"
cp "$ROOT/skills/omazettel/SKILL.md" "$OUT/skills/omazettel/"
cp "$ROOT/packaging/windows/README.txt" "$OUT/"

# Qt's libraries, plugins and QML modules.
"$DEPLOY" --release --no-translations --qmldir "$ROOT/src" --dir "$OUT" "$OUT/omazettel.exe"
if [ -f "$OUT/omazettel-pdf.dll" ]; then
  "$DEPLOY" --release --no-translations --dir "$OUT" "$OUT/omazettel-pdf.dll"
fi

# Everything else they load from MSYS2 (the compiler's runtime, ICU, Poppler
# and its image libraries, and so on), until nothing new turns up.
while :; do
  added=0
  for file in $(find "$OUT" -name '*.exe' -o -name '*.dll'); do
    for dll in $(ldd "$file" 2>/dev/null | awk '{print $3}' | grep -i "^$PREFIX/bin/" || true); do
      base="$(basename "$dll")"
      if [ ! -f "$OUT/$base" ]; then
        cp "$dll" "$OUT/"
        added=1
      fi
    done
  done
  [ "$added" = 0 ] && break
done

(cd "$ROOT/dist" && if command -v zip >/dev/null 2>&1; then zip -qr "$NAME.zip" "$NAME"; else
  powershell -NoProfile -Command "Compress-Archive -Path '$NAME' -DestinationPath '$NAME.zip'"; fi)
echo "Made $OUT and dist/$NAME.zip"
