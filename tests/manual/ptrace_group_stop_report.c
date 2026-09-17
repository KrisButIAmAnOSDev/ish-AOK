// How a job-control group-stop is REPORTED to a tracer, for both kinds of
// tracee. tests/manual/ptrace_group_stop.c already covers that the stop is
// reported at all (before it, strace -f hung on a group-stopped child); this
// covers what the report says, which is what a real tracer switches on.
//
// A tracer has to tell a group-stop from a signal-delivery-stop of the very
// same signal, and the two attach styles answer that differently. Every
// expectation here was measured on Linux 6.12.101 (x86_64) before it was
// written, with a C tracer probe:
//
//   UNSEIZED (PTRACE_TRACEME/PTRACE_ATTACH -- gdb). Both stops report the
//   IDENTICAL status word, 0x137f. The only difference is PTRACE_GETSIGINFO:
//   it succeeds at the signal-delivery-stop and fails with EINVAL at the
//   group-stop, because Linux's do_jobctl_trap passes a NULL siginfo there.
//   AOK answered both, so a tracer that re-injects each stop's own signal --
//   which gdb and strace both can -- re-delivered the SIGSTOP and group-stopped
//   again forever: a probe logged 2.8 million consecutive 0x137f stops in four
//   minutes, where Linux takes two and runs on.
//
//   SEIZED (PTRACE_SEIZE -- strace). The status carries the STOP SIGNAL with a
//   PTRACE_EVENT_STOP event: 0x80137f for SIGSTOP, 0x80147f for SIGTSTP. AOK
//   reported SIGTRAP (0x80057f), which strace does not recognise as a
//   group-stop -- it resumed with PTRACE_CONT, which lifts the stop, so
//   `kill -STOP` on a process under `strace -f` did not stop it at all.
//
//   PTRACE_LISTEN is how a seizing tracer says "leave it stopped". Reporting
//   the group-stop correctly without it would be worse than not reporting it:
//   strace answers a recognised group-stop with LISTEN, which used to fall
//   through to EPERM. A SIGCONT that lifts a listened-to stop is reported as a
//   PTRACE_EVENT_STOP carrying SIGTRAP (0x80057f); a PTRACE_INTERRUPT re-reports
//   the group-stop (0x80137f), since the tracee is still group-stopped.

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif
#ifndef PTRACE_LISTEN
#define PTRACE_LISTEN 0x4208
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif

#define STOP_STATUS(sig, event) (((event) << 16) | ((sig) << 8) | 0x7f)

static pid_t g_victim;
static int g_pipe[2] = { -1, -1 };

static void check(const char *name, const char *what, uint64_t got, uint64_t want) {
    char label[160];
    snprintf(label, sizeof label, "%s: %s", name, what);
    if (got != want)
        failf(label, got, 0, 0, want, 0, 0);
    else
        test_logf("  ok   %-56s %#" PRIx64 "\n", label, got);
}

static void nap_ms(int ms) {
    struct timespec ts = { ms / 1000, (long) (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// The victim writes a byte every few ms, so "did it run?" is a positive
// observation rather than an absence of stops: a task that was killed, or is
// still parked, writes nothing, and only a resumed one writes.
static pid_t spawn_victim(int traceme, int stop_self) {
    if (pipe(g_pipe) != 0) {
        failf("ptrace_group_stop_report pipe", (uint64_t) errno, 0, 0, 0, 0, 0);
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(g_pipe[0]);
        if (traceme)
            ptrace(PTRACE_TRACEME, 0, 0, 0);
        if (stop_self)
            raise(SIGSTOP);
        for (;;) {
            if (write(g_pipe[1], "x", 1) != 1)
                _exit(0);
            nap_ms(5);
        }
    }
    close(g_pipe[1]);
    g_pipe[1] = -1;
    fcntl(g_pipe[0], F_SETFL, O_NONBLOCK);
    g_victim = pid;
    return pid;
}

static int drain_ticks(void) {
    char buf[4096];
    int total = 0, n;
    while ((n = (int) read(g_pipe[0], buf, sizeof buf)) > 0)
        total += n;
    return total;
}

// Ticks the victim managed over `ms`. Zero means it is not running.
static int ticks_over(int ms) {
    drain_ticks();
    nap_ms(ms);
    return drain_ticks();
}

static pid_t wait_ms(pid_t pid, int *status, int ms) {
    for (int waited = 0; waited <= ms; waited += 5) {
        pid_t w = waitpid(pid, status, WNOHANG);
        if (w == pid)
            return w;
        if (w < 0 && errno != EINTR)
            return w;
        nap_ms(5);
    }
    return 0;
}

static void reap_victim(void) {
    if (g_victim > 0) {
        ptrace(PTRACE_DETACH, g_victim, 0, 0);
        kill(g_victim, SIGKILL);
        kill(g_victim, SIGCONT);
        for (int i = 0; i < 200; i++) {
            int st;
            pid_t w = waitpid(g_victim, &st, WNOHANG);
            if (w == g_victim && !WIFSTOPPED(st))
                break;
            if (w == g_victim) {
                ptrace(PTRACE_CONT, g_victim, 0, (void *) (long) SIGKILL);
                kill(g_victim, SIGKILL);
            }
            nap_ms(5);
        }
        g_victim = -1;
    }
    if (g_pipe[0] >= 0) {
        close(g_pipe[0]);
        g_pipe[0] = -1;
    }
}

static void on_alarm(int sig) {
    (void) sig;
    if (g_victim > 0)
        kill(g_victim, SIGKILL);
    static const char msg[] = "ptrace_group_stop_report: FAIL timeout\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(1);
}

// errno from PTRACE_GETSIGINFO, or 0 with the siginfo filled in.
static int getsiginfo_errno(pid_t pid, siginfo_t *si) {
    memset(si, 0, sizeof *si);
    errno = 0;
    if (ptrace(PTRACE_GETSIGINFO, pid, 0, si) == -1)
        return errno;
    return 0;
}

// Drive a tracee to a real group-stop with `sig`: a tracee reports the
// SIGNAL-DELIVERY-stop for the stop signal first, and only group-stops once the
// tracer lets that signal through. Returns 0 on success.
static int reach_group_stop(const char *name, pid_t pid, int sig, int *status) {
    if (wait_ms(pid, status, 4000) != pid || !WIFSTOPPED(*status)) {
        check(name, "delivery-stop reported", (uint64_t) *status, 0xdead);
        return -1;
    }
    check(name, "signal-delivery-stop status", (uint64_t) *status, STOP_STATUS(sig, 0));
    siginfo_t si;
    check(name, "GETSIGINFO at the delivery-stop", (uint64_t) getsiginfo_errno(pid, &si), 0);
    if (ptrace(PTRACE_CONT, pid, 0, (void *) (long) sig) != 0) {
        check(name, "CONT injecting the stop signal", (uint64_t) errno, 0);
        return -1;
    }
    if (wait_ms(pid, status, 4000) != pid || !WIFSTOPPED(*status)) {
        check(name, "group-stop reported", (uint64_t) *status, 0xdead);
        return -1;
    }
    return 0;
}

// ---- unseized: the group-stop that only GETSIGINFO can identify -----------

static void test_unseized_group_stop(void) {
    const char *name = "unseized";
    pid_t pid = spawn_victim(1, 1);
    if (pid < 0)
        return;
    int status = 0;
    if (reach_group_stop(name, pid, SIGSTOP, &status) != 0) {
        reap_victim();
        return;
    }
    // Same status word as the delivery-stop above -- deliberately so.
    check(name, "group-stop status", (uint64_t) status, STOP_STATUS(SIGSTOP, 0));
    siginfo_t si;
    check(name, "GETSIGINFO at the group-stop is EINVAL",
          (uint64_t) getsiginfo_errno(pid, &si), EINVAL);

    // The stop signal injected FROM a group-stop is discarded, not delivered:
    // Linux's do_jobctl_trap throws away what ptrace_stop() returns. This is
    // the storm: delivering it re-enters the group-stop forever.
    if (ptrace(PTRACE_CONT, pid, 0, (void *) (long) SIGSTOP) != 0)
        check(name, "CONT(SIGSTOP) from the group-stop", (uint64_t) errno, 0);
    int ticks = ticks_over(300);
    check(name, "tracee runs after the injection is dropped", ticks > 0, 1);
    check(name, "no further stop from the dropped injection",
          (uint64_t) wait_ms(pid, &status, 100), 0);
    reap_victim();
}

// The whole defect in one shape: a tracer that re-injects every stop's own
// signal, which is what gdb does when told to pass a signal on. Linux takes two
// stops and runs; AOK took 2.8 million and counting.
static void test_unseized_injection_terminates(void) {
    const char *name = "unseized reinject";
    pid_t pid = spawn_victim(1, 1);
    if (pid < 0)
        return;
    int status = 0;
    long stops = 0;
    if (wait_ms(pid, &status, 4000) != pid || !WIFSTOPPED(status)) {
        check(name, "first stop", (uint64_t) status, 0xdead);
        reap_victim();
        return;
    }
    // Bounded: a tracer that never escapes would otherwise hit the watchdog
    // with no idea why. 64 is ~30x Linux's two.
    for (stops = 1; stops <= 64; stops++) {
        if (ptrace(PTRACE_CONT, pid, 0, (void *) (long) WSTOPSIG(status)) != 0)
            break;
        pid_t w = wait_ms(pid, &status, 300);
        if (w != pid || !WIFSTOPPED(status))
            break;
    }
    test_logf("  injections before the tracee ran: %ld\n", stops);
    check(name, "injection loop terminates", stops <= 8, 1);
    check(name, "and the tracee is running", ticks_over(200) > 0, 1);
    reap_victim();
}

// ---- seized: the status carries the stop signal, and LISTEN holds it ------

// SEIZE a freely running victim and drive it into a group-stop with `sig`.
static pid_t seize_and_group_stop(const char *name, int sig, int *status) {
    pid_t pid = spawn_victim(0, 0);
    if (pid < 0)
        return -1;
    // Let it get going, so the stop lands in the loop rather than in exec.
    for (int i = 0; i < 100 && drain_ticks() == 0; i++)
        nap_ms(5);
    if (ptrace(PTRACE_SEIZE, pid, 0, 0) != 0) {
        check(name, "PTRACE_SEIZE", (uint64_t) errno, 0);
        reap_victim();
        return -1;
    }
    kill(pid, sig);
    if (reach_group_stop(name, pid, sig, status) != 0) {
        reap_victim();
        return -1;
    }
    return pid;
}

static void test_seized_group_stop_status(void) {
    const int stop_sigs[] = { SIGSTOP, SIGTSTP };
    const char *names[] = { "seized SIGSTOP", "seized SIGTSTP" };
    for (unsigned i = 0; i < sizeof stop_sigs / sizeof stop_sigs[0]; i++) {
        int sig = stop_sigs[i];
        const char *name = names[i];
        int status = 0;
        pid_t pid = seize_and_group_stop(name, sig, &status);
        if (pid < 0)
            continue;
        // The STOP SIGNAL with a PTRACE_EVENT_STOP event, not SIGTRAP.
        check(name, "group-stop status", (uint64_t) status, STOP_STATUS(sig, PTRACE_EVENT_STOP));
        siginfo_t si;
        check(name, "GETSIGINFO at the group-stop", (uint64_t) getsiginfo_errno(pid, &si), 0);
        check(name, "si_signo", (uint64_t) si.si_signo, (uint64_t) sig);
        check(name, "si_code", (uint64_t) si.si_code,
              (uint64_t) ((PTRACE_EVENT_STOP << 8) | sig));
        // The stopped task's OWN pid, as in Linux's ptrace_do_notify.
        check(name, "si_pid is the tracee", (uint64_t) si.si_pid, (uint64_t) pid);

        // An injected signal is dropped here too, and the tracee runs.
        if (ptrace(PTRACE_CONT, pid, 0, (void *) (long) sig) != 0)
            check(name, "CONT injecting from the group-stop", (uint64_t) errno, 0);
        check(name, "tracee runs after the injection is dropped", ticks_over(300) > 0, 1);
        reap_victim();
    }
}

static void test_listen_holds_the_stop(void) {
    const char *name = "listen";
    int status = 0;
    pid_t pid = seize_and_group_stop(name, SIGSTOP, &status);
    if (pid < 0)
        return;
    check(name, "group-stop status", (uint64_t) status,
          STOP_STATUS(SIGSTOP, PTRACE_EVENT_STOP));

    errno = 0;
    check(name, "PTRACE_LISTEN", (uint64_t) (ptrace(PTRACE_LISTEN, pid, 0, 0) == 0 ? 0 : errno), 0);
    // The point of listening: the tracee stays job-control stopped and the
    // tracer is told nothing more until something lifts the stop.
    check(name, "tracee stays stopped while listening", ticks_over(250), 0);
    check(name, "no report while listening", (uint64_t) wait_ms(pid, &status, 150), 0);

    // A SIGCONT lifts it, and THAT is reported: PTRACE_EVENT_STOP with SIGTRAP.
    kill(pid, SIGCONT);
    if (wait_ms(pid, &status, 4000) != pid || !WIFSTOPPED(status)) {
        check(name, "SIGCONT reported to the listener", (uint64_t) status, 0xdead);
        reap_victim();
        return;
    }
    check(name, "SIGCONT-while-listening status", (uint64_t) status,
          STOP_STATUS(SIGTRAP, PTRACE_EVENT_STOP));
    siginfo_t si;
    check(name, "GETSIGINFO at that stop", (uint64_t) getsiginfo_errno(pid, &si), 0);
    check(name, "si_code", (uint64_t) si.si_code,
          (uint64_t) ((PTRACE_EVENT_STOP << 8) | SIGTRAP));

    // And the tracee runs again once the tracer lets it. Any further stop is
    // the SIGCONT's own signal-delivery-stop, which is passed over.
    for (int i = 0; i < 8; i++) {
        if (ptrace(PTRACE_CONT, pid, 0, 0) != 0)
            break;
        if (ticks_over(200) > 0)
            break;
        if (wait_ms(pid, &status, 200) != pid || !WIFSTOPPED(status))
            break;
    }
    check(name, "tracee runs after the listen ends", ticks_over(250) > 0, 1);
    reap_victim();
}

static void test_interrupt_ends_a_listen(void) {
    const char *name = "listen interrupt";
    int status = 0;
    pid_t pid = seize_and_group_stop(name, SIGSTOP, &status);
    if (pid < 0)
        return;
    if (ptrace(PTRACE_LISTEN, pid, 0, 0) != 0) {
        check(name, "PTRACE_LISTEN", (uint64_t) errno, 0);
        reap_victim();
        return;
    }
    errno = 0;
    check(name, "PTRACE_INTERRUPT while listening",
          (uint64_t) (ptrace(PTRACE_INTERRUPT, pid, 0, 0) == 0 ? 0 : errno), 0);
    if (wait_ms(pid, &status, 4000) != pid || !WIFSTOPPED(status)) {
        check(name, "interrupt reported", (uint64_t) status, 0xdead);
        reap_victim();
        return;
    }
    // Still group-stopped, so the report is the group-stop again -- the stop
    // signal, not the bare SIGTRAP an interrupt gets outside one.
    check(name, "re-reports the group-stop", (uint64_t) status,
          STOP_STATUS(SIGSTOP, PTRACE_EVENT_STOP));
    reap_victim();
}

static void test_listen_refused(void) {
    // An UNSEIZED tracee's group-stop: EIO. This is why the report and LISTEN
    // had to land together -- a tracer that recognises the stop then asks.
    const char *name = "listen refused";
    int status = 0;
    pid_t pid = spawn_victim(1, 1);
    if (pid < 0)
        return;
    if (reach_group_stop(name, pid, SIGSTOP, &status) == 0) {
        errno = 0;
        long rc = ptrace(PTRACE_LISTEN, pid, 0, 0);
        check(name, "LISTEN at an unseized group-stop is EIO",
              (uint64_t) (rc == 0 ? 0 : errno), EIO);
    }
    reap_victim();

    // And a seized tracee's SIGNAL-delivery-stop: also EIO. Only a stop whose
    // siginfo says PTRACE_EVENT_STOP can be listened to.
    pid = spawn_victim(0, 0);
    if (pid < 0)
        return;
    for (int i = 0; i < 100 && drain_ticks() == 0; i++)
        nap_ms(5);
    if (ptrace(PTRACE_SEIZE, pid, 0, 0) != 0) {
        check(name, "PTRACE_SEIZE", (uint64_t) errno, 0);
        reap_victim();
        return;
    }
    kill(pid, SIGUSR1);
    if (wait_ms(pid, &status, 4000) == pid && WIFSTOPPED(status)) {
        check(name, "seized signal-delivery-stop status", (uint64_t) status,
              STOP_STATUS(SIGUSR1, 0));
        errno = 0;
        long rc = ptrace(PTRACE_LISTEN, pid, 0, 0);
        check(name, "LISTEN at a signal-delivery-stop is EIO",
              (uint64_t) (rc == 0 ? 0 : errno), EIO);
    }
    reap_victim();
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    signal(SIGALRM, on_alarm);
    alarm(test_watchdog_secs(180));
    test_unseized_group_stop();
    test_unseized_injection_terminates();
    test_seized_group_stop_status();
    test_listen_holds_the_stop();
    test_interrupt_ends_a_listen();
    test_listen_refused();
    return finish_suite("ptrace_group_stop_report");
}
