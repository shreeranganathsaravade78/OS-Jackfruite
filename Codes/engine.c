// engine.c
//
// ══════════════════════════════════════════════════════════════
//  WHAT WE (STUDENTS) WROTE vs WHAT PROFESSOR GAVE
//
//  Professor gave: skeleton with empty function stubs,
//                  the overall design (supervisor + CLI idea)
//  We wrote:       ALL the logic, IPC, threads, containers
// ══════════════════════════════════════════════════════════════
//
// This is the USER-SPACE part of the project.
// It is ONE binary used in TWO ways:
//
//   Mode 1 — Supervisor (long-running daemon):
//     sudo ./engine supervisor ./rootfs-base
//     → stays alive, manages containers, runs logging threads
//
//   Mode 2 — CLI client (short-lived command):
//     sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
//     sudo ./engine ps
//     sudo ./engine logs alpha
//     sudo ./engine stop alpha
//     → sends command to supervisor over a UNIX socket, prints reply, exits
//
// IPC mechanisms used:
//   1. UNIX domain socket  → CLI ↔ Supervisor control channel
//   2. Pipes               → Container stdout/stderr → Supervisor logging
//
// Synchronization:
//   - pthread_mutex  → protects the container metadata array
//   - pthread_mutex + pthread_cond → bounded log buffer (producer-consumer)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sched.h>
#include <sys/ioctl.h>
#include "monitor_ioctl.h"

/* ─────────────────────────────────────────────
 * Constants
 * ───────────────────────────────────────────── */
#define MAX_CONTAINERS   16
#define LOG_DIR          "/tmp/container_logs"
#define SOCKET_PATH      "/tmp/engine_supervisor.sock"
#define BUF_SLOTS        64      /* bounded buffer size (number of log lines) */
#define BUF_LINE_LEN     512     /* max chars per log line                    */
#define MONITOR_DEV      "/dev/container_monitor"

/* ─────────────────────────────────────────────
 * Container states
 * ───────────────────────────────────────────── */
typedef enum {
    STATE_FREE     = 0,
    STATE_STARTING = 1,
    STATE_RUNNING  = 2,
    STATE_STOPPED  = 3,
    STATE_KILLED   = 4,   /* killed by supervisor stop command */
    STATE_MEM_KILL = 5,   /* killed by kernel memory monitor   */
} ContainerState;

static const char *state_name(ContainerState s) {
    switch (s) {
        case STATE_STARTING: return "starting";
        case STATE_RUNNING:  return "running";
        case STATE_STOPPED:  return "stopped";
        case STATE_KILLED:   return "killed";
        case STATE_MEM_KILL: return "mem-killed";
        default:             return "free";
    }
}

/* ─────────────────────────────────────────────
 * Container metadata — one per running container
 * ───────────────────────────────────────────── */
typedef struct {
    char           name[64];
    pid_t          host_pid;
    time_t         start_time;
    ContainerState state;
    long           soft_mib;       /* soft memory limit in MiB */
    long           hard_mib;       /* hard memory limit in MiB */
    char           log_path[256];
    int            exit_status;
    int            log_pipe_fd;    /* supervisor read end of container pipe */
} Container;

/* ─────────────────────────────────────────────
 * Bounded log buffer — producer-consumer
 *
 * Each log "slot" holds one text line.
 * Containers (producers) write to slots.
 * Logger thread (consumer) reads from slots and writes to files.
 * ───────────────────────────────────────────── */
typedef struct {
    char     lines[BUF_SLOTS][BUF_LINE_LEN];
    char     names[BUF_SLOTS][64];   /* which container produced each line */
    int      head;                   /* consumer reads from here  */
    int      tail;                   /* producer writes here      */
    int      count;                  /* how many slots are filled */
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;       /* consumer waits on this    */
    pthread_cond_t  not_full;        /* producer waits on this    */
} LogBuffer;

/* ─────────────────────────────────────────────
 * Global state (supervisor process)
 * ───────────────────────────────────────────── */
static Container      containers[MAX_CONTAINERS];
static pthread_mutex_t containers_mutex = PTHREAD_MUTEX_INITIALIZER;
static LogBuffer       log_buf;
static volatile int    supervisor_running = 1;
static int             monitor_fd = -1;   /* fd to /dev/container_monitor */

/* ═══════════════════════════════════════════════════════════════
 * HELPER: find container by name (call with containers_mutex held)
 * ═══════════════════════════════════════════════════════════════ */
static Container *find_container(const char *name)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].state != STATE_FREE &&
            strcmp(containers[i].name, name) == 0)
            return &containers[i];
    return NULL;
}

static Container *find_free_slot(void)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].state == STATE_FREE)
            return &containers[i];
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * LOG BUFFER — push one line (called from reader threads)
 * ═══════════════════════════════════════════════════════════════ */
static void logbuf_push(const char *cname, const char *line)
{
    pthread_mutex_lock(&log_buf.mutex);

    /* Wait if buffer is full (backpressure) */
    while (log_buf.count == BUF_SLOTS)
        pthread_cond_wait(&log_buf.not_full, &log_buf.mutex);

    strncpy(log_buf.lines[log_buf.tail], line,    BUF_LINE_LEN - 1);
    strncpy(log_buf.names[log_buf.tail], cname,   63);
    log_buf.tail  = (log_buf.tail + 1) % BUF_SLOTS;
    log_buf.count++;

    pthread_cond_signal(&log_buf.not_empty);   /* wake the consumer */
    pthread_mutex_unlock(&log_buf.mutex);
}

/* ═══════════════════════════════════════════════════════════════
 * LOG CONSUMER THREAD — reads from buffer, writes to log files
 * ═══════════════════════════════════════════════════════════════ */
static void *logger_consumer(void *arg)
{
    (void)arg;
    char line[BUF_LINE_LEN];
    char cname[64];

    while (supervisor_running || log_buf.count > 0) {
        pthread_mutex_lock(&log_buf.mutex);

        while (log_buf.count == 0 && supervisor_running)
            pthread_cond_wait(&log_buf.not_empty, &log_buf.mutex);

        if (log_buf.count == 0) {
            pthread_mutex_unlock(&log_buf.mutex);
            break;
        }

        strncpy(line,  log_buf.lines[log_buf.head], BUF_LINE_LEN - 1);
        strncpy(cname, log_buf.names[log_buf.head],  63);
        log_buf.head  = (log_buf.head + 1) % BUF_SLOTS;
        log_buf.count--;

        pthread_cond_signal(&log_buf.not_full);   /* wake producers */
        pthread_mutex_unlock(&log_buf.mutex);

        /* Find the log file path for this container and append */
        char log_path[256] = {0};
        pthread_mutex_lock(&containers_mutex);
        Container *c = find_container(cname);
        if (c) strncpy(log_path, c->log_path, 255);
        pthread_mutex_unlock(&containers_mutex);

        if (log_path[0]) {
            FILE *f = fopen(log_path, "a");
            if (f) {
                fputs(line, f);
                fclose(f);
            }
        }
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * PIPE READER THREAD — one per container
 * reads lines from the container's stdout/stderr pipe,
 * pushes them into the bounded log buffer
 * ═══════════════════════════════════════════════════════════════ */
typedef struct { char name[64]; int fd; } ReaderArg;

static void *pipe_reader(void *arg)
{
    ReaderArg *ra = (ReaderArg *)arg;
    char line[BUF_LINE_LEN];
    FILE *fp = fdopen(ra->fd, "r");

    if (fp) {
        while (fgets(line, sizeof(line), fp))
            logbuf_push(ra->name, line);
        fclose(fp);
    }

    free(ra);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * CONTAINER SETUP — runs inside the child process after fork()
 *
 * This is where the magic of isolation happens:
 *   1. unshare() creates new namespaces (PID, UTS, mount)
 *   2. chroot()  makes the container see rootfs as "/"
 *   3. mount()   makes /proc work inside the container
 *   4. exec()    replaces this process with the actual command
 * ═══════════════════════════════════════════════════════════════ */
static void container_child_setup(const char *rootfs, char *const argv[])
{
    /* Create isolated namespaces */
    if (unshare(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS) < 0) {
        perror("unshare");
        exit(EXIT_FAILURE);
    }

    /* Change filesystem root to our mini Alpine Linux rootfs */
    if (chroot(rootfs) < 0) {
        perror("chroot");
        exit(EXIT_FAILURE);
    }
    chdir("/");

    /* Mount /proc so processes inside container can see themselves */
    mkdir("/proc", 0755);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        /* non-fatal if already mounted */
    }

    /* Set a custom hostname for this container */
    char hostname[64];
    snprintf(hostname, sizeof(hostname), "container");
    sethostname(hostname, strlen(hostname));

    /* Run the requested command (e.g. /bin/sh) */
    execvp(argv[0], argv);
    perror("exec");
    exit(EXIT_FAILURE);
}

/* ═══════════════════════════════════════════════════════════════
 * SIGCHLD handler — called automatically when any child exits
 * Uses waitpid() to reap zombie processes
 * ═══════════════════════════════════════════════════════════════ */
static void sigchld_handler(int sig)
{
    (void)sig;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&containers_mutex);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].host_pid == pid &&
                containers[i].state == STATE_RUNNING) {

                if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
                    containers[i].state = STATE_KILLED;
                else
                    containers[i].state = STATE_STOPPED;

                containers[i].exit_status = status;
                containers[i].host_pid    = 0;
                break;
            }
        }
        pthread_mutex_unlock(&containers_mutex);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * SIGTERM/SIGINT handler — orderly supervisor shutdown
 * ═══════════════════════════════════════════════════════════════ */
static void sigterm_handler(int sig)
{
    (void)sig;
    supervisor_running = 0;
    /* Wake the log consumer so it can drain and exit */
    pthread_cond_broadcast(&log_buf.not_empty);
}

/* ═══════════════════════════════════════════════════════════════
 * REGISTER with kernel module — tell monitor.c about a container
 * ═══════════════════════════════════════════════════════════════ */
static void register_with_monitor(Container *c)
{
    if (monitor_fd < 0) return;

    struct container_info info;
    memset(&info, 0, sizeof(info));
    info.pid           = c->host_pid;
    info.soft_limit_kb = c->soft_mib * 1024;
    info.hard_limit_kb = c->hard_mib * 1024;
    strncpy(info.name, c->name, 63);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &info) < 0)
        perror("ioctl MONITOR_REGISTER");
}

static void unregister_with_monitor(pid_t pid)
{
    if (monitor_fd < 0) return;
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &pid) < 0)
        perror("ioctl MONITOR_UNREGISTER");
}

/* ═══════════════════════════════════════════════════════════════
 * COMMAND: start — launch a container in the background
 *
 * Usage: engine start <name> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]
 *
 * Steps:
 *   1. Create a pipe for container output
 *   2. fork() — create child process
 *   3. In child: redirect stdout/stderr to pipe, set up container
 *   4. In parent: record metadata, start reader thread, register with kernel
 * ═══════════════════════════════════════════════════════════════ */
static void cmd_start(const char *name, const char *rootfs,
                      char *const cmd_argv[], long soft_mib, long hard_mib,
                      char *reply, size_t reply_sz)
{
    pthread_mutex_lock(&containers_mutex);

    if (find_container(name)) {
        snprintf(reply, reply_sz, "ERROR: container '%s' already exists\n", name);
        pthread_mutex_unlock(&containers_mutex);
        return;
    }

    Container *c = find_free_slot();
    if (!c) {
        snprintf(reply, reply_sz, "ERROR: no free container slots\n");
        pthread_mutex_unlock(&containers_mutex);
        return;
    }

    /* Create log directory and file */
    mkdir(LOG_DIR, 0755);
    snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, name);

    /* Pipe: container writes → supervisor reads */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        snprintf(reply, reply_sz, "ERROR: pipe failed: %s\n", strerror(errno));
        pthread_mutex_unlock(&containers_mutex);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(reply, reply_sz, "ERROR: fork failed: %s\n", strerror(errno));
        close(pipefd[0]); close(pipefd[1]);
        pthread_mutex_unlock(&containers_mutex);
        return;
    }

    if (pid == 0) {
        /* ── CHILD PROCESS ── */
        close(pipefd[0]);                  /* close read end */
        dup2(pipefd[1], STDOUT_FILENO);    /* stdout → pipe  */
        dup2(pipefd[1], STDERR_FILENO);    /* stderr → pipe  */
        close(pipefd[1]);
        container_child_setup(rootfs, cmd_argv);
        exit(EXIT_FAILURE); /* never reached */
    }

    /* ── PARENT PROCESS ── */
    close(pipefd[1]);   /* close write end — child has it */

    strncpy(c->name, name, 63);
    c->host_pid    = pid;
    c->start_time  = time(NULL);
    c->state       = STATE_RUNNING;
    c->soft_mib    = soft_mib > 0 ? soft_mib : 48;
    c->hard_mib    = hard_mib > 0 ? hard_mib : 96;
    c->log_pipe_fd = pipefd[0];
    c->exit_status = 0;

    pthread_mutex_unlock(&containers_mutex);

    /* Register with kernel module */
    register_with_monitor(c);

    /* Start a pipe reader thread for this container */
    ReaderArg *ra = malloc(sizeof(ReaderArg));
    strncpy(ra->name, name, 63);
    ra->fd = pipefd[0];
    pthread_t reader_tid;
    pthread_create(&reader_tid, NULL, pipe_reader, ra);
    pthread_detach(reader_tid);

    snprintf(reply, reply_sz, "OK: started '%s' (pid=%d)\n", name, pid);
}

/* ═══════════════════════════════════════════════════════════════
 * COMMAND: run — launch a container and wait for it (foreground)
 *
 * Same as start, but the CLI blocks until the container exits.
 * This is useful for short-lived commands where you want to wait
 * for completion before returning to the prompt.
 * ═══════════════════════════════════════════════════════════════ */
static void cmd_run(const char *name, const char *rootfs,
                    char *const cmd_argv[], long soft_mib, long hard_mib,
                    char *reply, size_t reply_sz)
{
    /* Step 1: launch the container exactly like 'start' */
    cmd_start(name, rootfs, cmd_argv, soft_mib, hard_mib, reply, reply_sz);

    /* If start failed, reply already contains the error — return early */
    if (strncmp(reply, "ERROR", 5) == 0)
        return;

    /* Step 2: find the PID we just launched */
    pid_t target_pid = 0;
    pthread_mutex_lock(&containers_mutex);
    Container *c = find_container(name);
    if (c) target_pid = c->host_pid;
    pthread_mutex_unlock(&containers_mutex);

    /* Step 3: block here until the container finishes (foreground wait) */
    if (target_pid > 0) {
        int status;
        waitpid(target_pid, &status, 0);   /* blocking wait */
        snprintf(reply, reply_sz,
                 "OK: '%s' finished (foreground, exit=%d)\n",
                 name, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * COMMAND: stop — gracefully stop a container
 * ═══════════════════════════════════════════════════════════════ */
static void cmd_stop(const char *name, char *reply, size_t reply_sz)
{
    pthread_mutex_lock(&containers_mutex);
    Container *c = find_container(name);

    if (!c || c->state != STATE_RUNNING) {
        snprintf(reply, reply_sz, "ERROR: container '%s' not running\n", name);
        pthread_mutex_unlock(&containers_mutex);
        return;
    }

    pid_t pid = c->host_pid;
    c->state = STATE_KILLED;
    pthread_mutex_unlock(&containers_mutex);

    unregister_with_monitor(pid);
    kill(pid, SIGTERM);

    /* Give it 3 seconds to exit gracefully, then force kill */
    for (int i = 0; i < 30; i++) {
        usleep(100000);
        if (waitpid(pid, NULL, WNOHANG) == pid) {
            snprintf(reply, reply_sz, "OK: '%s' stopped gracefully\n", name);
            return;
        }
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    snprintf(reply, reply_sz, "OK: '%s' force-killed\n", name);
}

/* ═══════════════════════════════════════════════════════════════
 * COMMAND: ps — list all containers and their metadata
 * ═══════════════════════════════════════════════════════════════ */
static void cmd_ps(char *reply, size_t reply_sz)
{
    char buf[4096] = {0};
    char line[256];

    snprintf(line, sizeof(line),
             "%-12s %-8s %-12s %-10s %-8s %-8s\n",
             "NAME", "PID", "STATE", "UPTIME(s)", "SOFT(MiB)", "HARD(MiB)");
    strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
    strncat(buf, "----------------------------------------------------------------------\n",
            sizeof(buf) - strlen(buf) - 1);

    pthread_mutex_lock(&containers_mutex);
    int found = 0;
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        Container *c = &containers[i];
        if (c->state == STATE_FREE) continue;

        long uptime = (long)(time(NULL) - c->start_time);
        snprintf(line, sizeof(line),
                 "%-12s %-8d %-12s %-10ld %-8ld %-8ld\n",
                 c->name, c->host_pid, state_name(c->state),
                 uptime, c->soft_mib, c->hard_mib);
        strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
        found++;
    }
    pthread_mutex_unlock(&containers_mutex);

    if (!found) strncat(buf, "(no containers)\n", sizeof(buf) - strlen(buf) - 1);
    snprintf(reply, reply_sz, "%s", buf);
}

/* ═══════════════════════════════════════════════════════════════
 * COMMAND: logs — print last N lines of a container's log
 * ═══════════════════════════════════════════════════════════════ */
static void cmd_logs(const char *name, char *reply, size_t reply_sz)
{
    char log_path[256] = {0};

    pthread_mutex_lock(&containers_mutex);
    Container *c = find_container(name);
    if (c) strncpy(log_path, c->log_path, 255);
    pthread_mutex_unlock(&containers_mutex);

    if (!log_path[0]) {
        snprintf(reply, reply_sz, "ERROR: container '%s' not found\n", name);
        return;
    }

    FILE *f = fopen(log_path, "r");
    if (!f) {
        snprintf(reply, reply_sz, "ERROR: no log file yet for '%s'\n", name);
        return;
    }

    snprintf(reply, reply_sz, "=== Log: %s ===\n", name);
    size_t off = strlen(reply);
    char line[512];
    while (fgets(line, sizeof(line), f) && off < reply_sz - 256) {
        strncat(reply, line, reply_sz - off - 1);
        off = strlen(reply);
    }
    fclose(f);
}

/* ═══════════════════════════════════════════════════════════════
 * SUPERVISOR MAIN LOOP — listens on UNIX socket for CLI commands
 * ═══════════════════════════════════════════════════════════════ */
static void run_supervisor(const char *rootfs_base)
{
    (void)rootfs_base;   /* stored for reference */

    /* Set up signal handlers */
    signal(SIGCHLD, sigchld_handler);
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT,  sigterm_handler);

    /* Open kernel module device (optional — if module not loaded, skip) */
    monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (monitor_fd < 0)
        fprintf(stderr, "Warning: cannot open %s (kernel module not loaded?)\n",
                MONITOR_DEV);

    /* Initialize bounded log buffer */
    memset(&log_buf, 0, sizeof(log_buf));
    pthread_mutex_init(&log_buf.mutex,     NULL);
    pthread_cond_init (&log_buf.not_empty, NULL);
    pthread_cond_init (&log_buf.not_full,  NULL);

    /* Start the log consumer thread */
    pthread_t consumer_tid;
    pthread_create(&consumer_tid, NULL, logger_consumer, NULL);

    /* Create UNIX domain socket */
    int srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    unlink(SOCKET_PATH);
    bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv_fd, 10);

    printf("[supervisor] ready on %s\n", SOCKET_PATH);
    fflush(stdout);

    /* Main accept loop */
    while (supervisor_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(srv_fd, &fds);
        struct timeval tv = {1, 0};   /* 1-second timeout so we can check supervisor_running */

        if (select(srv_fd + 1, &fds, NULL, NULL, &tv) <= 0)
            continue;

        int cli_fd = accept(srv_fd, NULL, NULL);
        if (cli_fd < 0) continue;

        /* Read command from CLI client */
        char cmd[1024] = {0};
        read(cli_fd, cmd, sizeof(cmd) - 1);

        char reply[8192] = {0};

        /* Parse and dispatch commands */
        if (strncmp(cmd, "ps", 2) == 0) {
            cmd_ps(reply, sizeof(reply));

        } else if (strncmp(cmd, "logs ", 5) == 0) {
            cmd_logs(cmd + 5, reply, sizeof(reply));

        } else if (strncmp(cmd, "stop ", 5) == 0) {
            cmd_stop(cmd + 5, reply, sizeof(reply));

        } else if (strncmp(cmd, "start ", 6) == 0) {
            /* Parse: start <name> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] */
            char name[64], rootfs[256], argv0[256];
            long soft = 48, hard = 96;
            char rest[512] = {0};

            sscanf(cmd + 6, "%63s %255s %255s %511[^\n]",
                   name, rootfs, argv0, rest);

            /* Parse optional memory limit flags */
            char *p;
            if ((p = strstr(rest, "--soft-mib "))) soft = atol(p + 11);
            if ((p = strstr(rest, "--hard-mib "))) hard = atol(p + 11);

            char *cargv[] = {argv0, NULL};
            cmd_start(name, rootfs, cargv, soft, hard, reply, sizeof(reply));


        } else if (strncmp(cmd, "run ", 4) == 0) {
            /* Parse: run <n> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]
             * Same as start but blocks until the container exits (foreground) */
            char name[64], rootfs[256], argv0[256];
            long soft = 48, hard = 96;
            char rest[512] = {0};

            sscanf(cmd + 4, "%63s %255s %255s %511[^\n]",
                   name, rootfs, argv0, rest);

            /* Parse optional memory limit flags */
            char *p2;
            if ((p2 = strstr(rest, "--soft-mib "))) soft = atol(p2 + 11);
            if ((p2 = strstr(rest, "--hard-mib "))) hard = atol(p2 + 11);

            char *cargv[] = {argv0, NULL};
            cmd_run(name, rootfs, cargv, soft, hard, reply, sizeof(reply));
        } else {
            snprintf(reply, sizeof(reply),
                     "Unknown command. Available: start, run, ps, logs, stop\n");
        }

        write(cli_fd, reply, strlen(reply));
        close(cli_fd);
    }

    printf("[supervisor] shutting down...\n");

    /* Stop all running containers */
    pthread_mutex_lock(&containers_mutex);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].state == STATE_RUNNING) {
            kill(containers[i].host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&containers_mutex);

    sleep(1);

    /* Stop log consumer */
    supervisor_running = 0;
    pthread_cond_broadcast(&log_buf.not_empty);
    pthread_join(consumer_tid, NULL);

    close(srv_fd);
    unlink(SOCKET_PATH);
    if (monitor_fd >= 0) close(monitor_fd);

    printf("[supervisor] clean shutdown complete\n");
}

/* ═══════════════════════════════════════════════════════════════
 * CLI CLIENT — sends one command to supervisor and prints reply
 * ═══════════════════════════════════════════════════════════════ */
static int run_cli(int argc, char *argv[])
{
    /* Build command string from CLI arguments */
    char cmd[1024] = {0};
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
    }

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "ERROR: cannot connect to supervisor.\n");
        fprintf(stderr, "Is the supervisor running? Try:\n");
        fprintf(stderr, "  sudo ./engine supervisor ./rootfs-base\n");
        close(sock);
        return 1;
    }

    write(sock, cmd, strlen(cmd));

    char reply[8192] = {0};
    ssize_t n = read(sock, reply, sizeof(reply) - 1);
    if (n > 0) printf("%s", reply);

    close(sock);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * MAIN — decides supervisor mode or CLI mode
 * ═══════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  Supervisor:  sudo ./engine supervisor <rootfs-base>\n"
            "  CLI:\n"
            "    sudo ./engine start <name> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]\n"
            "    sudo ./engine run   <n> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]\n"
            "    sudo ./engine ps\n"
            "    sudo ./engine logs <name>\n"
            "    sudo ./engine stop <name>\n"
        );
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: ./engine supervisor <rootfs-base>\n");
            return 1;
        }
        run_supervisor(argv[2]);
        return 0;
    }

    /* Any other command: send to supervisor via socket */
    return run_cli(argc, argv);
}