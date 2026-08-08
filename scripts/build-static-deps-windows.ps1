# build-static-deps-windows.ps1
#
# Provision the static dependencies and configure the headless overte-server
# build for Windows using the MSVC toolchain (production build). Mirrors
# scripts/build-static-deps.sh for Linux/macOS; the output is a single
# self-contained overte-server.exe with no DLLs to copy and no installer.
#
# Everything is kept under <project>/deps/win/ and <project>/build/:
#   deps/win/qt5-static        - static Qt 5.15.13 (QtCore/QtGui/QtNetwork)
#   deps/win/onetbb-static     - static oneTBB 2021.10.0
#   build/                     - Conan toolchain + CMake build tree
#
# Requirements (see BUILD_STATIC.md):
#   Visual Studio 2022 Build Tools (MSVC x64), CMake, Python 3, Strawberry Perl,
#   Rust (rustup with the MSVC toolchain). Run from a plain PowerShell; the
#   script locates and activates the MSVC environment itself via vswhere, so it
#   also works from a "Developer PowerShell".
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts/build-static-deps-windows.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/build-static-deps-windows.ps1 -SkipDeps
#
# Params:
#   -Jobs <n>   jom build parallelism (default: logical processor count)

param(
    [switch]$SkipDeps,
    [int]$Jobs = 0
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root
$platformDir = Join-Path $root "deps\win"
$buildDir = Join-Path $root "build"
$qtPrefix = Join-Path $platformDir "qt5-static"
$tbbPrefix = Join-Path $platformDir "onetbb-static"
$toolsDir = Join-Path $buildDir "tools"
if ($Jobs -le 0) { $Jobs = [System.Environment]::ProcessorCount }

Write-Host "==> project:   $root"
Write-Host "==> platform:  windows (MSVC)"
Write-Host "==> deps dir:  $platformDir"
Write-Host "==> jobs:      $Jobs"

# --- 0. MSVC environment ----------------------------------------------------
# If not already running in a Developer PowerShell (vswhere->vcvars64.bat),
# activate the MSVC x64 environment and import it into this process.
if (-not $env:VSCMD_ARG_TGT_ARCH) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere not found - install Visual Studio 2022 Build Tools with the C++ workload."
    }
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw "No Visual Studio with the VC.Tools.x86.x64 component found." }
    $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
    Write-Host "==> activating MSVC environment: $vcvars"
    $envDump = cmd /c "call `"$vcvars`" >nul 2>&1 && set"
    foreach ($line in $envDump) {
        if ($line -match '^([^=]*)=(.*)$') {
            if ($matches[1] -ne '') {
                [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
            }
        }
    }
}

function require-tool([string]$name) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        throw "'$name' is required (see BUILD_STATIC.md)."
    }
}
require-tool cl
require-tool cmake
require-tool curl
require-tool python
require-tool perl
require-tool conan
require-tool cargo

New-Item -ItemType Directory -Force -Path $toolsDir | Out-Null

# --- 1. static Qt (MSVC) ----------------------------------------------------
if (-not $SkipDeps -and -not (Test-Path $qtPrefix)) {
    $QtVer = "5.15.13"
    $QtTar = "qtbase-everywhere-opensource-src-$QtVer.tar.xz"
    $QtDir = "qtbase-everywhere-src-$QtVer"
    $work = Join-Path $env:TEMP "overte-qt-windows"
    if (Test-Path $work) { Remove-Item -Recurse -Force $work }
    New-Item -ItemType Directory -Force -Path $work | Out-Null

    Write-Host "==> building static Qt $QtVer for MSVC (first run; takes a while)..."
    Push-Location $work
    try {
        curl.exe -sL -o $QtTar "https://download.qt.io/archive/qt/5.15/$QtVer/submodules/$QtTar"
        tar -xf $QtTar   # bsdtar on Windows handles .tar.xz
        if (-not (Test-Path $QtDir)) { throw "failed to extract Qt sources" }
    } finally {
        Pop-Location
    }

    # jom: parallel nmake. Fall back to nmake if the download fails.
    $jom = Join-Path $toolsDir "jom.exe"
    if (-not (Test-Path $jom)) {
        try {
            $jomZip = Join-Path $work "jom.zip"
            curl.exe -sL -o $jomZip "https://download.qt.io/official_releases/jom/jom_1_1_4.zip"
            tar -xf $jomZip -C $toolsDir
        } catch {
            Write-Warning "jom download failed; using single-threaded nmake."
        }
    }
    $make = if (Test-Path $jom) { "jom" } else { "nmake" }

    Push-Location (Join-Path $work $QtDir)
    try {
        # -prefix / bakes the neutral "qt_prfxpath=/" into QtCore so no
        # machine-specific path ends up in the final exe (same trick as the
        # Linux build). INSTALL_ROOT relocates the files into deps/win.
        # win32-msvc is the generic MSVC mkspec and works with VS2022.
        & .\configure.bat `
            -prefix / -static -release -opensource -confirm-license `
            -platform win32-msvc `
            -no-openssl -no-dbus -no-glib -no-icu -no-pch -no-opengl `
            -no-feature-zstd -no-feature-concurrent -no-feature-sql `
            -qt-libpng -qt-libjpeg -qt-harfbuzz `
            --pcre=qt `
            -nomake examples -nomake tests -nomake tools
        if ($LASTEXITCODE -ne 0) { throw "Qt configure failed (exit $LASTEXITCODE)" }

        $env:INSTALL_ROOT = $qtPrefix
        $env:OVERTE_BUILD_JOBS = "$Jobs"
        if ($make -eq "jom") { & $jom "-j$Jobs" } else { & nmake }
        if ($LASTEXITCODE -ne 0) { throw "Qt make failed (exit $LASTEXITCODE)" }
        if ($make -eq "jom") { & $jom "-j$Jobs" install } else { & nmake install }
        if ($LASTEXITCODE -ne 0) { throw "Qt install failed (exit $LASTEXITCODE)" }
        Remove-Item Env:\INSTALL_ROOT -ErrorAction SilentlyContinue
    } finally {
        Pop-Location
    }

    # Fix the .prl files produced by a "-prefix /" build: the QT_INSTALL_LIBS
    # property is emitted doubled ($$[QT_INSTALL_LIBS]$$[QT_INSTALL_LIBS]),
    # which CMake expands to "<prefix>/lib/<prefix>/lib/..."; collapse it to a
    # single placeholder. (MSVC library names are already correct: Qt5Core.lib.)
    $prlFix = 's/\$\$\[QT_INSTALL_LIBS\]\$\$\[QT_INSTALL_LIBS\]/\$\$[QT_INSTALL_LIBS]/g'
    Get-ChildItem -Path $qtPrefix -Recurse -Filter *.prl | ForEach-Object {
        perl -i -pe $prlFix $_.FullName
    }
    Write-Host "==> static Qt installed to $qtPrefix"
} elseif (-not $SkipDeps) {
    Write-Host "==> Qt already present at $qtPrefix"
}

# --- 2. static oneTBB (MSVC) ------------------------------------------------
if (-not $SkipDeps -and -not (Test-Path $tbbPrefix)) {
    $TbbVer = "2021.10.0"
    $work = Join-Path $env:TEMP "overte-tbb-windows"
    if (Test-Path $work) { Remove-Item -Recurse -Force $work }
    New-Item -ItemType Directory -Force -Path $work | Out-Null

    Write-Host "==> building static oneTBB $TbbVer (first run)..."
    Push-Location $work
    try {
        curl.exe -sL -o oneTBB.tar.gz "https://github.com/oneapi-src/oneTBB/archive/refs/tags/v$TbbVer.tar.gz"
        tar -xf oneTBB.tar.gz
        Set-Location "oneTBB-$TbbVer"
        cmake -S . -B build `
            -DCMAKE_BUILD_TYPE=Release `
            -DBUILD_SHARED_LIBS=OFF `
            -DTBB_TEST=OFF -DTBB_STRICT=OFF `
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON `
            -DCMAKE_CXX_STANDARD=17 `
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5 `
            "-DCMAKE_INSTALL_PREFIX=$tbbPrefix"
        cmake --build build --config Release
        cmake --install build --config Release
    } finally {
        Pop-Location
    }
    Write-Host "==> static oneTBB installed to $tbbPrefix"
} elseif (-not $SkipDeps) {
    Write-Host "==> oneTBB already present at $tbbPrefix"
}

# --- 3. Conan toolchain ------------------------------------------------------
Write-Host "==> running conan install (toolchain into $buildDir/generators)..."
conan profile detect --force 2>$null | Out-Null
conan install . -pr:h=default -pr:b=default -o headless=True -o qt_source=system `
    -o openssl*:shared=False --build=missing --output-folder="$buildDir"
if ($LASTEXITCODE -ne 0) { throw "conan install failed (exit $LASTEXITCODE)" }

# --- 4. CMake configure ------------------------------------------------------
Write-Host "==> configuring CMake..."
cmake -S . -B build `
    "-DCMAKE_TOOLCHAIN_FILE=$buildDir\generators\conan_toolchain.cmake" `
    "-DCMAKE_PREFIX_PATH=$qtPrefix;$tbbPrefix" `
    "-DTBB_DIR=$tbbPrefix\lib\cmake\TBB" `
    -DCMAKE_BUILD_TYPE=Release `
    -DBUILD_SHARED_LIBS=OFF `
    -DOVERTE_BUILD_SERVER=ON -DOVERTE_HEADLESS=ON -DOVERTE_SERVER_STRIP=ON `
    "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$buildDir\overte-server"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit $LASTEXITCODE)" }

Write-Host ""
Write-Host "Build is configured. Build the single binary with:"
Write-Host ""
Write-Host "    cmake --build build --target overte-server --config Release"
Write-Host ""
Write-Host "Binary: $buildDir\overte-server\overte-server.exe"
