<!--
SPDX-License-Identifier: Apache-2.0
-->

# Build a Fully Static Overte Headless Server

*Status: 2026-08-04*

This document describes how to produce **a single, copy-paste deployable binary** for the
complete Overte headless VR room stack (`domain-server`, `assignment-client` with the
audio/avatar/entity mixers). The binary depends only on the standard C/C++ runtime (`libc`,
`libstdc++`, `libgcc_s`, `libm`). No Qt runtime, no system libraries, no installer, no extra
files.

The build is **self-contained inside this project directory**: static Qt and oneTBB are built
from source into `<project>/deps/<platform>/`, and the Conan/CMake build tree lives in
`<project>/build/`. Nothing is written outside the project (only read, e.g. the Conan cache in
`~/.conan2` and a temporary download directory under `/tmp`). No build artifact embeds a
machine-specific path (`qt_prfxpath=/` is baked into QtCore).

The Linux build was verified on **Ubuntu 24.04 / GCC 13.3 / CMake 3.28 / Conan 2.x**. The
scripts also handle macOS; Windows needs the equivalent steps under MSVC (see
[Cross-platform](#cross-platform)).

## Result

A single `overte-server` binary runs the whole four-process stack as a multicall
(`busybox`-style) executable: `domain`, `audio`, `avatar` and `entity` subcommands dispatch to
the four applets, and `start` launches them as supervised children. It builds, starts, binds
all UDP ports and runs together:

```
$ ldd overte-server | grep -v vdso
	libm.so.6
	libstdc++.so.6
	libgcc_s.so.1
	libc.so.6
	/lib64/ld-linux-x86-64.so.2

$ ./overte-server start --domain-port 41302 --with-mixers \
                        --audio-port 41303 --avatar-port 41304 --entity-port 41305
overte-server: starting domain-server
overte-server: starting audio-mixer
overte-server: starting avatar-mixer
overte-server: starting entity-server
```

The `domain` applet serves its settings schema (`describe-settings.json`) from resources
embedded in the binary via a Qt resource file (`.qrc`), so no external `resources/` directory
is needed.

Binary size: `overte-server` ~348 MB with debug info, **~32 MB** with
`OVERTE_SERVER_STRIP=ON` (the default when using `scripts/build-static-deps.sh`).

## Overview

The stock Overte build links Qt and other dependencies dynamically via Conan. To make it
fully static, four shared dependencies had to be eliminated:

| Shared lib      | Origin              | Fix                                                        |
|-----------------|---------------------|------------------------------------------------------------|
| `libpcre2-16`   | Qt 5 Qt5Core        | Qt rebuilt with `--pcre=qt` (bundled `libqtpcre2.a`)        |
| `libzstd`       | Qt 5 Qt5Core        | Qt rebuilt with `--no-feature-zstd`                         |
| `libcrypto`     | system OpenSSL      | Conan `openssl/3.5.7` built statically                     |
| `libtbb`        | Conan `onetbb`      | oneTBB built statically from source                        |

## 1. One-shot build: `scripts/build-static-deps.sh`

The recommended way. It downloads and builds static Qt and oneTBB, runs `conan install` and
configures CMake:

```bash
scripts/build-static-deps.sh            # full run (first time; takes a while)
scripts/build-static-deps.sh --skip-deps   # only re-run conan + cmake
cmake --build build --target overte-server -j$(nproc)
```

What it does, in order:

1. **static Qt 5.15.13** → installed to `deps/<platform>/qt5-static/`

   Qt is built **with QtGui** (the headless server needs `QPainter`/`QImage` for entity
   image and avatar processing) but without any windowing: OpenGL, X11/xcb, and the
   platform plugins are disabled. FreeType/HarfBuzz are built from Qt's bundled copies when
   the system versions are absent, so no system graphics development packages are required.
   SSL, DBus, GLib, ICU are disabled to keep the toolchain small and dependency-free.

   ```bash
   ./configure \
     -prefix / \
     -opensource -confirm-license -static -release \
     -no-openssl -no-dbus -no-glib -no-icu -no-pch \
     -no-xcb -no-opengl -no-xkbcommon \
     --pcre=qt --no-feature-zstd \
     -nomake examples -nomake tests -nomake tools \
     -no-feature-concurrent -no-feature-sql
   make -j$(nproc)
   make install INSTALL_ROOT=<project>/deps/<platform>/qt5-static
   ```

   Notes:
   - `-prefix /` bakes the neutral `qt_prfxpath=/` into QtCore — **no build-machine path
     appears in the delivered binary**. `INSTALL_ROOT` relocates the actual files into the
     project's `deps/` directory; Qt's CMake config derives its prefix from the location of
     `Qt5Config.cmake`, so the install is fully relocatable.
   - Two further mechanisms keep machine paths out of the binary:
     - the top-level `CMakeLists.txt` adds `-ffile-prefix-map=<project>=.` so `__FILE__` /
       debug-info strings become relative instead of `/home/<user>/...` or `/mnt/...`;
     - the configure step sets `-DCMAKE_SKIP_RPATH=ON` so the Conan package-cache directory
       (e.g. `/home/<user>/.conan2/p/...`) is never recorded as a `DT_RUNPATH`.
   - The Qt source tarball is `qtbase-everywhere-opensource-src-5.15.13.tar.xz` (note the
     `opensource` segment; the `qtbase-everywhere-src-...` name 404s on download.qt.io). The
     extracted directory is `qtbase-everywhere-src-5.15.13`.
   - After install, the script patches the `.prl` files (both `lib/` and `plugins/`):
     - `-prefix /` makes qmake emit the `QT_INSTALL_LIBS` placeholder doubled
       (`$$[QT_INSTALL_LIBS]$$[QT_INSTALL_LIBS]Qt5Core.a`); CMake's prl processing
       expands that to `<prefix>/lib/<prefix>/lib/...`, so the pair is collapsed to a
       single placeholder and the missing `lib` prefix is restored
       (`$$[QT_INSTALL_LIBS]libQt5Core.a`). Without this, the link line contains
       doubled absolute paths and `ld` fails.
     - any `-lpcre2-16` is pointed at the bundled `libqtpcre2.a`.
   - The download URL and configure flags are verified to work; a clean rebuild from scratch
     was exercised with the `/mnt/Data/qt-static` legacy build tree removed.

2. **static oneTBB 2021.10.0** → installed to `deps/<platform>/onetbb-static/`

   Conan's `onetbb` recipe is hardcoded to `package_type = "shared-library"` (no `shared`
   option), so it cannot produce a static `libtbb.a`. The script builds oneTBB manually:

   ```bash
   cmake -S . -B build \
     -DCMAKE_BUILD_TYPE=Release \
     -DBUILD_SHARED_LIBS=OFF \
     -DTBB_TEST=OFF -DTBB_STRICT=OFF \
     -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
     -DCMAKE_CXX_STANDARD=17 \
     -DCMAKE_INSTALL_PREFIX=<project>/deps/<platform>/onetbb-static
   cmake --build build -j$(nproc)
   cmake --install build
   ```

   The controlling variable is the standard CMake `BUILD_SHARED_LIBS`; `-DTBB_BUILD_SHARED=OFF`
   alone does **not** produce a static build. This installs `libtbb.a`, `libtbbmalloc.a`,
   headers, and a CMake package providing the `TBB::tbb` target (same target the Overte
   `TargetTBB.cmake` macro links).

3. **Conan install** (headless):

   ```bash
   conan install . \
     -pr:h=default -pr:b=default \
     -o headless=True -o qt_source=system \
     -o openssl*:shared=False \
     --build=missing \
     --output-folder=build
   ```

   This produces the CMake toolchain in `build/generators/`. The `conanfile.py` headless
   branch requires a static OpenSSL (`openssl/3.5.7`) and drops the shared-only `onetbb`
   (provided manually in step 2).

4. **CMake configure:**

   ```bash
   cmake -S . -B build \
     -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_TOOLCHAIN_FILE=build/generators/conan_toolchain.cmake \
     -DCMAKE_PREFIX_PATH="<project>/deps/<platform>/qt5-static;<project>/deps/<platform>/onetbb-static" \
     -DTBB_DIR=<project>/deps/<platform>/onetbb-static/lib/cmake/TBB \
     -DBUILD_SHARED_LIBS=OFF \
     -DOVERTE_BUILD_SERVER=ON -DOVERTE_HEADLESS=ON \
     -DOVERTE_SERVER_STRIP=ON \
     -DCMAKE_SKIP_RPATH=ON
   ```

## 2. Source patches

These are local patches to the Overte source (the exact files in this working tree):

| File | Change |
|------|--------|
| `libraries/shared/src/Grab.cpp` | add `#include <QDataStream>` |
| `libraries/networking/src/AccountManager.cpp` | add `#include <QtNetwork/QSslConfiguration>`; guard `QSslConfiguration::defaultConfiguration()` with `#ifndef QT_NO_SSL` |
| `libraries/octree/src/OctreePersistThread.cpp` | add `#include <QDataStream>` |
| `domain-server/src/DomainServer.cpp` | add `#include <QDataStream>` |
| `domain-server/src/DomainServerSettingsManager.cpp` | guard two certificate/key sections with `#ifndef QT_NO_SSL` |
| `libraries/recording/src/recording/RecordingScriptingInterface.cpp` | guard the unused `#include <QtWidgets/QFileDialog>` with `#ifndef OVERTE_HEADLESS` |
| `libraries/shared/CMakeLists.txt` | add `target_tbb()` so the TBB include dir propagates to all consumers in headless mode |
| `cmake/init.cmake` | opt-in static build via `BUILD_SHARED_LIBS` |
| `CMakeLists.txt` | headless branch: `add_definitions(-DOVERTE_HEADLESS -DOVERTE_NO_QTWEBSOCKET)` so every target gets both defines |
| `libraries/shared/src/GLMHelpers.h` | declare `vec3 toGlm(const glm::u8vec3&)` (was defined but never declared) |
| `libraries/entities/src/EntityScriptingInterface.h` | guard `#include <QtQml/QJSValue>` / `QJSValueList` with `#ifndef OVERTE_HEADLESS` (headless Qt has no QtQml) |
| `assignment-client/src/octree/OctreeServer.h` | drop the headless guard on `#include <HTTPManager.h>` (the `compat/` stub provides it) |
| `assignment-client/src/octree/OctreeServer.cpp` | guard the `_httpManager(nullptr)` member initializer with `#ifndef OVERTE_HEADLESS` |
| `libraries/image/src/image/TextureProcessing.cpp` | move `convertToFloatFromPacked` / `convertToPackedFromFloat` outside the `NVTT_API` guard; add headless stubs for `convertImageToTexture` / `convertToTextureWithMips` / `convertToTexture` |
| `libraries/script-engine/src/ScriptManager.cpp` | guard `#include <QtConcurrent/QtConcurrentRun>`; `getLocalEntityScriptDetails` uses a synchronous `QFutureInterface<QVariant>` in headless; guard the `WebSocketClass.h` include |
| `libraries/script-engine/src/ScriptEngines.cpp` | guard `#include <QtConcurrent/QtConcurrent>`; `stopAllScripts` detaches a `std::thread` in headless |
| `libraries/script-engine/src/ScriptEngine.cpp` | add `#include <QThread>` |
| `libraries/entities/src/EntityScriptingInterface.cpp` | guard the (unused) `#include <QtConcurrent/QtConcurrentRun>` |
| `libraries/material-networking/src/material-networking/TextureCache.cpp` | guard the include; add a `dispatchToBackgroundThread` helper (`std::thread` in headless, `QtConcurrent::run` otherwise) and use it at the 3 call sites |
| `libraries/script-engine/src/WebSocketClass.h` / `WebSocketServerClass.h` | under `OVERTE_NO_QTWEBSOCKET` only forward-declare; otherwise define the real class |
| `assignment-client/src/Agent.cpp` / `src/scripts/EntityScriptServer.cpp` | guard the `WebSocketServerClass.h` include and the `WebSocketServer` constructor registration |
| `libraries/script-engine/CMakeLists.txt` | headless: `HEADER_FILE_ONLY TRUE SKIP_AUTOMOC TRUE` for the WebSocket and `src/v8/*` sources |
| `libraries/model-baker/CMakeLists.txt` + `src/model-baker/Baker.cpp` | headless: exclude `BuildDracoMeshTask.cpp`, move `target_draco()` into the else branch, guard the task use |
| `libraries/model-serializers/CMakeLists.txt` + `src/FBXSerializer_Mesh.cpp` + `src/model-networking/ModelCache.cpp` | headless: exclude `GLTFSerializer.cpp/h`, move `target_draco()`/`target_cgltf()` into else, guard draco decode and `addFormat(GLTFSerializer())` |
| `cmake/macros/AutoScribeShader.cmake` | `include(ConanToolsDirs.cmake)` guarded with `if(EXISTS ...)` so headless configures without the conan shader tooling |
| `libraries/shaders/CMakeLists.txt` + `src/shaders/ShadersHeadless.cpp` | headless: run `autoscribe_shader_libs(...)` (configure-time only; produces the full `ShaderEnums.h`) but compile the minimal `ShadersHeadless.cpp` instead of the scribe outputs. The `scribed_shaders`/`shadergen` targets are created but never built (nothing depends on them), so no glslang/scribe binaries are needed |
| `libraries/script-engine/src/HelperScriptEngine.{h,cpp}` | tolerate a null `newScriptEngine()` (headless has no V8): constructor skips the thread, `run()`/`runWithResult()` become no-ops |
| `domain-server/CMakeLists.txt` | copy `resources/` beside the binary in headless too (the settings schema default `metaverse.local_port=40102` lives there) |
| `ice-server/CMakeLists.txt` | restore the real build; headless: link `networking shared`, add the `compat/` include dir, keep OpenSSL, drop `embedded-webserver` (needs Qt SSL) |

Rationale:
- `QDataStream` / `QSslConfiguration` were previously pulled in transitively by QtGui headers;
  a static Qt without SSL exposes them as missing.
- Headless Qt is built `--no-feature-concurrent` and without QtWebSockets/QtQml, so every
  `QtConcurrent::run` / `WebSocket*` / `QtQml` use must be guarded.
- `embedded-webserver` compiles `HTTPSManager.cpp`/`HTTPSConnection.cpp` which need Qt SSL
  (`QSslError`); headless uses the `compat/HTTPManager.h` stubs instead.
- In non-headless builds `ShaderEnums.h` is generated from the shader sources at configure
  time and the scribe tools compile them at build time. In headless the enum generation is
  still run at configure time (pure CMake string processing — no tool needed) so that
  `gpu`/`render`/`procedural` (which link against the headless server) can resolve
  `shader::...` enum names, but the scribe/spirv artifacts are never produced.

## 3. Building just the binary

After step 1 you can iterate on the application sources only:

```bash
cmake --build build --target overte-server -j$(nproc)
```

Re-run `cmake --build build` after any CMakeLists change; the build regenerates automatically
when a `CMakeLists.txt` is newer.

### Shrinking the binary: `-DOVERTE_SERVER_STRIP=ON`

`overte-server/CMakeLists.txt` has an `OVERTE_SERVER_STRIP` option (default `OFF`) that adds
`-s` to the linker, stripping debug/annotation symbols. With debug info the binary is ~348 MB;
stripped it is **~32 MB**. `scripts/build-static-deps.sh` enables it by default.

```bash
cmake -S . -B build -DOVERTE_SERVER_STRIP=ON
cmake --build build --target overte-server -j$(nproc)
file build/overte-server/overte-server   # ... stripped
```

The strip block must stay **before** `target_quazip()` in `overte-server/CMakeLists.txt`: in
headless builds `TARGET_QUAZIP` runs `return()` from inside a macro, and since macros expand
inline that `return()` exits the whole CMakeLists — anything placed after `target_quazip()` is
dead code.

## 4. The `overte-server` multicall target

`overte-server/CMakeLists.txt` collects the sources of `domain-server` and
`assignment-client`, compiles them into one binary and defines `OVERTE_MULTICALL_APPLET`, which
renames each applet's `main` to `domainServerMain` / `assignmentClientMain`. The dispatcher in
`overte-server/src/main.cpp` parses the subcommand and either calls the applet directly or
forks it as a supervised child:

- `overte-server domain --port N`   — domain server directly (like a single-server run)
- `overte-server audio/avatar/entity -p N -a HOST --server-port D` — one assignment client
- `overte-server start --domain-port N` — the default room: only the domain server runs; the
  audio/avatar/entity servers are **not** started and nothing registers to a domain
- `overte-server start --domain-port N --with-mixers --audio-port N --avatar-port N --entity-port N`
  — fork all four applets, supervise them, forward SIGINT/SIGTERM, and shut everything down if
  any child exits unexpectedly
- `overte-server help | version`

Notes:
- **Domain registration is opt-in.** The standalone `domain` subcommand and the default `start`
  run your room with just the domain server. Only `--with-mixers` (or running the
  `audio`/`avatar`/`entity` subcommands yourself) starts the mixer/entity servers and registers
  them to the domain — the classic Overte node registration.
- `SKIP_AUTOMOC ON` in headless mode for `AssetsBackupHandler.h`: the class body disappears
  under `OVERTE_HEADLESS`, and generating a moc file for it would leave an undefined vtable
  reference.
- `resources.qrc` embeds `domain-server/resources/describe-settings.json` under `:/resources/`,
  and `DomainServerSettingsManager` falls back to it when the file is absent on disk.
- The `start` subcommand isolates `XDG_DATA_HOME`/`XDG_CONFIG_HOME`/`XDG_CACHE_HOME` under a
  `data/` directory next to the executable by default (override with `--data-dir`), so the
  whole server state is portable with the binary and no user home directory, `/run` or `/tmp`
  is touched.
- Children install `PR_SET_PDEATHSIG SIGTERM` so a crashed supervisor never leaves orphans.
- **Ports are command-line-only.** Every applet takes its UDP port from `--port`/`-p`
  (`--domain-port`/`--audio-port`/`--avatar-port`/`--entity-port` in `start`). The domain
  applet requires `--port` and never reads `metaverse.local_port` from the settings/config
  files; the port is also never persisted to any config file (the generated
  `domain-server/config.json` contains no port).

## 5. Verify

```bash
cd <project>/build/overte-server
ldd overte-server          # system shared libs only (libpng16/libharfbuzz/libjpeg/libz + libm/libstdc++/libc...)
strings overte-server | grep qt_prfxpath   # qt_prfxpath=/  (no machine path)
strings overte-server | grep -c '/mnt\|/home\|/Users'   # 0: no build-machine path embedded
readelf -d overte-server | grep -i rpath   # (no output: no RUNPATH either)
./overte-server help       # exit code 0
./overte-server domain --port 41302   # binds UDP 41302 (Ctrl-C to stop)

# default room: one domain server, no domain registration (one UDP socket)
./overte-server start --domain-port 41302 &
# ... ss -ulpn | grep 4130  -> 1 socket; log says "room mode"
kill -TERM $!

# full stack: --with-mixers registers the audio/avatar/entity servers to the domain
./overte-server start --domain-port 41302 --with-mixers --audio-port 41303 \
                      --avatar-port 41304 --entity-port 41305 &
# ... ss -ulpn | grep 4130[2-5]  -> 4 sockets
kill -TERM $!   # "overte-server: stopping", all children quit, ports released
```

## 6. Run the stack

Use the `start` subcommand of the single binary. By default it launches just the domain server
(your room) and isolates its config/cache/logs; add `--with-mixers` to also run the
audio/avatar/entity servers and register them to the domain:

```bash
# room only (no domain registration)
./overte-server start --domain-port 40102 [--data-dir /path/to/data]

# full stack (domain + mixers + entity server, all registering to the domain)
./overte-server start --domain-port 40102 --with-mixers \
                      --audio-port 40103 --avatar-port 40104 --entity-port 40105 \
                      [--data-dir /path/to/data]
```

Options: `--domain-port --audio-port --avatar-port --entity-port --with-mixers --data-dir`,
`-h/--help`. `--with-mixers` requires all three mixer/entity ports. The supervisor shuts
everything down together on SIGINT/SIGTERM. If a port is already in use, the affected applet
exits non-zero and the supervisor shuts down the rest of the stack and exits with status 1,
instead of leaving a half-running stack.

## Limitations

- **No SSL in Qt** (`QT_NO_SSL`). TLS-dependent server features are disabled/guarded;
  `embedded-webserver` is not built in headless mode.
- **No OpenGL / windowing**: Qt is built `-no-opengl -no-xcb`; GUI drawing is limited to the
  software `QPainter`/`QImage` raster pipeline.
- **No QtWebSockets / QtQml / QtConcurrent** — expected for a headless server. Anything that
  needs a script engine (V8) is a no-op: `HelperScriptEngine` returns null and
  `run()`/`runWithResult()` skip execution. The few background tasks that would use
  `QtConcurrent::run` fall back to `std::thread` or run synchronously.
- **Cross-platform (absolute requirement)**: the whole point of the project is that **a single
  self-contained binary plus a data folder copied next to it hosts a VR space on any OS** - no
  installer, no admin rights, no Qt runtime. Ports are passed as command-line arguments. All
  three targets are required:
  - **Linux** — verified end-to-end here (dependencies downloaded and built from scratch, no
    leftover build tree).
  - **macOS** — `scripts/build-static-deps.sh` handles Darwin; built as first-class in CI on
    `macos-15-intel` / `macos-15`. Qt is configured with `-no-framework` so the headless server
    links plain `.a` libraries.
  - **Windows** — production build uses **MSVC** (not MinGW) via
    `scripts/build-static-deps-windows.ps1`, which mirrors this document step-for-step. The
    exe is fully static (no Qt/VCRT DLLs to ship) and runs without admin rights. Windows needs
    the same static-specific source guards applied there (rust-udp links as `overte_udp.lib`,
    `ws2_32.lib` instead of `pthread/dl/m`).

## Legacy

A legacy shell launcher `overte-server.sh` (which starts the separate `domain-server` /
`assignment-client` binaries) still exists next to the working tree; it is superseded by the
single binary and kept only for reference.
