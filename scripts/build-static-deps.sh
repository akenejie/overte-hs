#!/usr/bin/env bash
#
# Provision the static dependencies and configure the headless overte-server
# build entirely inside this project directory.
#
# Everything is kept under <project>/deps/<platform>/ and <project>/build/:
#   deps/linux/qt5-static        - static Qt 5.15.13 (QtCore/QtGui/QtNetwork)
#   deps/linux/onetbb-static     - static oneTBB 2021.10.0
#   build/                       - Conan toolchain + CMake build tree
#
# Nothing outside the project directory is written (only read, e.g. the Conan
# package cache in ~/.conan2 and a temporary download dir under /tmp).
#
# Usage:
#   scripts/build-static-deps.sh        # build missing deps, then configure
#   scripts/build-static-deps.sh --skip-deps   # only re-run conan + cmake
#
# Environment:
#   OVERTE_BUILD_JOBS=<n>   override the make/cmake job count (default: nproc).
#                           Use a small value (e.g. 2-4) under QEMU emulation.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# --- platform ---------------------------------------------------------------
case "$(uname -s)" in
    Linux)  PLATFORM="linux" ;;
    Darwin) PLATFORM="macos" ;;
    *)      echo "error: unsupported platform $(uname -s) (only linux/macos handled here)" >&2; exit 1 ;;
esac

DEPS_DIR="$PROJECT_ROOT/deps/$PLATFORM"
BUILD_DIR="$PROJECT_ROOT/build"
QT_PREFIX="$DEPS_DIR/qt5-static"
TBB_PREFIX="$DEPS_DIR/onetbb-static"
TBB_DIR="$TBB_PREFIX/lib/cmake/TBB"

CONAN_PROFILE_ARGS=(
    -pr:h=default -pr:b=default
    -o headless=True -o qt_source=system
    -o openssl*:shared=False
)

CMAKE_ARGS=(
    -S "$PROJECT_ROOT" -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/generators/conan_toolchain.cmake"
    -DCMAKE_PREFIX_PATH="$QT_PREFIX;$TBB_PREFIX"
    -DTBB_DIR="$TBB_DIR"
    -DBUILD_SHARED_LIBS=OFF
    -DOVERTE_BUILD_SERVER=ON -DOVERTE_HEADLESS=ON
    -DOVERTE_SERVER_STRIP=ON
    -DCMAKE_SKIP_RPATH=ON
)

SKIP_DEPS=0
for a in "$@"; do
    case "$a" in
        --skip-deps) SKIP_DEPS=1 ;;
        -h|--help) sed -n '1,15p' "$0"; exit 0 ;;
        *) echo "error: unknown option $a" >&2; exit 1 ;;
    esac
done

require() {
    command -v "$1" >/dev/null 2>&1 || { echo "error: '$1' is required (see BUILD_STATIC.md)" >&2; exit 1; }
}
require g++
require make
require cmake
require curl
require python3
require conan

echo "==> project:   $PROJECT_ROOT"
echo "==> platform:  $PLATFORM"
echo "==> deps dir:  $DEPS_DIR"

# --- 1. static Qt -----------------------------------------------------------
if [ "$SKIP_DEPS" -eq 0 ] && [ ! -d "$QT_PREFIX" ]; then
    QT_VER=5.15.13
    QT_SRC="qtbase-everywhere-src-$QT_VER"
    QT_TAR="qtbase-everywhere-opensource-src-$QT_VER.tar.xz"
    WORK="$(mktemp -d /tmp/overte-qt.XXXXXX)"
    trap 'rm -rf "$WORK"' EXIT

    echo "==> building static Qt $QT_VER (first run; takes a while)..."
    cd "$WORK"
    curl -sL -o "$QT_TAR" \
        "https://download.qt.io/archive/qt/5.15/$QT_VER/submodules/$QT_TAR"
    tar xf "$QT_TAR"
    cd "$QT_SRC"
    # -prefix / bakes the neutral "qt_prfxpath=/" into QtCore (no machine path);
    # INSTALL_ROOT relocates the actual files into the project's deps dir.
    # QtGui (QPainter/QImage) is required by the headless server even though no
    # windowing is used; freetype/harfbuzz are built from Qt's bundled copies so
    # no system graphics dependencies are needed. OpenGL/X11/SSL are disabled.
    QT_CONFIGURE_ARGS=(
        -prefix / -opensource -confirm-license -static -release
        -no-openssl -no-dbus -no-glib -no-icu -no-pch
        -no-xcb -no-opengl -no-xkbcommon
        --pcre=qt --no-feature-zstd
        -nomake examples -nomake tests -nomake tools
        -no-feature-concurrent -no-feature-sql
    )
    if [ "$PLATFORM" = "macos" ]; then
        # macOS builds Qt as frameworks even in static builds; the headless
        # server links plain .a libraries, so install non-framework instead.
        QT_CONFIGURE_ARGS+=( -no-framework )
    fi
    ./configure "${QT_CONFIGURE_ARGS[@]}"
    make -j"${OVERTE_BUILD_JOBS:-$(nproc)}"
    make install INSTALL_ROOT="$QT_PREFIX"

    # Fix the .prl files produced by a "-prefix /" build:
    #  - the QT_INSTALL_LIBS property is emitted doubled ($$[QT_INSTALL_LIBS]$$[QT_INSTALL_LIBS]),
    #    which CMake expands to "<prefix>/lib/<prefix>/lib/..."; collapse it to one placeholder
    #  - the Qt library names lose their "lib" prefix; restore it
    #  - point any -lpcre2-16 at the bundled static libqtpcre2.a
    find "$QT_PREFIX" -name "*.prl" -exec perl -i -pe '
        s/\$\$\[QT_INSTALL_LIBS\]\$\$\[QT_INSTALL_LIBS\]/\$\$[QT_INSTALL_LIBS]/g;
        s/\$\$\[QT_INSTALL_LIBS\](Qt5[A-Za-z]*|qtpcre2)\.a/\$\$[QT_INSTALL_LIBS]lib$1.a/g;
        s/-lpcre2-16/\$\$[QT_INSTALL_LIBS]libqtpcre2.a/g
    ' {} +
    echo "==> static Qt installed to $QT_PREFIX"
else
    [ "$SKIP_DEPS" -eq 0 ] && [ -d "$QT_PREFIX" ] && echo "==> Qt already present at $QT_PREFIX"
fi

# --- 2. static oneTBB -------------------------------------------------------
if [ "$SKIP_DEPS" -eq 0 ] && [ ! -d "$TBB_PREFIX" ]; then
    TBB_VER=2021.10.0
    TBB_SRC="oneTBB-$TBB_VER"
    WORK="$(mktemp -d /tmp/overte-tbb.XXXXXX)"
    trap 'rm -rf "$WORK"' EXIT

    echo "==> building static oneTBB $TBB_VER (first run)..."
    cd "$WORK"
    curl -sL -o oneTBB.tar.gz \
        "https://github.com/oneapi-src/oneTBB/archive/refs/tags/v$TBB_VER.tar.gz"
    tar xzf oneTBB.tar.gz
    cd "$TBB_SRC"
    cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DTBB_TEST=OFF -DTBB_STRICT=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_CXX_STANDARD=17 \
        -DCMAKE_INSTALL_PREFIX="$TBB_PREFIX"
    cmake --build build -j"${OVERTE_BUILD_JOBS:-$(nproc)}"
    cmake --install build
    echo "==> static oneTBB installed to $TBB_PREFIX"
else
    [ "$SKIP_DEPS" -eq 0 ] && [ -d "$TBB_PREFIX" ] && echo "==> oneTBB already present at $TBB_PREFIX"
fi

# --- 3. Conan toolchain -----------------------------------------------------
echo "==> running conan install (toolchain into $BUILD_DIR/generators)..."
conan profile detect --force >/dev/null 2>&1 || true
( cd "$PROJECT_ROOT" && conan install . "${CONAN_PROFILE_ARGS[@]}" --build=missing --output-folder="$BUILD_DIR" )

# --- 4. CMake configure -----------------------------------------------------
echo "==> configuring CMake..."
cmake "${CMAKE_ARGS[@]}"

cat <<EOF

Build is configured. Build the single binary with:

    cmake --build "$BUILD_DIR" --target overte-server -j\$(nproc)

Binary: $BUILD_DIR/overte-server/overte-server
EOF
