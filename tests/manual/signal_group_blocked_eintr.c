// A process-directed signal must not interrupt a thread that blocks it.
//
// deliver_signal_to_group_locked woke EVERY member of the thread group and
// marked each wait interrupted, blocked or not. futex(FUTEX_WAIT*) and
// rt_sigtimedwait turn that mark straight into EINTR, so a thread with every
// signal blocked saw sem_wait() fail whenever a child exited (SIGCHLD) or an
// alarm fired (SIGALRM). foot's render workers are exactly that thread: they
// block every signal, park in an unchecked sem_wait(), and on the EINTR popped
// an empty work queue and faulted at 0x8 -- the guest-fatal record that
// started this. The per-thread path (kill() via pick_process_directed_target)
// already skipped blocked threads; only the group path was wrong.
//
// Linux never selects a thread that blocks the signal (complete_signal /
// wants_signal), so every count below is 0 there, except the control, which
// leaves SIGCHLD unblocked and so is the one thread the signal must interrupt.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>

#include "test_common.h"

#define NTHREADS 4

static sem_t start;
static atomic_int blocked_eintr, blocked_other, control_eintr;
static atomic_int stw_result; // 0 = still waiting; else signo or -errno
static atomic_int stw_tid;

static void chk(const char *w, long got, long want) {
    if (got != want)
        failf(w, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-62s got=%ld want=%ld\n", w, got, want);
}

static void nap(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) != 0 && errno == EINTR)
        ;
}

static volatile sig_atomic_t got_alrm;
static void on_signal(int sig) {
    if (sig == SIGALRM)
        got_alrm = 1;
}

static void *blocked_worker(void *arg) {
    (void) arg;
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, NULL);
    for (;;) {
        if (sem_wait(&start) == 0)
            return NULL; // one token per worker ends it
        if (errno == EINTR)
            atomic_fetch_add(&blocked_eintr, 1);
        else
            atomic_fetch_add(&blocked_other, 1);
    }
}

static void *control_worker(void *arg) {
    sem_t *s = arg;
    sigset_t all;
    sigfillset(&all);
    sigdelset(&all, SIGCHLD);
    pthread_sigmask(SIG_SETMASK, &all, NULL);
    for (;;) {
        if (sem_wait(s) == 0)
            return NULL;
        if (errno == EINTR)
            atomic_fetch_add(&control_eintr, 1);
    }
}

static void *sigtimedwait_worker(void *arg) {
    (void) arg;
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, NULL);
    atomic_store(&stw_tid, (int) syscall(SYS_gettid));
    sigset_t want;
    sigemptyset(&want);
    sigaddset(&want, SIGUSR2);
    struct timespec ts = {20, 0};
    int r = sigtimedwait(&want, NULL, &ts);
    atomic_store(&stw_result, r > 0 ? r : -errno);
    return NULL;
}

static void child_exits(void) {
    pid_t pid = fork();
    if (pid == 0)
        _exit(0);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    // foot's fdm_signal_add(): block in the main thread, handler with
    // sa_flags == 0 -- no SA_RESTART, so an interruption is visible.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGCHLD);
    sigaddset(&m, SIGALRM);
    sigprocmask(SIG_BLOCK, &m, NULL);

    sem_init(&start, 0, 0);
    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++)
        pthread_create(&th[i], NULL, blocked_worker, NULL);
    pthread_t stw;
    pthread_create(&stw, NULL, sigtimedwait_worker, NULL);
    nap(300);

    // 1. SIGCHLD from a child exit.
    child_exits();
    nap(400);
    chk("sem_wait EINTR, all signals blocked, child exit (SIGCHLD)",
        atomic_load(&blocked_eintr), 0);
    chk("sigtimedwait(SIGUSR2) still waiting after SIGCHLD",
        atomic_load(&stw_result), 0);

    // 2. SIGALRM from alarm(), taken by the main thread in sigsuspend().
    //    Suspend on SIGALRM ONLY: case 1's SIGCHLD is still pending on the
    //    main thread, and an empty mask returns for it at once -- then the
    //    watchdog re-arm below cancels alarm(1) and this case tests nothing.
    int before = atomic_load(&blocked_eintr);
    alarm(1);
    sigset_t only_alrm;
    sigfillset(&only_alrm);
    sigdelset(&only_alrm, SIGALRM);
    while (!got_alrm)
        sigsuspend(&only_alrm);
    alarm(test_watchdog_secs(120)); // re-arm the watchdog alarm(1) replaced
    chk("SIGALRM was delivered (the case ran)", (long) got_alrm, 1);
    nap(400);
    chk("sem_wait EINTR, all signals blocked, alarm (SIGALRM)",
        atomic_load(&blocked_eintr) - before, 0);
    chk("sigtimedwait(SIGUSR2) still waiting after SIGALRM",
        atomic_load(&stw_result), 0);

    // 3. Positive control: a thread that leaves SIGCHLD unblocked is the one
    //    the signal must interrupt -- proves the detector can see an EINTR.
    sem_t ctl_sem;
    sem_init(&ctl_sem, 0, 0);
    pthread_t ctl;
    pthread_create(&ctl, NULL, control_worker, &ctl_sem);
    nap(300);
    before = atomic_load(&blocked_eintr);
    child_exits();
    nap(400);
    chk("control: SIGCHLD-unblocked thread's sem_wait EINTR", atomic_load(&control_eintr), 1);
    chk("blocked threads still not interrupted", atomic_load(&blocked_eintr) - before, 0);
    sem_post(&ctl_sem);
    pthread_join(ctl, NULL);

    // The sigtimedwait thread still gets what it asked for by name.
    syscall(SYS_tgkill, getpid(), atomic_load(&stw_tid), SIGUSR2);
    pthread_join(stw, NULL);
    chk("sigtimedwait returns the SIGUSR2 it waits for", atomic_load(&stw_result), SIGUSR2);

    for (int i = 0; i < NTHREADS; i++)
        sem_post(&start);
    for (int i = 0; i < NTHREADS; i++)
        pthread_join(th[i], NULL);
    chk("sem_wait failures other than EINTR", atomic_load(&blocked_other), 0);

    return finish_suite("signal_group_blocked_eintr");
}
