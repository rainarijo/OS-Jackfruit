/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;          /* set to 1 before sending SIGTERM via "stop" command */
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;  /* write-end of the pipe — container writes stdout/stderr here */
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* ---------------------------------------------------------------
 * Global supervisor context pointer — used by signal handlers
 * --------------------------------------------------------------- */
static supervisor_ctx_t *g_ctx = NULL;

/* ---------------------------------------------------------------
 * Producer thread argument — one per running container
 * --------------------------------------------------------------- */
typedef struct {
    int pipe_read_fd;                    /* read end of container's stdout/stderr pipe */
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} producer_arg_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ===============================================================
 * BOUNDED BUFFER
 * Think of this as a fixed-size queue between threads.
 * Producers (log readers) push into it.
 * The consumer (log writer thread) pops from it.
 * =============================================================== */

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buffer->mutex); return rc; }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    /* wake ALL waiting threads so they can see the shutdown flag */
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * bounded_buffer_push — PRODUCER side
 *
 * Blocks if the buffer is full (waits until a slot frees up).
 * Returns 0 on success, -1 if we are shutting down.
 *
 * Why mutex + cond?
 *   Multiple producer threads run at the same time (one per container).
 *   The mutex ensures only one thread touches head/tail/count at a time.
 *   The condition variable lets a waiting producer sleep efficiently
 *   instead of spinning in a loop wasting CPU.
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait while full AND not shutting down */
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
        /* pthread_cond_wait atomically: releases the mutex, sleeps,
           then re-acquires the mutex when signalled */

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    /* Insert at tail (circular array) */
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty); /* wake the consumer */
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * bounded_buffer_pop — CONSUMER side
 *
 * Blocks if the buffer is empty.
 * Returns 0 and fills *item on success.
 * Returns 1 if shutdown AND buffer is empty → consumer should exit.
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    /* Wait while empty AND not shutting down */
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    /* If shutting down and nothing left, tell consumer to exit */
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 1;  /* sentinel: "you're done" */
    }

    /* Remove from head (oldest item first) */
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full); /* wake a waiting producer */
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ===============================================================
 * LOGGING CONSUMER THREAD
 *
 * Runs in the background. Pops log chunks and writes them to
 * per-container log files under LOG_DIR/<id>.log
 * =============================================================== */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    int rc;

    /* Make sure the logs directory exists */
    mkdir(LOG_DIR, 0755);

    while (1) {
        rc = bounded_buffer_pop(&ctx->log_buffer, &item);
        if (rc == 1)   /* shutdown + drained */
            break;
        if (rc != 0)
            continue;

        /* Build path: logs/<container_id>.log */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        /* Append to the log file (create if it does not exist) */
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("logging_thread: open");
            continue;
        }

        ssize_t written = 0;
        while (written < (ssize_t)item.length) {
            ssize_t n = write(fd, item.data + written, item.length - written);
            if (n < 0) { perror("logging_thread: write"); break; }
            written += n;
        }
        close(fd);
    }

    return NULL;
}

/* ===============================================================
 * PRODUCER THREAD
 *
 * One of these runs per container. It reads from the pipe that
 * the container writes its stdout/stderr into, and pushes
 * chunks into the bounded buffer for the logging thread.
 * =============================================================== */
static void *producer_thread(void *arg)
{
    producer_arg_t *parg = (producer_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, parg->container_id, CONTAINER_ID_LEN - 1);

    /* Read until the pipe closes (container exits) */
    while ((n = read(parg->pipe_read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(parg->log_buffer, &item);
        memset(item.data, 0, LOG_CHUNK_SIZE);
    }

    close(parg->pipe_read_fd);
    free(parg);
    return NULL;
}

/* ===============================================================
 * CHILD FUNCTION — runs inside the new container after clone()
 *
 * clone() is like fork() but lets us create NEW namespaces.
 * This function runs in the new namespace context.
 *
 * Steps:
 *  1. Redirect stdout+stderr → the log pipe
 *  2. Mount /proc so "ps" works inside the container
 *  3. chroot into the container's own rootfs
 *  4. Set hostname to container ID
 *  5. Set nice value (scheduling priority)
 *  6. exec the requested command
 * =============================================================== */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Step 1: Redirect stdout and stderr to the log pipe
     * dup2(src, dst) makes dst point to the same file as src.
     * After this, anything the container prints goes into the pipe. */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0) {
        perror("child_fn: dup2 stdout");
        return 1;
    }
    if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("child_fn: dup2 stderr");
        return 1;
    }
    close(cfg->log_write_fd); /* close original after dup */

    /* Step 2: Mount /proc inside the container's rootfs
     * Without this, commands like "ps" and "top" won't work.
     * We mount it at <rootfs>/proc before chrooting. */
    char proc_path[PATH_MAX];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", cfg->rootfs);
    mkdir(proc_path, 0555);
    if (mount("proc", proc_path, "proc", 0, NULL) < 0)
        perror("child_fn: mount /proc (non-fatal)");

    /* Step 3: chroot — make the container see only its rootfs as "/"
     * After chroot, the container cannot access anything outside rootfs. */
    if (chroot(cfg->rootfs) < 0) {
        perror("child_fn: chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("child_fn: chdir /");
        return 1;
    }

    /* Step 4: Set a custom hostname for this container (UTS namespace) */
    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        perror("child_fn: sethostname (non-fatal)");

    /* Step 5: Set nice value — higher nice = lower priority (0 is default)
     * This is used in Task 5 scheduling experiments. */
    if (cfg->nice_value != 0) {
        if (nice(cfg->nice_value) < 0)
            perror("child_fn: nice (non-fatal)");
    }

    /* Step 6: exec the command — replaces this process with the real program
     * We use /bin/sh -c "<command>" so any shell command works. */
    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);

    /* If execl returns, something went wrong */
    perror("child_fn: execl");
    return 1;
}

/* ===============================================================
 * Already provided — no changes needed
 * =============================================================== */
int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ===============================================================
 * CONTAINER METADATA HELPERS
 * =============================================================== */

/* Find a container record by ID. Call with metadata_lock held. */
static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strcmp(c->id, id) == 0) return c;
        c = c->next;
    }
    return NULL;
}

/* Add a new container record to the front of the list. */
static void add_container(supervisor_ctx_t *ctx, container_record_t *rec)
{
    rec->next = ctx->containers;
    ctx->containers = rec;
}

/* ===============================================================
 * LAUNCH CONTAINER
 *
 * Called when the supervisor receives a "start" or "run" command.
 * Creates a pipe, calls clone() with namespaces, starts the
 * producer thread, registers with the kernel monitor.
 * =============================================================== */
static int launch_container(supervisor_ctx_t *ctx, const control_request_t *req)
{
    int pipefd[2];   /* pipefd[0]=read end, pipefd[1]=write end */
    pid_t pid;
    char *stack;
    char *stack_top;

    /* Check for duplicate ID */
    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        fprintf(stderr, "Container '%s' already exists\n", req->container_id);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create a pipe: container writes to pipefd[1], supervisor reads from pipefd[0] */
    if (pipe(pipefd) < 0) {
        perror("launch_container: pipe");
        return -1;
    }

    /* Build the config for child_fn */
    child_config_t *cfg = malloc(sizeof(child_config_t));
    if (!cfg) { close(pipefd[0]); close(pipefd[1]); return -1; }
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1]; /* child writes here */

    /* Allocate a stack for the cloned child
     * clone() needs an explicit stack unlike fork() */
    stack = malloc(STACK_SIZE);
    if (!stack) { free(cfg); close(pipefd[0]); close(pipefd[1]); return -1; }
    stack_top = stack + STACK_SIZE; /* stack grows downward on x86 */

    /*
     * clone() — like fork() but with namespaces.
     * CLONE_NEWPID: new PID namespace (container gets its own PID 1)
     * CLONE_NEWUTS: new hostname namespace
     * CLONE_NEWNS:  new mount namespace (container gets its own /proc)
     * SIGCHLD:      parent gets SIGCHLD when child exits (for reaping)
     */
    pid = clone(child_fn, stack_top,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                cfg);

    if (pid < 0) {
        perror("launch_container: clone");
        free(stack);
        free(cfg);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    /* Parent side: close the write end — only the child writes to it */
    close(pipefd[1]);
    free(stack); /* stack memory is safe to free after clone returns */
    free(cfg);

    /* Create a metadata record for this container */
    container_record_t *rec = malloc(sizeof(container_record_t));
    if (!rec) { close(pipefd[0]); return -1; }
    memset(rec, 0, sizeof(*rec));
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid = pid;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->stop_requested = 0;
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    pthread_mutex_lock(&ctx->metadata_lock);
    add_container(ctx, rec);
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Start a producer thread to read this container's pipe and feed the buffer */
    producer_arg_t *parg = malloc(sizeof(producer_arg_t));
    if (parg) {
        parg->pipe_read_fd = pipefd[0];
        parg->log_buffer = &ctx->log_buffer;
        strncpy(parg->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        pthread_t ptid;
        pthread_create(&ptid, NULL, producer_thread, parg);
        pthread_detach(ptid); /* we don't join producer threads explicitly */
    }

    /* Register with the kernel memory monitor module */
    if (ctx->monitor_fd >= 0) {
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);
    }

    fprintf(stderr, "[supervisor] started container '%s' pid=%d\n",
            req->container_id, pid);
    return 0;
}

/* ===============================================================
 * SIGCHLD HANDLER — called when any child process exits
 *
 * waitpid(-1, ..., WNOHANG) reaps ALL exited children without
 * blocking. WNOHANG means "don't wait if no child has exited".
 * Without reaping, dead children become "zombies".
 * =============================================================== */
static void sigchld_handler(int sig)
{
    (void)sig;
    int status;
    pid_t pid;

    /* Loop to catch multiple children that may have exited at once */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx) continue;

        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    /* Normal exit */
                    c->exit_code = WEXITSTATUS(status);
                    c->state = CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    if (WTERMSIG(status) == SIGKILL && !c->stop_requested)
                        c->state = CONTAINER_KILLED; /* hard limit kill */
                    else
                        c->state = CONTAINER_STOPPED;
                }
                /* Unregister from kernel monitor */
                if (g_ctx->monitor_fd >= 0)
                    unregister_from_monitor(g_ctx->monitor_fd, c->id, pid);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

/* ===============================================================
 * SIGTERM/SIGINT HANDLER — graceful shutdown
 * =============================================================== */
static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

/* ===============================================================
 * HANDLE A SINGLE CLIENT REQUEST
 *
 * The supervisor accepts a connection on its UNIX socket,
 * reads one control_request_t, acts on it, sends a response.
 * =============================================================== */
static void handle_client(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;
    ssize_t n;

    memset(&resp, 0, sizeof(resp));

    /* Read the request struct sent by the CLI client */
    n = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
    if (n != sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "bad request");
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    if (req.kind == CMD_START || req.kind == CMD_RUN) {
        int rc = launch_container(ctx, &req);
        if (rc == 0) {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "started container '%s'", req.container_id);
        } else {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "failed to start '%s'", req.container_id);
        }

        send(client_fd, &resp, sizeof(resp), 0);

        /* For CMD_RUN: block until the container exits */
        if (req.kind == CMD_RUN && resp.status == 0) {
            while (1) {
                pthread_mutex_lock(&ctx->metadata_lock);
                container_record_t *c = find_container(ctx, req.container_id);
                int done = c && (c->state == CONTAINER_EXITED ||
                                 c->state == CONTAINER_STOPPED ||
                                 c->state == CONTAINER_KILLED);
                int exit_code = c ? c->exit_code : -1;
                pthread_mutex_unlock(&ctx->metadata_lock);
                if (done) {
                    /* Send final exit code to the run client */
                    send(client_fd, &exit_code, sizeof(exit_code), 0);
                    break;
                }
                usleep(100000); /* poll every 100ms */
            }
        }
        return;
    }

    if (req.kind == CMD_PS) {
        /* Build a text table of all containers */
        char buf[4096];
        int off = 0;
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%-16s %-8s %-10s %-12s %-10s %-10s\n",
                        "ID", "PID", "STATE", "SOFT(MiB)", "HARD(MiB)", "STARTED");

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c && off < (int)sizeof(buf) - 128) {
            char tbuf[32];
            struct tm *tm_info = localtime(&c->started_at);
            strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm_info);
            off += snprintf(buf + off, sizeof(buf) - off,
                            "%-16s %-8d %-10s %-12lu %-10lu %-10s\n",
                            c->id, c->host_pid,
                            state_to_string(c->state),
                            c->soft_limit_bytes >> 20,
                            c->hard_limit_bytes >> 20,
                            tbuf);
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "%s", buf);
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    if (req.kind == CMD_LOGS) {
        /* Read and return the container's log file */
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req.container_id);

        FILE *f = fopen(log_path, "r");
        if (!f) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "no log found for '%s'", req.container_id);
            send(client_fd, &resp, sizeof(resp), 0);
            return;
        }

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "log follows:");
        send(client_fd, &resp, sizeof(resp), 0);

        /* Stream the log file content back to the client */
        char fbuf[4096];
        size_t nr;
        while ((nr = fread(fbuf, 1, sizeof(fbuf), f)) > 0)
            write(client_fd, fbuf, nr);
        fclose(f);
        return;
    }

    if (req.kind == CMD_STOP) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        if (!c) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "container '%s' not found", req.container_id);
            send(client_fd, &resp, sizeof(resp), 0);
            return;
        }
        /* Mark stop_requested BEFORE sending the signal.
         * The SIGCHLD handler uses this to classify the exit reason. */
        c->stop_requested = 1;
        pid_t pid = c->host_pid;
        pthread_mutex_unlock(&ctx->metadata_lock);

        kill(pid, SIGTERM);

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "sent SIGTERM to '%s' (pid=%d)", req.container_id, pid);
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    resp.status = -1;
    snprintf(resp.message, sizeof(resp.message), "unknown command");
    send(client_fd, &resp, sizeof(resp), 0);
}

/* ===============================================================
 * RUN SUPERVISOR — the main long-running daemon
 *
 * Creates a UNIX domain socket, starts the logger thread,
 * then loops accepting CLI connections and handling them.
 * =============================================================== */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    (void)rootfs; /* rootfs arg kept for CLI compatibility */

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    /* Init metadata lock */
    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { perror("pthread_mutex_init"); return 1; }

    /* Init bounded buffer */
    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) { perror("bounded_buffer_init"); return 1; }

    /* Make sure log dir exists */
    mkdir(LOG_DIR, 0755);

    /* Step 1: Open kernel monitor device (optional — don't fail if absent) */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] kernel monitor not available (run without it)\n");

    /* Step 2: Create UNIX domain socket for CLI communication
     *
     * A UNIX domain socket is like a TCP socket but local-only.
     * The CLI client connects here to send commands.
     * AF_UNIX = local socket, SOCK_STREAM = reliable byte stream.
     */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    /* Remove stale socket file if it exists */
    unlink(CONTROL_PATH);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen"); return 1;
    }

    /* Step 3: Install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_RESTART;

    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Step 4: Start the logging consumer thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) { perror("pthread_create logger"); return 1; }

    fprintf(stderr, "[supervisor] ready on %s\n", CONTROL_PATH);

    /* Step 5: Main event loop — accept and handle CLI connections */
    while (!ctx.should_stop) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ctx.server_fd, &fds);

        /* Use select with a timeout so we check should_stop periodically */
        struct timeval tv = {1, 0}; /* 1 second timeout */
        int nready = select(ctx.server_fd + 1, &fds, NULL, NULL, &tv);

        if (nready < 0) {
            if (errno == EINTR) continue; /* interrupted by signal, check should_stop */
            perror("select");
            break;
        }

        if (nready == 0) continue; /* timeout, loop again */

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        handle_client(&ctx, client_fd);
        close(client_fd);
    }

    fprintf(stderr, "[supervisor] shutting down...\n");

    /* Orderly shutdown: kill all remaining containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Give containers a moment to exit, then reap */
    sleep(1);
    while (waitpid(-1, NULL, WNOHANG) > 0) {}

    /* Stop the logger thread */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    /* Free container list */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Cleanup */
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.server_fd >= 0) close(ctx.server_fd);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    unlink(CONTROL_PATH);

    fprintf(stderr, "[supervisor] clean exit\n");
    return 0;
}

/* ===============================================================
 * CLIENT SIDE — send a control request to the running supervisor
 * =============================================================== */
static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(fd);
        return 1;
    }

    /* Send the request struct */
    if (send(fd, req, sizeof(*req), 0) != sizeof(*req)) {
        perror("send");
        close(fd);
        return 1;
    }

    /* Receive and print the response */
    ssize_t n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
    if (n == sizeof(resp)) {
        printf("%s\n", resp.message);

        /* For CMD_RUN: also wait for the exit code */
        if (req->kind == CMD_RUN && resp.status == 0) {
            int exit_code = -1;
            recv(fd, &exit_code, sizeof(exit_code), MSG_WAITALL);
            printf("[run] container exited with code %d\n", exit_code);
        }

        /* For CMD_LOGS: print everything that follows */
        if (req->kind == CMD_LOGS && resp.status == 0) {
            char buf[4096];
            ssize_t nr;
            while ((nr = recv(fd, buf, sizeof(buf), 0)) > 0)
                fwrite(buf, 1, nr, stdout);
        }
    }

    close(fd);
    return 0;
}

/* ===============================================================
 * CLI command handlers — parse args and call send_control_request
 * =============================================================== */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s logs <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s stop <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
