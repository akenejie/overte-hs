//
//  main.cpp
//  overte-server/src
//
//  Multicall single-binary entry point for the Overte headless VR room stack.
//
//  Subcommands:
//    domain ...              run the domain-server applet
//    audio ...               run the audio-mixer assignment-client (type 0)
//    avatar ...              run the avatar-mixer assignment-client (type 1)
//    entity ...              run the entity-server assignment-client (type 6)
//    start --domain-port P [--with-mixers --audio-port P --avatar-port P --entity-port P]
//                            run the domain-server (your room); with --with-mixers also run
//                            the audio/avatar/entity servers and register them to the domain
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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <unistd.h>

// Applet entry points (renamed from `main` by OVERTE_MULTICALL_APPLET)
int domainServerMain(int argc, char* argv[]);
int assignmentClientMain(int argc, char* argv[]);

namespace {

void printUsage(const char* prog) {
    std::printf(
        "overte-server - single-binary Overte headless VR server\n"
        "\n"
        "Usage:\n"
        "  %1$s domain --port <port> [other domain-server options]\n"
        "  %1$s audio  -p <port> -a <host> --server-port <domain-port> [options]\n"
        "  %1$s avatar -p <port> -a <host> --server-port <domain-port> [options]\n"
        "  %1$s entity -p <port> -a <host> --server-port <domain-port> [options]\n"
        "  %1$s start --domain-port <p>\n"
        "             [--with-mixers --audio-port <p> --avatar-port <p> --entity-port <p>]\n"
        "             [--data-dir <dir>] [--log-options <opts>]\n"
        "  %1$s help | version\n"
        "\n"
        "By default 'start' runs only the domain-server (your room): no mixers are run and\n"
        "nothing registers to a domain. Add --with-mixers to also run the audio/avatar/entity\n"
        "servers and register them to the domain (classic domain registration).\n"
        "\n"
        "All config/cache/log data is kept next to this executable in %1$s's\n"
        "'data' directory (override with --data-dir); nothing is written to the\n"
        "home directory, /run or /tmp, and no port is stored in any config file.\n"
        "\n"
        "Examples:\n"
        "  %1$s start --domain-port 40102   # room only, no domain registration\n"
        "  %1$s start --domain-port 40102 --with-mixers --audio-port 40103 \\\n"
        "             --avatar-port 40104 --entity-port 40105   # full stack\n"
        "  %1$s domain --port 40102\n"
        "  %1$s audio -p 40103 -a 127.0.0.1 --server-port 40102\n",
        prog);
}

std::string getDataDir() {
    // Default to a directory next to the executable: the whole server state
    // (config, cache, logs) travels with the binary, so the install is portable
    // and nothing is written to $HOME, /run or /tmp.
    char exePath[PATH_MAX];
    ssize_t len = ::readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0) {
        exePath[len] = '\0';
        std::string exe(exePath);
        size_t slash = exe.find_last_of('/');
        std::string dir = (slash == std::string::npos) ? "." : exe.substr(0, slash);
        return dir + "/data";
    }
    return "./data";
}

bool makeDirs(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::string current;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!current.empty()) {
                if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                    return false;
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

void setEnv(const char* name, const std::string& value) {
    ::setenv(name, value.c_str(), 1);
}

struct ServerSpec {
    const char* label;
    int (*appletMain)(int, char**);
    std::vector<std::string> args;
};

static volatile sig_atomic_t g_stopRequested = 0;

void forwardSignal(int) {
    g_stopRequested = 1;
}

int runChildren(std::vector<ServerSpec>& servers) {
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
    int unexpectedLabel = -1;

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
                    unexpectedLabel = (int)i;
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
}

int startSupervisor(int argc, char* argv[]) {
    std::string domainPort, audioPort, avatarPort, entityPort;
    std::string dataDir;
    std::string logOptions = "nojournald";
    bool withMixers = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        auto valueOf = [&](const char* name) -> std::string {
            if (arg == name && i + 1 < argc) {
                return argv[++i];
            }
            return "";
        };
        if (arg == "--domain-port") {
            domainPort = valueOf("--domain-port");
        } else if (arg == "--audio-port") {
            audioPort = valueOf("--audio-port");
        } else if (arg == "--avatar-port") {
            avatarPort = valueOf("--avatar-port");
        } else if (arg == "--entity-port") {
            entityPort = valueOf("--entity-port");
        } else if (arg == "--with-mixers") {
            withMixers = true;
        } else if (arg == "--data-dir") {
            dataDir = valueOf("--data-dir");
        } else if (arg == "--log-options") {
            logOptions = valueOf("--log-options");
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "overte-server: unknown start option: %s\n", arg.c_str());
            printUsage(argv[0]);
            return 1;
        }
    }

    if (domainPort.empty()) {
        std::fprintf(stderr, "overte-server: start requires --domain-port\n");
        printUsage(argv[0]);
        return 1;
    }

    if (withMixers && (audioPort.empty() || avatarPort.empty() || entityPort.empty())) {
        std::fprintf(stderr, "overte-server: --with-mixers requires --audio-port, --avatar-port and --entity-port\n");
        printUsage(argv[0]);
        return 1;
    }

    if (dataDir.empty()) {
        dataDir = getDataDir();
    }
    if (!makeDirs(dataDir)) {
        std::fprintf(stderr, "overte-server: cannot create data dir %s (use --data-dir to point elsewhere)\n", dataDir.c_str());
        return 1;
    }

    // isolate settings, cache and logs from the user's home directory
    setEnv("XDG_DATA_HOME", dataDir + "/data");
    setEnv("XDG_CONFIG_HOME", dataDir + "/config");
    setEnv("XDG_CACHE_HOME", dataDir + "/cache");
    makeDirs(dataDir + "/data");
    makeDirs(dataDir + "/config");
    makeDirs(dataDir + "/cache");

    std::vector<ServerSpec> servers;
    servers.push_back({ "domain-server", domainServerMain,
        { "--port", domainPort, "--logOptions=" + logOptions } });
    if (withMixers) {
        servers.push_back({ "audio-mixer", assignmentClientMain,
            { "-t", "0", "-p", audioPort, "-a", "127.0.0.1", "--server-port", domainPort, "--logOptions=" + logOptions } });
        servers.push_back({ "avatar-mixer", assignmentClientMain,
            { "-t", "1", "-p", avatarPort, "-a", "127.0.0.1", "--server-port", domainPort, "--logOptions=" + logOptions } });
        servers.push_back({ "entity-server", assignmentClientMain,
            { "-t", "6", "-p", entityPort, "-a", "127.0.0.1", "--server-port", domainPort, "--logOptions=" + logOptions } });
    } else {
        std::printf("overte-server: room mode (domain-server only, no domain registration); "
                    "add --with-mixers to run the audio/avatar/entity servers\n");
    }

    return runChildren(servers);
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
        std::printf("overte-server multicall binary\n");
        return 0;
    }

    if (sub == "start") {
        return startSupervisor(argc, argv);
    }

    // applet dispatch: drop the subcommand token so applet parsers only see their own options
    char** appletArgv = argv + 1;
    int appletArgc = argc - 1;

    if (sub == "domain") {
        return domainServerMain(appletArgc, appletArgv);
    }

    if (sub == "audio" || sub == "avatar" || sub == "entity") {
        const char* type = (sub == "audio") ? "0" : (sub == "avatar") ? "1" : "6";
        std::vector<std::string> args;
        args.push_back("-t");
        args.push_back(type);
        for (int i = 2; i < argc; ++i) {
            args.push_back(argv[i]);
        }
        std::vector<char*> argvPtrs;
        argvPtrs.reserve(args.size() + 1);
        argvPtrs.push_back(const_cast<char*>("overte-server"));
        for (auto& arg : args) {
            argvPtrs.push_back(const_cast<char*>(arg.c_str()));
        }
        argvPtrs.push_back(nullptr);
        return assignmentClientMain((int)argvPtrs.size() - 1, argvPtrs.data());
    }

    std::fprintf(stderr, "overte-server: unknown subcommand: %s\n", sub.c_str());
    printUsage(argv[0]);
    return 1;
}
