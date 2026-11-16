#!/usr/bin/env bash
#
# Build SuperTerm for Windows, self-contained, in a zip file. Run it in an
# MSYS2 UCRT64 shell (https://www.msys2.org):
#
#   packaging/windows/build-app.sh            # set up what's missing, build, package
#   packaging/windows/build-app.sh --no-zip   # stop at dist/SuperTerm
#
# What it needs it installs with pacman: the compiler, CMake, Ninja, Qt 6,
# yaml-cpp and zlib (each skipped once there).
#
# The result is dist/SuperTerm, a folder with SuperTerm.exe and every DLL it
# needs, and dist/SuperTerm-<version>-x64.zip of it. It needs Windows 10
# 1809 or later (as Qt 6 does, and the pseudoconsole).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build-release"
DIST="$ROOT/dist"
MAKE_ZIP=1

for arg in "$@"; do
    case "$arg" in
    --no-zip) MAKE_ZIP=0 ;;
    -h | --help)
        sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "unknown option: $arg" >&2
        exit 2
        ;;
    esac
done

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() {
    echo "error: $*" >&2
    exit 1
}

[ "${MSYSTEM:-}" = UCRT64 ] || die "run this in an MSYS2 UCRT64 shell (MSYSTEM is '${MSYSTEM:-unset}')"

step "Packages"
prefix=mingw-w64-ucrt-x86_64
pacman -S --needed --noconfirm \
    $prefix-gcc $prefix-cmake $prefix-ninja \
    $prefix-qt6-base $prefix-qt6-svg $prefix-qt6-tools \
    $prefix-yaml-cpp $prefix-zlib zip

step "Build"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSUPERTERM_BUILD_QML=OFF
cmake --build "$BUILD" --target superterm

version="$(sed -n 's/^ *VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -1)"
app="$DIST/SuperTerm"

step "Bundle Qt and the other libraries"
rm -rf "$app"
mkdir -p "$app"
cp "$BUILD/SuperTerm.exe" "$app/"
# Qt's libraries and plugins, by its own tool; the rest (the compiler's
# runtime, yaml-cpp, zlib, what Qt itself uses) by what the loader would find.
windeployqt6 --release --no-translations --no-system-d3d-compiler --no-opengl-sw \
    --compiler-runtime "$app/SuperTerm.exe" >/dev/null
copied=1
while [ "$copied" = 1 ]; do   # (until nothing new: the copies have needs too)
    copied=0
    while read -r dll; do
        if [ ! -e "$app/$(basename "$dll")" ]; then
            cp "$dll" "$app/"
            copied=1
        fi
    done < <(find "$app" \( -name '*.exe' -o -name '*.dll' \) -print0 | xargs -0 ldd 2>/dev/null \
        | awk '$3 ~ /^\/ucrt64\// {print $3}' | sort -u)
done

step "Check nothing outside the folder is needed"
# Run from the folder, with only Windows on the path, every DLL must be found
# there or be the system's.
outside="$(cd "$app" && PATH="/c/Windows/System32:/c/Windows" /usr/bin/ldd SuperTerm.exe \
    | grep -v -i '/c/windows/' | grep -v "$(cygpath -u "$app" | sed 's/[][\.*^$]/\\&/g')" || true)"
[ -z "$outside" ] || die "the program still needs files outside its folder:
$outside"
echo "  all self-contained"

if [ "$MAKE_ZIP" = 1 ]; then
    step "Zip"
    zipfile="$DIST/SuperTerm-$version-x64.zip"
    rm -f "$zipfile"
    (cd "$DIST" && zip -q -r "$zipfile" SuperTerm)
    echo "  $zipfile"
fi

step "Done"
du -sh "$app"
echo "  run it: $(cygpath -w "$app/SuperTerm.exe")"
