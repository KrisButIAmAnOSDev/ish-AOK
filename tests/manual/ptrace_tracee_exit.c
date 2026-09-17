// ptrace_tracee_exit: a traced task's exit goes to its tracer first.
//
// Every `strace -f` of a program that made a thread or forked ended with
// "strace: wait4(__WALL): No child processes" and exit status 1, with a
// "+++ exited" line for the top process only, though the program itself ran
// fine. A tracer never learned that a tracee it was not the parent of had
// exited: do_wait asked the tracer's other tracees about stops and nothing
// else, a thread was destroyed the moment it exited, and a process became a
// zombie for its real parent alone. A tracer waiting on such a process by pid
// could also reach the parent's reap and destroy it, and the parent's own
// waitpid then failed.
//
// Linux's rules, each measured on 6.12 (x86_64, -m64 and -m32) before AOK was
// changed -- "A zombie ptracee is only visible to its ptracer":
//
//   - A traced thread's exit is reported to its tracer (with or without
//     __WALL) and the thread is released when the tracer reaps it. Its process
//     cannot be reaped before that, even by a tracer that is also the parent;
//     it is already a zombie in /proc, and its pidfd is readable.
//   - A traced process whose parent is not the tracer is reaped by the tracer
//     first. Until then its parent's wait says "not yet" (WNOHANG 0, a blocking
//     wait blocks) and it has no SIGCHLD. Once reaped it is passed to the
//     parent, which gets SIGCHLD and reaps it -- and is the only one charged
//     its CPU time. A parent that ignores SIGCHLD has it released instead.
//   - A tracer that dies holding the zombie hands it over the same way.
//   - A zombie reports the process's exit code once the process has exited as
//     a whole, and with no exit_group the code is the last thread's (6.0+).
//     SIGCHLD names the process and carries the leader's own code.
//   - Threads zapped by an exec from another thread exit 0; the exec'ing
//     thread's old tid never exits.
//   - Attaching to a zombie, or to a thread of one's own, is EPERM.
//   - A signal a tracer passes on from a signal-delivery-stop keeps its
//     siginfo; a different one is SI_USER from the tracer. This is how a
//     traced parent receives the SIGCHLD above.
//
// Each case forks its own process group and is killed whole if it hangs.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "test_common.h"

#ifndef __WALL
#define __WALL 0x40000000
#endif
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
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
#ifndef PTRACE_O_TRACEEXEC
#define PTRACE_O_TRACEEXEC 0x10
#endif
#ifndef PTRACE_EVENT_EXEC
#define PTRACE_EVENT_EXEC 4
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

#define FOLLOW_OPTS (PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK)
#define EXEC_TARGET_ARG "--ptrace-tracee-exit-exec-target"
#define EXITED(code) ((code) << 8)

// ---- shared state ----------------------------------------------------------

struct shm {
    volatile int step;
    volatile int go;
    volatile pid_t tid, tid2, g;
    volatile int thread_go, thread_started;

    volatile int t_ready, t_seize_err;
    volatile int t_saw, t_saw_ret, t_saw_err, t_si_pid, t_si_code, t_si_status;
    volatile int t_cmd, t_done, t_exit;
    volatile int t_ret, t_err, t_status;
    volatile int t_after_ret, t_after_err;

    volatile int g_zombie, t_reaped;
    volatile int p_probed;
    volatile int p_nohang_pid_ret, p_nohang_pid_err;
    volatile int p_nohang_any_ret, p_nohang_any_err;
    volatile int p_sigchld_at_probe, p_pidfd_revents;
    volatile int p_wait_ret, p_wait_err, p_wait_status, p_wait_saw_reaped;
    volatile int p_sigchld_count, p_si_code, p_si_pid, p_si_status;
    volatile long p_cru_us;
    volatile int p_after_ret, p_after_err;
    volatile int p_exiting;
    volatile int inj_sig[4], inj_code[4], inj_pid[4], inj_count;
};

static struct shm *S;

static void nap_ms(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

// Poll a flag another process sets. 0 on timeout.
static int await(volatile int *flag, long ms) {
    for (long waited = 0; *flag == 0; waited += 2) {
        if (waited >= ms)
            return 0;
        nap_ms(2);
    }
    return 1;
}

static void check(const char *name, const char *what, long long got, long long want) {
    if (got == want) {
        test_logf("  ok   %s: %s = %lld (%#llx)\n", name, what, got, (unsigned long long) got);
        return;
    }
    printf("FAIL %s: %s: got %lld (%#llx), want %lld (%#llx)\n", name, what,
           got, (unsigned long long) got, want, (unsigned long long) want);
    failures_total++;
}

static void note(const char *name, const char *what, long long got) {
    test_logf("  note %s: %s = %lld (%#llx)\n", name, what, got, (unsigned long long) got);
}

static void on_alarm(int sig) { (void) sig; }

// A blocking waitpid bounded by SIGALRM without SA_RESTART: EINTR means the
// wait hung, which is a finding, not a flake.
static pid_t wait_blocking(pid_t pid, int *st, int options, unsigned secs) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm(test_watchdog_secs(secs));
    pid_t got = waitpid(pid, st, options);
    int err = errno;
    alarm(0);
    errno = err;
    return got;
}

static pid_t wait4_blocking(pid_t pid, int *st, int options, struct rusage *ru, unsigned secs) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm(test_watchdog_secs(secs));
    pid_t got = wait4(pid, st, options, ru);
    int err = errno;
    alarm(0);
    errno = err;
    return got;
}

// strace's resume: events and the attach SIGSTOP are swallowed, real signals
// are delivered.
static void resume(pid_t pid, int st) {
    int sig = WSTOPSIG(st);
    int event = (st >> 16) & 0xff;
    int deliver = (event != 0 || (sig & 0x7f) == SIGTRAP || sig == SIGSTOP) ? 0 : sig;
    ptrace(PTRACE_CONT, pid, 0, (void *) (long) deliver);
}

static long cru_us(void) {
    struct rusage ru;
    getrusage(RUSAGE_CHILDREN, &ru);
    return (long) (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000L +
        ru.ru_utime.tv_usec + ru.ru_stime.tv_usec;
}

static long ru_us(const struct rusage *ru) {
    return (long) (ru->ru_utime.tv_sec + ru->ru_stime.tv_sec) * 1000000L +
        ru->ru_utime.tv_usec + ru->ru_stime.tv_usec;
}

static char proc_state(pid_t pid) {
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", (int) pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return '?';
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return '?';
    buf[n] = '\0';
    char *rp = strrchr(buf, ')');
    return rp != NULL && rp[1] == ' ' ? rp[2] : '?';
}

// A thread that ends with a code of its own: a pthread whose body returns the
// code and then makes the exit system call itself, since pthread_exit always
// exits 0. clone() with CLONE_THREAD would be simpler and musl refuses it. The
// thread first points the kernel's clear-child-tid at *tid_word, so its
// creator can watch the word fall to 0 as the thread exits; libc's own record
// of the thread is abandoned with it, so nothing may join or cancel it.
struct thread_start {
    int (*fn)(void *);
    pid_t *tid_word;
};

static void *thread_trampoline(void *p) {
    struct thread_start *start = p;
    *start->tid_word = (pid_t) syscall(SYS_gettid);
    syscall(SYS_set_tid_address, start->tid_word);
    syscall(SYS_exit, start->fn(NULL));
    return NULL;
}

// Returns the new thread's id once it is running, or -1.
static pid_t spawn_thread(int (*fn)(void *), pid_t *tid_word) {
    static struct thread_start starts[8];
    static int nstarts;
    if (nstarts == 8)
        return -1;
    struct thread_start *start = &starts[nstarts++];
    start->fn = fn;
    start->tid_word = tid_word;
    *tid_word = 0;
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int err = pthread_create(&thread, &attr, thread_trampoline, start);
    pthread_attr_destroy(&attr);
    if (err != 0)
        return -1;
    while (*tid_word == 0)
        nap_ms(1);
    return *tid_word;
}

static void wait_tid_gone(pid_t *tid_word) {
    while (*tid_word != 0)
        nap_ms(1);
}

static int thread_exit7(void *arg) {
    (void) arg;
    S->thread_started = 1;
    while (!S->thread_go)
        nap_ms(1);
    return 7;
}

static int thread_nap_then_exit7(void *arg) {
    (void) arg;
    S->thread_started = 1;
    nap_ms(300);
    return 7;
}

static void burn_cpu_ms(long ms) {
    struct timespec t;
    volatile unsigned long x = 0;
    for (;;) {
        for (int i = 0; i < 100000; i++)
            x += i;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
        if (t.tv_sec * 1000L + t.tv_nsec / 1000000L >= ms)
            break;
    }
}

// ---- 1. a thread's exit, tracer is the real parent -------------------------

#ifndef PTRACE_GETEVENTMSG
#define PTRACE_GETEVENTMSG 0x4201
#endif
#ifndef PTRACE_EVENT_CLONE
#define PTRACE_EVENT_CLONE 3
#endif

// strace -f ./threaded. P makes a thread that exits 7.
//
// how 0: P exits 0 once the tracer has reaped the thread.
// how 1: P exits 4 while the thread is still an unreaped zombie. The process
//        cannot be reaped before the thread, and the thread's status reads as
//        the process's exit code by then (Linux: SIGNAL_GROUP_EXIT).
// how 2: P's leader exits 5 on its own and the thread, the last one, exits 7
//        later. Since Linux 6.0 the last thread's code is the process's.
static void case_thread_exit(int how) {
    const char *name = how == 0 ? "thread exit" :
        how == 1 ? "process exits before its thread is reaped" : "thread outlives its leader";
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        while (!S->go)
            nap_ms(1);
        static pid_t tid_word;
        pid_t tid = spawn_thread(how == 2 ? thread_nap_then_exit7 : thread_exit7, &tid_word);
        if (tid < 0)
            _exit(90);
        S->tid = tid;
        if (how == 2)
            syscall(SYS_exit, 5);       // the leader alone; the thread runs on
        S->thread_go = 1;
        wait_tid_gone(&tid_word);
        if (how == 0) {
            await(&S->t_reaped, 20000);
            _exit(0);
        }
        _exit(4);
    }
    if (ptrace(PTRACE_SEIZE, p, 0, (void *) (long) FOLLOW_OPTS) != 0) {
        check(name, "seize", errno, 0);
        kill(p, SIGKILL);
        return;
    }
    S->go = 1;

    if (how == 1) {
        // Keep everything running, but reap nothing, until the thread is a
        // zombie and P has exited.
        S->step = 1;
        pid_t tid = 0;
        int early = 0, thread_stopped = 0, thread_zombie = 0;
        for (int i = 0; i < 20000 && !thread_zombie; i++) {
            int st = 0;
            pid_t w = waitpid(p, &st, __WALL | WNOHANG);
            if (w == p && WIFSTOPPED(st)) {
                if (((st >> 16) & 0xff) == PTRACE_EVENT_CLONE) {
                    unsigned long msg = 0;
                    ptrace(PTRACE_GETEVENTMSG, p, 0, &msg);
                    tid = (pid_t) msg;
                }
                resume(w, st);
            } else if (w == p) {
                early = 1;
                break;
            }
            if (tid > 0 && !thread_stopped) {
                w = waitpid(tid, &st, __WALL | WNOHANG);
                if (w == tid && WIFSTOPPED(st)) {
                    thread_stopped = 1;
                    resume(w, st);
                }
            }
            if (tid > 0 && thread_stopped) {
                siginfo_t si;
                memset(&si, 0, sizeof si);
                if (waitid(P_PID, tid, &si, WEXITED | WNOWAIT | WNOHANG | __WALL) == 0 && si.si_pid == tid)
                    thread_zombie = 1;
            }
            nap_ms(1);
        }
        check(name, "process not reported before its thread", early, 0);
        check(name, "thread became a zombie", thread_zombie, 1);
        char state = '?';
        for (int i = 0; i < 5000 && (state = proc_state(p)) != 'Z'; i++)
            nap_ms(1);
        check(name, "process is a zombie in /proc", state, 'Z');
        int st = 0;
        pid_t w = waitpid(p, &st, __WALL | WNOHANG);
        check(name, "process is not reapable while its thread is unreaped", w, 0);
        S->step = 2;
        w = wait_blocking(-1, &st, __WALL, 10);
        check(name, "first report is the thread", w, tid);
        check(name, "  ...with the process's exit code", st, EXITED(4));
        w = wait_blocking(-1, &st, __WALL, 10);
        check(name, "second report is the process", w, p);
        check(name, "  ...status", st, EXITED(4));
        errno = 0;
        w = waitpid(-1, &st, __WALL | WNOHANG);
        check(name, "then nothing is left: wait", w, -1);
        check(name, "then nothing is left: errno", errno, ECHILD);
        return;
    }

    pid_t exits[8];
    int statuses[8];
    int nexits = 0;
    int got_p = 0;
    for (int i = 0; i < 64 && !got_p; i++) {
        int st = 0;
        pid_t w = wait_blocking(-1, &st, __WALL, 10);
        if (w < 0) {
            check(name, "wait during the run (EINTR here is a hang)", errno, 0);
            break;
        }
        if (WIFSTOPPED(st)) {
            resume(w, st);
            continue;
        }
        if (nexits < 8) {
            exits[nexits] = w;
            statuses[nexits] = st;
            nexits++;
        }
        if (w != p)
            S->t_reaped = 1;
        if (w == p)
            got_p = 1;
    }
    int thread_at = -1, p_at = -1;
    for (int i = 0; i < nexits; i++) {
        note(name, "exit reported for", exits[i]);
        if (exits[i] == S->tid && thread_at < 0)
            thread_at = i;
        if (exits[i] == p && p_at < 0)
            p_at = i;
    }
    check(name, "thread's exit is reported", thread_at >= 0, 1);
    if (thread_at >= 0)
        check(name, "thread's exit status", statuses[thread_at], EXITED(7));
    check(name, "process's exit is reported", p_at >= 0, 1);
    if (p_at >= 0)
        check(name, "process's exit status", statuses[p_at], EXITED(how == 2 ? 7 : 0));
    check(name, "thread is reported before its process", thread_at >= 0 && thread_at < p_at, 1);
    check(name, "exits reported", nexits, 2);
    int st;
    errno = 0;
    pid_t w = waitpid(-1, &st, __WALL | WNOHANG);
    check(name, "then nothing is left: wait", w, -1);
    check(name, "then nothing is left: errno", errno, ECHILD);
}

// ---- 2. a fork child's exit, tracer is NOT its parent ----------------------

static void p_on_sigchld(int sig, siginfo_t *si, void *uc) {
    (void) sig; (void) uc;
    if (S->p_sigchld_count++ == 0) {
        S->p_si_code = si->si_code;
        S->p_si_pid = si->si_pid;
        S->p_si_status = si->si_status;
    }
}

// P forks G, G burns CPU and exits 3. M traces both (G by auto-attach). P's
// wait must not see G until M has reaped it, and then must. strace -f sh -c
// 'a | b'. by_any: P waits with -1 instead of G's pid.
static void case_fork_child(int by_any) {
    const char *name = by_any ? "fork child, parent waits -1" : "fork child, parent waits pid";
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = p_on_sigchld;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGCHLD, &sa, NULL);
        while (!S->go)
            nap_ms(1);
        pid_t g = fork();
        if (g == 0) {
            burn_cpu_ms(250);
            _exit(3);
        }
        S->g = g;
        int pidfd = (int) syscall(SYS_pidfd_open, g, 0);
        await(&S->g_zombie, 20000);
        S->p_pidfd_revents = -1;
        if (pidfd >= 0) {
            struct pollfd pfd = { .fd = pidfd, .events = POLLIN };
            S->p_pidfd_revents = poll(&pfd, 1, 0) == 1 ? pfd.revents : 0;
        }
        int st = 0;
        errno = 0;
        S->p_nohang_pid_ret = waitpid(g, &st, WNOHANG);
        S->p_nohang_pid_err = errno;
        errno = 0;
        S->p_nohang_any_ret = waitpid(-1, &st, WNOHANG);
        S->p_nohang_any_err = errno;
        S->p_sigchld_at_probe = S->p_sigchld_count;
        S->p_probed = 1;
        pid_t w;
        do {
            errno = 0;
            w = waitpid(by_any ? -1 : g, &st, 0);
        } while (w < 0 && errno == EINTR);
        S->p_wait_saw_reaped = S->t_reaped;
        S->p_wait_ret = w;
        S->p_wait_err = errno;
        S->p_wait_status = st;
        for (int i = 0; i < 2000 && S->p_sigchld_count == 0; i++)
            nap_ms(1);
        S->p_cru_us = cru_us();
        _exit(0);
    }
    if (ptrace(PTRACE_SEIZE, p, 0, (void *) (long) FOLLOW_OPTS) != 0) {
        check(name, "seize", errno, 0);
        kill(p, SIGKILL);
        return;
    }
    S->go = 1;

    // Run everything until G has exited, without reaping G.
    S->step = 1;
    int g_stopped_once = 0;
    siginfo_t si;
    memset(&si, 0, sizeof si);
    for (int i = 0; i < 20000; i++) {
        int st = 0;
        pid_t w = waitpid(p, &st, __WALL | WNOHANG);
        if (w == p && WIFSTOPPED(st))
            resume(w, st);
        pid_t g = S->g;
        if (g > 0 && !g_stopped_once) {
            w = waitpid(g, &st, __WALL | WNOHANG);
            if (w == g && WIFSTOPPED(st)) {
                g_stopped_once = 1;
                resume(w, st);
            }
        }
        if (g > 0 && g_stopped_once) {
            memset(&si, 0, sizeof si);
            if (waitid(P_PID, g, &si, WEXITED | WNOWAIT | WNOHANG | __WALL) == 0 && si.si_pid == g)
                break;
        }
        nap_ms(1);
    }
    pid_t g = S->g;
    check(name, "tracer sees the child's exit without reaping it", si.si_pid, g);
    check(name, "  ...as CLD_EXITED", si.si_code, CLD_EXITED);
    check(name, "  ...with status 3", si.si_status, 3);
    S->g_zombie = 1;

    // P probes while G belongs to us; keep P running meanwhile.
    S->step = 2;
    for (int i = 0; i < 10000 && !S->p_probed; i++) {
        int st = 0;
        pid_t w = waitpid(p, &st, __WALL | WNOHANG);
        if (w == p && WIFSTOPPED(st))
            resume(w, st);
        nap_ms(1);
    }
    check(name, "parent probed", S->p_probed, 1);
    check(name, "parent's waitpid(child, WNOHANG) before the tracer reaps", S->p_nohang_pid_ret, 0);
    check(name, "parent's waitpid(-1, WNOHANG) before the tracer reaps", S->p_nohang_any_ret, 0);
    check(name, "parent has no SIGCHLD before the tracer reaps", S->p_sigchld_at_probe, 0);
    note(name, "parent's pidfd poll revents before the tracer reaps", S->p_pidfd_revents);
    if (S->p_pidfd_revents >= 0)
        check(name, "parent's pidfd is readable before the tracer reaps", (S->p_pidfd_revents & POLLIN) != 0, 1);

    // Let P settle into its blocking wait, keeping it running.
    for (int i = 0; i < 200; i++) {
        int st = 0;
        pid_t w = waitpid(p, &st, __WALL | WNOHANG);
        if (w == p && WIFSTOPPED(st))
            resume(w, st);
        nap_ms(1);
    }
    check(name, "parent's blocking wait has not returned early", S->p_wait_ret == 0 && S->p_wait_err == 0, 1);

    S->step = 3;
    long before = cru_us();
    struct rusage ru;
    memset(&ru, 0, sizeof ru);
    int st = 0;
    S->t_reaped = 1;
    pid_t w = wait4_blocking(g, &st, __WALL, &ru, 10);
    long after = cru_us();
    check(name, "tracer's wait4(child) returns it", w, g);
    check(name, "  ...with its exit status", st, EXITED(3));
    note(name, "tracer's wait4 rusage of the child, us", ru_us(&ru));
    check(name, "  ...and the child's rusage (>= 150ms)", ru_us(&ru) >= 150000, 1);
    note(name, "tracer's RUSAGE_CHILDREN delta across that reap, us", after - before);
    check(name, "tracer's RUSAGE_CHILDREN is not charged (< 50ms)", after - before < 50000, 1);
    errno = 0;
    w = waitpid(g, &st, __WALL | WNOHANG);
    int e = errno;
    check(name, "tracer's second wait for the child", w, -1);
    check(name, "  ...errno", e, ECHILD);

    // Now P gets G, and a SIGCHLD for it.
    S->step = 4;
    int p_status = -1;
    for (int i = 0; i < 64; i++) {
        st = 0;
        w = wait_blocking(-1, &st, __WALL, 10);
        if (w < 0) {
            check(name, "wait for the parent (EINTR here is a hang)", errno, 0);
            break;
        }
        if (WIFSTOPPED(st)) {
            resume(w, st);
            continue;
        }
        if (w == p) {
            p_status = st;
            break;
        }
        note(name, "unexpected exit reported for", w);
    }
    check(name, "parent exits 0", p_status, 0);
    check(name, "parent's blocking wait returned the child", S->p_wait_ret, g);
    check(name, "  ...with its status", S->p_wait_status, EXITED(3));
    check(name, "  ...only after the tracer reaped it", S->p_wait_saw_reaped, 1);
    check(name, "parent got SIGCHLD", S->p_sigchld_count >= 1, 1);
    check(name, "  ...si_code", S->p_si_code, CLD_EXITED);
    check(name, "  ...si_pid", S->p_si_pid, g);
    check(name, "  ...si_status", S->p_si_status, 3);
    note(name, "parent's RUSAGE_CHILDREN after reaping, us", S->p_cru_us);
    check(name, "parent's RUSAGE_CHILDREN is charged (>= 150ms)", S->p_cru_us >= 150000, 1);
}

// ---- 3. real parent ignores SIGCHLD ----------------------------------------

// P ignores SIGCHLD and forks G; T (not P's relative) seizes G. G's exit is
// T's; once T reaps it -- or T dies holding it -- nobody is left to reap it,
// so it is released, and P's wait says ECHILD.
static void case_autoreap(int tracer_dies) {
    const char *name = tracer_dies ? "ignored SIGCHLD, tracer dies" : "ignored SIGCHLD, tracer reaps";
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        signal(SIGCHLD, SIG_IGN);
        pid_t g = fork();
        if (g == 0) {
            while (!S->go)
                nap_ms(1);
            _exit(3);
        }
        S->g = g;
        await(&S->g_zombie, 20000);
        int st;
        errno = 0;
        S->p_nohang_pid_ret = waitpid(g, &st, WNOHANG);
        S->p_nohang_pid_err = errno;
        S->p_probed = 1;
        await(&S->t_done, 20000);
        // Released at once on Linux; allow a moment for AOK's side to settle.
        int ret = 0, err = 0;
        for (int i = 0; i < 1000; i++) {
            errno = 0;
            ret = waitpid(g, &st, WNOHANG);
            err = errno;
            if (ret != 0)
                break;
            nap_ms(1);
        }
        S->p_after_ret = ret;
        S->p_after_err = err;
        _exit(0);
    }
    for (int i = 0; i < 10000 && S->g == 0; i++)
        nap_ms(1);
    pid_t g = S->g;

    fflush(NULL);
    pid_t t = fork();
    if (t == 0) {
        errno = 0;
        S->t_seize_err = ptrace(PTRACE_SEIZE, g, 0, 0) == 0 ? 0 : errno;
        S->t_ready = 1;
        siginfo_t si;
        memset(&si, 0, sizeof si);
        S->t_saw_ret = waitid(P_PID, g, &si, WEXITED | WNOWAIT | __WALL);
        S->t_saw_err = errno;
        S->t_si_pid = si.si_pid;
        S->t_si_code = si.si_code;
        S->t_si_status = si.si_status;
        S->t_saw = 1;
        await(&S->t_cmd, 20000);
        if (S->t_cmd == 3)
            _exit(0);
        int st = 0;
        errno = 0;
        S->t_ret = waitpid(g, &st, __WALL);
        S->t_err = errno;
        S->t_status = st;
        S->t_done = 1;
        await(&S->t_exit, 20000);
        _exit(0);
    }
    await(&S->t_ready, 10000);
    check(name, "tracer seizes the child", S->t_seize_err, 0);
    S->go = 1;
    S->step = 1;
    check(name, "tracer saw the exit", await(&S->t_saw, 10000), 1);
    check(name, "  ...waitid", S->t_saw_ret, 0);
    check(name, "  ...si_pid", S->t_si_pid, g);
    check(name, "  ...si_code", S->t_si_code, CLD_EXITED);
    check(name, "  ...si_status", S->t_si_status, 3);
    S->g_zombie = 1;
    S->step = 2;
    check(name, "parent probed", await(&S->p_probed, 10000), 1);
    check(name, "parent's waitpid(child, WNOHANG) while traced", S->p_nohang_pid_ret, 0);
    S->step = 3;
    int st;
    if (tracer_dies) {
        S->t_cmd = 3;
        pid_t w = wait_blocking(t, &st, 0, 10);
        check(name, "tracer exits", w, t);
        S->t_done = 1;
    } else {
        S->t_cmd = 1;
        check(name, "tracer reaped", await(&S->t_done, 10000), 1);
        check(name, "  ...tracer's waitpid", S->t_ret, g);
        check(name, "  ...status", S->t_status, EXITED(3));
    }
    S->step = 4;
    pid_t w = wait_blocking(p, &st, 0, 10);
    check(name, "parent exits", w, p);
    check(name, "  ...0", st, 0);
    check(name, "then the parent's waitpid(child)", S->p_after_ret, -1);
    check(name, "  ...errno", S->p_after_err, ECHILD);
    S->t_exit = 1;
    if (!tracer_dies)
        wait_blocking(t, &st, 0, 10);
}

// ---- 4. tracer is a stranger: reaps by pid, by -1, or dies -----------------

static volatile int m_si_pids[16];
static volatile int m_si_codes[16];
static volatile int m_si_statuses[16];
static volatile int m_si_count;

static void m_on_sigchld(int sig, siginfo_t *si, void *uc) {
    (void) sig; (void) uc;
    int i = m_si_count;
    if (i < 16) {
        m_si_pids[i] = si->si_pid;
        m_si_codes[i] = si->si_code;
        m_si_statuses[i] = si->si_status;
        m_si_count = i + 1;
    }
}

static int m_got_sigchld_for(pid_t pid, int code, int status) {
    for (int i = 0; i < m_si_count; i++)
        if (m_si_pids[i] == pid && m_si_codes[i] == code && m_si_statuses[i] == status)
            return 1;
    return 0;
}

// how: 1 = tracer waits by pid, 2 = tracer waits -1, 3 = tracer exits holding it.
static void case_stranger(int how) {
    const char *name = how == 1 ? "stranger tracer reaps by pid" :
        how == 2 ? "stranger tracer reaps -1" : "stranger tracer dies";
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = m_on_sigchld;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);

    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        while (!S->go)
            nap_ms(1);
        _exit(5);
    }
    fflush(NULL);
    pid_t t = fork();
    if (t == 0) {
        errno = 0;
        S->t_seize_err = ptrace(PTRACE_SEIZE, p, 0, 0) == 0 ? 0 : errno;
        S->t_ready = 1;
        siginfo_t si;
        memset(&si, 0, sizeof si);
        S->t_saw_ret = waitid(P_PID, p, &si, WEXITED | WNOWAIT | __WALL);
        S->t_saw_err = errno;
        S->t_si_pid = si.si_pid;
        S->t_si_code = si.si_code;
        S->t_si_status = si.si_status;
        S->t_saw = 1;
        await(&S->t_cmd, 20000);
        if (S->t_cmd == 3)
            _exit(0);
        int st = 0;
        errno = 0;
        S->t_ret = waitpid(S->t_cmd == 1 ? p : -1, &st, __WALL);
        S->t_err = errno;
        S->t_status = st;
        errno = 0;
        S->t_after_ret = waitpid(-1, &st, __WALL | WNOHANG);
        S->t_after_err = errno;
        S->t_done = 1;
        await(&S->t_exit, 20000);
        _exit(0);
    }
    await(&S->t_ready, 10000);
    check(name, "tracer seizes", S->t_seize_err, 0);
    S->go = 1;
    S->step = 1;
    check(name, "tracer saw the exit", await(&S->t_saw, 10000), 1);
    check(name, "  ...waitid", S->t_saw_ret, 0);
    check(name, "  ...si_pid", S->t_si_pid, p);
    check(name, "  ...si_code", S->t_si_code, CLD_EXITED);
    check(name, "  ...si_status", S->t_si_status, 5);
    nap_ms(100);
    int st = 0;
    errno = 0;
    pid_t w = waitpid(p, &st, WNOHANG);
    check(name, "parent's waitpid(WNOHANG) while traced", w, 0);
    check(name, "parent has no SIGCHLD for it yet", m_got_sigchld_for(p, CLD_EXITED, 5), 0);
    S->step = 2;
    if (how == 3) {
        S->t_cmd = 3;
        w = wait_blocking(t, &st, 0, 10);
        check(name, "tracer exits", w, t);
    } else {
        S->t_cmd = how;
        check(name, "tracer reaped", await(&S->t_done, 10000), 1);
        check(name, "  ...tracer's waitpid", S->t_ret, p);
        check(name, "  ...status", S->t_status, EXITED(5));
        check(name, "  ...then tracer's wait(-1, WNOHANG)", S->t_after_ret, -1);
        check(name, "  ...errno", S->t_after_err, ECHILD);
        for (int i = 0; i < 2000 && !m_got_sigchld_for(p, CLD_EXITED, 5); i++)
            nap_ms(1);
        check(name, "parent got SIGCHLD for it after the tracer reaped", m_got_sigchld_for(p, CLD_EXITED, 5), 1);
    }
    S->step = 3;
    w = wait_blocking(p, &st, 0, 10);
    check(name, "parent's waitpid returns it", w, p);
    check(name, "  ...status", st, EXITED(5));
    S->t_exit = 1;
    if (how != 3)
        wait_blocking(t, &st, 0, 10);
    signal(SIGCHLD, SIG_DFL);
}

// ---- 5. only a thread is traced; its process exits around the zombie -------

static void case_thread_only(int tracer_dies) {
    const char *name = tracer_dies ? "traced thread, tracer dies" : "traced thread, tracer reaps";
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        static pid_t tid_word;
        pid_t tid = spawn_thread(thread_exit7, &tid_word);
        if (tid < 0)
            _exit(90);
        S->tid = tid;
        wait_tid_gone(&tid_word);
        await(&S->t_saw, 20000);
        S->p_exiting = 1;
        _exit(4);
    }
    for (int i = 0; i < 10000 && (S->tid == 0 || !S->thread_started); i++)
        nap_ms(1);
    pid_t tid = S->tid;
    fflush(NULL);
    pid_t t = fork();
    if (t == 0) {
        errno = 0;
        S->t_seize_err = ptrace(PTRACE_SEIZE, tid, 0, 0) == 0 ? 0 : errno;
        S->t_ready = 1;
        siginfo_t si;
        memset(&si, 0, sizeof si);
        S->t_saw_ret = waitid(P_PID, tid, &si, WEXITED | WNOWAIT | __WALL);
        S->t_saw_err = errno;
        S->t_si_pid = si.si_pid;
        S->t_si_code = si.si_code;
        S->t_si_status = si.si_status;
        S->t_saw = 1;
        await(&S->t_cmd, 20000);
        if (S->t_cmd == 3)
            _exit(0);
        int st = 0;
        errno = 0;
        S->t_ret = waitpid(tid, &st, __WALL);
        S->t_err = errno;
        S->t_status = st;
        S->t_done = 1;
        await(&S->t_exit, 20000);
        _exit(0);
    }
    await(&S->t_ready, 10000);
    check(name, "tracer seizes the thread", S->t_seize_err, 0);
    S->thread_go = 1;
    S->step = 1;
    check(name, "tracer saw the thread's exit", await(&S->t_saw, 10000), 1);
    check(name, "  ...si_pid", S->t_si_pid, tid);
    check(name, "  ...si_code", S->t_si_code, CLD_EXITED);
    check(name, "  ...si_status", S->t_si_status, 7);
    check(name, "process exits", await(&S->p_exiting, 10000), 1);
    nap_ms(300);
    int st = 0;
    errno = 0;
    pid_t w = waitpid(p, &st, WNOHANG);
    note(name, "/proc state of the exited process", proc_state(p));
    check(name, "parent's waitpid(WNOHANG) while the thread is unreaped", w, 0);
    S->step = 2;
    if (tracer_dies) {
        S->t_cmd = 3;
        w = wait_blocking(t, &st, 0, 10);
        check(name, "tracer exits", w, t);
    } else {
        S->t_cmd = 1;
        check(name, "tracer reaped", await(&S->t_done, 10000), 1);
        check(name, "  ...tracer's waitpid", S->t_ret, tid);
        // The process exited 4 in the meantime, and that is what a zombie
        // thread reports from then on.
        check(name, "  ...status is the process's exit code", S->t_status, EXITED(4));
    }
    S->step = 3;
    w = wait_blocking(p, &st, 0, 10);
    check(name, "parent's waitpid returns the process", w, p);
    check(name, "  ...status", st, EXITED(4));
    S->t_exit = 1;
    if (!tracer_dies)
        wait_blocking(t, &st, 0, 10);
}

// ---- 6. exec from a thread while a sibling runs ----------------------------

static char *const exec_argv[] = { "/proc/self/exe", EXEC_TARGET_ARG, NULL };

static int thread_sleeper(void *arg) {
    (void) arg;
    for (;;)
        nap_ms(50);
    return 0;
}

static int thread_execer(void *arg) {
    (void) arg;
    nap_ms(100);
    syscall(SYS_execve, exec_argv[0], exec_argv, NULL);
    return 98;
}

static void case_thread_exec(int unused) {
    (void) unused;
    const char *name = "exec from a thread";
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        while (!S->go)
            nap_ms(1);
        static pid_t tw1, tw2;
        S->tid = spawn_thread(thread_sleeper, &tw1);
        S->tid2 = spawn_thread(thread_execer, &tw2);
        for (;;)
            nap_ms(50);
    }
    if (ptrace(PTRACE_SEIZE, p, 0, (void *) (long) (FOLLOW_OPTS | PTRACE_O_TRACEEXEC)) != 0) {
        check(name, "seize", errno, 0);
        kill(p, SIGKILL);
        return;
    }
    S->go = 1;
    pid_t exits[8];
    int statuses[8];
    int nexits = 0, execs = 0, exec_pid = 0, got_p = 0;
    for (int i = 0; i < 64 && !got_p; i++) {
        int st = 0;
        pid_t w = wait_blocking(-1, &st, __WALL, 10);
        if (w < 0) {
            check(name, "wait (EINTR here is a hang)", errno, 0);
            break;
        }
        if (WIFSTOPPED(st)) {
            if (((st >> 16) & 0xff) == PTRACE_EVENT_EXEC) {
                execs++;
                exec_pid = w;
            }
            resume(w, st);
            continue;
        }
        if (nexits < 8) {
            exits[nexits] = w;
            statuses[nexits] = st;
            nexits++;
        }
        if (w == p)
            got_p = 1;
    }
    check(name, "one exec event", execs, 1);
    check(name, "  ...under the process's pid", exec_pid, p);
    int sleeper_at = -1, execer_at = -1, p_at = -1;
    for (int i = 0; i < nexits; i++) {
        note(name, "exit reported for", exits[i]);
        note(name, "  ...status", statuses[i]);
        if (exits[i] == S->tid)
            sleeper_at = i;
        if (exits[i] == S->tid2)
            execer_at = i;
        if (exits[i] == p)
            p_at = i;
    }
    check(name, "the sibling's exit is reported", sleeper_at >= 0, 1);
    if (sleeper_at >= 0)
        check(name, "  ...status", statuses[sleeper_at], 0);
    check(name, "the exec'ing thread's old tid never exits", execer_at, -1);
    check(name, "the process exits 0", p_at >= 0 ? statuses[p_at] : -1, 0);
    check(name, "exits reported", nexits, 2);
}

// ---- 7. attach refusals -----------------------------------------------------

static void case_attach_refusals(int unused) {
    (void) unused;
    const char *name = "attach refusals";
    fflush(NULL);
    pid_t p = fork();
    if (p == 0)
        _exit(0);
    siginfo_t si;
    memset(&si, 0, sizeof si);
    waitid(P_PID, p, &si, WEXITED | WNOWAIT);
    errno = 0;
    int r = ptrace(PTRACE_SEIZE, p, 0, 0);
    check(name, "SEIZE a zombie", r, -1);
    check(name, "  ...errno", errno, EPERM);
    errno = 0;
    r = ptrace(PTRACE_ATTACH, p, 0, 0);
    check(name, "ATTACH a zombie", r, -1);
    check(name, "  ...errno", errno, EPERM);
    int st;
    check(name, "the zombie is still the parent's", waitpid(p, &st, 0), p);

    static pid_t tw;
    pid_t tid = spawn_thread(thread_exit7, &tw);
    for (int i = 0; i < 5000 && !S->thread_started; i++)
        nap_ms(1);
    errno = 0;
    r = ptrace(PTRACE_SEIZE, tid, 0, 0);
    check(name, "SEIZE a thread of our own", r, -1);
    check(name, "  ...errno", errno, EPERM);
    S->thread_go = 1;
    wait_tid_gone(&tw);
}

// ---- 8. without __WALL ------------------------------------------------------

static void case_no_wall(int unused) {
    (void) unused;
    const char *name = "tracer waits without __WALL";
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        while (!S->go)
            nap_ms(1);
        static pid_t tid_word;
        pid_t tid = spawn_thread(thread_exit7, &tid_word);
        if (tid < 0)
            _exit(90);
        S->tid = tid;
        S->thread_go = 1;
        wait_tid_gone(&tid_word);
        await(&S->t_reaped, 20000);
        _exit(0);
    }
    if (ptrace(PTRACE_SEIZE, p, 0, (void *) (long) FOLLOW_OPTS) != 0) {
        check(name, "seize", errno, 0);
        kill(p, SIGKILL);
        return;
    }
    S->go = 1;
    int thread_stops = 0, thread_exit_status = -1, p_status = -1, hung = 0;
    for (int i = 0; i < 64; i++) {
        int st = 0;
        pid_t w = wait_blocking(-1, &st, 0, 3);
        if (w < 0) {
            hung = errno == EINTR;
            break;
        }
        if (WIFSTOPPED(st)) {
            if (w != p)
                thread_stops++;
            resume(w, st);
            continue;
        }
        if (w != p) {
            thread_exit_status = st;
            S->t_reaped = 1;
        }
        if (w == p) {
            p_status = st;
            break;
        }
    }
    if (hung) {
        // Finish the run with __WALL so nothing is left behind.
        for (int i = 0; i < 64; i++) {
            int st = 0;
            pid_t w = wait_blocking(-1, &st, __WALL, 10);
            if (w < 0)
                break;
            if (WIFSTOPPED(st)) {
                resume(w, st);
                continue;
            }
            if (w == p)
                break;
        }
    }
    check(name, "no hang", hung, 0);
    check(name, "thread's stops are reported", thread_stops >= 1, 1);
    check(name, "thread's exit is reported", thread_exit_status, EXITED(7));
    check(name, "process's exit", p_status, 0);
}

// ---- 9. untraced: the leader exits first ----------------------------------

static int thread_nap_then_exit_group3(void *arg) {
    (void) arg;
    S->thread_started = 1;
    nap_ms(300);
    syscall(SYS_exit_group, 3);
    return 0;
}

// The leader exits 5 alone; the last thread then calls exit_group(3), or just
// exits 7. The process's status is the group's code, or the last thread's
// (Linux 6.0+); the parent's SIGCHLD names the leader and carries the leader's
// own code, as Linux's do_notify_parent does.
static void case_untraced_leader_first(int exit_group3) {
    const char *name = exit_group3 ? "untraced, leader first, then exit_group" :
        "untraced, leader first, then the last thread";
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = m_on_sigchld;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        static pid_t tw;
        S->tid = spawn_thread(exit_group3 ? thread_nap_then_exit_group3 : thread_nap_then_exit7, &tw);
        syscall(SYS_exit, 5);
    }
    int st = 0;
    pid_t w = wait_blocking(p, &st, 0, 10);
    check(name, "wait returns the process", w, p);
    check(name, "  ...status", st, EXITED(exit_group3 ? 3 : 7));
    for (int i = 0; i < 2000 && m_si_count == 0; i++)
        nap_ms(1);
    check(name, "SIGCHLD arrived", m_si_count >= 1, 1);
    if (m_si_count > 0) {
        check(name, "SIGCHLD si_pid names the process", m_si_pids[0], p);
        check(name, "SIGCHLD si_code", m_si_codes[0], CLD_EXITED);
        check(name, "SIGCHLD si_status is the leader's own code", m_si_statuses[0], 5);
    }
    signal(SIGCHLD, SIG_DFL);
}

// ---- 10. an injected signal keeps its siginfo ------------------------------

static void inj_handler(int sig, siginfo_t *si, void *uc) {
    (void) uc;
    int i = S->inj_count;
    if (i < 4) {
        S->inj_sig[i] = sig;
        S->inj_code[i] = si->si_code;
        S->inj_pid[i] = si->si_pid;
        S->inj_count = i + 1;
    }
}

// A traced parent's SIGCHLD -- the one the tracer's reap hands over -- reaches
// it through a signal-delivery-stop, and strace re-injects it. Linux's
// ptrace_signal delivers the signal with the siginfo it was queued with, and a
// different signal with SI_USER from the tracer. AOK queued every injected
// signal with no siginfo at all, so a traced shell's SIGCHLD handler read
// si_pid 0.
static void case_injected_siginfo(int unused) {
    (void) unused;
    const char *name = "injected signal siginfo";
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = inj_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);
        sigaction(SIGUSR2, &sa, NULL);
        while (!S->go)
            nap_ms(1);
        kill(getpid(), SIGUSR1);        // the tracer passes it on
        kill(getpid(), SIGUSR1);        // the tracer swaps in SIGUSR2
        for (int i = 0; i < 5000 && S->inj_count < 2; i++)
            nap_ms(1);
        _exit(0);
    }
    if (ptrace(PTRACE_SEIZE, p, 0, 0) != 0) {
        check(name, "seize", errno, 0);
        kill(p, SIGKILL);
        return;
    }
    S->go = 1;
    int delivery_stops = 0, p_status = -1;
    for (int i = 0; i < 400; i++) {
        int st = 0;
        pid_t w = wait_blocking(p, &st, __WALL, 10);
        if (w != p) {
            check(name, "wait (EINTR here is a hang)", errno, 0);
            break;
        }
        if (!WIFSTOPPED(st)) {
            p_status = st;
            break;
        }
        int sig = WSTOPSIG(st);
        if (sig == SIGUSR1)
            ptrace(PTRACE_CONT, p, 0, (void *) (long) (delivery_stops++ == 0 ? SIGUSR1 : SIGUSR2));
        else
            resume(p, st);
    }
    check(name, "tracee exits 0", p_status, 0);
    check(name, "delivery stops", delivery_stops, 2);
    check(name, "handler runs", S->inj_count, 2);
    check(name, "same signal: signo", S->inj_sig[0], SIGUSR1);
    check(name, "same signal: si_code is the original", S->inj_code[0], SI_USER);
    check(name, "same signal: si_pid is the original sender", S->inj_pid[0], p);
    check(name, "changed signal: signo", S->inj_sig[1], SIGUSR2);
    check(name, "changed signal: si_code", S->inj_code[1], SI_USER);
    check(name, "changed signal: si_pid is the tracer", S->inj_pid[1], getpid());
}

// ---- 11. autoreap: a leader that went first, and the process group ----------

// A parent that ignores SIGCHLD gets no zombie. Two shapes AOK got wrong in
// the code this test covers:
//
// leader_first: the leader exits on its own and the last thread later. AOK
//   left that zombie for good -- releasing the leader from the last thread's
//   exit was not safe -- so the parent's wait found it where Linux says ECHILD.
// own_group: the child leads a process group of its own. AOK's autoreap freed
//   the group without taking it off the pid's process-group list, so the dead
//   group still answered kill(-pgid) and the freed struct stayed linked.
static void case_autoreap_shapes(int leader_first) {
    const char *name = leader_first ? "ignored SIGCHLD, leader exits first" :
        "ignored SIGCHLD, child leads its own group";
    signal(SIGCHLD, SIG_IGN);
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        if (leader_first) {
            static pid_t tw;
            S->tid = spawn_thread(thread_nap_then_exit7, &tw);
            syscall(SYS_exit, 5);
        }
        setpgid(0, 0);
        S->p_probed = 1;
        while (!S->go)
            nap_ms(1);
        _exit(3);
    }
    if (!leader_first) {
        await(&S->p_probed, 10000);
        errno = 0;
        check(name, "the child's group exists while it runs", kill(-p, 0), 0);
        S->go = 1;
    }
    int ret = 0, err = 0, st = 0;
    for (int i = 0; i < 3000; i++) {
        errno = 0;
        ret = waitpid(p, &st, WNOHANG);
        err = errno;
        if (ret != 0)
            break;
        nap_ms(1);
    }
    check(name, "the parent's waitpid finds nothing", ret, -1);
    check(name, "  ...errno", err, ECHILD);
    if (!leader_first) {
        errno = 0;
        int k = kill(-p, 0);
        int kerr = errno;
        check(name, "the child's group is gone: kill(-pgid, 0)", k, -1);
        check(name, "  ...errno", kerr, ESRCH);
    }
    signal(SIGCHLD, SIG_DFL);
}

// ---- runner ------------------------------------------------------------------

static void run_case(const char *name, void (*fn)(int), int arg, unsigned secs) {
    memset(S, 0, sizeof *S);
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        setpgid(0, 0);
        failures_total = 0;
        test_logf("%s\n", name);
        fn(arg);
        fflush(NULL);
        _exit(failures_total > 100 ? 100 : (int) failures_total);
    }
    long limit_ms = (long) test_watchdog_secs(secs) * 1000L;
    for (long waited = 0;; waited += 5) {
        int st = 0;
        pid_t w = waitpid(c, &st, WNOHANG);
        if (w == c) {
            if (WIFEXITED(st)) {
                failures_total += WEXITSTATUS(st);
            } else {
                printf("FAIL %s: case process died, status %#x (step %d)\n", name, st, S->step);
                failures_total++;
            }
            break;
        }
        if (waited >= limit_ms) {
            printf("FAIL %s: timed out at step %d\n", name, S->step);
            failures_total++;
            kill(-c, SIGKILL);
            kill(c, SIGKILL);
            waitpid(c, &st, 0);
            break;
        }
        nap_ms(5);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], EXEC_TARGET_ARG) == 0)
        return 0;
    test_init(argc, argv);
    S = mmap(NULL, sizeof *S, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (S == MAP_FAILED) {
        printf("ptrace_tracee_exit: FAIL mmap\n");
        return 1;
    }
    run_case("thread exit", case_thread_exit, 0, 30);
    run_case("process exits before its thread is reaped", case_thread_exit, 1, 30);
    run_case("thread outlives its leader", case_thread_exit, 2, 30);
    run_case("fork child, parent waits pid", case_fork_child, 0, 60);
    run_case("fork child, parent waits -1", case_fork_child, 1, 60);
    run_case("ignored SIGCHLD, tracer reaps", case_autoreap, 0, 30);
    run_case("ignored SIGCHLD, tracer dies", case_autoreap, 1, 30);
    run_case("stranger tracer reaps by pid", case_stranger, 1, 30);
    run_case("stranger tracer reaps -1", case_stranger, 2, 30);
    run_case("stranger tracer dies", case_stranger, 3, 30);
    run_case("traced thread, tracer reaps", case_thread_only, 0, 30);
    run_case("traced thread, tracer dies", case_thread_only, 1, 30);
    run_case("exec from a thread", case_thread_exec, 0, 30);
    run_case("attach refusals", case_attach_refusals, 0, 30);
    run_case("tracer waits without __WALL", case_no_wall, 0, 60);
    run_case("untraced, leader first, then the last thread", case_untraced_leader_first, 0, 30);
    run_case("untraced, leader first, then exit_group", case_untraced_leader_first, 1, 30);
    run_case("injected signal siginfo", case_injected_siginfo, 0, 30);
    run_case("ignored SIGCHLD, leader exits first", case_autoreap_shapes, 1, 30);
    run_case("ignored SIGCHLD, child leads its own group", case_autoreap_shapes, 0, 30);
    return finish_suite("ptrace_tracee_exit");
}
