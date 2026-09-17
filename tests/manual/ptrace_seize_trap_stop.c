// ptrace_seize_trap_stop: the stops a seized tracee owes its tracer that are
// not signals -- Linux's JOBCTL_TRAP_STOP, reported as PTRACE_EVENT_STOP.
//
// Every expectation here was measured on Linux 6.12 (x86_64, glibc 2.41)
// before AOK was changed, and every case except the two detach ones failed on
// the unfixed kernel.
//
// new_child_case
//   A child a PTRACE_SEIZE tracer auto-attaches at clone, fork or vfork stops
//   first in a PTRACE_EVENT_STOP, status 0x80057f. Linux's ptrace_init_task
//   sets JOBCTL_TRAP_STOP for a PT_SEIZED child and queues SIGSTOP only for the
//   others. AOK queued SIGSTOP for both, 0x137f. strace -f takes a seized
//   tracee's SIGSTOP for a real signal and injects it, so every child it
//   followed printed "--- SIGSTOP {si_code=SI_KERNEL} ---", really stopped,
//   and sent its parent a SIGCHLD with CLD_STOPPED, which Linux never does.
//   Measured for a fork, a thread (glibc blocks every signal around
//   pthread_create's clone), vfork, posix_spawn, and a fork with every signal
//   blocked. The stop's siginfo names the stopped task itself.
//
// traceme_child_case
//   The other half, which must not change: a tracer that did not seize gets
//   the SIGSTOP, and its siginfo is SI_USER from pid 0 (Linux sets a bare
//   pending bit, with no queued siginfo behind it).
//
// interrupt_case
//   PTRACE_INTERRUPT stops a tracee whatever its SIGTRAP disposition, and a
//   blocking call it broke into restarts once the tracee is resumed -- no
//   handler ran. AOK's interrupt was a queued SIGTRAP: a tracee with SIGTRAP
//   blocked was never stopped, one with it ignored dropped the interrupt, and
//   an interrupted read() returned EINTR.
//
// interrupt_while_stopped
//   An interrupt sent to a tracee that is already in a stop is kept: once
//   resumed it traps again. AOK dropped it.
//
// detach_drops_the_interrupt, tracer_death_drops_the_interrupt
//   An interrupt the tracee has not taken yet goes with the tracer, whichever
//   way it leaves. These passed before the change; they guard the flag that
//   replaced the signal.
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <time.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "test_common.h"

extern char **environ;

#ifndef __WALL
#define __WALL 0x40000000
#endif
#ifndef PTRACE_SETOPTIONS
#define PTRACE_SETOPTIONS 0x4200
#endif
#ifndef PTRACE_GETEVENTMSG
#define PTRACE_GETEVENTMSG 0x4201
#endif
#ifndef PTRACE_GETSIGINFO
#define PTRACE_GETSIGINFO 0x4202
#endif
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif
#ifndef PTRACE_O_TRACEFORK
#define PTRACE_O_TRACEFORK 0x02
#endif
#ifndef PTRACE_O_TRACEVFORK
#define PTRACE_O_TRACEVFORK 0x04
#endif
#ifndef PTRACE_O_TRACECLONE
#define PTRACE_O_TRACECLONE 0x08
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif

#define STOP_STATUS(sig, event) (((event) << 16) | ((sig) << 8) | 0x7f)
#define EVENT_STOP STOP_STATUS(SIGTRAP, PTRACE_EVENT_STOP)
#define FOLLOW_OPTIONS (PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE)

static void nap(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) != 0 && errno == EINTR)
        ;
}

static void on_alarm(int sig) { (void) sig; }

// sigaction without SA_RESTART: the alarm exists to break a wait that would
// otherwise hang, and signal() would restart it.
static void install_watchdog(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
}

static pid_t wait_for(pid_t pid, int *status, int options) {
    alarm(test_watchdog_secs(10));
    pid_t got = waitpid(pid, status, __WALL | options);
    int saved = errno;
    alarm(0);
    errno = saved;
    return got;
}

static void check(const char *name, const char *what, uint64_t got, uint64_t want) {
    char label[200];
    snprintf(label, sizeof label, "%s: %s", name, what);
    if (got != want)
        failf(label, got, 0, 0, want, 0, 0);
    else
        test_logf("  ok   %-72s %#" PRIx64 "\n", label, got);
}

static void kill_and_reap(pid_t c) {
    if (c > 0)
        kill(c, SIGKILL);
    int st;
    while (wait_for(-1, &st, 0) > 0)
        ;
}

// Whether `fd` has something to read within `ms`.
static bool readable_within(int fd, int ms) {
    struct pollfd p = { .fd = fd, .events = POLLIN };
    int r;
    do
        r = poll(&p, 1, ms);
    while (r < 0 && errno == EINTR);
    return r == 1;
}

// A PTRACE_EVENT_STOP's siginfo: SIGTRAP, si_code (PTRACE_EVENT_STOP << 8) |
// SIGTRAP, and the id and uid of the task that stopped (do_jobctl_trap ->
// ptrace_do_notify, which uses task_pid_vnr(current)). Its message is 0.
static void check_event_stop_info(const char *name, const char *stop, pid_t pid) {
    char what[120];
    siginfo_t si;
    memset(&si, 0, sizeof si);
    long r = ptrace(PTRACE_GETSIGINFO, pid, 0, &si);
    snprintf(what, sizeof what, "%s: PTRACE_GETSIGINFO", stop);
    check(name, what, (uint64_t) (r == 0 ? 0 : errno), 0);
    snprintf(what, sizeof what, "%s: si_signo", stop);
    check(name, what, (uint64_t) si.si_signo, SIGTRAP);
    snprintf(what, sizeof what, "%s: si_code", stop);
    check(name, what, (uint64_t) (unsigned) si.si_code, (PTRACE_EVENT_STOP << 8) | SIGTRAP);
    snprintf(what, sizeof what, "%s: si_pid is the stopped task", stop);
    check(name, what, (uint64_t) si.si_pid, (uint64_t) pid);
    snprintf(what, sizeof what, "%s: si_uid", stop);
    check(name, what, (uint64_t) si.si_uid, (uint64_t) getuid());
    unsigned long msg = 0x5eed;
    ptrace(PTRACE_GETEVENTMSG, pid, 0, &msg);
    snprintf(what, sizeof what, "%s: message", stop);
    check(name, what, (uint64_t) msg, 0);
}

// ---- new_child_case ---------------------------------------------------------

static int reap(pid_t p) {
    int st;
    while (waitpid(p, &st, 0) < 0) {
        if (errno != EINTR)
            return 1;
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
}

static int op_fork(void) {
    pid_t p = fork();
    if (p == 0)
        _exit(0);
    return p < 0 ? 1 : reap(p);
}

static void *thread_body(void *arg) { return arg; }

static int op_thread(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, thread_body, NULL) != 0)
        return 1;
    return pthread_join(t, NULL) != 0;
}

static int op_vfork(void) {
    pid_t p = vfork();
    if (p == 0)
        _exit(0);
    return p < 0 ? 1 : reap(p);
}

// Also a seized tracee exec'ing without PTRACE_O_TRACEEXEC, which must NOT
// get the legacy post-exec SIGTRAP: Linux sends that only to a tracee that
// was not seized.
static int op_spawn(void) {
    pid_t p;
    char *argv[] = { "true", NULL };
    const char *path = access("/bin/true", X_OK) == 0 ? "/bin/true" : "/usr/bin/true";
    if (posix_spawn(&p, path, NULL, NULL, argv, environ) != 0)
        return 1;
    return reap(p);
}

// Every signal blocked across the fork, as glibc does around pthread_create
// and posix_spawn: the stop must not depend on the new task's mask.
static int op_fork_all_blocked(void) {
    sigset_t all, old;
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, &old);
    pid_t p = fork();
    if (p == 0) {
        sigprocmask(SIG_SETMASK, &old, NULL);
        _exit(0);
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    return p < 0 ? 1 : reap(p);
}

// A SIGCHLD handler, so the parent's SIGCHLDs are delivered -- and so reach
// the tracer as signal-delivery-stops whose si_code it can read.
static void on_sigchld(int sig) { (void) sig; }

// Follows the tracee the way strace -f does: every event-stop continued with
// no signal, every signal-delivery-stop continued with its own signal.
static void new_child_case(const char *name, int (*op)(void)) {
    int go[2];
    if (pipe(go) != 0) {
        check(name, "pipe", (uint64_t) errno, 0);
        return;
    }
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        close(go[1]);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_sigchld;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGCHLD, &sa, NULL);
        char ch;
        ssize_t n;
        do
            n = read(go[0], &ch, 1);
        while (n < 0 && errno == EINTR);
        _exit(n == 1 ? op() : 99);
    }
    close(go[0]);
    if (c < 0) {
        check(name, "fork", (uint64_t) errno, 0);
        close(go[1]);
        return;
    }
    if (ptrace(PTRACE_SEIZE, c, 0, (void *) (long) FOLLOW_OPTIONS) != 0) {
        check(name, "PTRACE_SEIZE", (uint64_t) errno, 0);
        close(go[1]);
        kill_and_reap(c);
        return;
    }
    if (write(go[1], "x", 1) != 1)
        check(name, "release the tracee", (uint64_t) errno, 0);
    close(go[1]);

    pid_t seen[8];
    int nseen = 0, sigstop_stops = 0, sigtrap_stops = 0, cld_stopped = 0;
    int st = 0;
    bool done = false;
    for (int guard = 0; guard < 200 && !done; guard++) {
        pid_t w = wait_for(-1, &st, 0);
        if (w < 0) {
            check(name, "wait (EINTR here is a hang)", (uint64_t) errno, 0);
            break;
        }
        if (!WIFSTOPPED(st)) {
            if (w == c) {
                check(name, "tracee's exit status", (uint64_t) st, 0);
                done = true;
            }
            continue;
        }
        int sig = WSTOPSIG(st);
        int event = (unsigned) st >> 16;
        bool fresh = w != c;
        for (int i = 0; i < nseen && i < (int) (sizeof seen / sizeof seen[0]); i++)
            if (seen[i] == w)
                fresh = false;
        if (fresh) {
            if (nseen < (int) (sizeof seen / sizeof seen[0]))
                seen[nseen] = w;
            nseen++;
            check(name, "new task's first stop", (uint64_t) st, EVENT_STOP);
            if (st == EVENT_STOP)
                check_event_stop_info(name, "new task's first stop", w);
        }
        int inject = 0;
        if (event == 0) {
            if (sig == SIGSTOP)
                sigstop_stops++;
            if (sig == SIGCHLD) {
                siginfo_t si;
                memset(&si, 0, sizeof si);
                if (ptrace(PTRACE_GETSIGINFO, w, 0, &si) == 0 && si.si_code == CLD_STOPPED)
                    cld_stopped++;
            }
            if (sig == SIGTRAP)
                sigtrap_stops++;     // not injected: SIGTRAP would kill it
            else
                inject = sig;
        }
        ptrace(PTRACE_CONT, w, 0, (void *) (long) inject);
    }
    check(name, "the tracer followed one new task", (uint64_t) nseen, 1);
    check(name, "SIGSTOP signal-delivery-stops", (uint64_t) sigstop_stops, 0);
    check(name, "SIGTRAP signal-delivery-stops", (uint64_t) sigtrap_stops, 0);
    check(name, "SIGCHLDs to the parent with CLD_STOPPED", (uint64_t) cld_stopped, 0);
    kill_and_reap(done ? 0 : c);
}

// ---- traceme_child_case -----------------------------------------------------

static void traceme_child_case(void) {
    const char *name = "TRACEME fork";
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(96);
        raise(SIGSTOP);
        _exit(op_fork());
    }
    if (c < 0) {
        check(name, "fork", (uint64_t) errno, 0);
        return;
    }
    int st = 0;
    if (wait_for(c, &st, 0) != c || st != STOP_STATUS(SIGSTOP, 0)) {
        check(name, "tracee's own SIGSTOP", (uint64_t) st, STOP_STATUS(SIGSTOP, 0));
        kill_and_reap(c);
        return;
    }
    ptrace(PTRACE_SETOPTIONS, c, 0, (void *) (long) FOLLOW_OPTIONS);
    ptrace(PTRACE_CONT, c, 0, 0);

    int nseen = 0;
    pid_t child = 0;
    bool done = false;
    for (int guard = 0; guard < 200 && !done; guard++) {
        pid_t w = wait_for(-1, &st, 0);
        if (w < 0) {
            check(name, "wait (EINTR here is a hang)", (uint64_t) errno, 0);
            break;
        }
        if (!WIFSTOPPED(st)) {
            done = w == c;
            continue;
        }
        int inject = 0;
        if (w != c && w != child) {
            // Its SIGSTOP, which a tracer that did not seize suppresses.
            child = w;
            nseen++;
            check(name, "new task's first stop", (uint64_t) st, STOP_STATUS(SIGSTOP, 0));
            siginfo_t si;
            memset(&si, 0x5a, sizeof si);
            ptrace(PTRACE_GETSIGINFO, w, 0, &si);
            check(name, "new task's first stop: si_signo", (uint64_t) si.si_signo, SIGSTOP);
            check(name, "new task's first stop: si_code is SI_USER", (uint64_t) (unsigned) si.si_code, SI_USER);
            check(name, "new task's first stop: si_pid", (uint64_t) si.si_pid, 0);
        } else if (((unsigned) st >> 16) == 0 && WSTOPSIG(st) != SIGTRAP) {
            inject = WSTOPSIG(st);
        }
        ptrace(PTRACE_CONT, w, 0, (void *) (long) inject);
    }
    check(name, "the tracer followed one new task", (uint64_t) nseen, 1);
    kill_and_reap(done ? 0 : c);
}

// ---- interrupt_case ---------------------------------------------------------

enum trap_disposition { TRAP_DEFAULT, TRAP_BLOCKED, TRAP_IGNORED, ALL_BLOCKED };
enum body { BODY_BUSY, BODY_READ, BODY_POLL, BODY_SLEEP };

struct report {
    long res;
    long err;
};

// A seized tracee doing `body` with SIGTRAP set up as `disp`, interrupted
// while it is inside it.
static void interrupt_case(const char *name, enum trap_disposition disp, enum body body) {
    int ready[2], data[2], report[2];
    if (pipe(ready) != 0 || pipe(data) != 0 || pipe(report) != 0) {
        check(name, "pipe", (uint64_t) errno, 0);
        return;
    }
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        close(ready[0]);
        close(data[1]);
        close(report[0]);
        sigset_t set;
        sigemptyset(&set);
        switch (disp) {
        case TRAP_DEFAULT:
            break;
        case TRAP_BLOCKED:
            sigaddset(&set, SIGTRAP);
            sigprocmask(SIG_BLOCK, &set, NULL);
            break;
        case TRAP_IGNORED:
            signal(SIGTRAP, SIG_IGN);
            break;
        case ALL_BLOCKED:
            sigfillset(&set);
            sigprocmask(SIG_BLOCK, &set, NULL);
            break;
        }
        if (write(ready[1], "r", 1) != 1)
            _exit(98);
        struct report r = { 0, 0 };
        switch (body) {
        case BODY_BUSY: {
            volatile unsigned long spins = 0;
            for (;;)
                spins++;
        }
        case BODY_READ: {
            char ch;
            r.res = read(data[0], &ch, 1);
            break;
        }
        case BODY_POLL: {
            struct pollfd p = { .fd = data[0], .events = POLLIN };
            r.res = poll(&p, 1, -1);
            break;
        }
        case BODY_SLEEP: {
            struct timespec t = { 1, 0 };
            r.res = nanosleep(&t, NULL);
            break;
        }
        }
        r.err = r.res < 0 ? errno : 0;
        if (write(report[1], &r, sizeof r) != (ssize_t) sizeof r)
            _exit(97);
        _exit(0);
    }
    close(ready[1]);
    close(data[0]);
    close(report[1]);
    if (c < 0) {
        check(name, "fork", (uint64_t) errno, 0);
        goto out;
    }
    char ch;
    if (!readable_within(ready[0], (int) test_watchdog_secs(10) * 1000) ||
            read(ready[0], &ch, 1) != 1) {
        check(name, "tracee ready", 0, 1);
        kill_and_reap(c);
        goto out;
    }
    nap(150);   // into the call

    int st = 0;
    if (ptrace(PTRACE_SEIZE, c, 0, 0) != 0) {
        check(name, "PTRACE_SEIZE", (uint64_t) errno, 0);
        kill_and_reap(c);
        goto out;
    }
    long r = ptrace(PTRACE_INTERRUPT, c, 0, 0);
    check(name, "PTRACE_INTERRUPT", (uint64_t) (r == 0 ? 0 : errno), 0);
    pid_t w = wait_for(c, &st, 0);
    if (w != c) {
        check(name, "interrupt stop (EINTR here: the tracee never stopped)", (uint64_t) errno, 0);
        kill_and_reap(c);
        goto out;
    }
    check(name, "interrupt stop", (uint64_t) st, EVENT_STOP);
    if (st != EVENT_STOP) {
        kill_and_reap(c);
        goto out;
    }
    check_event_stop_info(name, "interrupt stop", c);
    if (body == BODY_BUSY) {
        kill_and_reap(c);
        goto out;
    }

    // Resumed, the call carries on: nothing to report until it has cause.
    ptrace(PTRACE_CONT, c, 0, 0);
    if (body != BODY_SLEEP)
        check(name, "the interrupted call is still blocked after PTRACE_CONT",
              (uint64_t) readable_within(report[0], 300), 0);

    if (body == BODY_READ || body == BODY_POLL) {
        if (write(data[1], "y", 1) != 1)
            check(name, "write the data", (uint64_t) errno, 0);
    }

    struct report got = { -99, -99 };
    if (!readable_within(report[0], (int) test_watchdog_secs(10) * 1000) ||
            read(report[0], &got, sizeof got) != (ssize_t) sizeof got) {
        check(name, "the call returned", 0, 1);
        kill_and_reap(c);
        goto out;
    }
    switch (body) {
    case BODY_READ:
        check(name, "read() restarted and returned the byte", (uint64_t) got.res, 1);
        break;
    case BODY_POLL:
        check(name, "poll() restarted and returned the fd", (uint64_t) got.res, 1);
        break;
    case BODY_SLEEP:
        check(name, "nanosleep() restarted and completed", (uint64_t) got.res, 0);
        break;
    default:
        break;
    }
    check(name, "errno", (uint64_t) got.err, 0);

    w = wait_for(c, &st, 0);
    check(name, "tracee's exit status", (uint64_t) (w == c ? st : -1), 0);
    kill_and_reap(0);
out:
    close(ready[0]);
    close(data[1]);
    close(report[0]);
}

// ---- interrupt_while_stopped ------------------------------------------------

static void interrupt_while_stopped(void) {
    const char *name = "interrupt while stopped";
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        for (;;)
            nap(20);
    }
    if (c < 0) {
        check(name, "fork", (uint64_t) errno, 0);
        return;
    }
    nap(100);
    int st = 0;
    if (ptrace(PTRACE_SEIZE, c, 0, 0) != 0 || ptrace(PTRACE_INTERRUPT, c, 0, 0) != 0 ||
            wait_for(c, &st, 0) != c) {
        check(name, "seize, interrupt and wait", (uint64_t) errno, 0);
        kill_and_reap(c);
        return;
    }
    check(name, "first interrupt stop", (uint64_t) st, EVENT_STOP);
    long r = ptrace(PTRACE_INTERRUPT, c, 0, 0);
    check(name, "PTRACE_INTERRUPT of a stopped tracee", (uint64_t) (r == 0 ? 0 : errno), 0);
    ptrace(PTRACE_CONT, c, 0, 0);
    if (wait_for(c, &st, 0) != c) {
        check(name, "second stop (EINTR here: the interrupt was dropped)", (uint64_t) errno, 0);
        kill_and_reap(c);
        return;
    }
    check(name, "it traps again once resumed", (uint64_t) st, EVENT_STOP);
    check_event_stop_info(name, "second stop", c);
    // And only once.
    ptrace(PTRACE_CONT, c, 0, 0);
    nap(300);
    pid_t w = waitpid(c, &st, __WALL | WNOHANG);
    check(name, "no third stop", (uint64_t) w, 0);
    kill_and_reap(c);
}

// ---- detach_drops_the_interrupt, tracer_death_drops_the_interrupt ----------

// A tracee that writes a byte to `beat` every 20ms.
static pid_t spawn_heartbeat(int beat[2]) {
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        close(beat[0]);
        for (;;) {
            if (write(beat[1], "b", 1) != 1)
                _exit(0);
            nap(20);
        }
    }
    return c;
}

// Whether the heartbeat is still going: drain what is there, then wait for a
// fresh beat.
static bool still_beating(int fd) {
    char buf[256];
    while (readable_within(fd, 0) && read(fd, buf, sizeof buf) > 0)
        ;
    return readable_within(fd, 1000);
}

static void detach_drops_the_interrupt(void) {
    const char *name = "detach with an interrupt pending";
    int beat[2];
    if (pipe(beat) != 0) {
        check(name, "pipe", (uint64_t) errno, 0);
        return;
    }
    pid_t c = spawn_heartbeat(beat);
    close(beat[1]);
    if (c < 0) {
        check(name, "fork", (uint64_t) errno, 0);
        close(beat[0]);
        return;
    }
    nap(100);
    int st = 0;
    if (ptrace(PTRACE_SEIZE, c, 0, 0) != 0 || ptrace(PTRACE_INTERRUPT, c, 0, 0) != 0 ||
            wait_for(c, &st, 0) != c) {
        check(name, "seize, interrupt and wait", (uint64_t) errno, 0);
        kill_and_reap(c);
        close(beat[0]);
        return;
    }
    check(name, "interrupt stop", (uint64_t) st, EVENT_STOP);
    ptrace(PTRACE_INTERRUPT, c, 0, 0);          // owed, not yet taken
    long r = ptrace(PTRACE_DETACH, c, 0, 0);
    check(name, "PTRACE_DETACH", (uint64_t) (r == 0 ? 0 : errno), 0);
    check(name, "the detached tracee runs on", (uint64_t) still_beating(beat[0]), 1);
    check(name, "and has not died", (uint64_t) waitpid(c, &st, WNOHANG), 0);
    kill_and_reap(c);
    close(beat[0]);
}

static void tracer_death_drops_the_interrupt(void) {
    const char *name = "tracer dies with an interrupt pending";
    // The tracee is the dying tracer's own child, so tracing it needs no
    // privilege, and it is reparented to us.
    if (prctl(PR_SET_CHILD_SUBREAPER, 1L, 0L, 0L, 0L) != 0) {
        test_logf("  no PR_SET_CHILD_SUBREAPER (%s), %s skipped\n", strerror(errno), name);
        return;
    }
    int beat[2], idp[2];
    if (pipe(beat) != 0 || pipe(idp) != 0) {
        check(name, "pipe", (uint64_t) errno, 0);
        prctl(PR_SET_CHILD_SUBREAPER, 0L, 0L, 0L, 0L);
        return;
    }
    fflush(NULL);
    pid_t tracer = fork();
    if (tracer == 0) {
        close(idp[0]);
        pid_t c = spawn_heartbeat(beat);
        if (c < 0)
            _exit(90);
        if (write(idp[1], &c, sizeof c) != (ssize_t) sizeof c)
            _exit(91);
        nap(100);
        int st;
        if (ptrace(PTRACE_SEIZE, c, 0, 0) != 0 || ptrace(PTRACE_INTERRUPT, c, 0, 0) != 0)
            _exit(92);
        if (wait_for(c, &st, 0) != c || st != EVENT_STOP)
            _exit(93);
        ptrace(PTRACE_INTERRUPT, c, 0, 0);      // owed, not yet taken
        _exit(0);                               // without detaching
    }
    close(beat[1]);
    close(idp[1]);
    pid_t c = 0;
    if (tracer < 0 || read(idp[0], &c, sizeof c) != (ssize_t) sizeof c)
        c = 0;
    int st = 0;
    pid_t w = tracer > 0 ? wait_for(tracer, &st, 0) : -1;
    check(name, "the tracer's exit status", (uint64_t) (w == tracer ? st : -1), 0);
    if (c > 0) {
        check(name, "the tracee runs on", (uint64_t) still_beating(beat[0]), 1);
        check(name, "and has not died", (uint64_t) waitpid(c, &st, WNOHANG), 0);
    }
    kill_and_reap(c);
    close(beat[0]);
    close(idp[0]);
    prctl(PR_SET_CHILD_SUBREAPER, 0L, 0L, 0L, 0L);
}

// PTRACE_INTERRUPT of a task this process does not trace is ESRCH, like any
// other request's "not your tracee". strace reports anything else as an error
// when it detaches from a task that has just gone.
static void interrupt_untraced(void) {
    const char *name = "interrupt an untraced child";
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        for (;;)
            nap(50);
    }
    if (c < 0) {
        check(name, "fork", (uint64_t) errno, 0);
        return;
    }
    long r = ptrace(PTRACE_INTERRUPT, c, 0, 0);
    check(name, "errno", (uint64_t) (r == 0 ? 0 : errno), ESRCH);
    kill_and_reap(c);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    install_watchdog();

    new_child_case("seized fork", op_fork);
    new_child_case("seized thread", op_thread);
    new_child_case("seized vfork", op_vfork);
    new_child_case("seized posix_spawn", op_spawn);
    new_child_case("seized fork, every signal blocked", op_fork_all_blocked);
    traceme_child_case();

    interrupt_case("interrupt busy loop", TRAP_DEFAULT, BODY_BUSY);
    interrupt_case("interrupt busy loop, SIGTRAP blocked", TRAP_BLOCKED, BODY_BUSY);
    interrupt_case("interrupt busy loop, SIGTRAP ignored", TRAP_IGNORED, BODY_BUSY);
    interrupt_case("interrupt read", TRAP_DEFAULT, BODY_READ);
    interrupt_case("interrupt read, SIGTRAP blocked", TRAP_BLOCKED, BODY_READ);
    interrupt_case("interrupt read, SIGTRAP ignored", TRAP_IGNORED, BODY_READ);
    interrupt_case("interrupt poll, every signal blocked", ALL_BLOCKED, BODY_POLL);
    interrupt_case("interrupt nanosleep, SIGTRAP ignored", TRAP_IGNORED, BODY_SLEEP);
    interrupt_while_stopped();
    detach_drops_the_interrupt();
    tracer_death_drops_the_interrupt();
    interrupt_untraced();

    return finish_suite("ptrace_seize_trap_stop");
}
