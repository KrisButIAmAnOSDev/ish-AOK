// ptrace_stop_races: which task sees a ptrace-stop, and whether it is the one
// stop Linux would report.
//
// Four ways AOK's stop bookkeeping went wrong once gdb and strace -f could run
// programs that fork (see ptrace_eventmsg). Each was measured against Linux
// 6.12 before it was changed, and each case below failed on the unfixed kernel:
//
// new_children_stop_first
//   A child a tracer attaches to at fork must report its SIGSTOP stop to that
//   tracer before anything else, even when the tracer resumes the parent at
//   the event without waiting for the child, as strace -f does. On AOK the
//   parent's own waitpid could take that stop (below), which lost the first
//   stop of 17 to 37 of 200 forks; and, rarely, a child that _exits at once
//   finished before its SIGSTOP was even queued (1 of 2000 vforks). Linux
//   reported all of them.
//
// parent_cannot_take_the_stop
//   The stop is the tracer's. AOK let the child's real parent collect it: its
//   waitpid(child, 0) returned 0x137f while the tracer, never seeing the stop,
//   never resumed the child.
//
// interrupt_lands_on_syscall_stop
//   A PTRACE_INTERRUPT that reaches a tracee on its way into another stop is
//   answered by that stop. AOK's interrupt is a queued SIGTRAP, which the other
//   stop left behind: that stop read 0x80857f instead of 0x857f, and the
//   SIGTRAP then arrived as a second, plain SIGTRAP stop (0x57f), which a
//   tracer re-injects. strace -f killed the program it started with SIGTRAP in
//   23 of 50 runs this way.
//
// sigkill_in_syscall
//   A tracee killed inside a syscall under PTRACE_SYSCALL must die. AOK
//   reported its syscall-exit stop and then waited forever for a wakeup the
//   kill had already spent, while the tracer waited for it to die.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <time.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "test_common.h"

#ifndef __WALL
#define __WALL 0x40000000
#endif
#ifndef PTRACE_SETOPTIONS
#define PTRACE_SETOPTIONS 0x4200
#endif
#ifndef PTRACE_GETEVENTMSG
#define PTRACE_GETEVENTMSG 0x4201
#endif
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif
#ifndef PTRACE_O_TRACESYSGOOD
#define PTRACE_O_TRACESYSGOOD 0x01
#endif
#ifndef PTRACE_O_TRACEFORK
#define PTRACE_O_TRACEFORK 0x02
#endif
#ifndef PTRACE_O_TRACEVFORK
#define PTRACE_O_TRACEVFORK 0x04
#endif
#ifndef PTRACE_EVENT_FORK
#define PTRACE_EVENT_FORK 1
#endif
#ifndef PTRACE_EVENT_VFORK
#define PTRACE_EVENT_VFORK 2
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif

#define STOP_STATUS(sig, event) (((event) << 16) | ((sig) << 8) | 0x7f)
#define SYSCALL_STOP STOP_STATUS(SIGTRAP | 0x80, 0)
#define EVENTMSG_SYSCALL_ENTRY 1
#define EVENTMSG_SYSCALL_EXIT  2

static void nap(long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

static void on_alarm(int sig) { (void) sig; }

// No SA_RESTART: the alarm exists to break a wait that would otherwise hang.
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

static unsigned long read_eventmsg(pid_t pid) {
    unsigned long msg = 0x5eed;
    ptrace(PTRACE_GETEVENTMSG, pid, 0, &msg);
    return msg;
}

static void check(const char *name, const char *what, uint64_t got, uint64_t want) {
    char label[160];
    snprintf(label, sizeof label, "%s: %s", name, what);
    if (got != want)
        failf(label, got, 0, 0, want, 0, 0);
    else
        test_logf("  ok   %-58s %#" PRIx64 "\n", label, got);
}

static void kill_and_reap(pid_t c) {
    if (c > 0)
        kill(c, SIGKILL);
    int st;
    while (wait_for(-1, &st, 0) > 0)
        ;
}

// Fork a child that asks to be traced and stops itself; `body` runs once the
// tracer continues it, and its return value is the exit status.
static pid_t spawn_traceme(int (*body)(void)) {
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(96);
        raise(SIGSTOP);
        _exit(body());
    }
    return c;
}

static int first_stop(const char *name, pid_t c) {
    int st = 0;
    pid_t got = wait_for(c, &st, 0);
    if (got != c || st != STOP_STATUS(SIGSTOP, 0)) {
        check(name, "first stop is the tracee's SIGSTOP", (uint64_t) st, STOP_STATUS(SIGSTOP, 0));
        return 0;
    }
    return 1;
}

// ---- new_children_stop_first ------------------------------------------------

#define ROUNDS 60

// A child that _exits before doing anything else -- the child most likely to
// finish before its SIGSTOP. Its own function, so the loop's counter is not a
// local of the frame the vfork child runs in.
static pid_t fork_quick_child(int use_vfork) {
    pid_t p = use_vfork ? vfork() : fork();
    if (p == 0)
        _exit(0);
    return p;
}

static int body_fork_many(void) {
    for (int i = 0; i < ROUNDS; i++) {
        pid_t p = fork_quick_child(i % 2);
        if (p < 0)
            return 90;
        int st;
        if (waitpid(p, &st, 0) != p)
            return 91;
    }
    return 0;
}

// The tracer resumes the parent at every event without waiting for the child,
// which is what strace -f does. Every child the events name must then report
// its SIGSTOP stop to the tracer -- before or after the event, but before
// anything else.
static void new_children_stop_first(void) {
    const char *name = "new children stop first";
    pid_t c = spawn_traceme(body_fork_many);
    if (c < 0 || !first_stop(name, c)) {
        kill_and_reap(c);
        return;
    }
    long opts = PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK;
    ptrace(PTRACE_SETOPTIONS, c, 0, (void *) opts);
    ptrace(PTRACE_CONT, c, 0, 0);

    // Children announced but not yet heard from, and children heard from
    // before their announcement. Only one or two are ever outstanding.
    pid_t announced[8] = {0}, early[8] = {0};
    int events = 0, stopped_first = 0, other_first = 0, exit_status = -1;
    for (int i = 0; i < ROUNDS * 8; i++) {
        int st = 0;
        pid_t w = wait_for(-1, &st, 0);
        if (w < 0) {
            check(name, "wait (EINTR here is a hang)", (uint64_t) errno, 0);
            break;
        }
        if (w == c) {
            if (WIFEXITED(st) || WIFSIGNALED(st)) {
                exit_status = st;
                break;
            }
            int event = (st >> 16) & 0xff;
            if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK) {
                events++;
                pid_t child = (pid_t) read_eventmsg(c);
                int k;
                for (k = 0; k < 8 && early[k] != child; k++)
                    ;
                if (k < 8 && child != 0) {
                    early[k] = 0;
                    stopped_first++;
                } else {
                    for (k = 0; k < 8 && announced[k] != 0; k++)
                        ;
                    if (k < 8)
                        announced[k] = child;
                }
                ptrace(PTRACE_CONT, c, 0, 0);
                continue;
            }
            int sig = WSTOPSIG(st);
            int deliver = (sig == SIGTRAP || sig == SIGSTOP) ? 0 : sig;
            ptrace(PTRACE_CONT, c, 0, (void *) (long) deliver);
            continue;
        }
        // A child. Its first report is the one that counts.
        int k;
        for (k = 0; k < 8 && announced[k] != w; k++)
            ;
        bool is_sigstop = WIFSTOPPED(st) && st == STOP_STATUS(SIGSTOP, 0);
        if (k < 8) {
            announced[k] = 0;
            if (is_sigstop)
                stopped_first++;
            else
                other_first++;
        } else if (is_sigstop) {
            for (k = 0; k < 8 && early[k] != 0; k++)
                ;
            if (k < 8)
                early[k] = w;
        }
        if (WIFSTOPPED(st))
            ptrace(PTRACE_CONT, w, 0, 0);
    }
    check(name, "tracee exit status", (uint64_t) exit_status, 0);
    check(name, "fork and vfork events", (uint64_t) events, ROUNDS);
    check(name, "children whose first report was their SIGSTOP stop",
          (uint64_t) stopped_first, ROUNDS);
    check(name, "children whose first report was something else", (uint64_t) other_first, 0);
    kill_and_reap(exit_status == -1 ? c : 0);
}

// ---- parent_cannot_take_the_stop -------------------------------------------

static int report_pipe[2] = { -1, -1 };

static int body_fork_and_wait(void) {
    pid_t p = fork();
    if (p < 0)
        return 90;
    if (p == 0)
        _exit(3);
    int st = -1;
    // Announce the wait just before entering it.
    if (write(report_pipe[1], "w", 1) != 1)
        return 92;
    pid_t w = waitpid(p, &st, 0);
    int res[2] = { w == p, st };
    if (write(report_pipe[1], res, sizeof res) != (ssize_t) sizeof res)
        return 93;
    return 0;
}

// The tracee forks, is resumed at the event before its tracer has seen the
// child, and waits for the child with plain waitpid(child, 0). The child's
// SIGSTOP stop belongs to the tracer, which still finds it once the parent is
// waiting; the parent's waitpid returns only when the child has exited.
static void parent_cannot_take_the_stop(void) {
    const char *name = "parent cannot take the stop";
    if (pipe(report_pipe) != 0) {
        check(name, "pipe", (uint64_t) errno, 0);
        return;
    }
    pid_t c = spawn_traceme(body_fork_and_wait);
    close(report_pipe[1]);
    if (c < 0 || !first_stop(name, c)) {
        kill_and_reap(c);
        close(report_pipe[0]);
        return;
    }
    ptrace(PTRACE_SETOPTIONS, c, 0, (void *) (long) PTRACE_O_TRACEFORK);
    ptrace(PTRACE_CONT, c, 0, 0);
    int st = 0;
    if (wait_for(c, &st, 0) != c || st != STOP_STATUS(SIGTRAP, PTRACE_EVENT_FORK)) {
        check(name, "fork event", (uint64_t) st, STOP_STATUS(SIGTRAP, PTRACE_EVENT_FORK));
        kill_and_reap(c);
        close(report_pipe[0]);
        return;
    }
    pid_t child = (pid_t) read_eventmsg(c);
    ptrace(PTRACE_CONT, c, 0, 0);

    // Give the parent time to be inside its waitpid, then look for the stop.
    char ch;
    if (read(report_pipe[0], &ch, 1) != 1)
        check(name, "parent reached its waitpid", 0, 1);
    nap(200);
    int child_st = 0;
    pid_t got = 0;
    for (int i = 0; i < 250 && got == 0; i++) {
        got = waitpid(child, &child_st, __WALL | WNOHANG);
        if (got == 0)
            nap(20);
    }
    check(name, "tracer still gets the child's stop", (uint64_t) got, (uint64_t) child);
    check(name, "child's stop status", (uint64_t) child_st, STOP_STATUS(SIGSTOP, 0));
    if (got == child)
        ptrace(PTRACE_CONT, child, 0, 0);

    int exit_status = -1;
    for (int i = 0; i < 16; i++) {
        pid_t w = wait_for(-1, &st, 0);
        if (w < 0)
            break;
        if (WIFSTOPPED(st)) {
            int sig = WSTOPSIG(st);
            ptrace(PTRACE_CONT, w, 0, (void *) (long) ((sig == SIGTRAP || sig == SIGSTOP) ? 0 : sig));
            continue;
        }
        if (w == c) {
            exit_status = st;
            break;
        }
    }
    check(name, "tracee exit status", (uint64_t) exit_status, 0);
    kill_and_reap(exit_status == -1 ? c : 0);
    int res[2] = { 0, 0 };
    if (read(report_pipe[0], res, sizeof res) != (ssize_t) sizeof res)
        res[0] = -1;
    check(name, "parent's waitpid returned the child", (uint64_t) res[0], 1);
    check(name, "parent's waitpid status is the exit", (uint64_t) res[1], 3 << 8);
    close(report_pipe[0]);
    report_pipe[0] = -1;
}

// ---- interrupt_lands_on_syscall_stop ---------------------------------------

static void interrupt_lands_on_syscall_stop(void) {
    const char *name = "interrupt lands on a syscall stop";
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        for (;;)
            nap(300);
    }
    if (c < 0) {
        check(name, "fork", (uint64_t) errno, 0);
        return;
    }
    nap(100);
    int st = 0;
    if (ptrace(PTRACE_SEIZE, c, 0, (void *) (long) PTRACE_O_TRACESYSGOOD) != 0 ||
            ptrace(PTRACE_INTERRUPT, c, 0, 0) != 0 || wait_for(c, &st, 0) != c) {
        check(name, "seize, interrupt and wait", (uint64_t) errno, 0);
        kill_and_reap(c);
        return;
    }
    check(name, "first interrupt stop", (uint64_t) st, STOP_STATUS(SIGTRAP, PTRACE_EVENT_STOP));

    ptrace(PTRACE_SYSCALL, c, 0, 0);
    if (wait_for(c, &st, 0) != c)
        goto hung;
    check(name, "next stop is a syscall entry", (uint64_t) st, SYSCALL_STOP);
    check(name, "its message", read_eventmsg(c), EVENTMSG_SYSCALL_ENTRY);

    // Into the sleep, then interrupt it there.
    ptrace(PTRACE_SYSCALL, c, 0, 0);
    nap(60);
    ptrace(PTRACE_INTERRUPT, c, 0, 0);
    if (wait_for(c, &st, 0) != c)
        goto hung;
    check(name, "the interrupt is answered by the syscall-exit stop", (uint64_t) st, SYSCALL_STOP);
    check(name, "its message", read_eventmsg(c), EVENTMSG_SYSCALL_EXIT);

    // And by nothing more: the next stop is the next syscall's entry, not a
    // leftover SIGTRAP.
    ptrace(PTRACE_SYSCALL, c, 0, 0);
    if (wait_for(c, &st, 0) != c)
        goto hung;
    check(name, "no second stop for the interrupt", (uint64_t) st, SYSCALL_STOP);
    check(name, "the stop after it is a syscall entry", read_eventmsg(c), EVENTMSG_SYSCALL_ENTRY);
    kill_and_reap(c);
    return;

hung:
    check(name, "wait (EINTR here is a hang)", (uint64_t) errno, 0);
    kill_and_reap(c);
}

// ---- sigkill_in_syscall -----------------------------------------------------

static int body_sleep_forever(void) {
    for (;;)
        nap(400);
    return 0;
}

static void sigkill_in_syscall(void) {
    const char *name = "SIGKILL inside a traced syscall";
    pid_t c = spawn_traceme(body_sleep_forever);
    if (c < 0 || !first_stop(name, c)) {
        kill_and_reap(c);
        return;
    }
    ptrace(PTRACE_SETOPTIONS, c, 0, (void *) (long) PTRACE_O_TRACESYSGOOD);
    // Step to the third syscall entry, which is in the sleep loop.
    int st = 0, entries = 0;
    for (int i = 0; i < 20 && entries < 3; i++) {
        if (ptrace(PTRACE_SYSCALL, c, 0, 0) != 0 || wait_for(c, &st, 0) != c)
            goto hung;
        if (st == SYSCALL_STOP && read_eventmsg(c) == EVENTMSG_SYSCALL_ENTRY)
            entries++;
    }
    check(name, "reached a syscall entry in the loop", (uint64_t) entries, 3);
    ptrace(PTRACE_SYSCALL, c, 0, 0);
    nap(100);
    kill(c, SIGKILL);

    int stops = 0;
    for (;;) {
        pid_t w = wait_for(c, &st, 0);
        if (w != c)
            goto hung;
        if (WIFSTOPPED(st)) {
            stops++;
            continue;
        }
        break;
    }
    check(name, "died of SIGKILL", (uint64_t) (WIFSIGNALED(st) ? WTERMSIG(st) : -1), SIGKILL);
    check(name, "stops reported while dying", (uint64_t) stops, 0);
    kill_and_reap(0);
    return;

hung:
    check(name, "wait (EINTR here is a hang)", (uint64_t) errno, 0);
    ptrace(PTRACE_CONT, c, 0, 0);
    kill_and_reap(c);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    install_watchdog();

    new_children_stop_first();
    parent_cannot_take_the_stop();
    interrupt_lands_on_syscall_stop();
    sigkill_in_syscall();

    return finish_suite("ptrace_stop_races");
}
