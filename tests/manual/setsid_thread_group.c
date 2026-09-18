// A group signal must reach a process whose session was created by a
// NON-LEADER THREAD.
//
// task_setsid() sets group->sid and group->pgid to group->leader->pid, but the
// membership it files -- the list_add that puts the tgroup onto a struct pid's
// session and pgroup lists -- has to hang off that SAME pid. It used to use
// pid_get(task->pid), the CALLING task's pid. For the ordinary caller the two
// are the same pid, because a process calling setsid() is its own group leader.
// They diverge only here: when a non-leader thread makes the call, group->sid
// names the leader while the tgroup is linked into the THREAD's pid struct.
//
// The damage is invisible to everything that ASKS about the ids. getpgid() and
// getsid() read group->pgid and group->sid, which are set correctly in the
// broken build too -- the pgid/sid legs below pass either way, and are here to
// prove that they cannot be the thing under test. Only delivery separates them:
//
//   - kill(-pgid) -> kill_group() finds the leader's pid struct, walks a pgroup
//     list that does not contain this group, and returns ESRCH for a live,
//     correctly numbered process group. Loud.
//   - send_group_signal() -- the tty hangup path, and the orphaned-pgroup
//     SIGHUP/SIGCONT in kernel/exit.c -- has no equivalent guard: it walks the
//     same empty list, signals nobody and returns success. Silent.
//
// Linux files both under task_pid(group_leader): ksys_setsid -> set_special_pids.
// Fixed in 555. Against the unfixed kernel the killpg leg reports ESRCH and
// group_delivered=0 while every id leg still passes.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#define DELIVERY_WAIT_MS 2000
#define SETSID_WAIT_MS   5000

struct result {
    long leader_pid;
    long thread_tid;
    long setsid_ret;
    long setsid_err;
    long pgid_after;
    long sid_after;
    long killpg_ret;
    long killpg_err;
    long group_delivered;
    long direct_delivered;
    long self_raise_delivered;
};

static volatile sig_atomic_t got_usr1;
static void on_usr1(int sig) { (void) sig; got_usr1 = 1; }

// Handed from the setsid thread to the main thread. Atomics rather than plain
// longs: the flag is the release, and the three values must be visible with it.
static atomic_int setsid_done;
static atomic_long t_tid, t_ret, t_err;

static pid_t child_pid;
static void on_child_alarm(int sig) {
    // The parent owns the verdict: die quietly and let it report the silence.
    (void) sig;
    _exit(7);
}

static void on_alarm(int sig) {
    (void) sig;
    static const char msg[] =
        "setsid_thread_group: FAIL timeout (no verdict from the child)\n";
    if (child_pid > 0)
        kill(child_pid, SIGKILL);
    write(2, msg, sizeof(msg) - 1);
    _exit(1);
}

static void nap_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// Poll rather than sleep a fixed slug: fast when delivery works, and tolerant
// of a loaded host without making a correct run wait for the worst case.
static int wait_flag(volatile sig_atomic_t *flag, long ms) {
    for (long waited = 0; waited < ms; waited += 10) {
        if (*flag)
            return 1;
        nap_ms(10);
    }
    return *flag != 0;
}

static int wait_atomic(atomic_int *flag, long ms) {
    for (long waited = 0; waited < ms; waited += 10) {
        if (atomic_load(flag))
            return 1;
        nap_ms(10);
    }
    return atomic_load(flag) != 0;
}

static void *setsid_thread(void *arg) {
    (void) arg;
    atomic_store(&t_tid, (long) syscall(SYS_gettid));
    errno = 0;
    long ret = (long) setsid();
    atomic_store(&t_err, (long) errno);
    atomic_store(&t_ret, ret);
    atomic_store(&setsid_done, 1);
    // Stay alive. Against the unfixed kernel the tgroup is linked into THIS
    // task's pid struct, and letting the thread exit would measure teardown
    // rather than the filing bug.
    for (;;)
        nap_ms(1000);
    return NULL;
}

// Runs in a forked child, so that the caller of setsid() is guaranteed not to
// be a process-group leader already (that is EPERM, not a bug). The child also
// lands in its own process group, so the killpg below cannot touch the parent.
static void child_main(int wfd) {
    struct result r;
    memset(&r, 0, sizeof r);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) < 0)
        _exit(2);

    signal(SIGALRM, on_child_alarm);
    alarm(test_watchdog_secs(60));
    r.leader_pid = (long) getpid();

    // Control for the probe itself: watch the handler fire before trusting a
    // zero from the group leg.
    got_usr1 = 0;
    raise(SIGUSR1);
    r.self_raise_delivered = wait_flag(&got_usr1, DELIVERY_WAIT_MS);

    pthread_t th;
    if (pthread_create(&th, NULL, setsid_thread, NULL) != 0)
        _exit(3);
    if (!wait_atomic(&setsid_done, SETSID_WAIT_MS))
        _exit(4);

    r.thread_tid = atomic_load(&t_tid);
    r.setsid_ret = atomic_load(&t_ret);
    r.setsid_err = atomic_load(&t_err);
    r.pgid_after = (long) getpgid(0);
    r.sid_after = (long) getsid(0);

    got_usr1 = 0;
    errno = 0;
    r.killpg_ret = killpg((pid_t) r.pgid_after, SIGUSR1);
    r.killpg_err = errno;
    r.group_delivered = wait_flag(&got_usr1, DELIVERY_WAIT_MS);

    // A directly addressed signal must work either way: it separates "the
    // group lookup is broken" from "this process cannot be signalled at all".
    got_usr1 = 0;
    kill(getpid(), SIGUSR1);
    r.direct_delivered = wait_flag(&got_usr1, DELIVERY_WAIT_MS);

    if (write(wfd, &r, sizeof r) != (ssize_t) sizeof r)
        _exit(5);
    _exit(0);
}

static void test_setsid_from_non_leader_thread(void) {
    int fds[2];
    if (pipe(fds) < 0) {
        failf("setsid_thread_group pipe", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }

    child_pid = fork();
    if (child_pid < 0) {
        failf("setsid_thread_group fork", (uint64_t) errno, 0, 0, 0, 0, 0);
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (child_pid == 0) {
        close(fds[0]);
        child_main(fds[1]);
        _exit(6);
    }
    close(fds[1]);

    struct result r;
    memset(&r, 0, sizeof r);
    ssize_t n = read(fds[0], &r, sizeof r);
    close(fds[0]);

    int status = 0;
    if (waitpid(child_pid, &status, 0) < 0) {
        failf("setsid_thread_group waitpid", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    child_pid = 0;

    if (n != (ssize_t) sizeof r) {
        // The child never reached its verdict: report how it died instead of
        // reading an uninitialised struct.
        failf("setsid_thread_group no_verdict_from_child", (uint64_t) n,
              (uint64_t) (WIFEXITED(status) ? WEXITSTATUS(status) : -1),
              (uint64_t) (WIFSIGNALED(status) ? WTERMSIG(status) : 0),
              (uint64_t) sizeof r, 0, 0);
        return;
    }

    test_logf("leader_pid=%ld thread_tid=%ld setsid_ret=%ld pgid=%ld sid=%ld "
              "killpg=%ld errno=%ld group_delivered=%ld direct=%ld self=%ld\n",
              r.leader_pid, r.thread_tid, r.setsid_ret, r.pgid_after,
              r.sid_after, r.killpg_ret, r.killpg_err, r.group_delivered,
              r.direct_delivered, r.self_raise_delivered);

    // The probe must be able to fail: if the handler never runs for a plain
    // raise(), a zero on the group leg would mean nothing.
    if (!r.self_raise_delivered) {
        failf("setsid_thread_group handler_never_ran", 0, 0, 0, 1, 0, 0);
        return;
    }

    // And it must still be exercising the intended path. If a thread ever
    // stopped getting a pid of its own, every leg below would pass while
    // testing the ordinary caller instead.
    if (r.thread_tid == r.leader_pid) {
        failf("setsid_thread_group caller_was_the_leader",
              (uint64_t) r.thread_tid, (uint64_t) r.leader_pid, 0, 0, 0, 0);
        return;
    }

    if (r.setsid_ret < 0) {
        failf("setsid_thread_group setsid_failed", (uint64_t) r.setsid_ret,
              (uint64_t) r.setsid_err, 0, (uint64_t) r.leader_pid, 0, 0);
        return;
    }

    // setsid() reports the LEADER's pid, not the calling thread's.
    if (r.setsid_ret != r.leader_pid)
        failf("setsid_thread_group setsid_returned_wrong_pid",
              (uint64_t) r.setsid_ret, (uint64_t) r.thread_tid, 0,
              (uint64_t) r.leader_pid, 0, 0);
    if (r.pgid_after != r.leader_pid)
        failf("setsid_thread_group pgid", (uint64_t) r.pgid_after, 0, 0,
              (uint64_t) r.leader_pid, 0, 0);
    if (r.sid_after != r.leader_pid)
        failf("setsid_thread_group sid", (uint64_t) r.sid_after, 0, 0,
              (uint64_t) r.leader_pid, 0, 0);

    // The regression proper.
    if (r.killpg_ret != 0)
        failf("setsid_thread_group killpg_own_group_failed",
              (uint64_t) r.killpg_ret, (uint64_t) r.killpg_err,
              (uint64_t) r.pgid_after, 0, 0, 0);
    if (!r.group_delivered)
        failf("setsid_thread_group group_signal_not_delivered",
              (uint64_t) r.group_delivered, (uint64_t) r.killpg_ret,
              (uint64_t) r.pgid_after, 1, 0, 0);
    if (!r.direct_delivered)
        failf("setsid_thread_group direct_signal_not_delivered",
              (uint64_t) r.direct_delivered, 0, 0, 1, 0, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    signal(SIGALRM, on_alarm);
    alarm(test_watchdog_secs(90));
    test_setsid_from_non_leader_thread();
    alarm(0);
    return finish_suite("setsid_thread_group");
}
