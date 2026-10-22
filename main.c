#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "common.h"
#if defined(_WIN32)
#include <windows.h>
#include "windows/initcore.h"
#include "windows/resolver.h"
#include "windows/servicefunc.h"
#else
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include "linux/initcore.h"
#include "linux/resolver.h"
#include "linux/servicefunc.h"
#endif

static volatile int g_running = 1;

#if defined(_WIN32)
static BOOL WINAPI ConsoleHandlerRoutine(DWORD dwCtrlType) {
    (void)dwCtrlType;
    g_running = 0;
    return TRUE;
}
#else
static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}
#endif
typedef struct {
    char target[MINDRIVER_MAX_PATH_LEN];
    int is_file;
    int is_stop;
    int is_replay;
    int is_delete;
    int is_status;
    int is_exit;
    int loop;
    EndAction end_action;
    int loop_delay_sec;
} ParsedCmd;

static void parse_cli_args(int argc, char* argv[], ParsedCmd* cmd) {
    memset(cmd, 0, sizeof(ParsedCmd));
    cmd->end_action = END_ACTION_STOP_AT_END;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];

        if (strcmp(arg, "--stop") == 0) {
            cmd->is_stop = 1;
        } else if (strcmp(arg, "--replay") == 0) {
            cmd->is_replay = 1;
        } else if (strcmp(arg, "--delete") == 0) {
            cmd->is_delete = 1;
        } else if (strcmp(arg, "--status") == 0 || strcmp(arg, "-s") == 0) {
            cmd->is_status = 1;
        } else if (strcmp(arg, "--exit") == 0 || strcmp(arg, "--kill") == 0) {
            cmd->is_exit = 1;
        } else if (strcmp(arg, "--loop") == 0) {
            cmd->loop = 1;
        } else if (strcmp(arg, "--deleteatend") == 0) {
            cmd->end_action = END_ACTION_DELETE_AT_END;
        } else if (strcmp(arg, "--stopatend") == 0) {
            cmd->end_action = END_ACTION_STOP_AT_END;
        } else if (strncmp(arg, "--loopatend", 11) == 0) {
            int sec = atoi(arg + 11);
            if (sec < 1) sec = 1;
            if (sec > 20) sec = 20;
            cmd->loop_delay_sec = sec;
            cmd->end_action = (EndAction)(END_ACTION_LOOP_AT_END_BASE + sec);
        } else if (arg[0] != '-') {
            if (cmd->target[0] == '\0') {
                strncpy(cmd->target, arg, sizeof(cmd->target) - 1);
            } else {
                size_t cur_len = strlen(cmd->target);
                if (cur_len + 1 < sizeof(cmd->target)) {
                    strcat(cmd->target, " ");
                    strncat(cmd->target, arg, sizeof(cmd->target) - cur_len - 2);
                }
            }
        }
    }
    size_t tlen = strlen(cmd->target);
    if (tlen >= 2 && ((cmd->target[0] == '\"' && cmd->target[tlen - 1] == '\"') ||
                      (cmd->target[0] == '\'' && cmd->target[tlen - 1] == '\''))) {
        memmove(cmd->target, cmd->target + 1, tlen - 2);
        cmd->target[tlen - 2] = '\0';
    }
}
static void serialize_cmd(const ParsedCmd* cmd, char* out_buf, size_t max_len) {
    if (cmd->is_exit) {
        snprintf(out_buf, max_len, "EXIT");
    } else if (cmd->is_status) {
        if (cmd->target[0] != '\0') {
            snprintf(out_buf, max_len, "STATUS \"%s\"", cmd->target);
        } else {
            snprintf(out_buf, max_len, "STATUS");
        }
    } else if (cmd->is_stop) {
        snprintf(out_buf, max_len, "STOP \"%s\"", cmd->target);
    } else if (cmd->is_replay) {
        snprintf(out_buf, max_len, "REPLAY \"%s\"", cmd->target);
    } else if (cmd->is_delete) {
        snprintf(out_buf, max_len, "DELETE \"%s\"", cmd->target);
    } else if (cmd->target[0] != '\0') {
        char flags[128] = "";
        if (cmd->loop) strcat(flags, " --loop");
        if (cmd->end_action == END_ACTION_DELETE_AT_END) {
            strcat(flags, " --deleteatend");
        } else if (cmd->loop_delay_sec > 0) {
            char delay_str[32];
            snprintf(delay_str, sizeof(delay_str), " --loopatend%d", cmd->loop_delay_sec);
            strcat(flags, delay_str);
        } else {
            strcat(flags, " --stopatend");
        }
        snprintf(out_buf, max_len, "PLAY \"%s\"%s", cmd->target, flags);
    } else {
        snprintf(out_buf, max_len, "STATUS");
    }
}
static void extract_param(const char* src, char* dst, size_t max_len) {
    while (*src == ' ') src++;
    if (*src == '\"' || *src == '\'') {
        char q = *src++;
        const char* end = strchr(src, q);
        size_t len = end ? (size_t)(end - src) : strlen(src);
        if (len >= max_len) len = max_len - 1;
        strncpy(dst, src, len);
        dst[len] = '\0';
    } else {
        strncpy(dst, src, max_len - 1);
        dst[max_len - 1] = '\0';
        size_t l = strlen(dst);
        while (l > 0 && (dst[l - 1] == ' ' || dst[l - 1] == '\r' || dst[l - 1] == '\n')) {
            dst[--l] = '\0';
        }
    }
}
static void handle_ipc_request(const char* request, char* response, size_t max_resp) {
    if (!request || !response || max_resp == 0) return;
    response[0] = '\0';

    if (strncmp(request, "EXIT", 4) == 0) {
        g_running = 0;
        snprintf(response, max_resp, "OK: server shutting down");
        return;
    }

    if (strncmp(request, "STATUS", 6) == 0) {
        const char* target_src = request + 6;
        while (*target_src == ' ') target_src++;
        if (*target_src) {
            char target[MINDRIVER_MAX_PATH_LEN];
            extract_param(target_src, target, sizeof(target));
            char id[MINDRIVER_MAX_ID_LEN];
            resolver_sanitize_name(target, id, sizeof(id));
            if (servifunc_status(id, response, max_resp) != 0) {
                snprintf(response, max_resp, "ERR: thread '%s' not found", id);
            }
        } else {
            if (servifunc_list(response, max_resp) == 0) {
                snprintf(response, max_resp, "no active threads");
            }
        }
        return;
    }

    if (strncmp(request, "STOP", 4) == 0) {
        char target[MINDRIVER_MAX_PATH_LEN];
        extract_param(request + 4, target, sizeof(target));
        char id[MINDRIVER_MAX_ID_LEN];
        resolver_sanitize_name(target, id, sizeof(id));
        if (servifunc_stop(id) == 0) {
            snprintf(response, max_resp, "OK: '%s' stopped", id);
        } else {
            snprintf(response, max_resp, "ERR: thread '%s' not found", id);
        }
        return;
    }

    if (strncmp(request, "REPLAY", 6) == 0) {
        char target[MINDRIVER_MAX_PATH_LEN];
        extract_param(request + 6, target, sizeof(target));
        char id[MINDRIVER_MAX_ID_LEN];
        resolver_sanitize_name(target, id, sizeof(id));
        if (servifunc_replay(id) == 0) {
            snprintf(response, max_resp, "OK: '%s' replaying", id);
        } else {
            snprintf(response, max_resp, "ERR: thread '%s' not found", id);
        }
        return;
    }

    if (strncmp(request, "DELETE", 6) == 0) {
        char target[MINDRIVER_MAX_PATH_LEN];
        extract_param(request + 6, target, sizeof(target));
        char id[MINDRIVER_MAX_ID_LEN];
        resolver_sanitize_name(target, id, sizeof(id));
        if (servifunc_delete(id) == 0) {
            snprintf(response, max_resp, "OK: '%s' deleted", id);
        } else {
            snprintf(response, max_resp, "ERR: thread '%s' not found", id);
        }
        return;
    }

    if (strncmp(request, "PLAY ", 5) == 0) {
        const char* p = request + 5;
        while (*p == ' ') p++;

        char path[MINDRIVER_MAX_PATH_LEN] = "";
        const char* flags_str = NULL;

        if (*p == '\"' || *p == '\'') {
            char q = *p++;
            const char* end_quote = strchr(p, q);
            if (end_quote) {
                size_t len = (size_t)(end_quote - p);
                if (len >= sizeof(path)) len = sizeof(path) - 1;
                strncpy(path, p, len);
                path[len] = '\0';
                flags_str = end_quote + 1;
            } else {
                strncpy(path, p, sizeof(path) - 1);
                path[sizeof(path) - 1] = '\0';
            }
        } else {
            const char* flag_start = strstr(p, " --");
            if (flag_start) {
                size_t len = (size_t)(flag_start - p);
                if (len >= sizeof(path)) len = sizeof(path) - 1;
                strncpy(path, p, len);
                path[len] = '\0';
                flags_str = flag_start;
            } else {
                strncpy(path, p, sizeof(path) - 1);
                path[sizeof(path) - 1] = '\0';
            }
        }

        size_t plen = strlen(path);
        while (plen > 0 && (path[plen - 1] == ' ' || path[plen - 1] == '\r' || path[plen - 1] == '\n')) {
            path[--plen] = '\0';
        }

        int loop = 0;
        EndAction end_act = END_ACTION_STOP_AT_END;
        int loop_delay = 0;

        if (flags_str) {
            if (strstr(flags_str, "--loop")) loop = 1;
            if (strstr(flags_str, "--deleteatend")) end_act = END_ACTION_DELETE_AT_END;
            if (strstr(flags_str, "--stopatend")) end_act = END_ACTION_STOP_AT_END;
            const char* lpa = strstr(flags_str, "--loopatend");
            if (lpa) {
                int sec = atoi(lpa + 11);
                if (sec < 1) sec = 1;
                if (sec > 20) sec = 20;
                loop_delay = sec;
                end_act = (EndAction)(END_ACTION_LOOP_AT_END_BASE + sec);
            }
        }

        char resolved[MINDRIVER_MAX_PATH_LEN];
        if (resolver_resolve_path(path, resolved, sizeof(resolved)) != 0) {
            snprintf(response, max_resp, "ERR: cannot find or resolve file '%s'", path);
            return;
        }

        char id[MINDRIVER_MAX_ID_LEN];
        resolver_sanitize_name(resolved, id, sizeof(id));

        AudioBuffer buf;
        if (resolver_load_audio(resolved, &buf) != 0) {
            snprintf(response, max_resp, "ERR: failed to decode audio file '%s'", resolved);
            return;
        }

        if (servifunc_create(id, resolved, &buf, loop, end_act, loop_delay) != 0) {
            resolver_free_audio(&buf);
            snprintf(response, max_resp, "ERR: failed to launch audio thread for '%s'", id);
            return;
        }
        resolver_free_audio(&buf);

        char st[256];
        if (servifunc_status(id, st, sizeof(st)) == 0) {
            snprintf(response, max_resp, "OK: %s", st);
        } else {
            snprintf(response, max_resp, "OK: playing '%s'", id);
        }
        return;
    }

    snprintf(response, max_resp, "ERR: unknown command");
}

#if defined(_WIN32)
static DWORD WINAPI Win32IpcServerThread(LPVOID lpParam) {
    (void)lpParam;
    while (g_running) {
        HANDLE hPipe = CreateNamedPipeA(
            MINDRIVER_PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            MINDRIVER_IPC_BUFFER_SIZE,
            MINDRIVER_IPC_BUFFER_SIZE,
            0,
            NULL
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(100);
            continue;
        }

        BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            char in_buffer[MINDRIVER_IPC_BUFFER_SIZE];
            DWORD bytesRead = 0;
            if (ReadFile(hPipe, in_buffer, sizeof(in_buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
                in_buffer[bytesRead] = '\0';
                char out_buffer[MINDRIVER_IPC_BUFFER_SIZE];
                handle_ipc_request(in_buffer, out_buffer, sizeof(out_buffer));

                DWORD bytesWritten = 0;
                WriteFile(hPipe, out_buffer, (DWORD)strlen(out_buffer) + 1, &bytesWritten, NULL);
                FlushFileBuffers(hPipe);
            }
            DisconnectNamedPipe(hPipe);
        }
        CloseHandle(hPipe);
    }
    return 0;
}

static int try_client_connect(const char* cmd_str) {
    HANDLE hPipe = CreateFileA(
        MINDRIVER_PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (hPipe == INVALID_HANDLE_VALUE) {
        return 0; 
    }

    DWORD bytesWritten = 0;
    WriteFile(hPipe, cmd_str, (DWORD)strlen(cmd_str) + 1, &bytesWritten, NULL);

    char reply[MINDRIVER_IPC_BUFFER_SIZE];
    DWORD bytesRead = 0;
    if (ReadFile(hPipe, reply, sizeof(reply) - 1, &bytesRead, NULL) && bytesRead > 0) {
        reply[bytesRead] = '\0';
        printf("%s\n", reply);
    }

    CloseHandle(hPipe);
    return 1;
}

#else

static void* LinuxIpcServerThread(void* arg) {
    (void)arg;
    unlink(MINDRIVER_SOCKET_PATH);

    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) return NULL;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, MINDRIVER_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(sfd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(sfd);
        return NULL;
    }

    listen(sfd, 10);

    while (g_running) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            usleep(50000);
            continue;
        }

        char in_buffer[MINDRIVER_IPC_BUFFER_SIZE];
        ssize_t n = read(cfd, in_buffer, sizeof(in_buffer) - 1);
        if (n > 0) {
            in_buffer[n] = '\0';
            char out_buffer[MINDRIVER_IPC_BUFFER_SIZE];
            handle_ipc_request(in_buffer, out_buffer, sizeof(out_buffer));
            write(cfd, out_buffer, strlen(out_buffer) + 1);
        }
        close(cfd);
    }

    close(sfd);
    unlink(MINDRIVER_SOCKET_PATH);
    return NULL;
}

static int try_client_connect(const char* cmd_str) {
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) return 0;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, MINDRIVER_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sfd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(sfd);
        return 0;
    }

    write(sfd, cmd_str, strlen(cmd_str) + 1);

    char reply[MINDRIVER_IPC_BUFFER_SIZE];
    ssize_t n = read(sfd, reply, sizeof(reply) - 1);
    if (n > 0) {
        reply[n] = '\0';
        printf("%s\n", reply);
    }

    close(sfd);
    return 1;
}
#endif

static void print_usage(void) {
    printf("mindriver %s (%s)\n", MINDRIVER_VERSION, MINDRIVER_PUBLISHER);
    printf("usage:\n");
    printf("  mindriver <filepath> [--loop] [--stopatend|--deleteatend|--loopatend<1-20>]\n");
    printf("  mindriver <id> --stop\n");
    printf("  mindriver <id> --replay\n");
    printf("  mindriver <id> --delete\n");
    printf("  mindriver <id> --status\n");
    printf("  mindriver --status\n");
    printf("  mindriver --exit\n");
}

static int run_server_loop(void) {
#if defined(_WIN32)
    SetConsoleCtrlHandler(ConsoleHandlerRoutine, TRUE);
#else
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
#endif

    if (initcore_init() != 0) {
#if !defined(_WIN32)
        initcore_show_missing_alert();
#endif
        return 1;
    }

    servifunc_init();

#if defined(_WIN32)
    DWORD srvThreadId = 0;
    HANDLE hSrvThread = CreateThread(NULL, 0, Win32IpcServerThread, NULL, 0, &srvThreadId);
    (void)hSrvThread;
#else
    pthread_t srvThread;
    pthread_create(&srvThread, NULL, LinuxIpcServerThread, NULL);
#endif

    while (g_running) {
#if defined(_WIN32)
        Sleep(200);
#else
        usleep(200000);
#endif
    }

    servifunc_stop_all();
    initcore_shutdown();
    return 0;
}

static int spawn_daemon(void) {
#if defined(_WIN32)
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, exePath, sizeof(exePath)) == 0) {
        return -1;
    }

    char cmdLine[MAX_PATH + 32];
    snprintf(cmdLine, sizeof(cmdLine), "\"%s\" --server", exePath);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    BOOL ok = CreateProcessA(
        NULL,
        cmdLine,
        NULL,
        NULL,
        FALSE,
        DETACHED_PROCESS | CREATE_NO_WINDOW,
        NULL,
        NULL,
        &si,
        &pi
    );

    if (!ok) return -1;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    for (int i = 0; i < 40; i++) {
        if (WaitNamedPipeA(MINDRIVER_PIPE_NAME, 50)) {
            return 0;
        }
        Sleep(50);
    }
    return 0;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        run_server_loop();
        exit(0);
    }
    for (int i = 0; i < 40; i++) {
        if (access(MINDRIVER_SOCKET_PATH, F_OK) == 0) {
            return 0;
        }
        usleep(50000);
    }
    return 0;
#endif
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        print_usage();
        return 0;
    }

    // Internal background daemon
    if (argc == 2 && strcmp(argv[1], "--server") == 0) {
        return run_server_loop();
    }

    ParsedCmd cmd;
    parse_cli_args(argc, argv, &cmd);

    char cmd_str[MINDRIVER_IPC_BUFFER_SIZE];
    serialize_cmd(&cmd, cmd_str, sizeof(cmd_str));

    if (try_client_connect(cmd_str)) {
        return 0;
    }

    if (cmd.is_stop || cmd.is_replay || cmd.is_delete || cmd.is_status || cmd.is_exit) {
        printf("mindriver: no active server instance\n");
        return 1;
    }

    if (cmd.target[0] == '\0') {
        print_usage();
        return 1;
    }

#if !defined(_WIN32)
    if (initcore_init() != 0) {
        initcore_show_missing_alert();
        return 1;
    }
    initcore_shutdown();
#endif

    if (spawn_daemon() != 0) {
        printf("mindriver: failed to start background audio service\n");
        return 1;
    }

    if (!try_client_connect(cmd_str)) {
        printf("mindriver: failed to connect to audio service\n");
        return 1;
    }

    return 0;
}
