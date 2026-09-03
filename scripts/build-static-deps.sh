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
#   OVERTE_QT_OPT=<flags>   extra ./configure flags for the static Qt build.
#                           Pass "-optimize-size" under slow QEMU armv7 CI.
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

# nproc is GNU-only; macOS provides sysctl instead.
if [ "$PLATFORM" = "macos" ]; then
    JOBS="$(sysctl -n hw.ncpu)"
else
    JOBS="$(nproc)"
fi

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
    # Try official mirrors first: download.qt.io can stall for hours from
    # GitHub Actions runners. QTSRC_URL can override the whole URL; otherwise
    # try a mirror list in order (berkeley OCF is fastest from US runners).
    QTSRC_URL="${QTSRC_URL:-https://mirrors.ocf.berkeley.edu/qt/archive/qt/5.15/$QT_VER/submodules/$QT_TAR}"
    QT_URLS=(
        "$QTSRC_URL"
        "https://download.qt.io/archive/qt/5.15/$QT_VER/submodules/$QT_TAR"
        "https://mirrors.tuna.tsinghua.edu.cn/qt/archive/qt/5.15/$QT_VER/submodules/$QT_TAR"
    )
    QT_DOWNLOADED=0
    for url in "${QT_URLS[@]}"; do
        echo "==> downloading Qt from $url"
        if curl -sfSL --connect-timeout 20 --max-time 600 --retry 2 -o "$QT_TAR" "$url"; then
            if [ "$(stat -c%s "$QT_TAR" 2>/dev/null || echo 0)" -gt 10000000 ]; then
                QT_DOWNLOADED=1
                break
            fi
            echo "==> $url returned a too-small file; trying next mirror"
        fi
    done
    if [ "$QT_DOWNLOADED" -ne 1 ]; then
        echo "ERROR: could not download Qt sources from any mirror" >&2
        exit 1
    fi
    tar xf "$QT_TAR"
    cd "$QT_SRC"
    # -prefix / bakes the neutral "qt_prfxpath=/" into QtCore (no machine path);
    # INSTALL_ROOT relocates the actual files into the project's deps dir.
    # QtGui (QPainter/QImage) is required by the headless server even though no
    # windowing is used; freetype/harfbuzz are built from Qt's bundled copies so
    # no system graphics dependencies are needed. OpenGL/X11/SSL are disabled.
    if [ "$PLATFORM" = "macos" ]; then
        # Qt 5.15's bundled libpng includes the legacy <fp.h>, which was
        # removed from the macOS SDK; use <math.h>, as in libpng 1.6.41.
        if grep -q 'include <fp\.h>' src/3rdparty/libpng/pngpriv.h; then
            perl -pi -e 's{\#\s*include <fp\.h>}{#          include <math.h>}' \
                src/3rdparty/libpng/pngpriv.h
        fi
    fi
    QT_CONFIGURE_ARGS=(
        -prefix / -opensource -confirm-license -static -release
        -no-openssl -no-dbus -no-glib -no-icu -no-pch
        -no-xcb -no-opengl -no-xkbcommon
        -qt-libpng -qt-libjpeg -qt-harfbuzz
        --pcre=qt --no-feature-zstd
        -nomake examples -nomake tests -nomake tools
        -no-feature-concurrent -no-feature-sql
    )
    if [ "$PLATFORM" = "macos" ]; then
        # macOS builds Qt as frameworks even in static builds; the headless
        # server links plain .a libraries, so install non-framework instead.
        QT_CONFIGURE_ARGS+=( -no-framework )
    fi
    if [ -n "${OVERTE_QT_OPT:-}" ]; then
        # QEMU-emulated armv7 compiles are 20-50x slower; lowering Qt's
        # optimization (e.g. -optimize-size / -Os) massively cuts compile
        # time. QtCore's non-hot code is not perf-relevant for a headless
        # server, so this is a free win on emulated CI only.
        QT_CONFIGURE_ARGS+=( "$OVERTE_QT_OPT" )
    fi
    ./configure "${QT_CONFIGURE_ARGS[@]}"
    make -j"${OVERTE_BUILD_JOBS:-$JOBS}"
    make install INSTALL_ROOT="$QT_PREFIX"

    # Fix the .prl files produced by a "-prefix /" build:
    #  - the QT_INSTALL_LIBS property is expanded doubled ($$[QT_INSTALL_LIBS]$$[QT_INSTALL_LIBS]),
    #    which CMake expands to "<prefix>/lib/<prefix>/lib/..."; collapse it to one placeholder
    #  - the Qt library names (Qt5*, plus the bundled 3rd-party ones such
    #    as qtlibpng/qtharfbuzz/qtlibjpeg/qtpcre2) lose their "lib" prefix;
    #    restore it so the installed libqtlibpng.a etc. are found
    #  - point any -lpcre2-16 at the bundled static libqtpcre2.a
    find "$QT_PREFIX" -name "*.prl" -exec perl -pi -e '
        s/\$\$\[QT_INSTALL_LIBS\]\$\$\[QT_INSTALL_LIBS\]/\$\$[QT_INSTALL_LIBS]/g;
        s/\$\$\[QT_INSTALL_LIBS\](?!lib)([A-Za-z0-9]+)\.a/\$\$[QT_INSTALL_LIBS]lib$1.a/g;
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

    # oneTBB cmake_minimum_required is 3.1; CMake on newer runners (e.g. the
    # macOS image) removed compatibility for projects below 3.5, so lift it.
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
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_INSTALL_PREFIX="$TBB_PREFIX"
    cmake --build build -j"${OVERTE_BUILD_JOBS:-$JOBS}"
    cmake --install build
    echo "==> static oneTBB installed to $TBB_PREFIX"
else
    [ "$SKIP_DEPS" -eq 0 ] && [ -d "$TBB_PREFIX" ] && echo "==> oneTBB already present at $TBB_PREFIX"
fi

# --- 3. Conan toolchain -----------------------------------------------------
echo "==> running conan install (toolchain into $BUILD_DIR/generators)..."
conan profile detect --force >/dev/null 2>&1 || true

# Some recipes (e.g. opus/1.5.2, which we need to build *from source* on
# 32-bit targets because there is no prebuilt x86/armv7 binary) declare a
# `tool_requires("cmake/[>=3.16 <5]")`. Conan then resolves the `cmake`
# package, but conancenter ships no cmake binary for the x86/armv7 32-bit
# architectures, so the resolved tool is "Invalid". The conan-recommended fix
# is to substitute the system cmake (always installed in our containers). We
# do that with two cooperating profile sections:
#   - [replace_tool_requires] pins whatever cmake the recipes request into one
#     known version (robust to which version conan would otherwise resolve).
#   - [platform_tool_requires] then maps that exact version to the cmake that
#     is already on the system PATH, so no conan cmake package is fetched at
#     all. NOTE: platform_tool_requires requires an EXACT (strict) version
#     match - a wildcard like `cmake/*` does NOT work.
mkdir -p "$BUILD_DIR"
PLATFORM_PROFILE="$BUILD_DIR/platform-tool-requires.ini"
cat > "$PLATFORM_PROFILE" <<'EOF'
[replace_tool_requires]
cmake/*: cmake/3.25.2
[platform_tool_requires]
cmake/3.25.2
EOF
PLATFORM_TOOL_ARGS=( -pr:h="$PLATFORM_PROFILE" -pr:b="$PLATFORM_PROFILE" )
# QEMU-emulated containers (i386/armv7) may confuse conan's arch detection
# (it can see the host's x86_64/aarch64), which would bake the wrong -m flag
# into the toolchain. When CONAN_ARCH is set (per CI matrix), override it on
# the install command line for both host and build contexts.
if [ -n "${CONAN_ARCH:-}" ]; then
    echo "==> overriding conan arch to $CONAN_ARCH"
    CONAN_PROFILE_ARGS+=( -s:a "arch=${CONAN_ARCH}" )
fi
# QEMU armv7 (CONAN_ARCH=armv7) builds opus/1.5.2 from source. Opus's CMake
# auto-detects arm_neon.h and compiles its NEON intrinsics, but conan's generic
# armv7 flags (plain -march=armv7) carry no FPU/float-ABI options, so GCC fails
# to inline the always_inline NEON builtins ("target specific option mismatch").
# Baking -mfpu=neon-vfpv4 -mfloat-abi=hard via [conf] into the conan toolchain
# (tools.build:cflags/cxxflags -> conan_toolchain.cmake) fixes that and is
# applied consistently to every conan-built package, avoiding ABI mismatches.
NEON_PROFILE_ARGS=()
if [ "${CONAN_ARCH:-}" = "armv7" ]; then
    NEON_PROFILE="$BUILD_DIR/neon.ini"
    cat > "$NEON_PROFILE" <<'EOF'
[conf]
tools.build:cflags=["-mfloat-abi=hard", "-mfpu=neon-vfpv4"]
tools.build:cxxflags=["-mfloat-abi=hard", "-mfpu=neon-vfpv4"]
EOF
    # -pr:a applies these flags to both the host and build profiles.
    NEON_PROFILE_ARGS=( -pr:a="$NEON_PROFILE" )
    echo "==> armv7: enabling NEON/FPU flags via $NEON_PROFILE"
fi
CONAN_PROFILE_ARGS+=( "${PLATFORM_TOOL_ARGS[@]}" "${NEON_PROFILE_ARGS[@]}" )
( cd "$PROJECT_ROOT" && conan install . "${CONAN_PROFILE_ARGS[@]}" --build=missing --output-folder="$BUILD_DIR" )

# --- 4. CMake configure -----------------------------------------------------
echo "==> configuring CMake..."
cmake "${CMAKE_ARGS[@]}"

cat <<EOF

Build is configured. Build the single binary with:

    cmake --build "$BUILD_DIR" --target overte-server -j\$(nproc)

Binary: $BUILD_DIR/overte-server/overte-server
EOF
