// A clean PTRACE_DETACH must leave the tracee running.
//
// Reported as "strace and gdb kill the process they attach to", and the
// carried diagnosis (a wait after attaching to a non-leader thread resolving
// to the wrong task) turned out to be wrong. Measured 2026-09-08 on an
// aarch64 guest against Devuan 6 / Linux 6.12, running `strace -p` against a
// two-thread process and then making strace detach:
//
//   strace -p <non-leader tid>, SIGINT to detach   AOK: target DEAD   Linux: alive
//   strace -p <leader>,         SIGINT to detach   AOK: target DEAD   Linux: alive
//   strace -f -p <leader>,      SIGINT to detach   AOK: target DEAD   Linux: alive
//   strace -p <non-leader tid>, tracer SIGKILLed   AOK: target alive  Linux: alive
//
// So it is not about non-leader threads (the leader dies too), and it is not
// the attach or the tracing: strace -c produced a correct 260-call profile with
// the target alive throughout.
//
// It looked like the explicit PTRACE_DETACH path, since the surviving row is
// the one that never detaches -- but the eight attach/detach cases below all
// PASSED against the unfixed kernel. That is what pointed at something strace
// does which a minimal detach does not, and the shell named it: the target died
// of status 133, which is 128 + SIGTRAP.
//
// PTRACE_INTERRUPT queued the tracee a REAL SIGTRAP. While the task is traced
// signal_delivery_stop intercepts it and reports the stop, so it worked; once
// the tracer detaches it is an ordinary SIGTRAP, whose default action is to
// terminate. strace interrupts a RUNNING tracee and then waits, and a program
// making a syscall every few milliseconds reaches a syscall-stop of its own
// first -- so strace saw a stop, detached, and left the trap queued. Linux has
// no such window: its interrupt is JOBCTL_TRAP_STOP, a flag, cleared by
// __ptrace_unlink. Fixed by tagging the trap with SI_PTRACE_INTERRUPT_ and
// discarding it on both detach paths; interrupt_then_detach_case below is the
// shape that reproduces it.
//
// Two more bugs found beside it, both covered here. waitpid(<tid>, __WALL) on
// a traced non-leader must report that thread's stop -- AOK hung, because
// do_wait's P_PID_ branch admits the thread and then tests
// task->group->leader for the stop, which is exactly the wait gdb's
// post-attach path performs. And a tracer that dies without detaching must
// still leave the tracee alive.
//
// Every expectation was measured on Linux 6.12 first, including the two
// different stop status words -- see WANT_ATTACH_STOP / WANT_SEIZE_STOP below,
// where getting that wrong is recorded rather than quietly corrected.
//
// NOTE for anyone extending this: install the alarm handler with sigaction()
// and no SA_RESTART. signal() sets SA_RESTART on both glibc and musl, so the
// alarm fires, the handler returns, and waitpid restarts -- which is how the
// first version of this probe hung for fifteen minutes instead of reporting
// a failure in three seconds.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
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

// The two stops differ, and the oracle is the reason this is not one constant.
//
// PTRACE_ATTACH stops the tracee with SIGSTOP, so the first wait reports a
// signal-delivery-stop: (SIGSTOP << 8) | 0x7f == 0x137f.
//
// PTRACE_SEIZE does not stop anything; PTRACE_INTERRUPT does, and it produces
// a PTRACE_EVENT_STOP rather than a signal: (PTRACE_EVENT_STOP << 16) |
// (SIGTRAP << 8) | 0x7f == 0x80057f. Measured on Linux 6.12 -- the first draft
// of this test expected 0x137f for both and the oracle refused all four SEIZE
// cases, which is the whole argument for running it there first.
#define WANT_ATTACH_STOP ((SIGSTOP << 8) | 0x7f)
#define WANT_SEIZE_STOP  ((128 << 16) | (SIGTRAP << 8) | 0x7f)

static int pfd[2];

static void nap(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

static void on_alarm(int sig) { (void) sig; }

// No SA_RESTART: the point of the alarm is to break a wait that hangs.
static void arm_alarm(unsigned secs) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);
    alarm(secs);
}

static void *worker(void *arg) {
    (void) arg;
    pid_t tid = (pid_t) syscall(SYS_gettid);
    if (write(pfd[1], &tid, sizeof tid) != (ssize_t) sizeof tid)
        _exit(9);
    for (;;)
        nap(20);
    return NULL;
}

// Fork a two-thread child and hand back its pid; *tid_out is the worker's.
static pid_t spawn_pair(pid_t *tid_out) {
    if (pipe(pfd) != 0)
        return -1;
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        close(pfd[0]);
        pthread_t t;
        if (pthread_create(&t, NULL, worker, NULL) != 0)
            _exit(9);
        for (;;)
            nap(50);
        _exit(0);
    }
    close(pfd[1]);
    if (c < 0) {
        close(pfd[0]);
        return -1;
    }
    pid_t tid = 0;
    ssize_t n = read(pfd[0], &tid, sizeof tid);
    close(pfd[0]);
    if (n != (ssize_t) sizeof tid) {
        kill(c, SIGKILL);
        waitpid(c, NULL, 0);
        return -1;
    }
    *tid_out = tid;
    return c;
}

static void reap(pid_t c) {
    int st;
    kill(c, SIGKILL);
    waitpid(c, &st, 0);
    while (waitpid(-1, &st, WNOHANG | __WALL) > 0)
        ;
}

// One attach/detach shape. `seize` picks SEIZE+INTERRUPT over ATTACH;
// `target_thread` aims at the worker rather than the leader; `wait_by_pid`
// waits on the target specifically rather than on -1.
static void one_case(const char *name, int seize, int target_thread,
                     int wait_by_pid) {
    pid_t tid = 0;
    pid_t c = spawn_pair(&tid);
    if (c < 0) {
        test_logf("  %-44s SKIP (fork/pipe failed)\n", name);
        return;
    }
    nap(200);
    pid_t tgt = target_thread ? tid : c;

    errno = 0;
    if (ptrace(seize ? PTRACE_SEIZE : PTRACE_ATTACH, tgt, 0, 0) != 0) {
        failf(name, (uint64_t) errno, 0, 0, 0, 0, 0);
        test_logf("  %-44s attach failed: %s\n", name, strerror(errno));
        reap(c);
        return;
    }
    // SEIZE does not stop the tracee; INTERRUPT is what produces the stop.
    if (seize && ptrace(PTRACE_INTERRUPT, tgt, 0, 0) != 0)
        test_logf("  %-44s (PTRACE_INTERRUPT: %s)\n", name, strerror(errno));

    arm_alarm(test_watchdog_secs(10));
    int st = 0;
    pid_t got = wait_by_pid ? waitpid(tgt, &st, __WALL)
                            : waitpid(-1, &st, __WALL);
    int werr = got < 0 ? errno : 0;
    alarm(0);

    if (got != tgt) {
        // EINTR here means the wait never returned on its own -- the bug this
        // test exists to catch, not a flake.
        failf(werr == EINTR ? "wait hung (EINTR from watchdog)" : name,
              (uint64_t) got, (uint64_t) werr, 0, (uint64_t) tgt, 0, 0);
        test_logf("  %-44s wait%s -> %d (%s), want %d\n", name,
                  wait_by_pid ? "(tgt)" : "(-1)", (int) got,
                  werr ? strerror(werr) : "-", (int) tgt);
    } else {
        int want = seize ? WANT_SEIZE_STOP : WANT_ATTACH_STOP;
        if (st != want) {
            failf(name, (uint64_t) st, 0, 0, (uint64_t) want, 0, 0);
            test_logf("  %-44s status=0x%06x want=0x%06x\n", name, st, want);
        }
    }

    nap(100);
    errno = 0;
    int derr = ptrace(PTRACE_DETACH, tgt, 0, 0) != 0 ? errno : 0;
    if (derr != 0) {
        failf(name, (uint64_t) derr, 0, 0, 0, 0, 0);
        test_logf("  %-44s PTRACE_DETACH: %s\n", name, strerror(derr));
    }

    // The whole point: the process must still be there afterwards. Give it a
    // beat, because the observed death was not synchronous with the detach.
    nap(500);
    int alive = kill(c, 0) == 0;
    if (!alive) {
        failf(name, 0, 0, 0, 1, 0, 0);
        test_logf("  %-44s leader %d DEAD after a clean detach\n", name,
                  (int) c);
    } else {
        test_logf("  %-44s ok (leader alive, status 0x%06x)\n", name, st);
    }
    reap(c);
}

// A tracer that dies without detaching must not take the tracee with it. This
// is the one shape that already works, and it is here so a fix for the others
// cannot regress it by making every detach fatal.
static void tracer_death_case(void) {
    const char *name = "tracer SIGKILLed, no detach";
    pid_t tid = 0;
    pid_t c = spawn_pair(&tid);
    if (c < 0) {
        test_logf("  %-44s SKIP (fork/pipe failed)\n", name);
        return;
    }
    nap(200);

    fflush(NULL);
    pid_t tracer = fork();
    if (tracer == 0) {
        if (ptrace(PTRACE_ATTACH, tid, 0, 0) != 0)
            _exit(1);
        int st;
        waitpid(tid, &st, __WALL);
        for (;;)
            nap(50);
        _exit(0);
    }
    if (tracer < 0) {
        test_logf("  %-44s SKIP (fork failed)\n", name);
        reap(c);
        return;
    }
    nap(600);
    kill(tracer, SIGKILL);
    waitpid(tracer, NULL, 0);
    nap(500);

    if (kill(c, 0) != 0) {
        failf(name, 0, 0, 0, 1, 0, 0);
        test_logf("  %-44s leader %d DEAD after the tracer was killed\n", name,
                  (int) c);
    } else {
        test_logf("  %-44s ok (leader alive)\n", name);
    }
    reap(c);
}

// The shape that actually reproduced the death, and the reason the eight cases
// above did not: strace interrupts a tracee that is *running*, and a program
// making a syscall every few milliseconds usually reaches a syscall-stop of its
// own first. The tracer sees the stop it was waiting for and detaches, leaving
// the interrupt's SIGTRAP unconsumed in the tracee's queue -- fatal the moment
// the task is no longer traced.
//
// Reproduced here by asking for the interrupt and then NOT consuming the stop
// it produces: resume under PTRACE_SYSCALL, let a syscall-stop arrive, request
// PTRACE_INTERRUPT, take one more syscall-stop, and detach from that.
static void interrupt_then_detach_case(void) {
    const char *name = "INTERRUPT unconsumed, then DETACH";
    pid_t tid = 0;
    pid_t c = spawn_pair(&tid);
    if (c < 0) {
        test_logf("  %-44s SKIP (fork/pipe failed)\n", name);
        return;
    }
    nap(200);

    if (ptrace(PTRACE_SEIZE, tid, 0, 0) != 0) {
        failf(name, (uint64_t) errno, 0, 0, 0, 0, 0);
        reap(c);
        return;
    }
    if (ptrace(PTRACE_INTERRUPT, tid, 0, 0) != 0) {
        failf(name, (uint64_t) errno, 0, 0, 0, 0, 0);
        reap(c);
        return;
    }
    arm_alarm(test_watchdog_secs(10));
    int st = 0;
    if (waitpid(-1, &st, __WALL) < 0) {
        alarm(0);
        failf(name, (uint64_t) errno, 0, 0, 0, 0, 0);
        reap(c);
        return;
    }
    // TRACESYSGOOD, as strace sets, then run syscall stops like strace does.
    ptrace(PTRACE_SETOPTIONS, tid, 0, (void *) 1L);
    for (int i = 0; i < 6; i++) {
        if (ptrace(PTRACE_SYSCALL, tid, 0, 0) != 0)
            break;
        if (waitpid(-1, &st, __WALL) < 0)
            break;
    }
    // Ask for an interrupt while it is running, then let its own syscall-stop
    // be what we see -- so the interrupt's trap is still queued at detach.
    if (ptrace(PTRACE_SYSCALL, tid, 0, 0) == 0) {
        ptrace(PTRACE_INTERRUPT, tid, 0, 0);
        waitpid(-1, &st, __WALL);
    }
    alarm(0);
    if (ptrace(PTRACE_DETACH, tid, 0, 0) != 0)
        test_logf("  %-44s PTRACE_DETACH: %s\n", name, strerror(errno));

    nap(700);
    if (kill(c, 0) != 0) {
        failf(name, 0, 0, 0, 1, 0, 0);
        test_logf("  %-44s leader %d DEAD -- an unconsumed interrupt trap "
                  "outlived the detach\n", name, (int) c);
    } else {
        test_logf("  %-44s ok (leader alive)\n", name);
    }
    reap(c);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    // Generous: eight cases, each with its own sub-second naps, and the
    // per-wait watchdog above is what actually bounds a hang.
    alarm(test_watchdog_secs(180));

    test_logf("ptrace_detach_survives: a clean detach must leave the target running\n");

    //         name                                     seize thread by_pid
    one_case("ATTACH leader,      wait(-1),  DETACH",       0,     0,     0);
    one_case("ATTACH thread,      wait(-1),  DETACH",       0,     1,     0);
    one_case("ATTACH leader,      wait(pid), DETACH",       0,     0,     1);
    one_case("ATTACH thread,      wait(tid), DETACH",       0,     1,     1);
    one_case("SEIZE+INT leader,   wait(-1),  DETACH",       1,     0,     0);
    one_case("SEIZE+INT thread,   wait(-1),  DETACH",       1,     1,     0);
    one_case("SEIZE+INT leader,   wait(pid), DETACH",       1,     0,     1);
    one_case("SEIZE+INT thread,   wait(tid), DETACH",       1,     1,     1);
    interrupt_then_detach_case();
    tracer_death_case();

    return finish_suite("ptrace_detach_survives");
}
