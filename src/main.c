/* Pull in XSI extensions for sigaltstack(2)/SA_ONSTACK/SIGSTKSZ; the project
 * already sets _POSIX_C_SOURCE=200809L, but those identifiers are XSI. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <sys/signalfd.h>
#include <ctype.h>
#include "neowall.h"
#include "config_access.h"
#include "constants.h"
#include "compositor.h"
#include "egl/egl_core.h"
#include "output/output.h"

/* Get path to the set-index command file */
static const char *get_set_index_file_path(void) {
    static char path[MAX_PATH_LENGTH];
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");

    if (runtime_dir) {
        snprintf(path, sizeof(path), "%s/neowall-set-index", runtime_dir);
    } else {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(path, sizeof(path), "%s/.neowall-set-index", home);
        } else {
            snprintf(path, sizeof(path), "/tmp/neowall-set-index-%d", getuid());
        }
    }

    return path;
}

/* Write the requested index to a file for the daemon to read */
static bool write_set_index_file(int index) {
    const char *path = get_set_index_file_path();
    int fd = open(path,
                  O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                  0600);
    if (fd < 0) {
        fprintf(stderr, "Failed to write index file: %s\n", strerror(errno));
        return false;
    }
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        fprintf(stderr, "fdopen failed: %s\n", strerror(errno));
        close(fd);
        return false;
    }
    fprintf(fp, "%d\n", index);
    fclose(fp);
    return true;
}

/* Read the requested index from the file (called by daemon) */
int read_set_index_file(void) {
    const char *path = get_set_index_file_path();
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    FILE *fp = fdopen(fd, "r");
    if (!fp) {
        close(fd);
        return -1;
    }
    int index = -1;
    if (fscanf(fp, "%d", &index) != 1) {
        index = -1;
    }
    fclose(fp);
    /* Remove the file after reading */
    unlink(path);
    return index;
}

static struct neowall_state *global_state = NULL;

/* Forward declarations */
static void handle_crash(int signum);
static const char *get_pid_file_path(void);
static void remove_pid_file(void);
static bool can_cycle_wallpaper(void);

/* Command descriptor structure */
typedef struct {
    const char *name;           /* Command name (e.g., "next") */
    int signal;                 /* Signal to send */
    const char *description;    /* Help text */
    const char *action_message; /* Message shown when executed */
    void (*handler)(int);       /* Signal handler function */
    bool needs_state_check;     /* Whether to check wallpaper state instead of signaling */
    bool check_cycle;           /* Whether to check if cycling is possible before sending signal */
} DaemonCommand;

/* Centralized command registry - Single source of truth
 * Note: 'set' command is handled specially, not via this table */
static DaemonCommand daemon_commands[] = {
    {"next",              SIGUSR1,      "Skip to next wallpaper",                  "Skipping to next wallpaper...",      NULL,  false, true},   /* check_cycle = true */
    {"pause",             SIGUSR2,      "Pause wallpaper cycling",                 "Pausing wallpaper cycling...",       NULL,  false, false},
    {"resume",            SIGCONT,      "Resume wallpaper cycling",                "Resuming wallpaper cycling...",      NULL,  false, false},
    {"set",               0,            "Set wallpaper by index (set <index>)",    NULL,                                 NULL,  false, false},   /* Handled specially */
    {"list",              0,            "List all wallpapers with indices",        NULL,                                 NULL,  false, false},   /* Handled specially */
    {"current",           0,            "Show current wallpaper",                  NULL,                                 NULL,  true,  false},
    {"status",            0,            "Show current wallpaper",                  NULL,                                 NULL,  true,  false},
    {NULL, 0, NULL, NULL, NULL, false, false}  /* Sentinel */
};



/* Get PID file path */
static const char *get_pid_file_path(void) {
    static char pid_path[MAX_PATH_LENGTH];
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");

    if (runtime_dir) {
        snprintf(pid_path, sizeof(pid_path), "%s/neowall.pid", runtime_dir);
    } else {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(pid_path, sizeof(pid_path), "%s/.neowall.pid", home);
        } else {
            snprintf(pid_path, sizeof(pid_path), "/tmp/neowall-%d.pid", getuid());
        }
    }

    return pid_path;
}

/* Read process start-time (jiffies since boot) from /proc/<pid>/stat field 22.
 * Returns 0 on failure (caller should treat as "unavailable" and skip the
 * recycling check). The field is unsigned long long. /proc/<pid>/stat has the
 * format: "pid (comm) state ppid ..." where comm itself may contain spaces and
 * parentheses, so we parse from the LAST ')' onward to avoid being fooled by
 * a process named e.g. "weird ) name". */
static unsigned long long read_proc_starttime(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;

    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    char *rparen = strrchr(buf, ')');
    if (!rparen) return 0;
    /* Field 3 (state) starts after ") ". After that we want field 22, i.e.
     * skip 19 more whitespace-delimited tokens (state, ppid ... itrealvalue). */
    char *p = rparen + 1;
    for (int i = 0; i < 19; i++) {
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
        if (!*p) return 0;
    }
    while (*p == ' ') p++;
    unsigned long long st = 0;
    if (sscanf(p, "%llu", &st) != 1) return 0;
    return st;
}

/* Write PID file. We append the start-time (in /proc clock ticks since boot)
 * on a second line so subsequent daemon-running checks can detect PID
 * recycling: if a new process happens to be assigned the same PID after our
 * daemon dies, its start-time will not match. The second line is optional;
 * older readers ignore it gracefully. */
static bool write_pid_file(void) {
    const char *pid_path = get_pid_file_path();
    int fd = open(pid_path,
                  O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                  0600);
    if (fd < 0) {
        log_error("Failed to create PID file %s: %s", pid_path, strerror(errno));
        return false;
    }

    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        log_error("Failed to fdopen PID file: %s", strerror(errno));
        close(fd);
        return false;
    }

    unsigned long long starttime = read_proc_starttime(getpid());
    fprintf(fp, "%d\n%llu\n", getpid(), starttime);
    fclose(fp);

    log_debug("Created PID file: %s", pid_path);
    return true;
}

/* Read PID and (optional) start-time from PID file. Returns true on success;
 * `*out_starttime` is set to 0 if no second line is present. */
static bool read_pid_file(pid_t *out_pid, unsigned long long *out_starttime) {
    const char *pid_path = get_pid_file_path();
    FILE *fp = fopen(pid_path, "r");
    if (!fp) return false;

    pid_t pid;
    if (fscanf(fp, "%d", &pid) != 1) {
        fclose(fp);
        return false;
    }
    unsigned long long st = 0;
    /* Best-effort: file may pre-date the start-time addition; if absent we
     * leave `st = 0` and the verify pass falls back to plain kill(0). */
    if (fscanf(fp, "%llu", &st) != 1) {
        st = 0;
    }
    fclose(fp);

    *out_pid = pid;
    *out_starttime = st;
    return true;
}

/* Verify that the process at `pid` matches the recorded start-time `expected`.
 * Returns true if the start-time matches OR if /proc is unavailable (we cannot
 * check, so we fall back to the old kill(0)-style behaviour). Returns false
 * only when /proc says a different start-time, i.e. PID was recycled. */
static bool verify_pid_starttime(pid_t pid, unsigned long long expected) {
    if (expected == 0) return true;  /* No recorded start-time: skip check. */
    unsigned long long actual = read_proc_starttime(pid);
    if (actual == 0) return true;    /* /proc unreadable: fall back. */
    return actual == expected;
}

/* Remove PID file */
static void remove_pid_file(void) {
    const char *pid_path = get_pid_file_path();
    if (unlink(pid_path) == 0) {
        log_debug("Removed PID file: %s", pid_path);
    }
}

/* Check if daemon is already running */
static bool is_daemon_running(void) {
    pid_t pid;
    unsigned long long starttime;
    if (!read_pid_file(&pid, &starttime)) {
        /* Either no PID file (daemon not running) or unreadable contents.
         * Distinguish via stat() to keep behaviour: missing -> not running;
         * present-but-corrupt -> remove and treat as not running. */
        const char *pid_path = get_pid_file_path();
        struct stat st;
        if (stat(pid_path, &st) == 0) {
            log_debug("Invalid PID file, removing");
            remove_pid_file();
        }
        return false;
    }

    /* Detect PID recycling: if /proc says the live process at this PID has a
     * different start-time, our daemon died and its PID was reused. */
    if (!verify_pid_starttime(pid, starttime)) {
        log_debug("PID %d was recycled (start-time mismatch), removing stale PID file", pid);
        remove_pid_file();
        return false;
    }

    /* Check if process exists */
    if (kill(pid, 0) == 0) {
        /* Process exists and we can signal it */
        return true;
    }

    if (errno == ESRCH) {
        /* Process doesn't exist, stale PID file */
        log_debug("Stale PID file found (PID %d not running), removing", pid);
        remove_pid_file();
        return false;
    }

    /* Other error (EPERM, etc.) - assume process exists */
    return true;
}

/* Kill running daemon */
static bool kill_daemon(void) {
    const char *pid_path = get_pid_file_path();
    pid_t pid;
    unsigned long long starttime;
    if (!read_pid_file(&pid, &starttime)) {
        printf("No running neowall daemon found (no PID file at %s)\n", pid_path);
        return false;
    }

    /* Refuse to signal a recycled PID. */
    if (!verify_pid_starttime(pid, starttime)) {
        printf("PID %d was recycled by another process. Cleaning up stale PID file.\n", pid);
        remove_pid_file();
        return false;
    }

    /* Check if process exists */
    if (kill(pid, 0) == -1) {
        if (errno == ESRCH) {
            printf("NeoWall daemon (PID %d) is not running. Cleaning up stale PID file.\n", pid);
            remove_pid_file();
            return false;
        }
    }

    /* Send SIGTERM */
    printf("Stopping neowall daemon (PID %d)...\n", pid);
    if (kill(pid, SIGTERM) == -1) {
        log_error("Failed to kill process %d: %s", pid, strerror(errno));
        return false;
    }

    /* Wait a bit for graceful shutdown */
    struct timespec sleep_time = {0, SLEEP_100MS_NS};  /* 100ms */
    int attempts = 0;
    while (attempts < 50) {  /* Wait up to 5 seconds */
        if (kill(pid, 0) == -1 && errno == ESRCH) {
            printf("NeoWall daemon stopped successfully.\n");
            remove_pid_file();
            return true;
        }
        nanosleep(&sleep_time, NULL);
        attempts++;
    }

    /* Force kill if still running */
    printf("Daemon didn't stop gracefully, forcing...\n");
    if (kill(pid, SIGKILL) == 0) {
        printf("NeoWall daemon killed.\n");
        remove_pid_file();
        return true;
    }

    log_error("Failed to kill daemon process");
    return false;
}

/* Send signal to running daemon */
/* Check if cycling is possible by reading state file */
static bool can_cycle_wallpaper(void) {
    /* BUG FIX #7: Add file-level locking for cross-process synchronization
     * Note: We can't use state_file_lock mutex here because we're in a different
     * process. We need file-level locking (flock/fcntl) for cross-process sync. */
    const char *state_path = get_state_file_path();
    FILE *fp = fopen(state_path, "r");

    if (!fp) {
        return false;  /* No state file = unknown, let daemon handle it */
    }

    /* Acquire read lock on the file */
    int fd = fileno(fp);
    struct flock lock = {
        .l_type = F_RDLCK,      /* Read lock */
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0              /* Lock entire file */
    };

    if (fcntl(fd, F_SETLKW, &lock) == -1) {
        /* Failed to lock, but continue anyway (non-critical) */
        log_debug("Failed to lock state file for reading: %s", strerror(errno));
    }

    char line[MAX_PATH_LENGTH];
    int max_cycle_total = 0;

    /* BUG FIX #8: Check ALL outputs for cycle_total, not just the first one.
     * The state file contains multiple [output] sections, and we need to find
     * if ANY of them have cycle_total > 1 (can cycle). Previously, we stopped
     * at the first cycle_total= line, which might be 0 for a non-cycling output. */
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = 0;
        if (strncmp(line, "cycle_total=", 12) == 0) {
            int cycle_total = atoi(line + 12);
            if (cycle_total > max_cycle_total) {
                max_cycle_total = cycle_total;
            }
            /* Don't break - continue checking all outputs */
        }
    }

    /* Release lock */
    lock.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &lock);

    fclose(fp);
    return max_cycle_total > 1;  /* Can cycle if ANY output has more than 1 wallpaper */
}

static bool send_daemon_signal(int signal, const char *action, bool check_cycle) {
    pid_t pid;
    unsigned long long starttime;
    if (!read_pid_file(&pid, &starttime)) {
        printf("No running neowall daemon found.\n");
        printf("Start the daemon first with: neowall\n");
        return false;
    }

    /* Refuse to signal a recycled PID. */
    if (!verify_pid_starttime(pid, starttime)) {
        printf("PID %d was recycled by another process. Cleaning up stale PID file.\n", pid);
        remove_pid_file();
        return false;
    }

    /* Check if process exists */
    if (kill(pid, 0) == -1) {
        if (errno == ESRCH) {
            printf("NeoWall daemon (PID %d) is not running.\n", pid);
            remove_pid_file();
            return false;
        }
    }

    /* For 'next' command, check if cycling is possible before claiming to skip */
    if (check_cycle && !can_cycle_wallpaper()) {
        printf("Cannot cycle wallpaper: Only one wallpaper/shader configured.\n");
        printf("\n");
        printf("To enable cycling:\n");
        printf("  - Use a directory path ending with '/' in your config\n");
        printf("    Example: path ~/Pictures/Wallpapers/\n");
        printf("  - Or configure a 'duration' to cycle through wallpapers\n");
        printf("  - Multiple files will be loaded and cycled alphabetically\n");
        printf("\n");
        printf("Check current status with: neowall current\n");
        return false;
    }

    /* Send signal */
    if (kill(pid, signal) == -1) {
        log_error("Failed to send signal to daemon: %s", strerror(errno));
        return false;
    }

    printf("%s\n", action);
    return true;
}

static void print_usage(const char *program_name) {
    printf("NeoWall v%s - GPU-accelerated wallpapers for Wayland. Take the red pill. 🔴\n\n", NEOWALL_VERSION_STRING);
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("       %s set <index>   Set wallpaper by index (0-based)\n\n", program_name);
    printf("Options:\n");
    printf("  -c, --config PATH     Path to configuration file\n");
    printf("  -f, --foreground      Run in foreground (for debugging)\n");
    printf("  -v, --verbose         Enable verbose logging\n");
    printf("  -h, --help            Show this help message\n");
    printf("  -V, --version         Show version information\n");
    printf("\n");
    printf("Daemon Control Commands (when daemon is running):\n");
    printf("  kill                  Stop running daemon\n");

    /* Auto-generate command list from table - DRY principle */
    for (size_t i = 0; daemon_commands[i].name != NULL; i++) {
        /* Skip duplicate "status" command (alias for "current") */
        if (strcmp(daemon_commands[i].name, "status") == 0) {
            continue;
        }
        printf("  %-21s %s\n", daemon_commands[i].name, daemon_commands[i].description);
    }
    printf("\n");
    printf("Note: By default, neowall runs as a daemon. Use -f for foreground.\n");
    printf("If a daemon is already running, subsequent calls act as control commands.\n");
    printf("\n");
    printf("Configuration file locations (in order of preference):\n");
    printf("  1. $XDG_CONFIG_HOME/neowall/config.vibe\n");
    printf("  2. $HOME/.config/neowall/config.vibe\n");
    printf("  3. /etc/neowall/config.vibe\n");
    printf("\n");
    printf("Example config.vibe:\n");
    printf("  default {\n");
    printf("    path ~/Pictures/wallpaper.png\n");
    printf("    mode fill\n");
    printf("  }\n");
    printf("\n");
    printf("  output {\n");
    printf("    eDP-1 {\n");
    printf("      path ~/Pictures/laptop-wallpaper.jpg\n");
    printf("      mode fit\n");
    printf("    }\n");
    printf("  }\n");
    printf("\n");
}

static void print_version(void) {
    printf("NeoWall v%s\n", NEOWALL_VERSION_STRING);
    printf("GPU-accelerated wallpapers for Wayland.\n");
    printf("Take the red pill. 🔴💊\n");
    printf("\nSupported features:\n");
    printf("  - Live GPU shaders at 60 FPS (Shadertoy compatible)\n");
    printf("  - 2%% CPU usage (lighter than video wallpapers)\n");
    printf("  - Multi-monitor support\n");
    printf("  - Smooth transitions (fade, slide, glitch, pixelate)\n");
    printf("  - Works on Hyprland, Sway, River, and other Wayland compositors\n");
    printf("\nSupported image formats:\n");
    printf("  - PNG\n");
    printf("  - JPEG/JPG\n");
}

/* ============================================================================
 * Signal Handling Using signalfd - RACE-FREE APPROACH
 * ============================================================================
 *
 * Instead of traditional signal handlers, we use signalfd which converts
 * signals into file descriptor events. This approach:
 *
 * 1. Eliminates ALL signal handler race conditions
 * 2. No async-signal-safe restrictions (can use any functions)
 * 3. Integrates cleanly with poll() event loop
 * 4. Guaranteed signal delivery and ordering
 * 5. No signal handler execution context issues
 *
 * Signals are blocked for all threads and read from signalfd in main loop.
 * ============================================================================ */

/* Process signals received via signalfd - called from eventloop.c */
void handle_signal_from_fd(struct neowall_state *state, int signum) {
    switch (signum) {
        case SIGTERM:
        case SIGINT:
            log_info("Received shutdown signal %d, exiting gracefully...", signum);
            atomic_store_explicit(&state->running, false, memory_order_release);
            break;

        case SIGUSR1: {
            int old_count = atomic_fetch_add_explicit(&state->next_requested, 1, memory_order_acq_rel);
            int new_count = old_count + 1;
            log_info("Received SIGUSR1, skipping to next wallpaper (queue: %d -> %d)",
                     old_count, new_count);

            /* Prevent counter overflow */
            if (new_count > MAX_NEXT_REQUESTS) {
                log_error("Too many queued next requests (%d), resetting to 10", new_count);
                /* BUG FIX #6: Use seq_cst to prevent reordering on weak architectures */
                atomic_store_explicit(&state->next_requested, 10, memory_order_seq_cst);
            }
            break;
        }

        case SIGUSR2:
            log_info("Received SIGUSR2, pausing wallpaper cycling...");
            atomic_store_explicit(&state->paused, true, memory_order_release);
            break;

        case SIGCONT:
            log_info("Received SIGCONT, resuming wallpaper cycling...");
            atomic_store_explicit(&state->paused, false, memory_order_release);
            break;

        default:
            /* Check for SIGRTMIN (real-time signal for set-index) */
            if (signum == SIGRTMIN) {
                /* Read the requested index from file */
                int index = read_set_index_file();
                if (index >= 0) {
                    log_info("Received SIGRTMIN, setting wallpaper to index %d", index);
                    atomic_store_explicit(&state->set_index_requested, index, memory_order_release);
                } else {
                    log_error("Received SIGRTMIN but no valid index file found");
                }
            } else {
                log_debug("Received signal: %d", signum);
            }
            break;
    }
}

/* Handle crash signals — async-signal-safe ONLY.
 * No localtime/printf/exit; just write a fixed message and _exit. */
static void handle_crash(int signum) {
    const char *signame;
    switch (signum) {
        case SIGSEGV: signame = "neowall: fatal SIGSEGV\n"; break;
        case SIGBUS:  signame = "neowall: fatal SIGBUS\n";  break;
        case SIGILL:  signame = "neowall: fatal SIGILL\n";  break;
        case SIGFPE:  signame = "neowall: fatal SIGFPE\n";  break;
        case SIGABRT: signame = "neowall: fatal SIGABRT\n"; break;
        default:      signame = "neowall: fatal signal\n";  break;
    }
    /* write(2) is async-signal-safe; ignore short-write/EINTR — best effort. */
    ssize_t r = write(STDERR_FILENO, signame, strlen(signame));
    (void)r;
    _exit(128 + signum);
}

/* ============================================================================
 * Signal Setup Using signalfd - RACE-FREE APPROACH
 * ============================================================================ */

static int setup_signalfd(void) {
    sigset_t mask;
    sigemptyset(&mask);

    /* Block all signals we want to handle via signalfd */
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    sigaddset(&mask, SIGCONT);
    sigaddset(&mask, SIGRTMIN);  /* For set-index command */

    /* Block these signals for all threads */
    if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0) {
        log_error("Failed to block signals: %s", strerror(errno));
        return -1;
    }

    /* Create signalfd to receive signals as file descriptor events */
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) {
        log_error("Failed to create signalfd: %s", strerror(errno));
        return -1;
    }

    log_info("Signal handling configured with signalfd (race-free)");
    return sfd;
}

static void setup_crash_handlers(void) {
    /* Run crash handlers on a dedicated stack so SIGSEGV from stack overflow
     * still has somewhere to live. */
    static char altstack[SIGSTKSZ];
    stack_t ss = { .ss_sp = altstack, .ss_size = SIGSTKSZ, .ss_flags = 0 };
    if (sigaltstack(&ss, NULL) != 0) {
        log_error("Failed to install signal alt-stack: %s", strerror(errno));
    }

    struct sigaction crash_sa;
    memset(&crash_sa, 0, sizeof(crash_sa));
    crash_sa.sa_handler = handle_crash;
    sigemptyset(&crash_sa.sa_mask);
    crash_sa.sa_flags = SA_RESETHAND | SA_ONSTACK;

    sigaction(SIGSEGV, &crash_sa, NULL);
    sigaction(SIGBUS,  &crash_sa, NULL);
    sigaction(SIGILL,  &crash_sa, NULL);
    sigaction(SIGFPE,  &crash_sa, NULL);
    sigaction(SIGABRT, &crash_sa, NULL);

    /* Ignore SIGPIPE */
    struct sigaction ign_sa;
    memset(&ign_sa, 0, sizeof(ign_sa));
    ign_sa.sa_handler = SIG_IGN;
    sigemptyset(&ign_sa.sa_mask);
    sigaction(SIGPIPE, &ign_sa, NULL);

    log_debug("Crash signal handlers installed");
}

static bool daemonize(void) {
    pid_t pid = fork();

    if (pid < 0) {
        log_error("Failed to fork: %s", strerror(errno));
        return false;
    }

    if (pid > 0) {
        /* Parent process exits */
        exit(EXIT_SUCCESS);
    }

    /* Child process continues */
    if (setsid() < 0) {
        log_error("Failed to create new session: %s", strerror(errno));
        return false;
    }

    /* Fork again to prevent acquiring a controlling terminal */
    pid = fork();
    if (pid < 0) {
        log_error("Failed to fork second time: %s", strerror(errno));
        return false;
    }

    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    /* Change working directory to root */
    if (chdir("/") < 0) {
        log_error("Failed to change directory: %s", strerror(errno));
        return false;
    }

    /* Close standard file descriptors */
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    /* Redirect to /dev/null */
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) {
            close(fd);
        }
    }

    /* Write PID file */
    if (!write_pid_file()) {
        log_error("Failed to write PID file, but continuing anyway");
    }

    return true;
}

static bool create_config_directory(void) {
    const char *config_home = getenv("XDG_CONFIG_HOME");
    char config_dir[MAX_PATH_LENGTH];

    if (config_home) {
        snprintf(config_dir, sizeof(config_dir), "%s/neowall", config_home);
    } else {
        const char *home = getenv("HOME");
        if (!home) {
            log_error("Cannot determine home directory");
            return false;
        }
        snprintf(config_dir, sizeof(config_dir), "%s/.config/neowall", home);
    }

    /* Create directories recursively */
    char tmp[MAX_PATH_LENGTH];
    char *p = NULL;
    snprintf(tmp, sizeof(tmp), "%s", config_dir);

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) == -1 && errno != EEXIST) {
                log_error("Failed to create directory %s: %s", tmp, strerror(errno));
                return false;
            }
            *p = '/';
        }
    }

    struct stat st;
    bool created_final_dir = false;
    if (stat(config_dir, &st) == -1) {
        if (mkdir(tmp, 0755) == -1) {
            log_error("Failed to create config directory %s: %s", tmp, strerror(errno));
            return false;
        }
        created_final_dir = true;
    }

    if (created_final_dir) {
        log_info("Created config directory: %s", config_dir);
    }
    return true;
}

int main(int argc, char *argv[]) {

    struct neowall_state state = {0};
    char config_path[MAX_PATH_LENGTH] = {0};
    bool daemon_mode = true;  /* Default to daemon mode */
    bool verbose = false;
    int opt;

    /* ========================================================================
     * Command Dispatch - Table-driven lookup (Command Pattern)
     * ======================================================================== */
    if (argc >= 2 && argv[1][0] != '-') {
        const char *cmd = argv[1];

        /* Special case: kill command */
        if (strcmp(cmd, "kill") == 0) {
            return kill_daemon() ? EXIT_SUCCESS : EXIT_FAILURE;
        }

        /* Special case: list command shows all wallpapers with indices */
        if (strcmp(cmd, "list") == 0) {
            return read_cycle_list() ? EXIT_SUCCESS : EXIT_FAILURE;
        }

        /* Special case: set command requires an index argument */
        if (strcmp(cmd, "set") == 0) {
            if (argc < 3) {
                fprintf(stderr, "Usage: %s set <index>\n", argv[0]);
                fprintf(stderr, "  <index>  Wallpaper index (0-based)\n");
                fprintf(stderr, "\nUse '%s current' to see available wallpapers and their indices.\n", argv[0]);
                return EXIT_FAILURE;
            }
            /* Validate index is a number */
            const char *index_str = argv[2];
            for (const char *p = index_str; *p; p++) {
                if (!isdigit((unsigned char)*p)) {
                    fprintf(stderr, "Error: Index must be a non-negative integer, got '%s'\n", index_str);
                    return EXIT_FAILURE;
                }
            }
            int index = atoi(index_str);

            /* Send set-index command to daemon */
            pid_t pid;
            unsigned long long starttime;
            if (!read_pid_file(&pid, &starttime)) {
                printf("No running neowall daemon found.\n");
                printf("Start the daemon first with: neowall\n");
                return EXIT_FAILURE;
            }

            /* Refuse to signal a recycled PID. */
            if (!verify_pid_starttime(pid, starttime)) {
                printf("PID %d was recycled by another process. Cleaning up stale PID file.\n", pid);
                remove_pid_file();
                return EXIT_FAILURE;
            }

            /* Check if process exists */
            if (kill(pid, 0) == -1 && errno == ESRCH) {
                printf("NeoWall daemon (PID %d) is not running.\n", pid);
                remove_pid_file();
                return EXIT_FAILURE;
            }

            /* Check if cycling is possible */
            if (!can_cycle_wallpaper()) {
                printf("Cannot set wallpaper index: Only one wallpaper/shader configured.\n");
                return EXIT_FAILURE;
            }

            /* Write the index to a file for the daemon to read */
            if (!write_set_index_file(index)) {
                return EXIT_FAILURE;
            }

            /* Send SIGRTMIN to notify daemon */
            if (kill(pid, SIGRTMIN) == -1) {
                fprintf(stderr, "Failed to send signal to daemon: %s\n", strerror(errno));
                unlink(get_set_index_file_path());
                return EXIT_FAILURE;
            }

            printf("Setting wallpaper to index %d...\n", index);
            return EXIT_SUCCESS;
        }

        /* Lookup command in table and dispatch */
        for (size_t i = 0; daemon_commands[i].name != NULL; i++) {
            if (strcmp(cmd, daemon_commands[i].name) == 0) {
                /* Command found - execute it */
                if (daemon_commands[i].needs_state_check) {
                    /* Commands like "current" that read state */
                    return read_wallpaper_state() ? EXIT_SUCCESS : EXIT_FAILURE;
                } else {
                    /* Commands that send signals to daemon */
                    return send_daemon_signal(daemon_commands[i].signal,
                                            daemon_commands[i].action_message,
                                            daemon_commands[i].check_cycle)
                           ? EXIT_SUCCESS : EXIT_FAILURE;
                }
            }
        }

        /* Command not found - print error with available commands */
        fprintf(stderr, "Unknown command: %s\n\n", cmd);
        fprintf(stderr, "Available commands:\n");
        fprintf(stderr, "  kill");
        for (size_t i = 0; daemon_commands[i].name != NULL; i++) {
            fprintf(stderr, ", %s", daemon_commands[i].name);
        }
        fprintf(stderr, "\n\nRun '%s --help' for more information.\n", argv[0]);
        return EXIT_FAILURE;
    }

    static struct option long_options[] = {
        {"config",     required_argument, 0, 'c'},
        {"foreground", no_argument,       0, 'f'},
        {"verbose",    no_argument,       0, 'v'},
        {"help",       no_argument,       0, 'h'},
        {"version",    no_argument,       0, 'V'},
        {0, 0, 0, 0}
    };

    /* Parse command line arguments */
    while ((opt = getopt_long(argc, argv, "c:fvhV", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c':
                strncpy(config_path, optarg, sizeof(config_path) - 1);
                break;
            case 'f':
                daemon_mode = false;  /* Explicit foreground */
                break;
            case 'v':
                /* Verbose mode - enable debug logging */
                verbose = true;
                break;
            case 'h':
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            case 'V':
                print_version();
                return EXIT_SUCCESS;
            default:
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    /* Set up logging */
    if (verbose) {
        log_set_level(LOG_LEVEL_DEBUG);
    }
    log_info("NeoWall v%s starting...", NEOWALL_VERSION_STRING);

    /* Ensure config directory exists */
    if (!create_config_directory()) {
        log_error("Failed to create configuration directory");
        return EXIT_FAILURE;
    }

    /* Determine config file path */
    if (config_path[0] == '\0') {
        const char *default_path = config_get_default_path();
        if (default_path) {
            strncpy(config_path, default_path, sizeof(config_path) - 1);
        } else {
            log_error("Could not determine config file path");
            return EXIT_FAILURE;
        }
    }

    log_info("Using configuration file: %s", config_path);

    /* Check if already running */
    if (is_daemon_running()) {
        const char *pid_path = get_pid_file_path();
        FILE *fp = fopen(pid_path, "r");
        pid_t existing_pid = 0;
        if (fp) {
            if (fscanf(fp, "%d", &existing_pid) != 1) {
                existing_pid = 0;
            }
            fclose(fp);
        }
        log_error("NeoWall is already running (PID %d)", existing_pid);
        fprintf(stderr, "Error: NeoWall is already running (PID %d)\n", existing_pid);
        fprintf(stderr, "PID file: %s\n", pid_path);
        fprintf(stderr, "Use 'neowall kill' to stop the running instance.\n");
        return EXIT_FAILURE;
    }

    /* Daemonize if requested */
    if (daemon_mode) {
        log_info("Running as daemon...");
        if (!daemonize()) {
            return EXIT_FAILURE;
        }
    } else {
        /* In foreground mode, still write PID file for client commands */
        if (!write_pid_file()) {
            log_error("Failed to write PID file, but continuing anyway");
        }
    }

    /* Set up crash handlers first */
    setup_crash_handlers();
    global_state = &state;

    /* Initialize atomic state flags - MUST use atomic_init before any access */
    atomic_init(&state.running, true);
    atomic_init(&state.paused, false);
    atomic_init(&state.outputs_need_init, false);
    atomic_init(&state.next_requested, 0);
    atomic_init(&state.set_index_requested, -1);  /* -1 means no request */
    state.timer_fd = -1;
    state.wakeup_fd = -1;
    strncpy(state.config_path, config_path, sizeof(state.config_path) - 1);
    state.config_path[sizeof(state.config_path) - 1] = '\0';
    pthread_mutex_init(&state.state_mutex, NULL);
    pthread_rwlock_init(&state.output_list_lock, NULL);
    pthread_mutex_init(&state.state_file_lock, NULL);

    /* Set up signalfd for race-free signal handling */
    state.signal_fd = setup_signalfd();
    if (state.signal_fd < 0) {
        log_error("Failed to set up signal handling");
        return EXIT_FAILURE;
    }

    /* Initialize compositor backend (auto-detects Wayland/X11) */
    log_info("Initializing compositor backend...");
    state.compositor_backend = compositor_backend_init(&state);
    if (!state.compositor_backend) {
        log_error("Failed to initialize compositor backend");
        log_error("Ensure you're running under a Wayland compositor or X11 window manager");
        close(state.signal_fd);
        return EXIT_FAILURE;
    }

    log_info("Compositor backend initialized: %s", state.compositor_backend->name);
    log_info("Description: %s", state.compositor_backend->description);

    /* Initialize EGL/OpenGL */
    if (!egl_core_init(&state)) {
        log_error("Failed to initialize EGL");
        compositor_backend_cleanup(state.compositor_backend);
        close(state.signal_fd);
        return EXIT_FAILURE;
    }

    /* Load configuration and apply to outputs */
    if (!config_load(&state, config_path)) {
        log_error("Failed to load configuration");
        egl_core_cleanup(&state);
        compositor_backend_cleanup(state.compositor_backend);
        close(state.signal_fd);
        return EXIT_FAILURE;
    }

    log_info("Initialization complete, entering main loop...");

    /* Run main event loop */
    event_loop_run(&state);

    /* Cleanup */
    log_info("Shutting down...");

    /* Set alarm as last resort - force exit after 2 seconds if cleanup hangs */
    alarm(2);

    /* Quick cleanup - don't spend too much time on this during shutdown */
    egl_core_cleanup(&state);

    /* Cleanup compositor backend */
    if (state.compositor_backend) {
        compositor_backend_cleanup(state.compositor_backend);
        state.compositor_backend = NULL;
    }

    /* Close signal fd */
    if (state.signal_fd >= 0) {
        close(state.signal_fd);
    }

    /* Skip mutex/lock destruction during fast shutdown - OS will clean up */
    pthread_rwlock_destroy(&state.output_list_lock);
    pthread_mutex_destroy(&state.state_mutex);
    pthread_mutex_destroy(&state.state_file_lock);

    /* Remove PID file */
    remove_pid_file();

    /* Cancel alarm - we finished cleanup in time */
    alarm(0);

    log_info("NeoWall terminated successfully");

    return EXIT_SUCCESS;
}
