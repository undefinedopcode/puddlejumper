#!/usr/bin/env bash
#
# Build PuddleJumper.app on macOS, self-contained, in a disk image:
#
#   packaging/macos/build-app.sh            # set up what's missing, build, package
#   packaging/macos/build-app.sh --no-dmg   # stop at dist/PuddleJumper.app
#
# What it needs, it sets up (each step is skipped once done):
#   - Apple's command line tools (asks to install them; not the whole Xcode)
#   - Miniforge, in ~/miniforge3 (or $MINIFORGE)
#   - the conda environment in packaging/macos/environment.yml
#
# The app is built for this Mac's architecture, signed ad hoc (enough to run
# here, and on other Macs once let through Gatekeeper; not notarised), and
# lands in dist/: PuddleJumper.app and PuddleJumper-<version>-<arch>.dmg.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MINIFORGE="${MINIFORGE:-$HOME/miniforge3}"
ENV_NAME="puddlejumper"
BUILD="$ROOT/build-release"
DIST="$ROOT/dist"
MAKE_DMG=1

for arg in "$@"; do
    case "$arg" in
    --no-dmg) MAKE_DMG=0 ;;
    -h | --help)
        sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
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

[ "$(uname)" = Darwin ] || die "this builds the macOS app; run it on a Mac"

step "Command line tools"
if ! xcode-select -p >/dev/null 2>&1; then
    xcode-select --install || true
    die "install Apple's command line tools from the dialog, then run this again"
fi
xcode-select -p

step "Miniforge"
if [ ! -x "$MINIFORGE/bin/conda" ]; then
    installer="$(mktemp -d)/miniforge.sh"
    curl -fsSL -o "$installer" \
        "https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-MacOSX-$(uname -m).sh"
    # -u: the folder may be there already, with only a package cache in it
    # (restored by CI, say).
    bash "$installer" -b -u -p "$MINIFORGE"
fi
# (Not "source .../activate", which would take this script's arguments.)
eval "$("$MINIFORGE/bin/conda" shell.bash hook)"
conda --version

step "Conda environment '$ENV_NAME'"
if conda env list | awk '{print $1}' | grep -qx "$ENV_NAME"; then
    conda env update -q -n "$ENV_NAME" -f "$ROOT/packaging/macos/environment.yml"
else
    conda env create -q -n "$ENV_NAME" -f "$ROOT/packaging/macos/environment.yml"
fi
# Web tabs need Qt WebEngine, which conda-forge builds for Apple Silicon only:
# an Intel Mac's app goes without them.
if [ "$(uname -m)" = arm64 ]; then
    conda install -q -y -n "$ENV_NAME" -c conda-forge qt6-webengine
else
    echo "  (no Qt WebEngine for Intel Macs on conda-forge: no web tabs)"
fi
conda activate "$ENV_NAME"

step "Build"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$CONDA_PREFIX" -DPUDDLEJUMPER_BUILD_QML=OFF
cmake --build "$BUILD" --target puddlejumper

version="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' \
    "$BUILD/PuddleJumper.app/Contents/Info.plist")"
arch="$(uname -m)"

step "Bundle Qt and the other libraries"
rm -rf "$DIST/PuddleJumper.app"
mkdir -p "$DIST"
cp -R "$BUILD/PuddleJumper.app" "$DIST/"
# The libraries come from the environment, by rpath; macdeployqt copies them
# in, Qt's plugins with them, and points everything at the copies.
deployqt="$(command -v macdeployqt6 || command -v macdeployqt || echo "$CONDA_PREFIX/lib/qt6/bin/macdeployqt")"
"$deployqt" "$DIST/PuddleJumper.app" -libpath="$CONDA_PREFIX/lib" -verbose=1

step "Check nothing outside the app is needed"
# Every Mach-O file may only use the app's own copies and the system's.
bad=0
while IFS= read -r -d '' file; do
    file -b "$file" | grep -q Mach-O || continue
    while read -r dep; do
        case "$dep" in
        /usr/lib/* | /System/* | @executable_path/* | @loader_path/* | @rpath/*) ;;
        *)
            echo "  $file -> $dep"
            bad=1
            ;;
        esac
    done < <(otool -L "$file" | tail -n +2 | awk '{print $1}')
done < <(find "$DIST/PuddleJumper.app" -type f -print0)
# @rpath ones must resolve inside: the executable's rpaths may only point in.
if otool -l "$DIST/PuddleJumper.app/Contents/MacOS/PuddleJumper" | awk '/LC_RPATH/{getline; getline; print $2}' \
    | grep -v '^@' | grep -q .; then
    echo "  the executable still has rpaths outside the app:"
    otool -l "$DIST/PuddleJumper.app/Contents/MacOS/PuddleJumper" | awk '/LC_RPATH/{getline; getline; print "    " $2}'
    bad=1
fi
[ "$bad" = 0 ] || die "the app still refers to files outside it (above)"
echo "  all self-contained"

step "Sign (ad hoc)"
codesign --force --deep --sign - "$DIST/PuddleJumper.app"
codesign --verify --deep --strict "$DIST/PuddleJumper.app"
echo "  signed"

if [ "$MAKE_DMG" = 1 ]; then
    step "Disk image"
    dmg="$DIST/PuddleJumper-$version-$arch.dmg"
    staging="$(mktemp -d)"
    cp -R "$DIST/PuddleJumper.app" "$staging/"
    ln -s /Applications "$staging/Applications"   # drag the app onto it to install
    rm -f "$dmg"
    hdiutil create -quiet -volname "PuddleJumper $version" -srcfolder "$staging" \
        -fs HFS+ -format UDZO "$dmg"
    rm -rf "$staging"
    echo "  $dmg"
fi

step "Done"
du -sh "$DIST/PuddleJumper.app"
echo "  run it: open \"$DIST/PuddleJumper.app\""
