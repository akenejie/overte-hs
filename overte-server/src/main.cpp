// Copyright (C) 2026 アケネＪ / Akenejie
// SPDX-License-Identifier: AGPL-3.0-only
//
// This file is part of Overte Headless-Server (overte-hs), an unofficial
// stripped-down, headless-only derivative of Overte. It is licensed under
// the GNU Affero General Public License v3.0 (see LICENSE-AGPL-3.0.txt and
// NOTICE in the repository root).
//
//  main.cpp
//  overte-server/src
//
//  Multicall single-binary entry point for the Overte headless VR room stack.
//
//  Usage: start any combination of servers with --domain/--audio/--avatar/
//  --entity/--entity-script/--assets/--messages <port>; the --host/--data/
//  --log-options flags configure registration target, data directory and
//  logging. See printUsage() below.
//
//  (An undocumented applet-token dispatch - "domain", "audio", "avatar",
//  "entity", "entity-script", "assets", "messages" as argv[1] - is kept as the
//  internal spawn target that the Windows supervisor uses to re-run this
//  binary per applet.)
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <string>
#include <vector>
#include <climits>
#include <cctype>

#include <PathUtils.h>

// Byte arrays for the embedded default room (entity persist file + the asset
// files and map the entities reference), generated at build time by
// cmake/GenerateDefaultData.cmake from default-data/ (entities + assets).
#include "default_data.hpp"

// Release version as a C macro, generated at build time by
// cmake/GenerateVersionHeader.cmake from version.txt at the repository root
// (the single source of truth). `--version` reports it so the running binary
// matches the GitHub Actions tag and binary name.
#include "overte_hs_version.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <unistd.h>
#endif

#if defined(_WIN32)
#define WINDOWS_MAX_PATH 260
#endif

// Applet entry points (renamed from `main` by OVERTE_MULTICALL_APPLET)
int domainServerMain(int argc, char* argv[]);
int assignmentClientMain(int argc, char* argv[]);

namespace {

// Last path component of a raw string, handling both '/' and '\' so argv[0] is
// normalized the same way on POSIX and Windows.
std::string fileNameOf(const char* path) {
    if (!path || !*path) {
        return "overte-server";
    }
    std::string p(path);
    size_t pos = p.find_last_of("/\\");
    return (pos == std::string::npos) ? p : p.substr(pos + 1);
}

void printVersion() {
    std::printf("overte-hs %s (overte-server multicall binary)\n", OVERTE_HS_VERSION);
}

void printUsage(const char* prog) {
    // Show only the executable's own file name, never the path it was invoked
    // with (argv[0] is a bare name on Windows but often "./path/.../overte-server"
    // on POSIX). Long paths wrap and look different per OS, so normalize all
    // platforms to the basename.
    const std::string progName = fileNameOf(prog ? prog : "overte-server");
    // POSIX positional parameters (%1$s) are a glibc extension and print
    // literally ("$s") in the MSVC CRT, so they must not be used. Plain %s has
    // identical semantics on every supported platform, so the same format
    // string and the same printf call work unchanged on both Linux and Windows;
    // the program name is passed once for each %s occurrence.
    std::printf(
        "Overte Headless Server - single-binary VR\n"
        "\n"
        "Usage:\n"
        "%s [--domain <port>] [--audio <port>] [--avatar <port>] [--entity <port>] [--entity-script <port>] [--assets <port>] [--messages <port>] [--host <host[:port]>] [--data <dir>] [--log-options <opts>]\n"
        "%s -h | --help\n"
        "%s --version\n"
        "\n"
"Run any combination of the Overte servers in one process group. Each --* <port>\n"
        "flag starts that server on the given UDP port:\n"
        "  --domain <port>        the domain-server (your room). The other servers in this\n"
        "                         invocation check in with it at localhost:<port>.\n"
        "  --audio  <port>        the audio mixer (voice)\n"
        "  --avatar <port>        the avatar mixer\n"
        "  --entity <port>        the entity server (world content)\n"
        "  --entity-script <port> the entity-script server (scripted/ interactive entities)\n"
        "  --assets <port>        the asset server (models, textures, scripts)\n"
        "  --messages <port>      the messages mixer (text chat and script messages)\n"
        "\n"
        "--domain and --host are mutually exclusive. Without --domain the other servers join\n"
        "an existing domain instead of starting one:\n"
        "  --host <host:port>     address of the domain they check in with (the port is\n"
        "                         required, use the host:port form).\n"
        "\n"
        "All server state lives in a 'data' directory in the current directory by default (override with --data): config.json, entities/ and assets/ are kept there, and any transient cache is removed when the server shuts down. Each file belongs to one server - config.json to the domain-server, entities/ to the domain/entity servers, assets/ to the asset-server - so copying the folders a server owns to another\n machine stands that server up there. Nothing is written to the home directory, /run or /tmp, and no port is stored in any config file.\n"
        "\n"
        "Examples:\n"
        "%s --domain 40102 # room only\n"
        "%s --domain 40102 --audio 40103 --avatar 40104 --entity 40105 --assets 40106 # full stack\n"
        "%s --domain 40102 --audio 40103 --avatar 40104 --entity 40105 --entity-script 40107 --assets 40106 --messages 40108 # full stack + chat/scripts\n"
        "%s --entity 40105 --assets 40106 --host 192.168.1.5:40102 # entity+asset servers with an existing domain\n",
        progName.c_str(), progName.c_str(), progName.c_str(), progName.c_str(), progName.c_str(), progName.c_str(), progName.c_str());
}

std::string getDataDir() {
    // Default to a 'data' directory in the current working directory: the whole
    // server state (config, entities, assets) travels with the folder you run the
    // command from, so setups are portable and nothing is written to $HOME, /run
    // or /tmp. Override with --data.
#if defined(_WIN32)
    char cwdBuf[32768];
    if (::_getcwd(cwdBuf, (int)sizeof(cwdBuf))) {
        return std::string(cwdBuf) + "/data";
    }
#else
    char cwdBuf[PATH_MAX];
    if (::getcwd(cwdBuf, sizeof(cwdBuf))) {
        return std::string(cwdBuf) + "/data";
    }
#endif
    return "data";
}

bool makeDirs(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::string current;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/' || path[i] == '\\') {
            if (!current.empty()) {
                bool isDriveLetter = (current.size() == 2 && current[1] == ':');
                if (!isDriveLetter) {
#if defined(_WIN32)
                    if (_mkdir(current.c_str()) != 0 && errno != EEXIST) {
                        return false;
                    }
#else
                    if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                        return false;
                    }
#endif
                }
            }
            if (i < path.size()) {
                current += path[i];
            }
        } else {
            current += path[i];
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Data layout
//
// The launcher pins the whole server state to one directory (--data, or a
// 'data' folder in the current working directory). Everything a server keeps is
// written flat into it, and everything transient goes into a cache/ subtree that
// is removed on shutdown:
//
//   <data-dir>/config.json   domain settings (also a directory-independent copy
//                            source for manual back-ups)
//   <data-dir>/entities/     entity model backups / entity-server persistence
//   <data-dir>/assets/       uploaded asset files
//   <data-dir>/cache/        QSettings + QStandardPaths + resource caches;
//                            deleted on exit
//
// Applets locate their data through PathUtils::getAppDataPath()/getAppLocalDataPath(),
// which read the portable data dir from a process-global set via
// PathUtils::setAppDataDir() (see PathUtils.cpp). No process environment
// variables are involved: a POSIX fork() child inherits the global, and a
// Windows child is spawned with a --data argument that re-arms the same global
// in its own process.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Tiny portable filesystem helpers.
//
// std::filesystem is deliberately NOT used here: Apple's libc++ marks it
// unavailable on macOS < 10.15, and the launcher must still build for the
// project's 10.13 deployment target. The stat/mkdir/rename/readdir split below
// mirrors makeDirs().
// ---------------------------------------------------------------------------

bool pathExists(const std::string& path, bool* isDirOut = nullptr) {
#if defined(_WIN32)
    DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    if (isDirOut) {
        *isDirOut = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    return true;
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    if (isDirOut) {
        *isDirOut = S_ISDIR(st.st_mode);
    }
    return true;
#endif
}

bool isDirectory(const std::string& path) {
    bool isDir = false;
    return pathExists(path, &isDir) && isDir;
}

bool isFile(const std::string& path) {
    bool isDir = true;
    return pathExists(path, &isDir) && !isDir;
}

std::string fileNameOf(const std::string& path) {
    return fileNameOf(path.c_str());
}

bool renamePath(const std::string& from, const std::string& to) {
#if defined(_WIN32)
    return MoveFileA(from.c_str(), to.c_str()) != 0;
#else
    return ::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// Remove a directory tree (never follows symlinks out of the tree).
void removeTree(const std::string& path) {
    if (isDirectory(path)) {
#if defined(_WIN32)
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA((path + "/*").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0) {
                    continue;
                }
                removeTree(path + "/" + fd.cFileName);
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
        SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        RemoveDirectoryA(path.c_str());
#else
        DIR* dir = ::opendir(path.c_str());
        if (dir) {
            struct dirent* entry;
            while ((entry = ::readdir(dir)) != nullptr) {
                if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                removeTree(path + "/" + entry->d_name);
            }
            ::closedir(dir);
        }
        ::rmdir(path.c_str());
#endif
        return;
    }
    if (pathExists(path)) {
#if defined(_WIN32)
        SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileA(path.c_str());
#else
        ::unlink(path.c_str());
#endif
    }
}

// Move a single file into a destination directory; an existing target wins.
void moveFileInto(const std::string& src, const std::string& dstDir) {
    if (!isFile(src)) {
        return;
    }
    makeDirs(dstDir);
    std::string dst = dstDir + "/" + fileNameOf(src);
    if (!pathExists(dst)) {
        renamePath(src, dst);
    }
}

// Move the contents of a source directory into a destination directory.
void moveDirContentsInto(const std::string& srcDir, const std::string& dstDir) {
    if (!isDirectory(srcDir)) {
        return;
    }
    makeDirs(dstDir);
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA((srcDir + "/*").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0) {
                continue;
            }
            std::string dst = dstDir + "/" + fd.cFileName;
            if (!pathExists(dst)) {
                renamePath(srcDir + "/" + fd.cFileName, dst);
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
#else
    DIR* dir = ::opendir(srcDir.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = ::readdir(dir)) != nullptr) {
            if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            std::string dst = dstDir + "/" + entry->d_name;
            if (!pathExists(dst)) {
                renamePath(srcDir + "/" + entry->d_name, dst);
            }
        }
        ::closedir(dir);
    }
#endif
}

// Migrate the pre-flat layout (<dataDir>/data/<organization>/<app>/...) into the
// flat layout (<dataDir>/config.json, <dataDir>/entities/, <dataDir>/assets/).
void migrateLegacyData(const std::string& dataDir) {
    const std::string legacyRoot = dataDir + "/data";
    if (isDirectory(legacyRoot)) {
#if defined(_WIN32)
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA((legacyRoot + "/*").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0) {
                    continue;
                }
                const std::string orgDir = legacyRoot + "/" + fd.cFileName;
                if (isDirectory(orgDir)) {
                    moveFileInto(orgDir + "/domain-server/config.json", dataDir);
                    moveDirContentsInto(orgDir + "/domain-server/entities", dataDir + "/entities");
                    moveDirContentsInto(orgDir + "/assignment-client/entities", dataDir + "/entities");
                    moveDirContentsInto(orgDir + "/assignment-client/assets", dataDir + "/assets");
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
#else
        DIR* dir = ::opendir(legacyRoot.c_str());
        if (dir) {
            struct dirent* entry;
            while ((entry = ::readdir(dir)) != nullptr) {
                if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                const std::string orgDir = legacyRoot + "/" + entry->d_name;
                if (isDirectory(orgDir)) {
                    moveFileInto(orgDir + "/domain-server/config.json", dataDir);
                    moveDirContentsInto(orgDir + "/domain-server/entities", dataDir + "/entities");
                    moveDirContentsInto(orgDir + "/assignment-client/entities", dataDir + "/entities");
                    moveDirContentsInto(orgDir + "/assignment-client/assets", dataDir + "/assets");
                }
            }
            ::closedir(dir);
        }
#endif
        removeTree(legacyRoot);
    }
    // the old 'start' also created an empty XDG_CONFIG_HOME container here; it
    // only held transient QSettings files, so drop it with the rest
    removeTree(dataDir + "/config");
}

// Write <data> to <path> atomically and only if absent. Returns 0 on a fresh
// write, 1 if the file already exists (another process won the race, or this
// data dir was already seeded), and -1 on a genuine error.
int seedFileIfAbsent(const std::string& path, const unsigned char* data, size_t size) {
#if defined(_WIN32)
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return (GetLastError() == ERROR_FILE_EXISTS) ? 1 : -1;
    }
    DWORD written = 0;
    const bool ok = WriteFile(h, data, (DWORD)size, &written, nullptr) && written == (DWORD)size;
    CloseHandle(h);
    return ok ? 0 : -1;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return (errno == EEXIST) ? 1 : -1;
    }
    const size_t written = ::write(fd, data, size);
    const bool ok = (written == size) && (::close(fd) == 0);
    return ok ? 0 : -1;
#endif
}

// Seed a fresh data dir with the embedded default room: the entity persist
// file (entities/models.json.gz) and the asset-server content it references
// (assets/files/<sha256> + assets/map.json). Every file is written
// create-if-absent, so the call is safe to run from every supervisor/applet
// process even though they start at the same time, and existing user data is
// never overwritten. A data dir that already holds entities but zero assets
// (created by earlier releases that only embedded the world file) is silently
// completed with the missing assets on the next boot.
bool seedDefaultData(const std::string& dataDir) {
    if (!makeDirs(dataDir + "/entities")) {
        std::fprintf(stderr, "overte-server: cannot create %s/entities\n", dataDir.c_str());
        return false;
    }
    if (!makeDirs(dataDir + "/assets/files")) {
        std::fprintf(stderr, "overte-server: cannot create %s/assets/files\n", dataDir.c_str());
        return false;
    }

    int seeded = 0;
    bool failed = false;

    switch (seedFileIfAbsent(dataDir + "/entities/models.json.gz", gDefaultModelsJsonGz, gDefaultModelsJsonGzSize)) {
        case 0: ++seeded; break;
        case -1: failed = true; break;
    }

    for (size_t i = 0; i < gDefaultAssetsCount; ++i) {
        const DefaultDataAsset& asset = gDefaultAssets[i];
        switch (seedFileIfAbsent(dataDir + "/assets/files/" + asset.fileName, asset.data, asset.size)) {
            case 0: ++seeded; break;
            case -1: failed = true; break;
        }
    }

    switch (seedFileIfAbsent(dataDir + "/assets/map.json", gDefaultAssetMapJson, gDefaultAssetMapJsonSize)) {
        case 0: ++seeded; break;
        case -1: failed = true; break;
    }

    if (seeded > 0) {
        std::printf("Seeded default room (%d new files, entities + %zu assets + map) at %s\n",
                    seeded, gDefaultAssetsCount, dataDir.c_str());
        std::fflush(stdout); // don't let fork() children echo this line via inherited buffer
    }
    if (failed) {
        std::fprintf(stderr, "overte-server: failed to seed part of the default room at %s\n", dataDir.c_str());
    }
    return !failed;
}

// Prepare <dataDir> as the single writable root for a run: migrate old layouts,
// drop a stale cache/ from a previous crashed run, and arm the PathUtils global
// so applets derive all persistent and transient paths from this one directory.
// Destructive and only safe to run once, before any other process of the same
// data dir is alive.
bool setupDataDir(const std::string& dataDir) {
    if (!makeDirs(dataDir)) {
        return false;
    }
    migrateLegacyData(dataDir);
    if (!seedDefaultData(dataDir)) {
        return false;
    }
    removeTree(dataDir + "/cache");

    // Arm the shared PathUtils global so applets (fork children on POSIX, or
    // spawned --data children on Windows) derive all persistent and transient
    // paths from this single portable directory.
    PathUtils::setAppDataDir(QString::fromUtf8(dataDir.c_str()));

    makeDirs(dataDir + "/cache");
    makeDirs(dataDir + "/cache/data");
    makeDirs(dataDir + "/cache/config");
    return true;
}

// Idempotent, non-destructive preparation for a spawned child on Windows (there
// is no fork()). The supervisor already did the destructive setup (migration,
// stale-cache removal, seeding) before spawning, so a child must NOT removeTree
// or migrate anything: siblings start at the same time and are already using
// <data>/cache and <data>/entities/ the moment this child is born. Removing the
// cache subtree here would race those siblings (the entity-server's 0xC0000409
// crash on the first Windows runs was the cache dir being recreated out from
// under it), so a child only arms its PathUtils global and idempotently ensures
// the leaf directories exist.
bool setupAppletDataDir(const std::string& dataDir) {
    if (!makeDirs(dataDir)) {
        return false;
    }
    if (!seedDefaultData(dataDir)) {
        return false;
    }
    PathUtils::setAppDataDir(QString::fromUtf8(dataDir.c_str()));
    makeDirs(dataDir + "/cache");
    makeDirs(dataDir + "/cache/data");
    makeDirs(dataDir + "/cache/config");
    return true;
}

// Remove the transient cache/ subtree so that only config.json, entities/ and
// assets/ remain in the data dir.
void cleanupTransientData(const std::string& dataDir) {
    removeTree(dataDir + "/cache");
}

// Look for a --data <path> pair in a subcommand's arguments (the applet
// parsers themselves do not know the option, so the launcher strips it before
// forwarding).
std::string findDataDir(int argc, char* argv[], int firstArg) {
    for (int i = firstArg; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--data") == 0) {
            return argv[i + 1];
        }
    }
    return "";
}

// Drop --data <path> pairs from an argument vector before forwarding it to
// an applet.
void stripDataDir(std::vector<std::string>& args) {
    std::vector<std::string> kept;
    kept.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--data" && i + 1 < args.size()) {
            ++i;
        } else {
            kept.push_back(args[i]);
        }
    }
    args = std::move(kept);
}

std::vector<char*> makeArgv(const std::string& prog, std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(prog.c_str()));
    for (auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

// Run an applet main under the isolated data dir, then clear the transient
// cache so only the flat persistent state remains.
int runApplet(int (*appletMain)(int, char**), const std::string& prog,
              std::vector<std::string> args, const std::string& dataDir) {
    stripDataDir(args);
    std::vector<char*> argvPtrs = makeArgv(prog, args);
    int result = appletMain((int)argvPtrs.size() - 1, argvPtrs.data());
    cleanupTransientData(dataDir);
    return result;
}

struct ServerSpec {
    const char* label;
    const char* labelCmd;   // multicall subcommand ("domain" | "audio" | ...)
    int (*appletMain)(int, char**);
    std::vector<std::string> args;
};

static volatile sig_atomic_t g_stopRequested = 0;

void forwardSignal(int) {
    g_stopRequested = 1;
}

int runChildren(std::vector<ServerSpec>& servers, const std::string& dataDir) {
#if defined(_WIN32)
    // Windows has no fork(): spawn each applet as a separate process of this
    // same multicall binary ("overte-server <sub> ...") and supervise the
    // resulting handles with WaitForSingleObject/GetExitCodeProcess polling.
    char modulePathBuf[WINDOWS_MAX_PATH];
    DWORD modulePathLen = GetModuleFileNameA(NULL, modulePathBuf, (DWORD)sizeof(modulePathBuf));
    std::string modulePath;
    if (modulePathLen > 0 && modulePathLen + 1 < sizeof(modulePathBuf)) {
        modulePath.assign(modulePathBuf, (size_t)modulePathLen);
    }
    if (modulePath.empty()) {
        std::fprintf(stderr, "overte-server: cannot resolve own exe path\n");
        return 1;
    }

    std::vector<HANDLE> handles;
    handles.reserve(servers.size());

    for (auto& spec : servers) {
        // build command line: "this.exe <sub> [--data <dir>] <arg>..." -- the
        // --data pair re-arms the applet's data-dir global in its own process
        std::string cmdLine = "\"" + modulePath + "\" " + spec.labelCmd
            + " --data \"" + dataDir + "\"";
        for (auto& arg : spec.args) {
            cmdLine += " \"" + arg + "\"";
        }

        std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
        cmdBuf.push_back('\0');

        STARTUPINFOA si;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(pi));

        if (!CreateProcessA(NULL, &cmdBuf[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            std::fprintf(stderr, "overte-server: failed to spawn %s: error %lu\n",
                         spec.label, (unsigned long)GetLastError());
            // cleanup already-started children
            for (auto h : handles) {
                TerminateProcess(h, 1);
                CloseHandle(h);
            }
            return 1;
        }
        CloseHandle(pi.hThread);
        handles.push_back(pi.hProcess);

        std::printf("overte-server: starting %s\n", spec.label);
        std::fflush(stdout);
    }

    std::signal(SIGINT, forwardSignal);
    std::signal(SIGTERM, forwardSignal);

    bool stopping = false;
    std::vector<bool> alive(handles.size(), true);
    bool anyUnexpectedExit = false;

    while (true) {
        if (g_stopRequested && !stopping) {
            std::fprintf(stderr, "overte-server: stopping\n");
            stopping = true;
            // Windows cannot deliver SIGTERM to other processes by PID; ask each
            // child to exit via a posted console ctrl event is unreliable in CI,
            // so fall back to TerminateProcess (best-effort graceful shutdown is
            // done by the child's own handler on lost console ctrl).
            for (size_t i = 0; i < handles.size(); ++i) {
                if (alive[i]) {
                    TerminateProcess(handles[i], 0);
                }
            }
        }

        bool allReaped = true;
        for (size_t i = 0; i < handles.size(); ++i) {
            if (!alive[i]) {
                continue;
            }
            DWORD waitResult = WaitForSingleObject(handles[i], 0);
            if (waitResult == WAIT_OBJECT_0) {
                DWORD exitCode = 0;
                GetExitCodeProcess(handles[i], &exitCode);
                alive[i] = false;
                if (!stopping && exitCode != 0) {
                    anyUnexpectedExit = true;
                    std::fprintf(stderr, "overte-server: %s exited unexpectedly (code %lu)\n",
                                 servers[i].label, (unsigned long)exitCode);
                }
            } else {
                allReaped = false;
            }
        }

        if (anyUnexpectedExit && !stopping) {
            std::fprintf(stderr, "overte-server: shutting down remaining servers\n");
            stopping = true;
            for (size_t i = 0; i < handles.size(); ++i) {
                if (alive[i]) {
                    TerminateProcess(handles[i], 0);
                }
            }
        }

        if (stopping) {
            bool anyAlive = false;
            for (size_t i = 0; i < handles.size(); ++i) {
                if (alive[i]) {
                    anyAlive = true;
                    break;
                }
            }
            if (!anyAlive) {
                break;
            }
            SleepEx(100, FALSE);
            continue;
        }

        if (allReaped) {
            break;
        }
        SleepEx(200, FALSE);
    }

    for (auto h : handles) {
        CloseHandle(h);
    }

    if (anyUnexpectedExit) {
        return 1;
    }
    return 0;
#else
    (void)dataDir; // Windows passes it to spawned children as --data; POSIX children inherit it via fork
    std::vector<pid_t> pids;
    pids.reserve(servers.size());

    for (auto& spec : servers) {
        pid_t pid = ::fork();
        if (pid < 0) {
            std::fprintf(stderr, "overte-server: fork failed: %s\n", std::strerror(errno));
            return 1;
        }
        if (pid == 0) {
            // child: die if the supervisor dies (Linux-only API)
            #if defined(__linux__)
            ::prctl(PR_SET_PDEATHSIG, SIGTERM);
            #endif

            // keep argv[0] as the program name, then applet args
            std::vector<char*> argvPtrs;
            argvPtrs.reserve(spec.args.size() + 1);
            argvPtrs.push_back(const_cast<char*>("overte-server"));
            for (auto& arg : spec.args) {
                argvPtrs.push_back(const_cast<char*>(arg.c_str()));
            }
            argvPtrs.push_back(nullptr);

            std::printf("overte-server: starting %s\n", spec.label);
            std::fflush(stdout);

            int rc = spec.appletMain((int)argvPtrs.size() - 1, argvPtrs.data());
            std::fflush(stdout);
            std::fflush(stderr);
            _exit(rc);
        }
        pids.push_back(pid);
    }

    // parent: supervise
    std::signal(SIGINT, forwardSignal);
    std::signal(SIGTERM, forwardSignal);
    std::signal(SIGCHLD, SIG_DFL);

    bool stopping = false;
    std::vector<bool> alive(pids.size(), true);
    std::vector<int> statuses(pids.size(), 0);
    bool anyUnexpectedExit = false;

    while (true) {
        if (g_stopRequested && !stopping) {
            std::fprintf(stderr, "overte-server: stopping\n");
            stopping = true;
            for (size_t i = 0; i < pids.size(); ++i) {
                if (alive[i]) {
                    ::kill(pids[i], SIGTERM);
                }
            }
        }

        bool allReaped = true;
        for (size_t i = 0; i < pids.size(); ++i) {
            if (!alive[i]) {
                continue;
            }
            int status = 0;
            pid_t w = ::waitpid(pids[i], &status, WNOHANG);
            if (w == pids[i]) {
                alive[i] = false;
                statuses[i] = status;
                if (!stopping && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
                    anyUnexpectedExit = true;
                    std::fprintf(stderr, "overte-server: %s exited unexpectedly (status %d)\n",
                                 servers[i].label, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                }
            } else if (w == 0) {
                allReaped = false;
            }
        }

        if (anyUnexpectedExit && !stopping) {
            std::fprintf(stderr, "overte-server: shutting down remaining servers\n");
            stopping = true;
            for (size_t i = 0; i < pids.size(); ++i) {
                if (alive[i]) {
                    ::kill(pids[i], SIGTERM);
                }
            }
        }

        if (stopping) {
            bool anyAlive = false;
            for (size_t i = 0; i < alive.size(); ++i) {
                if (alive[i]) {
                    anyAlive = true;
                    break;
                }
            }
            if (!anyAlive) {
                break;
            }
            ::usleep(100000);
            continue;
        }

        // wait for SIGINT/SIGTERM (forwardSignal interrupted usleep)
        if (allReaped) {
            // all children already gone on their own
            break;
        }
        ::usleep(200000);
    }

    // if we are exiting because the children all died without our request, propagate failure
    if (anyUnexpectedExit) {
        return 1;
    }
    return 0;
#endif
}

// Split "host[:port]" (also "[v6]:port") into its parts. A bare host yields an
// empty port; a host with a trailing numeric :port yields both parts.
void splitHostPort(const std::string& value, std::string& host, std::string& port) {
    host = value;
    port.clear();
    auto colon = value.find_last_of(':');
    if (colon == std::string::npos) {
        return;
    }
    std::string maybePort = value.substr(colon + 1);
    bool allDigits = !maybePort.empty();
    for (char c : maybePort) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            allDigits = false;
            break;
        }
    }
    if (!allDigits) {
        return;
    }
    host = value.substr(0, colon);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    port = maybePort;
}

// Flag-based supervisor: `--domain/--audio/--avatar/--entity/--entity-script/
// --assets/--messages <port>` start servers, `--host`/`--data`/`--log-options`
// configure them. See printUsage() for the full interface.
int flagSupervisor(int argc, char* argv[]) {
    std::string domainPort, audioPort, avatarPort, entityPort, entityScriptPort, assetsPort, messagesPort;
    std::string host;
    std::string dataDir;
    std::string logOptions = "nojournald";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto valueOf = [&](const char* name) -> std::string {
            if (arg == name && i + 1 < argc) {
                return argv[++i];
            }
            return "";
        };
        if (arg == "--domain") {
            domainPort = valueOf("--domain");
        } else if (arg == "--audio") {
            audioPort = valueOf("--audio");
        } else if (arg == "--avatar") {
            avatarPort = valueOf("--avatar");
        } else if (arg == "--entity") {
            entityPort = valueOf("--entity");
        } else if (arg == "--entity-script") {
            entityScriptPort = valueOf("--entity-script");
        } else if (arg == "--assets") {
            assetsPort = valueOf("--assets");
        } else if (arg == "--messages") {
            messagesPort = valueOf("--messages");
        } else if (arg == "--host") {
            host = valueOf("--host");
        } else if (arg == "--data") {
            dataDir = valueOf("--data");
        } else if (arg == "--log-options") {
            logOptions = valueOf("--log-options");
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--version") {
            printVersion();
            return 0;
        } else {
            std::fprintf(stderr, "overte-server: unknown option: %s\n", arg.c_str());
            printUsage(argv[0]);
            return 1;
        }
    }

    bool haveAny = !domainPort.empty() || !audioPort.empty() || !avatarPort.empty()
                || !entityPort.empty() || !entityScriptPort.empty() || !assetsPort.empty()
                || !messagesPort.empty();
    if (!haveAny) {
        std::fprintf(stderr, "overte-server: specify at least one of --domain/--audio/--avatar/--entity/--entity-script/--assets/--messages\n");
        printUsage(argv[0]);
        return 1;
    }

    // --domain starts the room's own domain-server, so the other servers always
    // check in with localhost:<domain port>; there is no reason to also accept
    // --host there. --host exists only to join an existing domain instead.
    if (!domainPort.empty() && !host.empty()) {
        std::fprintf(stderr, "overte-server: --host and --domain are mutually exclusive\n"
                             "  --host means the other servers join an EXTERNAL domain, while\n"
                             "  --domain starts your own domain on this machine, so the two cannot\n"
                             "  be combined (use --host <host:port> without --domain to join an external domain)\n");
        printUsage(argv[0]);
        return 1;
    }

    // registration target for the assignment-client servers
    std::string acHost, acPort;
    if (!domainPort.empty()) {
        acHost = "localhost";
        acPort = domainPort;
    } else {
        // remote domain: --host must carry the domain port (host:port form)
        if (host.empty()) {
            std::fprintf(stderr, "overte-server: --host is required when --domain is not given\n");
            printUsage(argv[0]);
            return 1;
        }
        std::string hostPart, portPart;
        splitHostPort(host, hostPart, portPart);
        if (portPart.empty()) {
            std::fprintf(stderr, "overte-server: --host must include the domain port (use host:port form) when --domain is not given\n");
            printUsage(argv[0]);
            return 1;
        }
        acHost = hostPart.empty() ? "localhost" : hostPart;
        acPort = portPart;
    }

    if (dataDir.empty()) {
        dataDir = getDataDir();
    }
    if (!setupDataDir(dataDir)) {
        std::fprintf(stderr, "overte-server: cannot prepare data dir %s (use --data to point elsewhere)\n", dataDir.c_str());
        return 1;
    }

    std::vector<ServerSpec> servers;
    if (!domainPort.empty()) {
        servers.push_back({ "domain-server", "domain", domainServerMain,
            { "--port", domainPort, "--logOptions=" + logOptions } });
    }
    if (!audioPort.empty()) {
        servers.push_back({ "audio-mixer", "audio", assignmentClientMain,
            { "-t", "0", "-p", audioPort, "-a", acHost, "--server-port", acPort, "--logOptions=" + logOptions } });
    }
    if (!avatarPort.empty()) {
        servers.push_back({ "avatar-mixer", "avatar", assignmentClientMain,
            { "-t", "1", "-p", avatarPort, "-a", acHost, "--server-port", acPort, "--logOptions=" + logOptions } });
    }
    if (!entityPort.empty()) {
        servers.push_back({ "entity-server", "entity", assignmentClientMain,
            { "-t", "6", "-p", entityPort, "-a", acHost, "--server-port", acPort, "--logOptions=" + logOptions } });
    }
    if (!entityScriptPort.empty()) {
        servers.push_back({ "entity-script-server", "entity-script", assignmentClientMain,
            { "-t", "5", "-p", entityScriptPort, "-a", acHost, "--server-port", acPort, "--logOptions=" + logOptions } });
    }
    if (!assetsPort.empty()) {
        servers.push_back({ "asset-server", "assets", assignmentClientMain,
            { "-t", "3", "-p", assetsPort, "-a", acHost, "--server-port", acPort, "--logOptions=" + logOptions } });
    }
    if (!messagesPort.empty()) {
        servers.push_back({ "messages-mixer", "messages", assignmentClientMain,
            { "-t", "4", "-p", messagesPort, "-a", acHost, "--server-port", acPort, "--logOptions=" + logOptions } });
    }

    int result = runChildren(servers, dataDir);
    cleanupTransientData(dataDir);
    return result;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::string sub = argv[1];

    if (sub == "help" || sub == "-h" || sub == "--help") {
        printUsage(argv[0]);
        return 0;
    }

    if (sub == "version" || sub == "--version") {
        printVersion();
        return 0;
    }

    // undocumented applet-token dispatch: the Windows supervisor re-runs this
    // binary with a token in argv[1] so each applet gets its own process. They
    // run inside the same isolated data dir as the flag mode.
    if (sub == "domain" || sub == "audio" || sub == "avatar" || sub == "entity" || sub == "entity-script"
        || sub == "assets" || sub == "messages") {
        std::string dataDir = findDataDir(argc, argv, 2);
        if (dataDir.empty()) {
            // a Windows supervisor-spawned child always carries --data, so a
            // missing one means this binary was invoked directly with a bare
            // applet token; fall back to the default portable directory.
            dataDir = getDataDir();
        }
        // A spawned child arrives while its siblings are already starting, so
        // use the non-destructive setup: the supervisor (or a prior bare-token
        // run) already migrated/removed-cache/seeded the dir.
        if (!setupAppletDataDir(dataDir)) {
            std::fprintf(stderr, "overte-server: cannot prepare data dir %s\n", dataDir.c_str());
            return 1;
        }
        if (sub == "domain") {
            std::vector<std::string> args;
            for (int i = 2; i < argc; ++i) {
                args.push_back(argv[i]);
            }
            return runApplet(domainServerMain, "overte-server", args, dataDir);
        }
        const char* type = (sub == "audio") ? "0" : (sub == "avatar") ? "1" : (sub == "assets") ? "3"
            : (sub == "messages") ? "4" : (sub == "entity-script") ? "5" : "6";
        std::vector<std::string> args;
        args.push_back("-t");
        args.push_back(type);
        for (int i = 2; i < argc; ++i) {
            args.push_back(argv[i]);
        }
        return runApplet(assignmentClientMain, "overte-server", args, dataDir);
    }

    return flagSupervisor(argc, argv);
}
