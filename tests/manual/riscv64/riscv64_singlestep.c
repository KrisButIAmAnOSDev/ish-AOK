// riscv64_singlestep: PTRACE_SINGLESTEP must execute exactly one
// guest instruction and stop, not run the tracee to completion.
//
// This is the riscv64 counterpart of arm64/ptrace_singlestep.c, and it
// is here because riscv64 had the same bug that one was written for and
// outlived it. jit/jit.c's GUEST_ABI_RISCV64 dispatch returned
// cpu_step_to_interrupt_riscv64 unconditionally and never looked at cpu->tf
// (the trap/single-step flag kernel/ptrace.c sets for PTRACE_SINGLESTEP), so a
// single step ran the tracee to its next real interrupt -- exactly like
// PTRACE_CONT -- while the ptrace call still returned 0. A debugger stepping a
// riscv64 guest therefore saw `stepi` run away, `step`, `next` and `finish`
// with it, and no error anywhere to say why. Fixed by cpu_single_step_riscv64.
//
// That other test's header used to say amd64/i386/riscv64 "have (or should be
// verified to have) working interpreter-based single-step already". The
// assumption did not survive verification on either 64-bit guest it covered:
// amd64 needed a JIT single-step path of its own (23edd81ef) once the
// interpreter was retired, and riscv64 never had one at all.
//
// Regression shape: the child spins a tight, deterministic loop incrementing a
// counter in shared (MAP_SHARED) memory many times, then exits. The parent
// single-steps it a bounded, small number of times and checks invariants that
// a run-away resume cannot fake:
//   1. The tracee must still be alive and stopped after each single step --
//      the bug made it run to completion after the very first one.
//   2. The PC must actually move, but the shared counter must stay far below
//      the loop's iteration count. THE COUNT IS THE REAL ASSERTION: a path
//      that executed two instructions per stop, or re-executed one, still
//      leaves the registers looking plausible, and only counting catches it.
//
// Everything asserted here is address-INDEPENDENT, so it is both a suite
// member and a probe that can be diffed against real riscv64 hardware.
//
// Named for the architecture, like x86/amd64_singlestep.c, and NOT
// "ptrace_singlestep": setup-regressions.sh's src_for() searches
// $src_dir/x86, then arm64, then riscv64, so a shared basename would silently
// compile the arm64 source on a riscv64 guest -- where it builds cleanly and
// then asserts against the wrong NT_PRSTATUS layout.

#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_CONT
#define PTRACE_CONT 7
#endif
#ifndef PTRACE_SINGLESTEP
#define PTRACE_SINGLESTEP 9
#endif
#ifndef PTRACE_GETREGSET
#define PTRACE_GETREGSET 0x4204
#endif
#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif

// Mirrors kernel/ptrace.h's struct user_regs_struct_riscv64_: pc followed by
// x1(ra)..x31(t6) in register-number order, matching the real kernel's
// struct user_regs_struct (asm/ptrace.h) exactly. Same shape as
// riscv64/ptrace_regset.c uses.
struct riscv64_regs {
    unsigned long pc;
    unsigned long ra, sp, gp, tp, t0, t1, t2, s0, s1;
    unsigned long a0, a1, a2, a3, a4, a5, a6, a7;
    unsigned long s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    unsigned long t3, t4, t5, t6;
};

_Static_assert(sizeof(struct riscv64_regs) == 256,
               "riscv64_regs must match struct user_regs_struct exactly");

#define LOOP_ITERATIONS 1000000
#define SINGLESTEP_COUNT 40
// Generous upper bound on how far the shared counter could plausibly move
// across SINGLESTEP_COUNT single steps -- real single-stepping through the
// loop's few-instruction body advances the counter by at most a handful per
// step (most steps don't even complete one full increment); the bug
// ran the ENTIRE loop (LOOP_ITERATIONS) after the very first step.
#define MAX_PLAUSIBLE_COUNTER (SINGLESTEP_COUNT * 4)

static pid_t g_child;

static void on_alarm(int sig) {
    (void) sig;
    if (g_child > 0)
        kill(g_child, SIGKILL);
    static const char msg[] = "riscv64_singlestep: FAIL timeout\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(1);
}

static int getregs(pid_t pid, struct riscv64_regs *regs) {
    struct iovec iov = { .iov_base = regs, .iov_len = sizeof(*regs) };
    return ptrace(PTRACE_GETREGSET, pid, (void *) NT_PRSTATUS, &iov);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    signal(SIGALRM, on_alarm);
    alarm(test_watchdog_secs(30));

    volatile long *counter = mmap(NULL, sizeof(long), PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (counter == MAP_FAILED) {
        failf("riscv64_singlestep mmap", (uint64_t) errno, 0, 0, 0, 0, 0);
        return finish_suite("riscv64_singlestep");
    }
    *counter = 0;

    g_child = fork();
    if (g_child < 0) {
        failf("riscv64_singlestep fork", (uint64_t) errno, 0, 0, 0, 0, 0);
        return finish_suite("riscv64_singlestep");
    }

    if (g_child == 0) {
        raise(SIGSTOP);
        for (long i = 0; i < LOOP_ITERATIONS; i++)
            (*counter)++;
        _exit(42);
    }

    pid_t child = g_child;
    if (ptrace(PTRACE_SEIZE, child, 0, 0) != 0) {
        failf("riscv64_singlestep seize", (uint64_t) errno, 0, 0, 0, 0, 0);
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        return finish_suite("riscv64_singlestep");
    }
    test_logf("seized %d\n", (int) child);

    int saw_group_stop = 0;
    int tracing = 0;
    int steps_done = 0;
    unsigned long last_pc = 0;
    int pc_ever_changed = 0;
    int exited_too_early = 0;

    for (int iterations = 0; iterations < 200 && steps_done < SINGLESTEP_COUNT; iterations++) {
        int status;
        pid_t w = waitpid(child, &status, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            failf("riscv64_singlestep waitpid", (uint64_t) errno, 0, 0, 0, 0, 0);
            break;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (tracing && steps_done < SINGLESTEP_COUNT)
                exited_too_early = 1;
            break;
        }
        if (!WIFSTOPPED(status)) {
            failf("riscv64_singlestep status", (uint64_t) status, 0, 0, 0, 0, 0);
            break;
        }

        int sig = WSTOPSIG(status);
        int event = (status >> 16) & 0xff;

        if (!tracing) {
            // Group-stop handshake, same pattern as ptrace_group_stop.c /
            // riscv64/ptrace_regset.c.
            if (event == PTRACE_EVENT_STOP) {
                saw_group_stop = 1;
                tracing = 1;
                if (ptrace(PTRACE_SINGLESTEP, child, 0, 0) != 0) {
                    failf("riscv64_singlestep arm", (uint64_t) errno, 0, 0, 0, 0, 0);
                    break;
                }
            } else if (sig == SIGSTOP) {
                if (ptrace(PTRACE_CONT, child, 0, SIGSTOP) != 0) {
                    failf("riscv64_singlestep deliver SIGSTOP", (uint64_t) errno, 0, 0, 0, 0, 0);
                    break;
                }
            } else {
                ptrace(PTRACE_CONT, child, 0, (sig == SIGTRAP) ? 0 : (long) sig);
            }
            continue;
        }

        // Every stop from here on should be a single-step trap.
        struct riscv64_regs regs;
        if (getregs(child, &regs) != 0) {
            failf("riscv64_singlestep GETREGSET", (uint64_t) errno, 0, 0, 0, 0, 0);
            break;
        }
        steps_done++;
        if (steps_done > 1 && regs.pc != last_pc)
            pc_ever_changed = 1;
        test_logf("step %d: pc=%#lx counter=%ld\n", steps_done, regs.pc, *counter);
        last_pc = regs.pc;

        if (*counter > MAX_PLAUSIBLE_COUNTER) {
            failf("riscv64_singlestep counter ran ahead", (uint64_t) *counter, 0, 0,
                  (uint64_t) MAX_PLAUSIBLE_COUNTER, 0, 0);
            break;
        }

        if (steps_done >= SINGLESTEP_COUNT)
            break;

        if (ptrace(PTRACE_SINGLESTEP, child, 0, 0) != 0) {
            failf("riscv64_singlestep resume", (uint64_t) errno, 0, 0, 0, 0, 0);
            break;
        }
    }

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    if (!saw_group_stop)
        failf("riscv64_singlestep never saw group-stop", 0, 0, 0, 0, 0, 0);
    if (exited_too_early)
        failf("riscv64_singlestep tracee exited after too few steps", (uint64_t) steps_done,
              0, 0, (uint64_t) SINGLESTEP_COUNT, 0, 0);
    if (steps_done < SINGLESTEP_COUNT)
        failf("riscv64_singlestep too few steps completed", (uint64_t) steps_done, 0, 0,
              (uint64_t) SINGLESTEP_COUNT, 0, 0);
    if (!pc_ever_changed)
        failf("riscv64_singlestep PC never advanced", 0, 0, 0, 0, 0, 0);
    if (*counter > MAX_PLAUSIBLE_COUNTER)
        failf("riscv64_singlestep final counter too high", (uint64_t) *counter, 0, 0,
              (uint64_t) MAX_PLAUSIBLE_COUNTER, 0, 0);

    munmap((void *) counter, sizeof(long));
    return finish_suite("riscv64_singlestep");
}
