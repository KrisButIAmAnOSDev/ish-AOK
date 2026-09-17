/*
 * blocked_wait_state -- a task blocked in epoll_wait, msgrcv, msgsnd, semop,
 * semtimedop, io_getevents or a FUSE request is asleep, as Linux says it is.
 *
 * AOK decides "asleep" from io_block, which TASK_MAY_BLOCK sets around read,
 * write, poll, futex, wait and the other blocking calls. These waits never set
 * it, and three things read it:
 *
 *   - /proc/<pid>/stat. A child blocked in any of them showed 'R' for the whole
 *     time it was sampled (2s, on the Devuan and Alpine arm64 roots); Linux 6.12
 *     shows 'S'. poll, read, recv, waitpid, futex and nanosleep already said S.
 *
 *   - The guest load average, which counts every task that is not io_block. A
 *     process idling in its event loop (node, python asyncio, dbus, systemd:
 *     anything built on epoll) added 1 to /proc/loadavg for as long as it
 *     idled. Measured before the fix: one child in epoll_wait for 65s took the
 *     1-minute average from 0.00 to 0.66, where the same child in poll left it
 *     at 0.00.
 *
 *   - The address-space barrier, which SIGUSR1s every sibling thread that is
 *     not io_block whenever one maps or unmaps memory. wait_for reported that
 *     poke as an interruption, so a thread in msgrcv, msgsnd, semop,
 *     semtimedop or io_getevents failed with EINTR 0-10ms into its wait, with
 *     no signal sent, while its siblings ran. (epoll_wait was spared: poll_wait
 *     already ignores a bare poke.)
 *
 * Checked here, each against Linux (camd, 6.12):
 *
 *   1. Two children blocked in each call read 'S' in /proc/<pid>/stat, with a
 *      child in poll and one in a pipe read as controls.
 *   2. Those children do not lift the 1-minute load average: 20 of them for
 *      11s would add ~2.5 if they were counted, so it may rise by at most 0.5.
 *   3. A thread blocked in each call, with a sibling spinning and another
 *      mapping and unmapping memory, returns EINTR only when a real signal
 *      arrives at 1s, with its handler run once. That is also the positive
 *      control: the wait still ends for a signal.
 *   4. The same for a FUSE request (a LOOKUP the daemon answers late), where
 *      the wait is reached from stat(2), which sets no io_block of its own:
 *      'S' while it waits, and under the same siblings the stat ends with the
 *      daemon's answer and the daemon is never sent a FUSE_INTERRUPT. Before
 *      the fix the poke cut the LOOKUP short and AOK told the daemon so; the
 *      stat still came back on time only because AOK looks a name up again
 *      after a failed lookup. No signal is involved there, because what a
 *      signal does to a FUSE request is a different question (Linux waits on
 *      for the daemon's answer). Needs mount(2), so it is skipped
 *      unprivileged.
 *
 * io_block keeps the barrier from poking a waiter at all, so an ordinary run
 * of 3 and 4 checks that half of the fix. The other half -- a bare poke that
 * does land in the wait is a spurious wakeup, not EINTR -- covers the barrier
 * reading io_block just before the waiter sets it, which is too rare to hit on
 * purpose. ISH_TEST_POKE_BLOCKED_TASKS makes it happen every time for tasks
 * whose comm starts with its value, so run this test with it as well:
 *
 *     ISH_TEST_POKE_BLOCKED_TASKS=blocked_wait_st ./build/ish -f <root> \
 *         /path/to/blocked_wait_state
 *
 * (the comm is the binary's name, cut to 15 characters). With the second half
 * taken out of wait_for_blocked, that run failed all five System V and aio
 * checks and the FUSE one, while the ordinary run still passed.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#ifndef SYS_epoll_pwait2
#define SYS_epoll_pwait2 441
#endif
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif

#define TEST_NAME "blocked_wait_state"

/* How many children sit in each call for the state and load checks. */
#define LOAD_COPIES 2
/* How long they sit there before the second load average read. Long enough to
 * take two of the 5-second samples whatever the phase. */
#define LOAD_SECONDS 11
/* When the signal (or the FUSE daemon's answer) arrives in the poke checks. */
#define ARRIVE_MS 1000
/* How long the thread under test waits alone before its siblings start. */
#define SETTLE_MS 200

static void check(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!ok || test_verbose) {
        printf("%s ", ok ? "ok" : "FAIL");
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
    if (!ok)
        failures_total++;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0)
        ;
}

/* How late a wait may end and still count, scaled like the watchdogs. Only
 * upper bounds use it: a loaded machine cannot make a wait end early. */
static long slack_ms(long base) {
    return base * (long) test_watchdog_secs(1);
}

static char proc_state(pid_t pid) {
    char path[64], buf[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int) pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return '?';
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return '?';
    buf[n] = '\0';
    char *rp = strrchr(buf, ')');
    if (rp == NULL || rp[1] != ' ')
        return '?';
    return rp[2];
}

static int read_loadavg(double *one, char *raw, size_t raw_size) {
    FILE *f = fopen("/proc/loadavg", "r");
    if (f == NULL)
        return -1;
    char buf[256];
    int ok = fgets(buf, sizeof(buf), f) != NULL;
    fclose(f);
    if (!ok || sscanf(buf, "%lf", one) != 1)
        return -1;
    snprintf(raw, raw_size, "%s", buf);
    raw[strcspn(raw, "\n")] = '\0';
    return 0;
}

/* ---- the calls ---------------------------------------------------------- */

/* The kernel's struct io_event, which is the same 32 bytes on every ABI. */
struct io_event_ {
    uint64_t data, obj;
    int64_t result, result2;
};

enum kind {
    K_EPOLL_WAIT, K_EPOLL_PWAIT, K_EPOLL_PWAIT2, K_MSGRCV, K_MSGSND,
    K_SEMOP, K_SEMTIMEDOP, K_IO_GETEVENTS, K_POLL, K_PIPE_READ, K_COUNT
};
static const char *const kind_names[K_COUNT] = {
    "epoll_wait", "epoll_pwait", "epoll_pwait2", "msgrcv", "msgsnd",
    "semop", "semtimedop", "io_getevents", "poll (control)", "pipe read (control)",
};

/* The System V objects are made by the parent, so that killing a child cannot
 * leak one: an empty queue, a queue already full, and a semaphore at 0. */
static int empty_queue = -1, full_queue = -1, zero_sem = -1;

static int make_ipc_objects(void) {
    empty_queue = msgget(IPC_PRIVATE, 0600);
    full_queue = msgget(IPC_PRIVATE, 0600);
    zero_sem = semget(IPC_PRIVATE, 1, 0600);
    if (empty_queue < 0 || full_queue < 0 || zero_sem < 0)
        return -1;
    struct msqid_ds ds;
    if (msgctl(full_queue, IPC_STAT, &ds) != 0)
        return -1;
    ds.msg_qbytes = 64;
    if (msgctl(full_queue, IPC_SET, &ds) != 0)
        return -1;
    struct { long mtype; char mtext[64]; } m = {.mtype = 1};
    return msgsnd(full_queue, &m, 64, IPC_NOWAIT);
}

static void remove_ipc_objects(void) {
    if (empty_queue >= 0)
        msgctl(empty_queue, IPC_RMID, NULL);
    if (full_queue >= 0)
        msgctl(full_queue, IPC_RMID, NULL);
    if (zero_sem >= 0)
        semctl(zero_sem, 0, IPC_RMID);
}

struct waiter {
    int fds[3];
    unsigned long aio_ctx;
};

/* Everything a call needs before it can block, made by whoever blocks.
 * Returns 0, or -1 with errno set. */
static int waiter_prepare(enum kind k, struct waiter *w) {
    memset(w, 0, sizeof(*w));
    w->fds[0] = w->fds[1] = w->fds[2] = -1;
    switch (k) {
    case K_EPOLL_WAIT:
    case K_EPOLL_PWAIT:
    case K_EPOLL_PWAIT2: {
        if (pipe(w->fds) != 0)
            return -1;
        w->fds[2] = epoll_create1(0);
        struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 1};
        if (w->fds[2] < 0 || epoll_ctl(w->fds[2], EPOLL_CTL_ADD, w->fds[0], &ev) != 0)
            return -1;
        return 0;
    }
    case K_POLL:
    case K_PIPE_READ:
        return pipe(w->fds);
    case K_IO_GETEVENTS:
#ifdef SYS_io_setup
        return (int) syscall(SYS_io_setup, 8, &w->aio_ctx);
#else
        errno = ENOSYS;
        return -1;
#endif
    default:
        return 0;
    }
}

static void waiter_release(enum kind k, struct waiter *w) {
    for (int i = 0; i < 3; i++)
        if (w->fds[i] >= 0)
            close(w->fds[i]);
#ifdef SYS_io_destroy
    if (k == K_IO_GETEVENTS && w->aio_ctx != 0)
        syscall(SYS_io_destroy, w->aio_ctx);
#else
    (void) k;
#endif
}

/* The blocking call. Returns its result, errno as the call left it. */
static long waiter_block(enum kind k, struct waiter *w) {
    struct epoll_event ev;
    switch (k) {
    case K_EPOLL_WAIT:
        return epoll_wait(w->fds[2], &ev, 1, -1);
    case K_EPOLL_PWAIT:
        return epoll_pwait(w->fds[2], &ev, 1, -1, NULL);
    case K_EPOLL_PWAIT2:
        return syscall(SYS_epoll_pwait2, w->fds[2], &ev, 1, NULL, NULL, 8);
    case K_MSGRCV: {
        struct { long mtype; char mtext[64]; } m;
        return msgrcv(empty_queue, &m, sizeof(m.mtext), 0, 0);
    }
    case K_MSGSND: {
        struct { long mtype; char mtext[8]; } m = {.mtype = 1};
        return msgsnd(full_queue, &m, sizeof(m.mtext), 0);
    }
    case K_SEMOP: {
        struct sembuf op = {.sem_num = 0, .sem_op = -1, .sem_flg = 0};
        return semop(zero_sem, &op, 1);
    }
    case K_SEMTIMEDOP: {
        struct sembuf op = {.sem_num = 0, .sem_op = -1, .sem_flg = 0};
        struct timespec ts = {.tv_sec = 60};
        return semtimedop(zero_sem, &op, 1, &ts);
    }
    case K_IO_GETEVENTS: {
        struct io_event_ events[1];
#ifdef SYS_io_getevents
        return syscall(SYS_io_getevents, w->aio_ctx, 1L, 1L, events, NULL);
#else
        (void) events;
        errno = ENOSYS;
        return -1;
#endif
    }
    case K_POLL: {
        struct pollfd pfd = {.fd = w->fds[0], .events = POLLIN};
        return poll(&pfd, 1, -1);
    }
    case K_PIPE_READ: {
        char buf[16];
        return read(w->fds[0], buf, sizeof(buf));
    }
    default:
        errno = EINVAL;
        return -1;
    }
}

/* ---- 1 and 2: /proc state and the load average -------------------------- */

/* A child blocked in `k`, returned once it is about to block; -1 if it could
 * not get there. */
static pid_t spawn_blocked_child(enum kind k) {
    int ready[2];
    if (pipe(ready) != 0)
        return -1;
    pid_t kid = fork();
    if (kid < 0) {
        close(ready[0]);
        close(ready[1]);
        return -1;
    }
    if (kid == 0) {
        close(ready[0]);
        struct waiter w;
        if (waiter_prepare(k, &w) != 0)
            _exit(3);
        if (write(ready[1], "r", 1) != 1)
            _exit(3);
        waiter_block(k, &w);
        _exit(4);
    }
    close(ready[1]);
    char c;
    ssize_t n = read(ready[0], &c, 1);
    close(ready[0]);
    if (n != 1) {
        waitpid(kid, NULL, 0);
        return -1;
    }
    return kid;
}

static void test_state_and_load(void) {
    double l0 = 0, l1 = 0;
    char raw0[256] = "", raw1[256] = "";
    int have_l0 = read_loadavg(&l0, raw0, sizeof(raw0)) == 0;
    check(have_l0, "read /proc/loadavg");
    long start = now_ms();

    pid_t kids[K_COUNT][LOAD_COPIES];
    for (int k = 0; k < K_COUNT; k++)
        for (int c = 0; c < LOAD_COPIES; c++) {
            kids[k][c] = spawn_blocked_child((enum kind) k);
            check(kids[k][c] > 0, "%s: a child could block in it", kind_names[k]);
        }

    /* Settle, then sample every child a few times: a wait that is 'S' only
     * some of the time is still wrong. */
    sleep_ms(300);
    for (int k = 0; k < K_COUNT; k++) {
        int total = 0, sleeping = 0;
        char seen[8] = "";
        size_t seen_len = 0;
        for (int round = 0; round < 5; round++) {
            for (int c = 0; c < LOAD_COPIES; c++) {
                if (kids[k][c] <= 0)
                    continue;
                char s = proc_state(kids[k][c]);
                total++;
                if (s == 'S')
                    sleeping++;
                if (strchr(seen, s) == NULL && seen_len + 1 < sizeof(seen)) {
                    seen[seen_len++] = s;
                    seen[seen_len] = '\0';
                }
            }
            sleep_ms(40);
        }
        check(total > 0 && sleeping == total,
              "%s: /proc/<pid>/stat reads S while blocked (%d of %d samples; saw \"%s\")",
              kind_names[k], sleeping, total, seen);
    }

    long left = LOAD_SECONDS * 1000L - (now_ms() - start);
    if (left > 0)
        sleep_ms(left);
    int have_l1 = read_loadavg(&l1, raw1, sizeof(raw1)) == 0;

    for (int k = 0; k < K_COUNT; k++)
        for (int c = 0; c < LOAD_COPIES; c++)
            if (kids[k][c] > 0) {
                kill(kids[k][c], SIGKILL);
                waitpid(kids[k][c], NULL, 0);
            }

    if (have_l0 && have_l1) {
        check(l1 <= l0 + 0.5,
              "%d blocked children for %ds leave the 1-minute load average alone: "
              "before \"%s\", after \"%s\" (a rise of %.2f; counted, they would add ~2.5)",
              K_COUNT * LOAD_COPIES, LOAD_SECONDS, raw0, raw1, l1 - l0);
    }
}

/* ---- 3: a sibling's mmap does not end the wait --------------------------- */

static atomic_int helpers_stop;
static atomic_long map_rounds;
static volatile sig_atomic_t handler_hits;

static void on_usr1(int sig) {
    (void) sig;
    handler_hits++;
}

static void *spin_main(void *arg) {
    (void) arg;
    volatile unsigned long spins = 0;
    while (!atomic_load(&helpers_stop))
        spins++;
    return NULL;
}

static void *map_main(void *arg) {
    (void) arg;
    while (!atomic_load(&helpers_stop)) {
        char *p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            p[0] = 1;
            munmap(p, 65536);
        }
        atomic_fetch_add(&map_rounds, 1);
    }
    return NULL;
}

/* Helper threads start with SIGUSR1 blocked, so it can land only on the
 * thread under test. */
static int start_helpers(pthread_t *spinner, pthread_t *mapper) {
    sigset_t usr1, old;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &usr1, &old);
    atomic_store(&helpers_stop, 0);
    int e1 = pthread_create(spinner, NULL, spin_main, NULL);
    int e2 = e1 == 0 ? pthread_create(mapper, NULL, map_main, NULL) : -1;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (e1 == 0 && e2 != 0) {
        atomic_store(&helpers_stop, 1);
        pthread_join(*spinner, NULL);
    }
    return e1 == 0 && e2 == 0 ? 0 : -1;
}

static void stop_helpers(pthread_t spinner, pthread_t mapper) {
    atomic_store(&helpers_stop, 1);
    pthread_join(spinner, NULL);
    pthread_join(mapper, NULL);
}

struct blocked_thread {
    enum kind k;
    struct waiter w;
    long ret;
    int err;
    long elapsed;
    int hits_at_return;
    atomic_int done;
};

static void *blocked_main(void *arg) {
    struct blocked_thread *b = arg;
    long start = now_ms();
    errno = 0;
    b->ret = waiter_block(b->k, &b->w);
    b->err = errno;
    b->elapsed = now_ms() - start;
    b->hits_at_return = handler_hits;
    atomic_store(&b->done, 1);
    return NULL;
}

/* The siblings start only once the thread under test is waiting. A poke sets
 * a flag its target clears when it next runs guest code, and the barrier skips
 * a thread whose flag is still up, so a thread blocked in a syscall is poked
 * at most once -- the first time after it last ran. With the siblings started
 * first, that one poke can be delivered on the way INTO the call, at its first
 * host syscall, before there is any wait for it to end. It was, every time,
 * for the FUSE request below, which passed on a kernel with the bug. */
static void test_poke_does_not_interrupt(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1; /* no SA_RESTART: the signal must end the wait */
    sigaction(SIGUSR1, &sa, NULL);

    for (int k = 0; k <= K_IO_GETEVENTS; k++) {
        struct blocked_thread *b = calloc(1, sizeof(*b));
        if (b == NULL)
            return;
        b->k = (enum kind) k;
        if (waiter_prepare(b->k, &b->w) != 0) {
            check(0, "%s: set up (%s)", kind_names[k], strerror(errno));
            free(b);
            continue;
        }
        handler_hits = 0;
        long start = now_ms();
        pthread_t spinner, mapper, blocked;
        if (pthread_create(&blocked, NULL, blocked_main, b) != 0) {
            check(0, "%s: blocked thread", kind_names[k]);
            waiter_release(b->k, &b->w);
            free(b);
            return;
        }
        sleep_ms(SETTLE_MS);
        atomic_store(&map_rounds, 0);
        int helpers = start_helpers(&spinner, &mapper) == 0;
        check(helpers, "%s: helper threads", kind_names[k]);
        long left = ARRIVE_MS - (now_ms() - start);
        if (left > 0)
            sleep_ms(left);
        long rounds_during = atomic_load(&map_rounds);
        pthread_kill(blocked, SIGUSR1);
        long deadline = now_ms() + slack_ms(3000);
        while (!atomic_load(&b->done) && now_ms() < deadline)
            sleep_ms(10);
        if (helpers)
            stop_helpers(spinner, mapper);
        if (!atomic_load(&b->done)) {
            /* Leave the thread and its waiter: the exit takes them. */
            check(0, "%s: a signal at %dms ends the wait (still blocked %ldms later)",
                  kind_names[k], ARRIVE_MS, slack_ms(3000));
            continue;
        }
        pthread_join(blocked, NULL);
        check(b->ret == -1 && b->err == EINTR && b->hits_at_return == 1 &&
                  b->elapsed >= ARRIVE_MS - 50 && b->elapsed <= ARRIVE_MS + slack_ms(1500),
              "%s, with sibling threads spinning and mapping memory, signal at %dms: "
              "ret=%ld errno=%s handler=%d after %ldms (want -1 EINTR handler=1 after ~%dms)",
              kind_names[k], ARRIVE_MS, b->ret, strerror(b->err), b->hits_at_return,
              b->elapsed, ARRIVE_MS);
        /* The positive control for the siblings: the barrier had work to do. */
        if (helpers)
            check(rounds_during > 0, "%s: the mapping thread ran during the wait (%ld rounds)",
                  kind_names[k], rounds_during);
        waiter_release(b->k, &b->w);
        free(b);
    }
    signal(SIGUSR1, SIG_DFL);
}

/* ---- 4: a FUSE request -------------------------------------------------- */

#define FUSE_LOOKUP_ 1
#define FUSE_GETATTR_ 3
#define FUSE_INIT_ 26
#define FUSE_INTERRUPT_ 36
#define FUSE_FORGET_ 2
#define FUSE_BATCH_FORGET_ 42
#define FUSE_DESTROY_ 38

struct fuse_in_header_ {
    uint32_t len, opcode;
    uint64_t unique, nodeid;
    uint32_t uid, gid, pid, padding;
};
struct fuse_out_header_ {
    uint32_t len;
    int32_t error;
    uint64_t unique;
};
struct fuse_attr_ {
    uint64_t ino, size, blocks, atime, mtime, ctime;
    uint32_t atimensec, mtimensec, ctimensec;
    uint32_t mode, nlink, uid, gid, rdev, blksize, padding;
};
struct fuse_attr_out_ {
    uint64_t attr_valid;
    uint32_t attr_valid_nsec, dummy;
    struct fuse_attr_ attr;
};
struct fuse_init_in_ {
    uint32_t major, minor, max_readahead, flags;
};
struct fuse_init_out_ {
    uint32_t major, minor, max_readahead, flags;
    uint16_t max_background, congestion_threshold;
    uint32_t max_write, time_gran;
    uint16_t max_pages, map_alignment;
    uint32_t unused[8];
};

static int fuse_reply(int devfd, uint64_t unique, int error, const void *body, size_t len) {
    char buf[sizeof(struct fuse_out_header_) + 256];
    struct fuse_out_header_ out = {
        .len = (uint32_t) (sizeof(out) + len), .error = error, .unique = unique,
    };
    memcpy(buf, &out, sizeof(out));
    if (len > 0)
        memcpy(buf + sizeof(out), body, len);
    return write(devfd, buf, out.len) == (ssize_t) out.len ? 0 : -1;
}

/* A daemon that holds the first LOOKUP of "slow1", and then of "slow2", until a
 * byte arrives on `release`, and answers everything else at once. Only the
 * first: one stat(2) can look a name up more than once (AOK resolves the path
 * again after a failed lookup), and holding the second too would wait on a
 * byte nobody sends. Every FUSE_INTERRUPT it is sent is reported on `report`. */
static void fuse_daemon(int devfd, int release, int report) {
    static char buf[160 * 1024];
    int held[2] = {0, 0};
    for (;;) {
        ssize_t n = read(devfd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            _exit(0); /* ENODEV once unmounted */
        }
        if ((size_t) n < sizeof(struct fuse_in_header_))
            _exit(5);
        struct fuse_in_header_ *in = (struct fuse_in_header_ *) buf;
        char *arg = buf + sizeof(*in);
        switch (in->opcode) {
        case FUSE_INIT_: {
            struct fuse_init_in_ *init = (struct fuse_init_in_ *) arg;
            struct fuse_init_out_ out;
            memset(&out, 0, sizeof(out));
            out.major = 7;
            out.minor = init->minor < 31 ? init->minor : 31;
            out.max_readahead = init->max_readahead;
            out.max_write = 128 * 1024;
            fuse_reply(devfd, in->unique, 0, &out, sizeof(out));
            break;
        }
        case FUSE_GETATTR_: {
            struct fuse_attr_out_ out;
            memset(&out, 0, sizeof(out));
            out.attr.ino = in->nodeid;
            out.attr.mode = S_IFDIR | 0755;
            out.attr.nlink = 2;
            out.attr.uid = (uint32_t) getuid();
            out.attr.gid = (uint32_t) getgid();
            fuse_reply(devfd, in->unique, in->nodeid == 1 ? 0 : -ENOENT, &out, sizeof(out));
            break;
        }
        case FUSE_LOOKUP_: {
            int which = strcmp(arg, "slow1") == 0 ? 0 : strcmp(arg, "slow2") == 0 ? 1 : -1;
            if (which >= 0 && !held[which]) {
                held[which] = 1;
                char c;
                if (read(release, &c, 1) != 1)
                    _exit(6);
            }
            fuse_reply(devfd, in->unique, -ENOENT, NULL, 0);
            break;
        }
        case FUSE_INTERRUPT_:
            if (write(report, "I", 1) != 1)
                _exit(7);
            break; /* no reply */
        case FUSE_FORGET_:
        case FUSE_BATCH_FORGET_:
            break; /* no reply */
        case FUSE_DESTROY_:
            fuse_reply(devfd, in->unique, 0, NULL, 0);
            break;
        default:
            fuse_reply(devfd, in->unique, -ENOSYS, NULL, 0);
            break;
        }
    }
}

struct stat_thread {
    const char *path;
    int ret, err;
    long elapsed;
    atomic_int done;
};

static void *stat_main(void *arg) {
    struct stat_thread *t = arg;
    struct stat st;
    long start = now_ms();
    errno = 0;
    t->ret = stat(t->path, &st);
    t->err = errno;
    t->elapsed = now_ms() - start;
    atomic_store(&t->done, 1);
    return NULL;
}

static void test_fuse_request(void) {
    int devfd = open("/dev/fuse", O_RDWR);
    if (devfd < 0) {
        test_logf("note: no /dev/fuse (%s); FUSE checks skipped\n", strerror(errno));
        return;
    }
    char base[] = "/tmp/blocked_wait_XXXXXX";
    if (mkdtemp(base) == NULL) {
        check(0, "mkdtemp (%s)", strerror(errno));
        close(devfd);
        return;
    }
    char mnt[64], slow1[80], slow2[80], prompt[80];
    snprintf(mnt, sizeof(mnt), "%s/mnt", base);
    snprintf(slow1, sizeof(slow1), "%s/slow1", mnt);
    snprintf(slow2, sizeof(slow2), "%s/slow2", mnt);
    snprintf(prompt, sizeof(prompt), "%s/prompt", mnt);
    mkdir(mnt, 0755);
    char opts[128];
    snprintf(opts, sizeof(opts), "fd=%d,rootmode=40000,user_id=%u,group_id=%u",
             devfd, (unsigned) getuid(), (unsigned) getgid());
    if (mount(TEST_NAME, mnt, "fuse", 0, opts) != 0) {
        int e = errno;
        if (e == EPERM || e == EACCES)
            test_logf("note: mount not permitted (%s); FUSE checks skipped\n", strerror(e));
        else
            check(0, "mount a FUSE filesystem (%s)", strerror(e));
        close(devfd);
        rmdir(mnt);
        rmdir(base);
        return;
    }
    int release[2], report[2];
    if (pipe(release) != 0 || pipe(report) != 0) {
        check(0, "pipe (%s)", strerror(errno));
        umount2(mnt, MNT_DETACH);
        close(devfd);
        return;
    }
    pid_t daemon = fork();
    if (daemon == 0) {
        close(release[1]);
        close(report[0]);
        fuse_daemon(devfd, release[0], report[1]);
        _exit(0);
    }
    close(release[0]);
    close(report[1]);
    fcntl(report[0], F_SETFL, O_NONBLOCK);

    /* 'S' while a child's stat waits for the daemon. */
    pid_t kid = fork();
    if (kid == 0) {
        struct stat st;
        _exit(stat(slow1, &st) == -1 && errno == ENOENT ? 0 : 1);
    }
    sleep_ms(300);
    int sleeping = 0, total = 0;
    char last = '?';
    for (int round = 0; round < 5; round++) {
        last = proc_state(kid);
        total++;
        if (last == 'S')
            sleeping++;
        sleep_ms(40);
    }
    check(sleeping == total,
          "stat of a FUSE file the daemon has not answered: /proc/<pid>/stat reads S "
          "(%d of %d samples; last '%c')", sleeping, total, last);
    if (write(release[1], "g", 1) != 1)
        check(0, "release the daemon (%s)", strerror(errno));
    int status = 0;
    waitpid(kid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "that stat ends with the daemon's ENOENT (status %#x)", status);

    /* No early EINTR while siblings spin and map memory: the daemon answers
     * at ARRIVE_MS and nothing is signalled. The siblings start once the stat
     * is waiting, as in the checks above. */
    pthread_t spinner, mapper, statter;
    struct stat_thread t = {.path = slow2};
    long start = now_ms();
    if (pthread_create(&statter, NULL, stat_main, &t) != 0) {
        check(0, "FUSE: stat thread");
    } else {
        sleep_ms(SETTLE_MS);
        atomic_store(&map_rounds, 0);
        int helpers = start_helpers(&spinner, &mapper) == 0;
        check(helpers, "FUSE: helper threads");
        long left = ARRIVE_MS - (now_ms() - start);
        if (left > 0)
            sleep_ms(left);
        long rounds_during = atomic_load(&map_rounds);
        if (write(release[1], "g", 1) != 1)
            check(0, "release the daemon (%s)", strerror(errno));
        long deadline = now_ms() + slack_ms(3000);
        while (!atomic_load(&t.done) && now_ms() < deadline)
            sleep_ms(10);
        if (helpers)
            stop_helpers(spinner, mapper);
        if (atomic_load(&t.done)) {
            pthread_join(statter, NULL);
            check(t.ret == -1 && t.err == ENOENT && t.elapsed >= ARRIVE_MS - 50,
                  "stat of a FUSE file, with sibling threads spinning and mapping "
                  "memory, answered at %dms: ret=%d errno=%s after %ldms "
                  "(want -1 ENOENT after ~%dms)",
                  ARRIVE_MS, t.ret, strerror(t.err), t.elapsed, ARRIVE_MS);
            if (helpers)
                check(rounds_during > 0, "FUSE: the mapping thread ran during the wait "
                      "(%ld rounds)", rounds_during);
        } else {
            check(0, "stat of a FUSE file ends once the daemon answers");
        }
    }

    /* Nothing was signalled, so the daemon must not have been told a request
     * was interrupted. This is the check that sees the poke: AOK looks a name
     * up again after a failed lookup, so a stat whose first LOOKUP the poke
     * cut short still came back with the daemon's ENOENT, on time -- having
     * sent the daemon a FUSE_INTERRUPT on the way. A stat the daemon answers
     * at once goes first, so everything sent before it has been read. */
    struct stat st;
    stat(prompt, &st);
    int interrupts = 0;
    char r;
    while (read(report[0], &r, 1) == 1)
        interrupts++;
    check(interrupts == 0,
          "FUSE: the daemon was sent no FUSE_INTERRUPT with nothing signalled (it got %d)",
          interrupts);
    close(report[0]);

    close(release[1]);
    umount2(mnt, MNT_DETACH);
    kill(daemon, SIGKILL);
    waitpid(daemon, NULL, 0);
    close(devfd);
    rmdir(mnt);
    rmdir(base);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    setvbuf(stdout, NULL, _IONBF, 0);

    if (make_ipc_objects() != 0) {
        printf("FAIL System V objects for the waits (%s)\n", strerror(errno));
        remove_ipc_objects();
        return finish_suite(TEST_NAME);
    }
    test_state_and_load();
    test_poke_does_not_interrupt();
    test_fuse_request();
    remove_ipc_objects();
    return finish_suite(TEST_NAME);
}
