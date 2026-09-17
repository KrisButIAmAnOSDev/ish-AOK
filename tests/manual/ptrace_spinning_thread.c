// ptrace_spinning_thread: attaching to a NON-LEADER thread that spins in user
// space with every signal blocked and makes no syscalls at all.
//
// This is the shape a debugger meets in a program that has wedged: GLib's
// "gmain" worker after g_error spun at 100% CPU with a full signal mask (see
// e88b6bac), and `strace -p` could not attach to it. Attaching hung, and so
// did strace's own exit -- its detach interrupts the target too, so the
// SIGTERM handler's cleanup waited for a stop that never came and only SIGKILL
// ended it.
//
// Cause, fixed by 87d3452f: PTRACE_INTERRUPT was a queued SIGTRAP, and
// receive_signals skips a blocked signal, so a tracee that had blocked
// everything was never stopped. `strace -p` seizes and interrupts, so this hit
// every attach and every detach. The interrupt is ptrace.trap_stop now, a flag
// the mask cannot hold. PTRACE_ATTACH's SIGSTOP was never affected: SIGSTOP is
// unblockable. Nothing here covered a spinning tracee with the whole mask set,
// or any tracee that was a thread rather than a whole process -- both halves of
// the reported shape.
//
// Every expectation was measured on Linux 6.12 (x86_64 glibc, -m64 and -m32)
// before this was written; the stops all arrive within about 2ms there.
//
// Bounded throughout: every wait polls with WNOHANG against a deadline and
// reports a timeout rather than hanging, and there is an alarm() over the lot.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "test_common.h"

#ifndef __WALL
#define __WALL 0x40000000
#endif
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif

// How long a stop may take before the test calls it lost. Linux answers in
// about 2ms; this is a hang detector, not a performance bound.
#define STOP_BUDGET_MS 3000

static volatile unsigned long spin_counter;
static volatile pid_t spin_tid;

static void nap(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

static void check(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-46s got=%ld want=%ld\n", label, got, want);
}

// The tracee thread: block every signal, then spin forever on a volatile with
// no syscall of any kind. pthread_sigmask cannot really mask SIGKILL or
// SIGSTOP, and a ptrace stop is not a signal the mask may hold either -- that
// is the whole point of the shape.
static void *spinner(void *unused) {
    (void) unused;
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, NULL);
    spin_tid = (pid_t) syscall(SYS_gettid);
    for (;;)
        spin_counter++;
    return NULL;
}

static pid_t child_leader, child_spin;
static int progress_fd = -1;    // read end: the leader's counter samples

// Start the tracee process: a leader parked in nanosleep plus the spinning
// thread. The leader reports the counter every 50ms so the test can tell,
// after a detach, that the thread really is running again.
static int start_child(void) {
    int id_pipe[2], prog_pipe[2];
    if (pipe(id_pipe) != 0)
        return -1;
    if (pipe(prog_pipe) != 0) {
        close(id_pipe[0]); close(id_pipe[1]);
        return -1;
    }
    fflush(NULL);
    pid_t c = fork();
    if (c < 0) {
        close(id_pipe[0]); close(id_pipe[1]);
        close(prog_pipe[0]); close(prog_pipe[1]);
        return -1;
    }
    if (c == 0) {
        close(id_pipe[0]);
        close(prog_pipe[0]);
        pthread_t th;
        if (pthread_create(&th, NULL, spinner, NULL) != 0)
            _exit(90);
        while (spin_tid == 0)
            nap(5);
        pid_t t = spin_tid;
        if (write(id_pipe[1], &t, sizeof(t)) != (ssize_t) sizeof(t))
            _exit(91);
        close(id_pipe[1]);
        // Never block on a full pipe: a sample the test did not drain is one
        // it did not want.
        fcntl(prog_pipe[1], F_SETFL, O_NONBLOCK);
        for (;;) {
            unsigned long v = spin_counter;
            ssize_t w = write(prog_pipe[1], &v, sizeof(v));
            (void) w;
            nap(50);
        }
        _exit(0);
    }
    close(id_pipe[1]);
    close(prog_pipe[1]);
    pid_t t = 0;
    ssize_t n = read(id_pipe[0], &t, sizeof(t));
    close(id_pipe[0]);
    if (n != (ssize_t) sizeof(t)) {
        close(prog_pipe[0]);
        kill(c, SIGKILL);
        while (waitpid(c, NULL, __WALL) < 0 && errno == EINTR)
            continue;
        return -1;
    }
    fcntl(prog_pipe[0], F_SETFL, O_NONBLOCK);
    child_leader = c;
    child_spin = t;
    progress_fd = prog_pipe[0];
    nap(150);   // let the thread get well into its loop
    return 0;
}

static void stop_child(void) {
    if (progress_fd >= 0) {
        close(progress_fd);
        progress_fd = -1;
    }
    if (child_leader == 0)
        return;
    kill(child_leader, SIGKILL);
    for (int i = 0; i < 400; i++) {
        if (waitpid(-1, NULL, WNOHANG | __WALL) > 0)
            continue;
        if (kill(child_leader, 0) != 0)
            break;
        nap(5);
    }
    while (waitpid(-1, NULL, WNOHANG | __WALL) > 0)
        continue;
    child_leader = child_spin = 0;
}

// Poll for a wait status from `target` (or -1 for any) with a deadline.
// Returns 1 and fills *status, 0 on timeout, -1 on a wait error.
static int wait_within(pid_t target, int *status, int budget_ms) {
    for (int waited = 0; waited <= budget_ms; waited += 2) {
        int st = 0;
        pid_t w = waitpid(target, &st, WNOHANG | __WALL);
        if (w > 0) {
            *status = st;
            return 1;
        }
        if (w < 0 && errno != EINTR)
            return -1;
        nap(2);
    }
    return 0;
}

// Expect a stop from `target` with this signal and ptrace event.
static void expect_stop(const char *label, pid_t target, int want_sig, int want_event) {
    int st = 0;
    int got = wait_within(target, &st, (int) test_watchdog_secs(STOP_BUDGET_MS / 1000) * 1000);
    if (got != 1) {
        // The failure the shape is about: the stop never arrives.
        failf(label, (uint64_t) got, 0, 0, 1, 0, 0);
        return;
    }
    check(label, WIFSTOPPED(st) ? WSTOPSIG(st) : -1, want_sig);
    if (want_event >= 0)
        check("  ... with the expected ptrace event", st >> 16, want_event);
}

// Did the spinning thread make progress after being let go? Two samples from
// the leader, at least one of which must be newer than the detach.
static int still_spinning(void) {
    unsigned long first = 0, last = 0;
    int seen = 0;
    // Drain whatever is queued, then look for growth over a fresh window.
    unsigned long v;
    while (read(progress_fd, &v, sizeof(v)) == (ssize_t) sizeof(v))
        continue;
    for (int i = 0; i < 100; i++) {
        if (read(progress_fd, &v, sizeof(v)) == (ssize_t) sizeof(v)) {
            if (seen == 0)
                first = v;
            last = v;
            if (++seen >= 3)
                break;
        }
        nap(20);
    }
    return seen >= 2 && last > first;
}

// PTRACE_ATTACH: its SIGSTOP is unblockable, so this half always worked. It is
// here because a debugger attaching to the spinning thread must also be able to
// read it, resume it, and let it go still running.
static void attach_case(int target_is_thread) {
    const char *who = target_is_thread ? "thread" : "leader";
    if (start_child() != 0) {
        printf("ptrace_spinning_thread: SKIP (could not start the tracee)\n");
        return;
    }
    pid_t T = target_is_thread ? child_spin : child_leader;
    test_logf("ATTACH the %s (leader=%d spinning thread=%d)\n", who, child_leader, child_spin);

    errno = 0;
    check("ATTACH rc", ptrace(PTRACE_ATTACH, T, 0, 0), 0);
    expect_stop("ATTACH stops the spinning thread", T, SIGSTOP, 0);

    // The stop is real: the tracee's memory can be read while it holds.
    errno = 0;
    long peeked = ptrace(PTRACE_PEEKDATA, T, (void *) &spin_counter, 0);
    check("PEEKDATA at the stop succeeds", errno, 0);
    check("the thread had been spinning", peeked > 0, 1);

    errno = 0;
    check("CONT rc", ptrace(PTRACE_CONT, T, 0, 0), 0);
    nap(150);

    // Detaching needs a stopped tracee; Linux answers ESRCH for a running one.
    errno = 0;
    long r = ptrace(PTRACE_DETACH, T, 0, 0);
    check("DETACH of a running tracee rc", r, -1);
    check("DETACH of a running tracee errno", r < 0 ? errno : 0, ESRCH);

    kill(T, SIGSTOP);
    expect_stop("kill(SIGSTOP) stops it again", T, SIGSTOP, 0);
    errno = 0;
    check("DETACH rc", ptrace(PTRACE_DETACH, T, 0, (void *) (long) SIGCONT), 0);
    nap(150);
    check("the tracee is alive after the detach", kill(child_leader, 0), 0);
    check("and its thread is spinning again", still_spinning(), 1);
    stop_child();
}

// PTRACE_SEIZE + PTRACE_INTERRUPT, which is what `strace -p` does to attach
// and again to detach. This is the half the full signal mask defeated.
static void seize_case(int target_is_thread) {
    const char *who = target_is_thread ? "thread" : "leader";
    if (start_child() != 0) {
        printf("ptrace_spinning_thread: SKIP (could not start the tracee)\n");
        return;
    }
    pid_t T = target_is_thread ? child_spin : child_leader;
    test_logf("SEIZE/INTERRUPT the %s (leader=%d spinning thread=%d)\n",
              who, child_leader, child_spin);

    errno = 0;
    check("SEIZE rc", ptrace(PTRACE_SEIZE, T, 0, 0), 0);
    errno = 0;
    check("INTERRUPT rc", ptrace(PTRACE_INTERRUPT, T, 0, 0), 0);
    expect_stop("INTERRUPT stops the spinning thread", T, SIGTRAP, PTRACE_EVENT_STOP);

    errno = 0;
    long peeked = ptrace(PTRACE_PEEKDATA, T, (void *) &spin_counter, 0);
    check("PEEKDATA at the stop succeeds", errno, 0);
    check("the thread had been spinning", peeked > 0, 1);

    // An interrupt sent while it is already stopped is kept, and taken once it
    // is resumed.
    errno = 0;
    check("INTERRUPT while stopped rc", ptrace(PTRACE_INTERRUPT, T, 0, 0), 0);
    errno = 0;
    check("CONT rc", ptrace(PTRACE_CONT, T, 0, 0), 0);
    expect_stop("the kept interrupt traps again", T, SIGTRAP, PTRACE_EVENT_STOP);

    // And one sent while it is running back in the loop.
    errno = 0;
    check("CONT rc (second)", ptrace(PTRACE_CONT, T, 0, 0), 0);
    nap(150);
    errno = 0;
    check("INTERRUPT while running rc", ptrace(PTRACE_INTERRUPT, T, 0, 0), 0);
    expect_stop("INTERRUPT stops it a second time", T, SIGTRAP, PTRACE_EVENT_STOP);

    errno = 0;
    check("DETACH rc", ptrace(PTRACE_DETACH, T, 0, 0), 0);
    nap(150);
    check("the tracee is alive after the detach", kill(child_leader, 0), 0);
    check("and its thread is spinning again", still_spinning(), 1);
    stop_child();
}

// What `strace -f -p` does: seize every thread of the process and interrupt
// each one, then let them all go.
static void both_threads_case(void) {
    if (start_child() != 0) {
        printf("ptrace_spinning_thread: SKIP (could not start the tracee)\n");
        return;
    }
    test_logf("SEIZE both threads (leader=%d spinning thread=%d)\n", child_leader, child_spin);

    errno = 0;
    check("SEIZE the leader rc", ptrace(PTRACE_SEIZE, child_leader, 0, 0), 0);
    errno = 0;
    check("SEIZE the spinning thread rc", ptrace(PTRACE_SEIZE, child_spin, 0, 0), 0);

    errno = 0;
    check("INTERRUPT the leader rc", ptrace(PTRACE_INTERRUPT, child_leader, 0, 0), 0);
    expect_stop("the leader stops", child_leader, SIGTRAP, PTRACE_EVENT_STOP);
    errno = 0;
    check("INTERRUPT the spinning thread rc", ptrace(PTRACE_INTERRUPT, child_spin, 0, 0), 0);
    expect_stop("the spinning thread stops", child_spin, SIGTRAP, PTRACE_EVENT_STOP);

    errno = 0;
    check("DETACH the leader rc", ptrace(PTRACE_DETACH, child_leader, 0, 0), 0);
    errno = 0;
    check("DETACH the spinning thread rc", ptrace(PTRACE_DETACH, child_spin, 0, 0), 0);
    nap(150);
    check("the tracee is alive after both detaches", kill(child_leader, 0), 0);
    check("and its thread is spinning again", still_spinning(), 1);
    stop_child();
}

// A killed tracee must die, stopped or running, and its tracer must be told.
static void kill_case(int stop_it_first) {
    if (start_child() != 0) {
        printf("ptrace_spinning_thread: SKIP (could not start the tracee)\n");
        return;
    }
    test_logf("SIGKILL a %s spinning tracee (leader=%d thread=%d)\n",
              stop_it_first ? "stopped" : "running", child_leader, child_spin);

    errno = 0;
    check("SEIZE the spinning thread rc", ptrace(PTRACE_SEIZE, child_spin, 0, 0), 0);
    if (stop_it_first) {
        errno = 0;
        check("INTERRUPT rc", ptrace(PTRACE_INTERRUPT, child_spin, 0, 0), 0);
        expect_stop("it stops", child_spin, SIGTRAP, PTRACE_EVENT_STOP);
    }

    kill(child_leader, SIGKILL);
    int st = 0;
    int got = wait_within(child_spin, &st, (int) test_watchdog_secs(STOP_BUDGET_MS / 1000) * 1000);
    check("the killed thread's exit reaches its tracer", got, 1);
    if (got == 1)
        check("and it died of SIGKILL", WIFSIGNALED(st) ? WTERMSIG(st) : -1, SIGKILL);
    got = wait_within(child_leader, &st, (int) test_watchdog_secs(STOP_BUDGET_MS / 1000) * 1000);
    check("the leader's exit is reported too", got, 1);
    stop_child();
}

// A tracer that dies holding the thread stopped must not leave it stopped.
static void tracer_death_case(void) {
    if (start_child() != 0) {
        printf("ptrace_spinning_thread: SKIP (could not start the tracee)\n");
        return;
    }
    test_logf("tracer dies holding the spinning thread (leader=%d thread=%d)\n",
              child_leader, child_spin);

    fflush(NULL);
    pid_t tracer = fork();
    if (tracer == 0) {
        if (ptrace(PTRACE_SEIZE, child_spin, 0, 0) != 0)
            _exit(81);
        if (ptrace(PTRACE_INTERRUPT, child_spin, 0, 0) != 0)
            _exit(82);
        int st = 0;
        for (int i = 0; i < 1500; i++) {
            if (waitpid(child_spin, &st, WNOHANG | __WALL) > 0)
                _exit(0);   // saw the stop, now exit without detaching
            nap(2);
        }
        _exit(83);          // the stop never came
    }
    if (tracer < 0) {
        printf("ptrace_spinning_thread: SKIP (could not fork a tracer)\n");
        stop_child();
        return;
    }
    int st = 0;
    int got = wait_within(tracer, &st, (int) test_watchdog_secs(10) * 1000);
    check("the tracer saw the stop and exited", got == 1 && WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
    check("the thread runs on once its tracer is gone", still_spinning(), 1);
    stop_child();
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(180));

    attach_case(1);
    attach_case(0);
    seize_case(1);
    seize_case(0);
    both_threads_case();
    kill_case(1);
    kill_case(0);
    tracer_death_case();

    return finish_suite("ptrace_spinning_thread");
}
