# Copyright (C) 2026 アケネＪ / Akenejie
# SPDX-License-Identifier: AGPL-3.0-only
#
# This file is part of Overte Headless-Server (overte-hs), an unofficial
# stripped-down, headless-only derivative of Overte. It is licensed under
# the GNU Affero General Public License v3.0 (see LICENSE-AGPL-3.0.txt and
# NOTICE in the repository root).

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
#   -Arch <arch>  target architecture: x64 (default) or arm64. Auto-detected
#               from the active MSVC env otherwise.

param(
    [switch]$SkipDeps,
    [int]$Jobs = 0,
    [string]$Arch = ""
)

$ErrorActionPreference = "Stop"

# PowerShell 7.3+ sets $PSNativeCommandUseErrorActionPreference=$true, which
# turns ANY native stderr output (progress bars, conan diagnostics, compiler
# chatter) into an error record - combined with $ErrorActionPreference='Stop'
# it makes well-behaved tools fail the run. conan/curl/nmake print to stderr
# routinely, so keep the classic behavior: native stderr is inert and we check
# $LASTEXITCODE for real failures.
$PSNativeCommandUseErrorActionPreference = $false

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root
$platformDir = Join-Path $root "deps\win"
$buildDir = Join-Path $root "build"
$qtPrefix = Join-Path $platformDir "qt5-static"
$tbbPrefix = Join-Path $platformDir "onetbb-static"
$toolsDir = Join-Path $buildDir "tools"
if ($Jobs -le 0) { $Jobs = [System.Environment]::ProcessorCount }

if (-not $Arch) {
    # Prefer the arch the active MSVC environment targets; fall back to the
    # host PROCESSOR_ARCHITECTURE (AMD64 on x64 machines, ARM64 on native arm64).
    $Arch = if ($env:VSCMD_ARG_TGT_ARCH) { $env:VSCMD_ARG_TGT_ARCH } else { $env:PROCESSOR_ARCHITECTURE }
}
switch ($Arch.ToLowerInvariant()) {
    { $_ -match "amd64|x64|^x$" } { $Arch = "x64"; break }
    { $_ -match "arm64|aarch64" } { $Arch = "arm64"; break }
    default { throw "Unsupported architecture '$Arch' (expected x64 or arm64)." }
}
$conanArch = if ($Arch -eq "arm64") { "armv8" } else { "x86_64" }
# Qt 5.15's ARM64 mkspec is named win32-arm64-msvc2017 (works with VS2022).
$qtPlatform = if ($Arch -eq "arm64") { "win32-arm64-msvc2017" } else { "win32-msvc" }

Write-Host "==> project:   $root"
Write-Host "==> platform:  windows (MSVC $Arch)"
Write-Host "==> deps dir:  $platformDir"
Write-Host "==> jobs:      $Jobs"

# --- 0. MSVC environment ----------------------------------------------------
# If not already running in a Developer PowerShell (vswhere->vcvars), activate
# the MSVC environment for the target arch and import it into this process.
if (-not $env:VSCMD_ARG_TGT_ARCH) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere not found - install Visual Studio 2022 Build Tools with the C++ workload."
    }
    if ($Arch -eq "arm64") {
        $comp = "Microsoft.VisualStudio.Component.VC.Tools.ARM64"
        # Native arm64 hosts use vcvarsall with 'arm64'; an x64 host cross-builds
        # with 'amd64_arm64'. Pick based on the actual host arch.
        $hostArch = $env:PROCESSOR_ARCHITECTURE
        $vcArg = if ($hostArch -match "ARM64") { "arm64" } else { "amd64_arm64" }
    } else {
        $comp = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
        $vcArg = "x64"
    }
    $vsPath = & $vswhere -latest -products * -requires $comp -property installationPath
    if (-not $vsPath) { throw "No Visual Studio with the '$comp' component found." }
    $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
    Write-Host "==> activating MSVC environment: $vcvars $vcArg"
    $envDump = cmd /c "call `"$vcvars`" $vcArg >nul 2>&1 && set"
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
        # The Qt source tarball on download.qt.io is often very slow (or stalls)
        # for hours from GitHub Actions runners. Use official mirrors first
        # (berkeley OCF is fastest from US runners), falling back to the other
        # mirrors. Use curl.exe directly (not Start-Process, whose exit code is
        # null here): --max-time caps the WHOLE transfer (Invoke-WebRequest's
        # -TimeoutSec only covers the response headers, so a stalled body never
        # times out), --speed-limit/--speed-time abort a stalled connection,
        # and -C - resumes a partial file across retries.
        $ProgressPreference = 'SilentlyContinue'  # large download, avoid progress overhead

        $QtUrls = @(
            "https://mirrors.ocf.berkeley.edu/qt/archive/qt/5.15/$QtVer/submodules/$QtTar"
            "https://download.qt.io/archive/qt/5.15/$QtVer/submodules/$QtTar"
            "https://mirrors.tuna.tsinghua.edu.cn/qt/archive/qt/5.15/$QtVer/submodules/$QtTar"
        )
        $ok = $false
        foreach ($url in $QtUrls) {
            Write-Host "==> downloading Qt from $url"
            foreach ($try in 1..3) {
                # The script runs with $ErrorActionPreference='Stop'; PS turns
                # native stderr into ErrorRecords and (7.3+) nonzero exits into
                # errors, so a plain call throws on curl's progress output.
                # Drop EAP to 'Continue' around the call only, so the exit code
                # is checked by us rather than thrown as a terminating error.
                # curl's progress bar goes to stderr, which PowerShell forwards
                # to the console/CI log as an ErrorRecord - with EAP=Continue
                # that is non-terminating and shows the live progress.
                # AVOID "2>&1 | ForEach-Object": the PS native-stderr pipeline is
                # a known source of hangs with large transfers, so let the bytes
                # flow straight through to the host instead.
                $oldEap = $ErrorActionPreference
                $ErrorActionPreference = 'Continue'
                curl.exe -fL --progress-bar --connect-timeout 20 --max-time 600 -C - `
                    --speed-limit 1024 --speed-time 30 --retry 2 --retry-delay 5 `
                    -o $QtTar $url
                $rc = $LASTEXITCODE
                $ErrorActionPreference = $oldEap
                if ($rc -eq 0 -and (Test-Path $QtTar) -and ((Get-Item $QtTar).Length -gt 10000000)) {
                    $ok = $true
                    break
                }
                Write-Warning "Qt source download attempt $try failed for $url (curl exit $rc)"
                Start-Sleep -Seconds 5
            }
            if ($ok) { break }
        }
        if (-not $ok) { throw "failed to download Qt sources from all mirrors" }
        Write-Host "==> Qt sources downloaded: $((Get-Item $QtTar).Length / 1MB) MB"
        Write-Host "==> extracting Qt sources (this can take a few minutes)..."
        # The Windows-bundled tar.exe (bsdtar) silently hangs on big .tar.xz
        # archives under CI, with zero output - hard to diagnose. python is a
        # required dependency, and its tarfile module reads .tar.xz natively, so
        # extract with it and print progress every 200 files. That both avoids
        # the hanging bsdtar and makes any real stall visible in the log.
        $pyExtract = @'
import sys, tarfile
t = tarfile.open(sys.argv[1], "r:xz")
members = t.getmembers()
n = len(members)
print(f"archive parsed: {n} members", flush=True)
for i, m in enumerate(members):
    t.extract(m, path=".")
    if (i + 1) % 50 == 0 or (i + 1) == n:
        print(f"extracted {i + 1}/{n} - {m.name}", flush=True)
t.close()
print("Qt source extraction complete", flush=True)
'@
        $pyFile = Join-Path $work "extract-qt.py"
        Set-Content -Path $pyFile -Value $pyExtract -Encoding utf8
        python $pyFile $QtTar
        if ($LASTEXITCODE -ne 0) { throw "Qt source extraction failed (exit $LASTEXITCODE)" }
        if (-not (Test-Path $QtDir)) { throw "failed to extract Qt sources (missing $QtDir)" }
        Write-Host "==> Qt sources extracted to $QtDir"
    } finally {
        Pop-Location
    }

    # jom: parallel nmake (essential - a serial nmake Qt build can take 4h+ on
    # a 2-core CI runner). Retry the download a few times; only fall back to
    # serial nmake as a last resort, with a loud warning.
    $jom = Join-Path $toolsDir "jom.exe"
    if (-not (Test-Path $jom)) {
        $jomZip = Join-Path $work "jom.zip"
        $ok = $false
        foreach ($try in 1..3) {
            try {
                Write-Host "==> downloading jom (parallel nmake)..."
                curl.exe -sSL --connect-timeout 20 --max-time 300 --retry 3 --retry-delay 5 `
                    -o $jomZip "https://download.qt.io/official_releases/jom/jom_1_1_4.zip"
                if ($LASTEXITCODE -ne 0) { throw "jom curl exit $LASTEXITCODE" }
                tar -xf $jomZip -C $toolsDir
                if (Test-Path $jom) { $ok = $true; break }
            } catch {
                Write-Warning "jom download attempt $try failed: $_"
                Start-Sleep -Seconds 5
            }
        }
        if (-not $ok) {
            Write-Warning "jom unavailable; building Qt serially with nmake (this will be very slow)."
        }
    }
    $make = if (Test-Path $jom) { "jom" } else { "nmake" }
    Write-Host "==> Qt build driver: $make"

    Push-Location (Join-Path $work $QtDir)
    try {
        # Qt 5.15's qmake does not recognize PROCESSOR_ARCHITECTURE_ARM64 and
        # reports QMAKE_HOST.arch=Unknown on a native ARM64 host. toolchain.prf
        # then builds "Unknown_arm64" as the vcvars arch and configure dies.
        # Teach qmake about arm64 (fixed upstream in Qt 6, not backported).
        $qmakeSrc = Join-Path $work (Join-Path $QtDir "qmake\library\qmakeevaluator.cpp")
        if ((Test-Path $qmakeSrc) -and
            -not (Select-String -Path $qmakeSrc -Quiet -SimpleMatch "PROCESSOR_ARCHITECTURE_ARM64")) {
            Write-Host "==> patching qmake for Windows ARM64 host detection"
            $armBlock = @(
                "# endif"
                "# ifdef PROCESSOR_ARCHITECTURE_ARM64"
                "    case PROCESSOR_ARCHITECTURE_ARM64:"
                '        archStr = ProString("arm64");'
                "        break;"
                "# endif"
                "    case PROCESSOR_ARCHITECTURE_INTEL:"
            ) -join "`n"
            $anchor = "# endif`n    case PROCESSOR_ARCHITECTURE_INTEL:"
            $qmakeText = Get-Content -Raw -Path $qmakeSrc
            if ($qmakeText.Contains($anchor)) {
                $qmakeText = $qmakeText.Replace($anchor, $armBlock)
                Set-Content -Path $qmakeSrc -Value $qmakeText -NoNewline -Encoding ascii
            } else {
                throw "qmake ARM64 patch failed: anchor not found in $qmakeSrc"
            }
        }
        # -prefix $qtPrefix installs straight into deps/win. Unlike the Linux
        # build we can NOT use the "-prefix / + INSTALL_ROOT" relocatability
        # trick: Windows needs a drive letter, qmake produces root-relative
        # install paths, and every install target dies with "The filename,
        # directory name, or volume label syntax is incorrect." Static Qt is
        # linked into overte-server.exe at build time, so no machine-specific
        # path ends up in the shipped binary regardless.
        # win32-msvc is the generic MSVC mkspec; win32-arm64-msvc2017 builds native
        # arm64 (Qt 5.15 supports Windows on ARM64 via this mkspec).
        & .\configure.bat `
            -prefix $qtPrefix -static -release -opensource -confirm-license `
            -platform $qtPlatform `
            -no-openssl -no-dbus -no-glib -no-icu -no-pch -no-opengl `
            -no-feature-zstd -no-feature-concurrent -no-feature-sql `
            -qt-libpng -qt-libjpeg -qt-harfbuzz `
            --pcre=qt `
            -nomake examples -nomake tests -nomake tools
        if ($LASTEXITCODE -ne 0) { throw "Qt configure failed (exit $LASTEXITCODE)" }

        $env:OVERTE_BUILD_JOBS = "$Jobs"
        if ($make -eq "jom") { & $jom "-j$Jobs" } else { & nmake }
        if ($LASTEXITCODE -ne 0) { throw "Qt make failed (exit $LASTEXITCODE)" }
        if ($make -eq "jom") { & $jom "-j$Jobs" install } else { & nmake install }
        if ($LASTEXITCODE -ne 0) { throw "Qt install failed (exit $LASTEXITCODE)" }
        Remove-Item Env:\INSTALL_ROOT -ErrorAction SilentlyContinue
    } finally {
        Pop-Location
    }

    # The "-prefix /" root build doubles $$[QT_INSTALL_LIBS] inside the .prl
    # files; with the real -prefix this no longer happens. Keep the fix anyway
    # as a no-op guard: it only rewrites the doubled form, never a valid path.
    $prlFix = 's/\$\$\[QT_INSTALL_LIBS\]\$\$\[QT_INSTALL_LIBS\]/\$\$[QT_INSTALL_LIBS]/g'
    Get-ChildItem -Path $qtPrefix -Recurse -Filter *.prl | ForEach-Object {
        perl -i -pe $prlFix $_.FullName
    }
    Write-Host "==> static Qt installed to $qtPrefix"
} elseif (-not $SkipDeps) {
    Write-Host "==> Qt already present at $qtPrefix"
}

# --- 2. static oneTBB (MSVC) ------------------------------------------------
$tbbConfigFile = "$tbbPrefix\lib\cmake\TBB\TBBConfig.cmake"
if (-not $SkipDeps -and -not (Test-Path $tbbConfigFile)) {
    if (Test-Path $tbbPrefix) {
        Write-Host "==> oneTBB cache incomplete (missing $tbbConfigFile); rebuilding"
        Remove-Item -Recurse -Force $tbbPrefix
    }
    $TbbVer = "2021.10.0"
    $work = Join-Path $env:TEMP "overte-tbb-windows"
    if (Test-Path $work) { Remove-Item -Recurse -Force $work }
    New-Item -ItemType Directory -Force -Path $work | Out-Null

    Write-Host "==> building static oneTBB $TbbVer (first run)..."
    Push-Location $work
    try {
        curl.exe -sL -o oneTBB.tar.gz "https://github.com/oneapi-src/oneTBB/archive/refs/tags/v$TbbVer.tar.gz"
        if ($LASTEXITCODE -ne 0) { throw "oneTBB download failed (exit $LASTEXITCODE)" }
        tar -xf oneTBB.tar.gz
        if (-not (Test-Path "oneTBB-$TbbVer")) { throw "oneTBB source extraction failed (missing oneTBB-$TbbVer)" }
        Set-Location "oneTBB-$TbbVer"
        $vsPlatform = if ($Arch -eq "arm64") { "ARM64" } else { "x64" }
        cmake -S . -B build `
            -A $vsPlatform `
            -DCMAKE_BUILD_TYPE=Release `
            -DBUILD_SHARED_LIBS=OFF `
            -DTBB_TEST=OFF -DTBB_STRICT=OFF `
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON `
            -DCMAKE_CXX_STANDARD=17 `
            "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" `
            "-DCMAKE_INSTALL_PREFIX=$tbbPrefix"
        if ($LASTEXITCODE -ne 0) { throw "oneTBB configure failed (exit $LASTEXITCODE)" }
        cmake --build build --config Release
        if ($LASTEXITCODE -ne 0) { throw "oneTBB build failed (exit $LASTEXITCODE)" }
        cmake --install build --config Release
        if ($LASTEXITCODE -ne 0) { throw "oneTBB install failed (exit $LASTEXITCODE)" }
    } finally {
        Pop-Location
    }
    if (-not (Test-Path "$tbbPrefix\lib\cmake\TBB\TBBConfig.cmake")) {
        throw "oneTBB install incomplete: missing $tbbPrefix\lib\cmake\TBB\TBBConfig.cmake"
    }
    Write-Host "==> static oneTBB installed to $tbbPrefix"
} elseif (-not $SkipDeps) {
    Write-Host "==> oneTBB already present at $tbbPrefix"
}

# --- 3. Conan toolchain ------------------------------------------------------
Write-Host "==> running conan install (toolchain into $buildDir/generators)..."
    # Do NOT redirect conan's stderr: under Windows PowerShell 5.1 each
    # redirected stderr line becomes an ErrorRecord which $ErrorActionPreference
    # 'Stop' escalates to a fatal error. Un-redirected native stderr just shows
    # in the log; conan's exit code is checked below for real failures.
    conan profile detect --force
    if ($LASTEXITCODE -ne 0) { throw "conan profile detect failed (exit $LASTEXITCODE)" }
conan install . -pr:h=default -pr:b=default -o headless=True -o qt_source=system `
    -o openssl*:shared=False --build=missing --output-folder="$buildDir" `
    -s:a arch=$conanArch
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
