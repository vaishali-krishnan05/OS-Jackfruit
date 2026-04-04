/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Design overview
 * ---------------
 * Control plane:  UNIX domain socket (SOCK_STREAM) at CONTROL_PATH.
 *   Each CLI invocation connects, sends a control_request_t, and reads
 *   back a control_response_t.  For CMD_PS / CMD_LOGS the supervisor
 *   streams additional text before the response frame.
 *
 * Log plane:  bounded_buffer_t (producer = per-container reader thread,
 *   consumer = single logger_thread).  Each container gets its own reader
 *   thread that reads stdout/stderr from the container and pushes
 *   log_item_t chunks into the shared bounded buffer. The logger thread
 *   pops items and appends them to per-container files under LOG_DIR/.
 *
 * Container isolation:
 *   clone(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS) spawns the child.
 *   child_fn() chroots into the container rootfs, mounts /proc, adjusts
 *   nice value, redirects stdout/stderr to a pipe, then exec()s the command.
 *
 * Signal handling:
 *   SIGCHLD  -> reaps exited children, updates metadata, unregisters from
 *               kernel monitor.
 *   SIGINT / SIGTERM -> sets ctx.should_stop, triggers graceful shutdown.
 *   Both are handled via a dedicated signal-handling thread using
 *   sigwaitinfo() against a blocked signal set, avoiding async-signal-safety
 *   pitfalls in the main event loop.
 *
 * Locking discipline:
 *   ctx.metadata_lock  protects the container_record_t linked list.
 *   log_buffer.mutex   protects the bounded buffer (with two condvars).
 *   The two locks are never held simultaneously, preventing deadlock.
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

/*
 * monitor_ioctl.h is a kernel header but only uses __u32 / unsigned long,
 * which are safe to include from user-space with _GNU_SOURCE.
 * We redefine the ioctl macros using <sys/ioctl.h> types here.
 */
#ifndef CONTAINER_ID_MAX
#define CONTAINER_ID_MAX 64
#endif

/* Mirrored from monitor_ioctl.h so we don't pull linux/ headers here */
#define MONITOR_MAGIC      0xCE

struct monitor_request_us {
    unsigned int  pid;
    char          container_id[CONTAINER_ID_MAX];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
};

#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct monitor_request_us)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_request_us)

/* ------------------------------------------------------------------ */

#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN   256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT  (40UL << 20)
#define DEFAULT_HARD_LIMIT  (64UL << 20)
#define MONITOR_DEV         "/dev/container_monitor"

/* ------------------------------------------------------------------ */

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
    char log_path[PATH_MAX];
    struct container_record *next;
    /* pipe read end for this container's stdout+stderr */
    int log_read_fd;
    /* reader thread for this container */
    pthread_t reader_thread;
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
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    volatile int should_stop;
    pthread_t logger_thread;
    pthread_t signal_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Global pointer used only by the signal-handling thread so it can
   reach the supervisor context without globals being spread everywhere. */
static supervisor_ctx_t *g_ctx;

/* ------------------------------------------------------------------ */
/* Utility / provided helpers                                          */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Bounded buffer                                                      */
/* ------------------------------------------------------------------ */

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
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * bounded_buffer_push - producer side.
 *
 * Blocks while the buffer is full (unless shutdown begins).
 * Returns  0 on success,
 *         -1 if shutting down (caller should stop producing).
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * bounded_buffer_pop - consumer side.
 *
 * Blocks while the buffer is empty.
 * Returns  0 on success,
 *          1 if shutting down AND buffer is empty (caller should exit),
 *         -1 on unexpected error.
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0) {
        if (buffer->shutting_down) {
            pthread_mutex_unlock(&buffer->mutex);
            return 1;   /* drained, caller should exit */
        }
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Logger thread                                                       */
/* ------------------------------------------------------------------ */

/*
 * logging_thread - single consumer that routes log chunks to files.
 *
 * Each container has a log file at LOG_DIR/<container_id>.log.
 * Files are opened in append mode so logs survive supervisor restarts.
 * The thread drains the buffer fully before exiting even after shutdown
 * begins (bounded_buffer_pop returns 1 only when count == 0).
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    int rc;

    while (1) {
        rc = bounded_buffer_pop(&ctx->log_buffer, &item);
        if (rc == 1)
            break;  /* shutdown and buffer drained */
        if (rc != 0)
            continue;

        /* Build path: LOG_DIR/<container_id>.log */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            fprintf(stderr, "[logger] Cannot open %s: %s\n",
                    path, strerror(errno));
            continue;
        }
        /* Write the chunk; handle short writes. */
        size_t written = 0;
        while (written < item.length) {
            ssize_t n = write(fd, item.data + written, item.length - written);
            if (n <= 0) break;
            written += (size_t)n;
        }
        close(fd);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Per-container reader thread (producer)                             */
/* ------------------------------------------------------------------ */

typedef struct {
    supervisor_ctx_t *ctx;
    int               read_fd;          /* pipe read end from container    */
    char              container_id[CONTAINER_ID_LEN];
} reader_arg_t;

/*
 * container_reader_thread
 *
 * Reads stdout/stderr piped from the container process and pushes
 * log_item_t chunks into the shared bounded buffer.
 * Exits when the pipe is closed (container exited or stopped).
 *
 * FIX: The pipe read end must be blocking (no O_NONBLOCK).  With
 * O_NONBLOCK, read() returns EAGAIN immediately when the container
 * has not written yet, n < 0 triggers the break, and the thread exits
 * before capturing any output.  A blocking read() returns 0 only on
 * true EOF (all write ends closed), which is the correct exit signal.
 */
static void *container_reader_thread(void *arg)
{
    reader_arg_t *ra = (reader_arg_t *)arg;
    log_item_t item;

    while (1) {
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, ra->container_id, CONTAINER_ID_LEN - 1);

        ssize_t n = read(ra->read_fd, item.data, LOG_CHUNK_SIZE);
        if (n == 0)
            break;  /* EOF: write end closed, container has exited */
        if (n < 0) {
            if (errno == EINTR)
                continue;   /* interrupted by signal, retry */
            break;          /* genuine read error */
        }

        item.length = (size_t)n;
        if (bounded_buffer_push(&ra->ctx->log_buffer, &item) != 0)
            break;  /* buffer shutting down */
    }

    close(ra->read_fd);
    free(ra);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Child entrypoint (runs inside clone'd namespace)                   */
/* ------------------------------------------------------------------ */

/*
 * child_fn - executed inside the new PID/UTS/mount namespace.
 *
 *  1. Redirect stdout and stderr to the write end of the logging pipe.
 *  2. Mount /proc (new PID namespace needs its own procfs).
 *  3. chroot into the container rootfs.
 *  4. Apply nice value.
 *  5. exec the requested command via /bin/sh -c.
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout + stderr to the supervisor's pipe. */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        return 1;
    }
    close(cfg->log_write_fd);

    /* Mount /proc inside the new mount namespace before chroot. */
    if (mount("proc", "/proc", "proc",
              MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
        /* Best-effort: /proc may already be mounted in the rootfs image. */
        fprintf(stderr, "[child] mount /proc: %s (continuing)\n",
                strerror(errno));
    }

    /* chroot into the container rootfs. */
    if (chroot(cfg->rootfs) != 0) {
        fprintf(stderr, "[child] chroot(%s): %s\n",
                cfg->rootfs, strerror(errno));
        return 1;
    }
    if (chdir("/") != 0) {
        perror("[child] chdir /");
        return 1;
    }

    /* Re-mount /proc now that we are inside the new root. */
    (void)umount2("/proc", MNT_DETACH);     /* detach any inherited proc  */
    if (mount("proc", "/proc", "proc",
              MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
        fprintf(stderr, "[child] mount /proc inside rootfs: %s\n",
                strerror(errno));
        /* non-fatal: continue without /proc */
    }

    /* Apply scheduling priority. */
    if (cfg->nice_value != 0) {
        errno = 0;
        if (nice(cfg->nice_value) == -1 && errno != 0)
            fprintf(stderr, "[child] nice(%d): %s\n",
                    cfg->nice_value, strerror(errno));
    }

    /* Set hostname to container ID for UTS namespace isolation. */
    sethostname(cfg->id, strlen(cfg->id));

    /* Execute via shell to support pipelines / shell builtins. */
    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);

    /* execl only returns on failure. */
    fprintf(stderr, "[child] execl: %s\n", strerror(errno));
    return 1;
}

/* ------------------------------------------------------------------ */
/* Kernel monitor helpers                                              */
/* ------------------------------------------------------------------ */

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request_us req;
    memset(&req, 0, sizeof(req));
    req.pid              = (unsigned int)host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd,
                            const char *container_id,
                            pid_t host_pid)
{
    struct monitor_request_us req;
    memset(&req, 0, sizeof(req));
    req.pid = (unsigned int)host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Container metadata helpers                                          */
/* ------------------------------------------------------------------ */

/* Caller must hold ctx->metadata_lock. */
static container_record_t *find_container(supervisor_ctx_t *ctx,
                                          const char *id)
{
    container_record_t *r;
    for (r = ctx->containers; r; r = r->next)
        if (strncmp(r->id, id, CONTAINER_ID_LEN) == 0)
            return r;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Spawn a container                                                   */
/* ------------------------------------------------------------------ */

/*
 * spawn_container
 *
 * Creates a new container record, allocates a log pipe, calls clone()
 * to start the child in new namespaces, registers with the kernel
 * monitor, and starts the per-container reader thread.
 *
 * FIX: Removed O_NONBLOCK from the pipe read end.  The read end must
 * be blocking so that container_reader_thread correctly blocks waiting
 * for output and only exits on genuine EOF (write end closed).
 *
 * Returns 0 on success, sets errbuf on failure.
 */
static int spawn_container(supervisor_ctx_t *ctx,
                           const control_request_t *req,
                           char *errbuf, size_t errbuf_len)
{
    int pipe_fds[2];
    char *stack = NULL;
    pid_t child_pid;
    child_config_t *cfg = NULL;
    container_record_t *rec = NULL;
    reader_arg_t *ra = NULL;

    /* Check for duplicate ID */
    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(errbuf, errbuf_len, "container '%s' already exists",
                 req->container_id);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create log pipe.
     * Both ends are left in their default blocking mode.
     * The reader thread must block on read() waiting for container output;
     * setting O_NONBLOCK on the read end caused read() to return EAGAIN
     * immediately (treated as n<=0), making the reader thread exit before
     * capturing any output. */
    if (pipe(pipe_fds) != 0) {
        snprintf(errbuf, errbuf_len, "pipe: %s", strerror(errno));
        return -1;
    }

    /* Build child config */
    cfg = calloc(1, sizeof(*cfg));
    if (!cfg) { snprintf(errbuf, errbuf_len, "calloc"); goto err; }
    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipe_fds[1];

    /* Allocate clone stack (grows down) */
    stack = malloc(STACK_SIZE);
    if (!stack) { snprintf(errbuf, errbuf_len, "malloc stack"); goto err; }

    /* Ensure log dir exists */
    mkdir(LOG_DIR, 0755);

    child_pid = clone(child_fn,
                      stack + STACK_SIZE,   /* stack grows down */
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);
    if (child_pid < 0) {
        snprintf(errbuf, errbuf_len, "clone: %s", strerror(errno));
        goto err;
    }

    /* Close write end in supervisor so we get EOF when child exits */
    close(pipe_fds[1]);
    pipe_fds[1] = -1;

    /* Stack is now owned by child's address space via clone; don't free it
       until child exits.  We intentionally leak it here; for a production
       runtime you'd track it in the record and free on reap. */
    stack = NULL;
    free(cfg);
    cfg = NULL;

    /* Build metadata record */
    rec = calloc(1, sizeof(*rec));
    if (!rec) { snprintf(errbuf, errbuf_len, "calloc record"); goto err; }
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid         = child_pid;
    rec->started_at       = time(NULL);
    rec->state            = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->log_read_fd      = pipe_fds[0];
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    /* Insert into list */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next        = ctx->containers;
    ctx->containers  = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register with kernel memory monitor */
    if (ctx->monitor_fd >= 0) {
        if (register_with_monitor(ctx->monitor_fd, req->container_id,
                                  child_pid, req->soft_limit_bytes,
                                  req->hard_limit_bytes) != 0)
            fprintf(stderr, "[supervisor] monitor register failed: %s\n",
                    strerror(errno));
    }

    /* Start per-container reader thread */
    ra = calloc(1, sizeof(*ra));
    if (!ra) {
        fprintf(stderr, "[supervisor] calloc reader_arg failed\n");
        /* Not fatal; logs just won't be collected */
    } else {
        ra->ctx     = ctx;
        ra->read_fd = pipe_fds[0];
        strncpy(ra->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        pthread_create(&rec->reader_thread, NULL,
                       container_reader_thread, ra);
        rec->log_read_fd = -1;  /* now owned by reader thread */
    }

    return 0;

err:
    if (pipe_fds[0] >= 0) close(pipe_fds[0]);
    if (pipe_fds[1] >= 0) close(pipe_fds[1]);
    free(cfg);
    free(stack);
    free(rec);
    return -1;
}

/* ------------------------------------------------------------------ */
/* SIGCHLD reaper                                                      */
/* ------------------------------------------------------------------ */

/*
 * reap_children - called from the signal-handling thread.
 *
 * Calls waitpid() in a loop to reap all terminated children, then
 * updates the matching container_record and unregisters from the
 * kernel monitor.
 */
static void reap_children(supervisor_ctx_t *ctx)
{
    int wstatus;
    pid_t pid;

    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        container_record_t *r;

        pthread_mutex_lock(&ctx->metadata_lock);
        for (r = ctx->containers; r; r = r->next) {
            if (r->host_pid != pid)
                continue;

            if (WIFEXITED(wstatus)) {
                r->exit_code  = WEXITSTATUS(wstatus);
                r->exit_signal = 0;
                r->state      = CONTAINER_EXITED;
            } else if (WIFSIGNALED(wstatus)) {
                r->exit_signal = WTERMSIG(wstatus);
                r->exit_code   = 0;
                r->state       = (r->exit_signal == SIGKILL)
                                  ? CONTAINER_KILLED
                                  : CONTAINER_STOPPED;
            }
            break;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        /* Unregister from kernel module */
        if (r && ctx->monitor_fd >= 0)
            unregister_from_monitor(ctx->monitor_fd, r->id, pid);
    }
}

/* ------------------------------------------------------------------ */
/* Signal-handling thread                                              */
/* ------------------------------------------------------------------ */

/*
 * signal_thread_fn
 *
 * Waits for SIGCHLD, SIGINT, SIGTERM using sigwaitinfo().
 * All three signals are blocked in every other thread via
 * pthread_sigmask before this thread is created.
 */
static void *signal_thread_fn(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    sigset_t mask;

    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    while (!ctx->should_stop) {
        siginfo_t info;
        int sig = sigwaitinfo(&mask, &info);
        if (sig < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (sig == SIGCHLD) {
            reap_children(ctx);
        } else {
            /* SIGINT or SIGTERM: request graceful shutdown */
            fprintf(stderr, "\n[supervisor] Caught signal %d, shutting down.\n",
                    sig);
            ctx->should_stop = 1;
            /* Wake the accept loop by closing the server socket */
            if (ctx->server_fd >= 0) {
                close(ctx->server_fd);
                ctx->server_fd = -1;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Control request handlers (supervisor side)                         */
/* ------------------------------------------------------------------ */

static void handle_ps(supervisor_ctx_t *ctx, int client_fd)
{
    container_record_t *r;
    char line[512];
    control_response_t resp;

    /* Print header */
    snprintf(line, sizeof(line),
             "%-16s %-8s %-12s %-10s %-12s %-12s\n",
             "ID", "PID", "STATE", "EXIT",
             "SOFT(MiB)", "HARD(MiB)");
    write(client_fd, line, strlen(line));

    pthread_mutex_lock(&ctx->metadata_lock);
    for (r = ctx->containers; r; r = r->next) {
        snprintf(line, sizeof(line),
                 "%-16s %-8d %-12s %-10d %-12lu %-12lu\n",
                 r->id,
                 (int)r->host_pid,
                 state_to_string(r->state),
                 r->exit_code,
                 r->soft_limit_bytes >> 20,
                 r->hard_limit_bytes >> 20);
        write(client_fd, line, strlen(line));
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    memset(&resp, 0, sizeof(resp));
    resp.status = 0;
    snprintf(resp.message, sizeof(resp.message), "ok");
    write(client_fd, &resp, sizeof(resp));
}

static void handle_logs(supervisor_ctx_t *ctx, int client_fd,
                        const control_request_t *req)
{
    char path[PATH_MAX];
    char buf[4096];
    int fd;
    ssize_t n;
    control_response_t resp;

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *r = find_container(ctx, req->container_id);
    if (r)
        snprintf(path, sizeof(path), "%s", r->log_path);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!r) {
        memset(&resp, 0, sizeof(resp));
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "container '%s' not found", req->container_id);
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        memset(&resp, 0, sizeof(resp));
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "log file not found (see logs dir)");
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    /* Stream log file contents to client */
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(client_fd, buf, (size_t)n);
    close(fd);

    memset(&resp, 0, sizeof(resp));
    resp.status = 0;
    snprintf(resp.message, sizeof(resp.message), "ok");
    write(client_fd, &resp, sizeof(resp));
}

static void handle_stop(supervisor_ctx_t *ctx, int client_fd,
                        const control_request_t *req)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *r = find_container(ctx, req->container_id);
    if (!r || r->state != CONTAINER_RUNNING) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 r ? "container not running" : "container not found");
        pthread_mutex_unlock(&ctx->metadata_lock);
        write(client_fd, &resp, sizeof(resp));
        return;
    }
    pid_t pid = r->host_pid;
    r->state  = CONTAINER_STOPPED;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (kill(pid, SIGTERM) != 0) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "kill: %s", strerror(errno));
    } else {
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "sent SIGTERM to container '%s' (pid %d)",
                 req->container_id, (int)pid);
    }
    write(client_fd, &resp, sizeof(resp));
}

static void handle_start_run(supervisor_ctx_t *ctx, int client_fd,
                             const control_request_t *req)
{
    control_response_t resp;
    char errbuf[256] = {0};
    memset(&resp, 0, sizeof(resp));

    if (spawn_container(ctx, req, errbuf, sizeof(errbuf)) != 0) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "error: %.240s", errbuf);
    } else {
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "container '%s' started", req->container_id);
    }
    write(client_fd, &resp, sizeof(resp));

    /* For CMD_RUN: wait until the container exits (blocking) */
    if (req->kind == CMD_RUN) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *r = find_container(ctx, req->container_id);
        pid_t pid = r ? r->host_pid : -1;
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (pid > 0) {
            int wstatus;
            waitpid(pid, &wstatus, 0);
            reap_children(ctx);  /* update metadata */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Supervisor event loop                                               */
/* ------------------------------------------------------------------ */

/*
 * run_supervisor
 *
 * Main supervisor process:
 *  1. Opens /dev/container_monitor (kernel module device).
 *  2. Creates a UNIX domain socket for control-plane communication.
 *  3. Blocks SIGCHLD/SIGINT/SIGTERM; spawns signal-handling thread.
 *  4. Spawns the logger thread.
 *  5. Accepts client connections and dispatches control requests.
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    /* --- metadata lock --- */
    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    /* --- bounded buffer --- */
    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc; perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* --- open kernel monitor device (non-fatal if not loaded) --- */
    ctx.monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] Warning: cannot open %s: %s "
                "(memory monitoring disabled)\n",
                MONITOR_DEV, strerror(errno));

    /* --- create control socket --- */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); goto cleanup; }

    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); goto cleanup;
    }
    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen"); goto cleanup;
    }

    /* --- block signals in all threads (signal thread will unblock them) --- */
    {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &mask, NULL);
    }

    /* --- signal-handling thread --- */
    rc = pthread_create(&ctx.signal_thread, NULL, signal_thread_fn, &ctx);
    if (rc != 0) { errno = rc; perror("pthread_create signal"); goto cleanup; }

    /* --- logger thread --- */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) { errno = rc; perror("pthread_create logger"); goto cleanup; }

    /* Suppress unused-variable warning for rootfs (used as a label/hint) */
    fprintf(stderr, "[supervisor] Ready. base-rootfs=%s socket=%s\n",
            rootfs, CONTROL_PATH);

    /* --- event loop: accept and handle control connections --- */
    while (!ctx.should_stop) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EBADF)
                break;  /* server_fd closed by signal thread */
            perror("accept");
            continue;
        }

        control_request_t req;
        ssize_t nr = read(client_fd, &req, sizeof(req));
        if (nr != (ssize_t)sizeof(req)) {
            close(client_fd);
            continue;
        }
        /* NUL-terminate safety */
        req.container_id[CONTAINER_ID_LEN - 1] = '\0';
        req.rootfs[PATH_MAX - 1] = '\0';
        req.command[CHILD_COMMAND_LEN - 1] = '\0';

        switch (req.kind) {
        case CMD_START:
        case CMD_RUN:
            handle_start_run(&ctx, client_fd, &req);
            break;
        case CMD_PS:
            handle_ps(&ctx, client_fd);
            break;
        case CMD_LOGS:
            handle_logs(&ctx, client_fd, &req);
            break;
        case CMD_STOP:
            handle_stop(&ctx, client_fd, &req);
            break;
        default: {
            control_response_t resp;
            memset(&resp, 0, sizeof(resp));
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "unknown command");
            write(client_fd, &resp, sizeof(resp));
            break;
        }
        }

        close(client_fd);
    }

    /* --- graceful shutdown --- */
    fprintf(stderr, "[supervisor] Shutting down.\n");

    /* Signal all running containers to stop */
    {
        container_record_t *r;
        pthread_mutex_lock(&ctx.metadata_lock);
        for (r = ctx.containers; r; r = r->next)
            if (r->state == CONTAINER_RUNNING)
                kill(r->host_pid, SIGTERM);
        pthread_mutex_unlock(&ctx.metadata_lock);
    }

    /* Give containers a moment, then SIGKILL stragglers */
    sleep(2);
    {
        container_record_t *r;
        pthread_mutex_lock(&ctx.metadata_lock);
        for (r = ctx.containers; r; r = r->next)
            if (r->state == CONTAINER_RUNNING)
                kill(r->host_pid, SIGKILL);
        pthread_mutex_unlock(&ctx.metadata_lock);
    }

    /* Wait for logger and signal threads */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    ctx.should_stop = 1;
    pthread_join(ctx.signal_thread, NULL);

cleanup:
    /* Free container records */
    {
        container_record_t *r = ctx.containers, *next;
        while (r) {
            next = r->next;
            if (r->log_read_fd >= 0) close(r->log_read_fd);
            free(r);
            r = next;
        }
    }
    if (ctx.server_fd >= 0) close(ctx.server_fd);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    unlink(CONTROL_PATH);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Client-side control request                                         */
/* ------------------------------------------------------------------ */

/*
 * send_control_request
 *
 * Connects to the supervisor's UNIX domain socket, sends the request,
 * and reads the response (plus any streamed data for ps/logs).
 */
static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;
    char buf[4096];
    ssize_t n;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\n"
                "Is the supervisor running?\n",
                CONTROL_PATH, strerror(errno));
        close(fd);
        return 1;
    }

    /* Send request */
    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write request");
        close(fd);
        return 1;
    }

    /*
     * For PS and LOGS the supervisor streams text first, then sends
     * the fixed control_response_t frame.  We read until we have
     * consumed exactly sizeof(control_response_t) bytes at the end.
     *
     * Simple strategy: read everything, treat last sizeof(resp) bytes
     * as the response frame, print the rest as text.
     */
    if (req->kind == CMD_PS || req->kind == CMD_LOGS) {
        /* Accumulate all data */
        char *accum = NULL;
        size_t total = 0;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            accum = realloc(accum, total + (size_t)n);
            memcpy(accum + total, buf, (size_t)n);
            total += (size_t)n;
        }
        if (total >= sizeof(resp)) {
            /* Print everything except the trailing response frame */
            size_t text_len = total - sizeof(resp);
            if (text_len > 0)
                fwrite(accum, 1, text_len, stdout);
            memcpy(&resp, accum + text_len, sizeof(resp));
        }
        free(accum);
    } else {
        /* Simple request: just read the response frame */
        n = read(fd, &resp, sizeof(resp));
        if (n != (ssize_t)sizeof(resp)) {
            fprintf(stderr, "Incomplete response from supervisor\n");
            close(fd);
            return 1;
        }
    }

    close(fd);

    if (resp.status != 0) {
        fprintf(stderr, "Error: %s\n", resp.message);
        return 1;
    }

    if (resp.message[0] && req->kind != CMD_PS && req->kind != CMD_LOGS)
        printf("%s\n", resp.message);

    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI command entry points                                            */
/* ------------------------------------------------------------------ */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
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
                "Usage: %s run <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
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
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

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
