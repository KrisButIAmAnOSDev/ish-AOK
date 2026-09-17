/*
 * sock_timeo_eintr.c -- a socket wait with SO_RCVTIMEO or SO_SNDTIMEO armed is
 * never restarted, not even for an SA_RESTART handler, and that holds for
 * read(2), readv(2), write(2) and writev(2) as much as for recv(2).
 *
 * signal(7) lists "a socket interface with a timeout (SO_RCVTIMEO) set" among
 * the interfaces SA_RESTART never restarts. The kernel ends such a wait with
 * sock_intr_errno(timeo): -EINTR while a timeout is armed, -ERESTARTSYS only
 * when none is. So a read with a 2s SO_RCVTIMEO, interrupted 200ms in by an
 * SA_RESTART handler, fails with EINTR at 200ms.
 *
 * AOK restarted it. fs/sock.c already made the decision -- its socket waits
 * return a plain EINTR when a timeout is armed and a restart only when none is
 * -- but read, readv, write and writev pass through kernel/fs.c's generic
 * dispatchers, which turned every EINTR into a restart whenever the handler had
 * SA_RESTART. The call re-executed and waited the whole timeout again. Measured
 * with a 600ms SO_RCVTIMEO and the signal at 150ms: EAGAIN after ~756ms on a
 * UDP socket, where Linux gives EINTR after 150ms. recv, recvfrom and recvmsg
 * never had the bug; they do not pass through that conversion.
 *
 * What is checked, with the elapsed time asserted on every one -- an EINTR or
 * an EAGAIN is only right if it comes at the right moment:
 *   - read and readv on a UDP, TCP, unix stream, unix datagram and netlink
 *     socket with SO_RCVTIMEO: an SA_RESTART signal 200ms into a 2s timeout
 *     ends the call with EINTR at ~200ms, and the handler ran once
 *   - write and writev on a full unix stream socket with SO_SNDTIMEO: the same
 *   - with no timeout the same signal still restarts the call, which carries
 *     on until it is really ended at 600ms: by data for a reader, by room for
 *     a writer, and on netlink, where nothing arrives unasked, by a handler
 *     installed without SA_RESTART
 *   - a positive control per socket: with no signal, the armed timeout ends
 *     the wait with EAGAIN at ~300ms, so the socket really blocked and the
 *     timeout was really set
 *   - a job-control stop inside the timeout ends the call with EINTR once the
 *     process is continued, with no handler at all (signal(7) documents this
 *     for the socket calls with a timeout); without a timeout the stop is
 *     invisible and the call returns the data that arrives after it
 *   - the interruption a timed netlink receive answered with EINTR, through
 *     read or recv, does not make the next interrupted call restart: a pipe
 *     read with a handler without SA_RESTART still fails with EINTR
 *
 * Checked against Linux 6.12 (Devuan 6, x86_64 and -m32, unprivileged).
 *
 * Exits 0 and prints "sock_timeo_eintr: PASS" on success.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

/* As netlink_route.c: Alpine has no linux-headers. */
#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
#define NETLINK_ROUTE_ 0

/* The interrupting signal lands this far into the call. */
#define SIGNAL_AT_MS 200
/* The timeout it lands inside: far enough past the signal that a restart,
 * which waits all of it again, cannot be mistaken for an interruption. */
#define TIMEO_MS 2000
/* The positive control's timeout, with no signal at all. */
#define CONTROL_TIMEO_MS 300
/* When an untimed call is really ended: data, room, or a non-restarting
 * signal. Well after SIGNAL_AT_MS, so ending at the signal is visible. */
#define ARRIVAL_MS 600
/* A stop at SIGNAL_AT_MS lasts until this, and an untimed call stopped that
 * way is ended at STOPPED_ARRIVAL_MS. */
#define CONT_AT_MS 400
#define STOPPED_ARRIVAL_MS 800

enum kind { K_UDP, K_TCP, K_UNIX_STREAM, K_UNIX_DGRAM, K_NETLINK };
static const char *const kind_names[] = {
    "udp", "tcp", "unix stream", "unix dgram", "netlink",
};

enum op { OP_READ, OP_READV, OP_WRITE, OP_WRITEV, OP_RECV };
static const char *const op_names[] = {"read", "readv", "write", "writev", "recv"};

static volatile sig_atomic_t alrm_hits;
static volatile sig_atomic_t usr1_hits;

static void on_signal(int sig) {
    if (sig == SIGALRM)
        alrm_hits++;
    else if (sig == SIGUSR1)
        usr1_hits++;
}

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
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        continue;
}

/* How late a call may end and still count, scaled like the watchdogs for a
 * heavily loaded run. Only upper bounds use it: a loaded machine cannot make a
 * wait end EARLY, and the lower bounds are what this test is about. */
static long slack_ms(long base) {
    return base * (long) test_watchdog_secs(1);
}

static void install(int sig, int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static void alarm_in_ms(long ms) {
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = ms / 1000;
    it.it_value.tv_usec = (ms % 1000) * 1000;
    setitimer(ITIMER_REAL, &it, NULL);
}

static int set_timeo(int fd, int option, long ms) {
    struct timeval tv = {.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
    return setsockopt(fd, SOL_SOCKET, option, &tv, sizeof(tv));
}

static bool is_write(enum op op) {
    return op == OP_WRITE || op == OP_WRITEV;
}

/* `fd` is the socket under test and `peer` the far end that feeds or drains
 * it, or -1 on netlink, which nothing but the kernel writes to. */
static int open_kind(enum kind k, int *fd, int *peer) {
    *fd = -1;
    *peer = -1;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(sin);
    int sv[2];
    switch (k) {
    case K_UDP:
        *fd = socket(AF_INET, SOCK_DGRAM, 0);
        *peer = socket(AF_INET, SOCK_DGRAM, 0);
        if (*fd < 0 || *peer < 0 || bind(*fd, (struct sockaddr *) &sin, sizeof(sin)) < 0 ||
                getsockname(*fd, (struct sockaddr *) &sin, &len) < 0 ||
                connect(*peer, (struct sockaddr *) &sin, len) < 0)
            return -1;
        return 0;
    case K_TCP: {
        int listener = socket(AF_INET, SOCK_STREAM, 0);
        *peer = socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0 || *peer < 0 ||
                bind(listener, (struct sockaddr *) &sin, sizeof(sin)) < 0 ||
                listen(listener, 1) < 0 ||
                getsockname(listener, (struct sockaddr *) &sin, &len) < 0 ||
                connect(*peer, (struct sockaddr *) &sin, len) < 0) {
            if (listener >= 0)
                close(listener);
            return -1;
        }
        *fd = accept(listener, NULL, NULL);
        close(listener);
        return *fd < 0 ? -1 : 0;
    }
    case K_UNIX_STREAM:
    case K_UNIX_DGRAM:
        if (socketpair(AF_UNIX, k == K_UNIX_STREAM ? SOCK_STREAM : SOCK_DGRAM, 0, sv) < 0)
            return -1;
        *fd = sv[0];
        *peer = sv[1];
        return 0;
    case K_NETLINK:
        *fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE_);
        return *fd < 0 ? -1 : 0;
    }
    return -1;
}

/* Fill fd's way out until a nonblocking write of even one byte would block,
 * so the next blocking write has to wait. */
static int fill(int fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        return -1;
    char chunk[4096];
    memset(chunk, 'f', sizeof(chunk));
    const size_t sizes[] = {sizeof(chunk), 1};
    for (int i = 0; i < 2; i++) {
        for (;;) {
            ssize_t r = write(fd, chunk, sizes[i]);
            if (r > 0)
                continue;
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            return -1;
        }
    }
    return fcntl(fd, F_SETFL, fl);
}

/* The call under test. A readv or writev spans two buffers, so a flattening
 * dispatcher is what gets exercised, not a single-buffer shortcut. */
static ssize_t do_io(enum op op, int fd) {
    char buf[16];
    char head[1], tail[15];
    struct iovec in[2] = {{head, sizeof(head)}, {tail, sizeof(tail)}};
    char v = 'v', w = 'w';
    struct iovec out[2] = {{&v, 1}, {&w, 1}};
    switch (op) {
    case OP_READ:
        return read(fd, buf, sizeof(buf));
    case OP_READV:
        return readv(fd, in, 2);
    case OP_WRITE:
        return write(fd, &w, 1);
    case OP_WRITEV:
        return writev(fd, out, 2);
    case OP_RECV:
        return recv(fd, buf, sizeof(buf), 0);
    }
    return -1;
}

/* Open the socket for this op, filled when the op writes. -1 has been
 * reported. */
static int prepare(enum kind k, enum op op, int *fd, int *peer) {
    if (open_kind(k, fd, peer) < 0) {
        check(0, "%s: open (%s)", kind_names[k], strerror(errno));
        return -1;
    }
    if (is_write(op) && fill(*fd) < 0) {
        check(0, "%s: fill the send buffer (%s)", kind_names[k], strerror(errno));
        return -1;
    }
    return 0;
}

/* An SA_RESTART signal inside an armed timeout ends the call with EINTR at
 * the signal. A restart would wait the whole timeout again. */
static void timed_signal(enum kind k, enum op op) {
    int fd, peer;
    if (prepare(k, op, &fd, &peer) < 0)
        return;
    const char *option = is_write(op) ? "SO_SNDTIMEO" : "SO_RCVTIMEO";
    if (set_timeo(fd, is_write(op) ? SO_SNDTIMEO : SO_RCVTIMEO, TIMEO_MS) < 0) {
        check(0, "%s on %s: setsockopt %s (%s)", op_names[op], kind_names[k], option,
              strerror(errno));
        return;
    }
    install(SIGALRM, SA_RESTART);
    long start = now_ms();
    alarm_in_ms(SIGNAL_AT_MS);
    errno = 0;
    ssize_t r = do_io(op, fd);
    int err = errno;
    long elapsed = now_ms() - start;
    check(r == -1 && err == EINTR && alrm_hits == 1 && elapsed >= SIGNAL_AT_MS - 50 &&
              elapsed < SIGNAL_AT_MS + slack_ms(800),
          "%s on %s, %s %dms, SA_RESTART signal at %dms: ret=%zd errno=%s handler=%d "
          "after %ldms (want -1 EINTR handler=1 after ~%dms; a restart waits out the "
          "timeout)",
          op_names[op], kind_names[k], option, TIMEO_MS, SIGNAL_AT_MS, r, strerror(err),
          (int) alrm_hits, elapsed, SIGNAL_AT_MS);
}

/* No signal: the timeout itself ends the wait, when it should. Without this a
 * socket that never blocked, or a timeout that was never set, could pass or
 * fail the other cases for reasons that have nothing to do with restarting. */
static void timed_control(enum kind k, enum op op) {
    int fd, peer;
    if (prepare(k, op, &fd, &peer) < 0)
        return;
    const char *option = is_write(op) ? "SO_SNDTIMEO" : "SO_RCVTIMEO";
    if (set_timeo(fd, is_write(op) ? SO_SNDTIMEO : SO_RCVTIMEO, CONTROL_TIMEO_MS) < 0) {
        check(0, "%s on %s: setsockopt %s (%s)", op_names[op], kind_names[k], option,
              strerror(errno));
        return;
    }
    long start = now_ms();
    errno = 0;
    ssize_t r = do_io(op, fd);
    int err = errno;
    long elapsed = now_ms() - start;
    check(r == -1 && (err == EAGAIN || err == EWOULDBLOCK) &&
              elapsed >= CONTROL_TIMEO_MS - 50 && elapsed < CONTROL_TIMEO_MS + slack_ms(1000),
          "%s on %s, %s %dms, no signal: ret=%zd errno=%s after %ldms "
          "(want -1 EAGAIN after ~%dms)",
          op_names[op], kind_names[k], option, CONTROL_TIMEO_MS, r, strerror(err), elapsed,
          CONTROL_TIMEO_MS);
}

/* In a helper process: end an untimed call for real, with data for a reader or
 * room for a writer. `fd` is the helper's copy of the socket under test. */
static void feed(enum op op, int fd, int peer) {
    close(fd);
    if (!is_write(op)) {
        (void) !write(peer, "d", 1);
        return;
    }
    /* Drain the fill, then take what the resumed write sends. The writer
     * closing its end instead (it failed, and says so) is end-of-file. */
    char buf[4096];
    int fl = fcntl(peer, F_GETFL);
    fcntl(peer, F_SETFL, fl | O_NONBLOCK);
    while (read(peer, buf, sizeof(buf)) > 0)
        continue;
    fcntl(peer, F_SETFL, fl);
    (void) !read(peer, buf, sizeof(buf));
}

/* The untimed call's result, once `feed` has ended it. */
static void check_fed(enum kind k, enum op op, const char *what, ssize_t r, int err,
        long elapsed, long fed_at_ms) {
    ssize_t want = op == OP_WRITEV ? 2 : 1;
    check(r == want && elapsed >= fed_at_ms - 100 && elapsed < fed_at_ms + slack_ms(2000),
          "%s on %s, no timeout, %s, %s at %ldms: ret=%zd errno=%s after %ldms "
          "(want %zd after ~%ldms)",
          op_names[op], kind_names[k], what, is_write(op) ? "room" : "data", fed_at_ms, r,
          r < 0 ? strerror(err) : "-", elapsed, want, fed_at_ms);
}

/* With no timeout the SA_RESTART signal restarts the call, which carries on
 * until ARRIVAL_MS: data for a reader, room for a writer, and on netlink a
 * handler installed without SA_RESTART. Ending at the signal would be the
 * opposite bug, and just as visible here. */
static void untimed_restart(enum kind k, enum op op) {
    int fd, peer;
    if (prepare(k, op, &fd, &peer) < 0)
        return;
    install(SIGALRM, SA_RESTART);
    install(SIGUSR1, 0);
    signal(SIGPIPE, SIG_IGN);
    pid_t self = getpid();
    fflush(stdout);
    pid_t helper = fork();
    if (helper < 0) {
        check(0, "%s on %s: fork (%s)", op_names[op], kind_names[k], strerror(errno));
        return;
    }
    if (helper == 0) {
        sleep_ms(ARRIVAL_MS);
        if (k == K_NETLINK)
            kill(self, SIGUSR1);
        else
            feed(op, fd, peer);
        _exit(0);
    }
    long start = now_ms();
    alarm_in_ms(SIGNAL_AT_MS);
    errno = 0;
    ssize_t r = do_io(op, fd);
    int err = errno;
    long elapsed = now_ms() - start;
    if (k == K_NETLINK) {
        check(r == -1 && err == EINTR && alrm_hits == 1 && usr1_hits == 1 &&
                  elapsed >= ARRIVAL_MS - 100 && elapsed < ARRIVAL_MS + slack_ms(2000),
              "%s on %s, no timeout, SA_RESTART signal at %dms, plain one at %dms: "
              "ret=%zd errno=%s handlers=%d,%d after %ldms "
              "(want -1 EINTR handlers=1,1 after ~%dms)",
              op_names[op], kind_names[k], SIGNAL_AT_MS, ARRIVAL_MS, r, strerror(err),
              (int) alrm_hits, (int) usr1_hits, elapsed, ARRIVAL_MS);
    } else {
        check(alrm_hits == 1, "%s on %s, no timeout: SA_RESTART handler runs=%d (want 1)",
              op_names[op], kind_names[k], (int) alrm_hits);
        char what[64];
        snprintf(what, sizeof(what), "SA_RESTART signal at %dms", SIGNAL_AT_MS);
        check_fed(k, op, what, r, err, elapsed, ARRIVAL_MS);
    }
    /* Closing first ends a helper still waiting for a write that failed. */
    close(fd);
    waitpid(helper, NULL, 0);
}

/* A job-control stop inside an armed timeout ends the call too: EINTR once the
 * process is continued, with no handler anywhere. signal(7) documents that for
 * the socket calls with a timeout, and it is the same decision, so a restart
 * here is the same bug. Without a timeout the stop is invisible: the call
 * resumes and returns what arrives afterwards. */
static void stopped(enum kind k, enum op op, bool timed) {
    int fd, peer;
    if (prepare(k, op, &fd, &peer) < 0)
        return;
    const char *option = is_write(op) ? "SO_SNDTIMEO" : "SO_RCVTIMEO";
    if (timed && set_timeo(fd, is_write(op) ? SO_SNDTIMEO : SO_RCVTIMEO, TIMEO_MS) < 0) {
        check(0, "%s on %s: setsockopt %s (%s)", op_names[op], kind_names[k], option,
              strerror(errno));
        return;
    }
    signal(SIGPIPE, SIG_IGN);
    pid_t self = getpid();
    fflush(stdout);
    pid_t helper = fork();
    if (helper < 0) {
        check(0, "%s on %s: fork (%s)", op_names[op], kind_names[k], strerror(errno));
        return;
    }
    if (helper == 0) {
        sleep_ms(SIGNAL_AT_MS);
        kill(self, SIGSTOP);
        sleep_ms(CONT_AT_MS - SIGNAL_AT_MS);
        kill(self, SIGCONT);
        if (!timed) {
            sleep_ms(STOPPED_ARRIVAL_MS - CONT_AT_MS);
            feed(op, fd, peer);
        }
        _exit(0);
    }
    long start = now_ms();
    errno = 0;
    ssize_t r = do_io(op, fd);
    int err = errno;
    long elapsed = now_ms() - start;
    if (timed) {
        check(r == -1 && err == EINTR && elapsed >= CONT_AT_MS - 50 &&
                  elapsed < CONT_AT_MS + slack_ms(800),
              "%s on %s, %s %dms, stopped at %dms and continued at %dms: ret=%zd errno=%s "
              "after %ldms (want -1 EINTR after ~%dms; a restart waits out the timeout)",
              op_names[op], kind_names[k], option, TIMEO_MS, SIGNAL_AT_MS, CONT_AT_MS, r,
              strerror(err), elapsed, CONT_AT_MS);
    } else {
        char what[64];
        snprintf(what, sizeof(what), "stopped at %dms and continued at %dms", SIGNAL_AT_MS,
                 CONT_AT_MS);
        check_fed(k, op, what, r, err, elapsed, STOPPED_ARRIVAL_MS);
    }
    close(fd);
    waitpid(helper, NULL, 0);
}

static void timed_stop(enum kind k, enum op op) {
    stopped(k, op, true);
}

static void untimed_stop(enum kind k, enum op op) {
    stopped(k, op, false);
}

/* An interruption a timed wait answered with EINTR is used up, and must not
 * decide whether the NEXT interrupted call restarts. AOK records, when a signal
 * interrupts a wait parked on a condition variable -- the netlink receive is
 * one -- whether its handler asked for a restart, and the next restart decision
 * reads that record. A timed wait that answers EINTR without reading it left it
 * behind: a pipe read interrupted afterwards by a handler WITHOUT SA_RESTART
 * found the stale "restart" and restarted, returning the data that came later
 * instead of EINTR. */
static void answered_interruption(enum kind k, enum op op) {
    timed_signal(k, op);

    int p[2];
    if (pipe(p) < 0) {
        check(0, "pipe (%s)", strerror(errno));
        return;
    }
    install(SIGUSR1, 0);
    pid_t self = getpid();
    fflush(stdout);
    pid_t helper = fork();
    if (helper < 0) {
        check(0, "fork (%s)", strerror(errno));
        return;
    }
    if (helper == 0) {
        sleep_ms(SIGNAL_AT_MS);
        kill(self, SIGUSR1);
        sleep_ms(ARRIVAL_MS - SIGNAL_AT_MS);
        (void) !write(p[1], "d", 1);
        _exit(0);
    }
    char buf[16];
    long start = now_ms();
    errno = 0;
    ssize_t r = read(p[0], buf, sizeof(buf));
    int err = errno;
    long elapsed = now_ms() - start;
    check(r == -1 && err == EINTR && usr1_hits == 1 && elapsed >= SIGNAL_AT_MS - 50 &&
              elapsed < SIGNAL_AT_MS + slack_ms(350),
          "after a timed %s on %s ended in EINTR, a pipe read and a handler without "
          "SA_RESTART at %dms: ret=%zd errno=%s handler=%d after %ldms "
          "(want -1 EINTR handler=1 after ~%dms, not a restart that reads the data sent "
          "at %dms)",
          op_names[op], kind_names[k], SIGNAL_AT_MS, r, r < 0 ? strerror(err) : "-",
          (int) usr1_hits, elapsed, SIGNAL_AT_MS, ARRIVAL_MS);
    waitpid(helper, NULL, 0);
}

/* Each case in a child of its own: a fresh socket, fresh dispositions, and a
 * hang that costs one case rather than the run. */
static void run(void (*scenario)(enum kind, enum op), const char *name, enum kind k,
        enum op op) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        check(0, "%s %s on %s: fork (%s)", name, op_names[op], kind_names[k], strerror(errno));
        return;
    }
    if (pid == 0) {
        failures_total = 0;
        scenario(k, op);
        fflush(stdout);
        _exit(failures_total > 100 ? 100 : (int) failures_total);
    }
    long deadline = now_ms() + (long) test_watchdog_secs(20) * 1000;
    int status = 0;
    for (;;) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
            break;
        if (done < 0 && errno != EINTR) {
            check(0, "%s %s on %s: waitpid (%s)", name, op_names[op], kind_names[k],
                  strerror(errno));
            return;
        }
        if (now_ms() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            check(0, "%s %s on %s: still blocked after %us", name, op_names[op],
                  kind_names[k], test_watchdog_secs(20));
            return;
        }
        sleep_ms(10);
    }
    if (WIFEXITED(status))
        failures_total += WEXITSTATUS(status);
    else
        check(0, "%s %s on %s: killed by signal %d", name, op_names[op], kind_names[k],
              WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    static const enum kind readers[] = {K_UDP, K_TCP, K_UNIX_STREAM, K_UNIX_DGRAM, K_NETLINK};
    for (size_t i = 0; i < sizeof(readers) / sizeof(readers[0]); i++) {
        enum kind k = readers[i];
        run(timed_control, "timed_control", k, OP_READ);
        run(timed_signal, "timed_signal", k, OP_READ);
        run(timed_signal, "timed_signal", k, OP_READV);
        run(untimed_restart, "untimed_restart", k, OP_READ);
        run(untimed_restart, "untimed_restart", k, OP_READV);
    }

    run(timed_control, "timed_control", K_UNIX_STREAM, OP_WRITE);
    run(timed_signal, "timed_signal", K_UNIX_STREAM, OP_WRITE);
    run(timed_signal, "timed_signal", K_UNIX_STREAM, OP_WRITEV);
    run(untimed_restart, "untimed_restart", K_UNIX_STREAM, OP_WRITE);
    run(untimed_restart, "untimed_restart", K_UNIX_STREAM, OP_WRITEV);

    run(timed_stop, "timed_stop", K_UDP, OP_READ);
    run(timed_stop, "timed_stop", K_NETLINK, OP_READ);
    run(timed_stop, "timed_stop", K_UNIX_STREAM, OP_WRITE);
    run(untimed_stop, "untimed_stop", K_UDP, OP_READ);
    run(untimed_stop, "untimed_stop", K_UNIX_STREAM, OP_WRITE);

    run(answered_interruption, "answered_interruption", K_NETLINK, OP_READ);
    run(answered_interruption, "answered_interruption", K_NETLINK, OP_RECV);

    return finish_suite("sock_timeo_eintr");
}
