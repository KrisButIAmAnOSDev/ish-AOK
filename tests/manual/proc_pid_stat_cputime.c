// proc_pid_stat_cputime -- a process's CPU time is the time of all its threads,
// including threads that have already exited; a thread's is its own; and the
// children's time a process reports includes what those children reaped.
//
// Measured against Linux 6.12 (x86_64, glibc 2.41, both -m64 and -m32), where
// every check here passes. What AOK got wrong:
//
//   /proc/<pid>/stat fields 14 and 15 (utime, stime) reported ONE thread: the
//   one whose id names the directory. Linux reports the whole thread group
//   there (do_task_stat with whole=1), live threads and exited ones alike, and
//   only /proc/<pid>/task/<tid>/stat is per thread. So ps %CPU and TIME, top,
//   htop and btop under-reported every multithreaded program -- Thunar's GLib
//   worker spun a whole core while /proc said Thunar was idle.
//   /proc/<tid>/stat for a non-leader thread is a process entry too, and was
//   that thread alone.
//
//   Fields 16 and 17 (cutime, cstime) were a hardcoded 0, in both views.
//
//   A zombie's /proc/<pid>/stat said 0 for a process that had burned CPU on a
//   thread still running when it exited.
//
//   wait4's rusage, getrusage(RUSAGE_CHILDREN) and times()'s tms_cutime left
//   out GRANDCHILDREN: a reaped child passed on only its own usage, not that of
//   the children it had reaped itself (Linux: wait4 reports RUSAGE_BOTH, and
//   the parent accumulates cutime += child + child's cutime). A parent that
//   reaped a shell was charged nothing for the programs the shell ran, and
//   RUSAGE_CHILDREN after a compile left out cc1, as and ld.
//
// Already right, and checked here so they stay that way: getrusage
// (RUSAGE_SELF) as the process total, wait4 for a multithreaded child, and the
// per-thread view.
//
// Every figure is CPU time the test measured itself -- a spinner runs until its
// own CLOCK_THREAD_CPUTIME_ID has advanced by a set amount, and a child reports
// its own getrusage(RUSAGE_SELF) through a pipe -- and every check is a ratio
// of those (at least 80%), so a heavily loaded host slows the test down but
// does not change a verdict.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#define THREAD_SPIN_S      0.5    // each of the two worker threads
#define CHILD_SPIN_S       0.3    // a child's worker thread
#define GRANDCHILD_SPIN_S  0.5    // more than a child, so losing it is unmissable
#define SPIN_WALL_CAP_S    60.0   // backstop if the host starves a spinner

static long clk_tck;

struct stat_cpu {
    long long ticks;    // utime + stime
    long long cticks;   // cutime + cstime
    char state;
    int ok;
};

// Fields 14-17 of a /proc stat line. The comm field can hold spaces and
// parentheses, so parse from the LAST ')'.
static struct stat_cpu read_stat_cpu(const char *path) {
    struct stat_cpu r = {0};
    char buf[1024];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return r;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return r;
    buf[n] = '\0';
    char *p = strrchr(buf, ')');
    if (p == NULL)
        return r;
    int ppid, pgrp, session, tty, tpgid;
    unsigned flags;
    unsigned long minflt, cminflt, majflt, cmajflt;
    long long utime, stime, cutime, cstime;
    if (sscanf(p + 2, "%c %d %d %d %d %d %u %lu %lu %lu %lu %lld %lld %lld %lld", &r.state, &ppid,
               &pgrp, &session, &tty, &tpgid, &flags, &minflt, &cminflt, &majflt, &cmajflt, &utime,
               &stime, &cutime, &cstime) != 15)
        return r;
    r.ticks = utime + stime;
    r.cticks = cutime + cstime;
    r.ok = 1;
    return r;
}

static double ts_secs(struct timespec ts) {
    return (double) ts.tv_sec + ts.tv_nsec / 1e9;
}

static double tv_secs(struct timeval tv) {
    return (double) tv.tv_sec + tv.tv_usec / 1e6;
}

static double monotonic_secs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts_secs(ts);
}

static double thread_cpu_secs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts_secs(ts);
}

static double rusage_secs(int who) {
    struct rusage r;
    if (getrusage(who, &r) != 0)
        return -1;
    return tv_secs(r.ru_utime) + tv_secs(r.ru_stime);
}

// Burn this thread's own CPU until it has used `secs` more of it.
static void spin(double secs) {
    double base = thread_cpu_secs(), start = monotonic_secs();
    volatile unsigned long x = 0;
    while (thread_cpu_secs() - base < secs && monotonic_secs() - start < SPIN_WALL_CAP_S)
        for (int i = 0; i < 100000; i++)
            x += (unsigned long) i;
}

__attribute__((format(printf, 3, 4)))
static void check(const char *name, int ok, const char *fmt, ...) {
    if (ok) {
        test_logf("  ok   %s\n", name);
        return;
    }
    printf("FAIL %s: ", name);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    failures_total++;
}

static double ticks(double secs) {
    return secs * (double) clk_tck;
}

// ---------------------------------------------------------------- threads

static double worker_a_cpu, worker_b_cpu;
static pid_t worker_b_tid;
static int b_ready[2], b_release[2];

static void *worker_a(void *arg) {
    (void) arg;
    spin(THREAD_SPIN_S);
    worker_a_cpu = thread_cpu_secs();
    return NULL;
}

static void *worker_b(void *arg) {
    (void) arg;
    worker_b_tid = (pid_t) syscall(SYS_gettid);
    spin(THREAD_SPIN_S);
    worker_b_cpu = thread_cpu_secs();
    char c = 1;
    if (write(b_ready[1], &c, 1) != 1)
        return NULL;
    // Parked, so its CPU time holds still while the main thread reads.
    if (read(b_release[0], &c, 1) != 1)
        return NULL;
    return NULL;
}

static void test_threads(void) {
    pid_t pid = getpid();
    char path[96];

    // A spins and exits before anything is read: its time must stay counted.
    pthread_t a, b;
    if (pthread_create(&a, NULL, worker_a, NULL) != 0) {
        check("threads_setup", 0, "pthread_create failed: %s", strerror(errno));
        return;
    }
    pthread_join(a, NULL);

    // B spins and then parks, alive, while the files are read.
    if (pipe(b_ready) != 0 || pipe(b_release) != 0 ||
            pthread_create(&b, NULL, worker_b, NULL) != 0) {
        check("threads_setup", 0, "pipe/pthread_create failed: %s", strerror(errno));
        return;
    }
    char c;
    if (read(b_ready[0], &c, 1) != 1) {
        check("threads_setup", 0, "worker B never reported");
        return;
    }

    double self_before = rusage_secs(RUSAGE_SELF);
    struct stat_cpu proc = read_stat_cpu("/proc/self/stat");
    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", (int) pid);
    struct stat_cpu task_main = read_stat_cpu(path);
    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", (int) worker_b_tid);
    struct stat_cpu task_b = read_stat_cpu(path);
    snprintf(path, sizeof(path), "/proc/%d/stat", (int) worker_b_tid);
    struct stat_cpu tid_entry = read_stat_cpu(path);

    double a_ticks = ticks(worker_a_cpu), b_ticks = ticks(worker_b_cpu);
    test_logf("threads: A spun %.0f ticks (exited), B %.0f (live); RUSAGE_SELF %.0f\n", a_ticks, b_ticks,
              ticks(self_before));
    test_logf("threads: /proc/self/stat %lld, task/<main> %lld, task/<B> %lld, /proc/<B tid>/stat %lld\n",
              proc.ticks, task_main.ticks, task_b.ticks, tid_entry.ticks);

    if (!proc.ok || !task_main.ok || !task_b.ok || !tid_entry.ok) {
        check("threads_read", 0, "could not read a stat file (process %d, task %d, task B %d, tid %d)",
              proc.ok, task_main.ok, task_b.ok, tid_entry.ok);
    } else {
        check("stat_counts_live_and_exited_threads", proc.ticks >= 0.8 * (a_ticks + task_b.ticks),
              "/proc/self/stat utime+stime %lld, but exited thread A spun %.0f ticks and live "
              "thread B has %lld -- Linux reports the whole thread group",
              proc.ticks, a_ticks, task_b.ticks);
        check("stat_matches_rusage_self", proc.ticks >= 0.8 * ticks(self_before),
              "/proc/self/stat utime+stime %lld, but getrusage(RUSAGE_SELF) read just before "
              "says %.0f ticks", proc.ticks, ticks(self_before));
        check("tid_entry_is_the_process", tid_entry.ticks >= 0.8 * (a_ticks + task_b.ticks),
              "/proc/<B tid>/stat utime+stime %lld is not the process total (A %.0f + B %lld) -- "
              "on Linux /proc/<tid> is a process entry", tid_entry.ticks, a_ticks, task_b.ticks);
        check("task_stat_is_one_thread",
              task_b.ticks >= 0.8 * b_ticks && task_b.ticks < 0.8 * (a_ticks + b_ticks),
              "/proc/self/task/<B>/stat utime+stime %lld; B itself spun %.0f ticks and the "
              "process total is over %.0f", task_b.ticks, b_ticks, a_ticks + b_ticks);
        check("task_stat_main_is_its_own", 2 * task_main.ticks < a_ticks + b_ticks,
              "/proc/self/task/<main>/stat utime+stime %lld for an idle main thread, against "
              "%.0f ticks the workers spun", task_main.ticks, a_ticks + b_ticks);
    }

    c = 1;
    if (write(b_release[1], &c, 1) != 1)
        check("threads_setup", 0, "could not release worker B");
    pthread_join(b, NULL);

    struct stat_cpu after = read_stat_cpu("/proc/self/stat");
    test_logf("threads: /proc/self/stat %lld after B exited too\n", after.ticks);
    check("stat_after_every_worker_exited",
          after.ok && after.ticks >= proc.ticks && after.ticks >= 0.8 * (a_ticks + b_ticks),
          "/proc/self/stat utime+stime %lld once both workers exited (was %lld; they spun %.0f)",
          after.ticks, proc.ticks, a_ticks + b_ticks);
    close(b_ready[0]);
    close(b_ready[1]);
    close(b_release[0]);
    close(b_release[1]);
}

// --------------------------------------------------------------- children

static int report_fd = -1;

static void report_self_and_exit(void) {
    double self = rusage_secs(RUSAGE_SELF);
    if (write(report_fd, &self, sizeof(self)) != sizeof(self))
        _exit(3);
    _exit(0);
}

static double read_report(int fd) {
    double v;
    if (read(fd, &v, sizeof(v)) != sizeof(v))
        return -1;
    return v;
}

static void *child_worker(void *arg) {
    (void) arg;
    spin(CHILD_SPIN_S);
    return NULL;
}

static volatile int forever_worker_spun;
static void *child_worker_forever(void *arg) {
    (void) arg;
    double base = thread_cpu_secs();
    volatile unsigned long x = 0;
    for (;;) {
        for (int i = 0; i < 100000; i++)
            x += (unsigned long) i;
        if (!forever_worker_spun && thread_cpu_secs() - base >= CHILD_SPIN_S)
            forever_worker_spun = 1;
    }
    return NULL;
}

static double wait4_secs(pid_t pid) {
    int status;
    struct rusage ru;
    pid_t got;
    do
        got = wait4(pid, &status, 0, &ru);
    while (got < 0 && errno == EINTR);
    if (got != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;
    return tv_secs(ru.ru_utime) + tv_secs(ru.ru_stime);
}

static void test_children(void) {
    int rep[2];
    if (pipe(rep) != 0) {
        check("children_setup", 0, "pipe failed: %s", strerror(errno));
        return;
    }
    report_fd = rep[1];
    pid_t pid = getpid();
    char path[96];

    double children_before = rusage_secs(RUSAGE_CHILDREN);
    struct tms tms_before;
    times(&tms_before);
    struct stat_cpu proc_before = read_stat_cpu("/proc/self/stat");
    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", (int) pid);
    struct stat_cpu task_before = read_stat_cpu(path);

    // 1: a worker thread does the child's work and is joined.
    pid_t c1 = fork();
    if (c1 == 0) {
        pthread_t t;
        if (pthread_create(&t, NULL, child_worker, NULL) != 0)
            _exit(2);
        pthread_join(t, NULL);
        report_self_and_exit();
    }
    double c1_self = read_report(rep[0]);
    double c1_wait = wait4_secs(c1);
    test_logf("children: joined-worker child used %.3fs, wait4 says %.3fs\n", c1_self, c1_wait);
    check("wait4_multithreaded_child", c1_self > 0 && c1_wait >= 0.8 * c1_self,
          "wait4 rusage %.3fs for a child whose worker thread made it %.3fs", c1_wait, c1_self);

    // 2: the worker is still spinning when the child exits. Read the zombie's
    // stat before reaping it.
    pid_t c2 = fork();
    if (c2 == 0) {
        pthread_t t;
        if (pthread_create(&t, NULL, child_worker_forever, NULL) != 0)
            _exit(2);
        while (!forever_worker_spun)
            usleep(1000);
        report_self_and_exit();
    }
    double c2_self = read_report(rep[0]);
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    int zombie_ok;
    do
        zombie_ok = waitid(P_PID, (id_t) c2, &info, WEXITED | WNOWAIT);
    while (zombie_ok < 0 && errno == EINTR);
    snprintf(path, sizeof(path), "/proc/%d/stat", (int) c2);
    struct stat_cpu zombie = read_stat_cpu(path);
    double c2_wait = wait4_secs(c2);
    test_logf("children: exited-with-live-worker child used %.3fs; its zombie stat %lld (state %c), "
              "wait4 %.3fs\n", c2_self, zombie.ticks, zombie.state ? zombie.state : '?', c2_wait);
    check("zombie_stat_counts_its_threads",
          zombie_ok == 0 && zombie.ok && c2_self > 0 && zombie.ticks >= 0.8 * ticks(c2_self),
          "zombie /proc/<pid>/stat utime+stime %lld (read %s) for a child that used %.0f ticks",
          zombie.ticks, zombie.ok ? "ok" : "failed", ticks(c2_self));
    check("wait4_worker_live_at_exit", c2_self > 0 && c2_wait >= 0.8 * c2_self,
          "wait4 rusage %.3fs for a child that used %.3fs", c2_wait, c2_self);

    // 3: the child reaps a grandchild that does the work.
    pid_t c3 = fork();
    if (c3 == 0) {
        pid_t g = fork();
        if (g == 0) {
            spin(GRANDCHILD_SPIN_S);
            report_self_and_exit();
        }
        int status;
        if (waitpid(g, &status, 0) != g)
            _exit(2);
        report_self_and_exit();
    }
    double g_self = read_report(rep[0]);
    double c3_self = read_report(rep[0]);
    double c3_wait = wait4_secs(c3);
    test_logf("children: grandchild used %.3fs, its parent %.3fs; wait4 of that parent %.3fs\n", g_self,
              c3_self, c3_wait);
    check("wait4_includes_grandchildren", g_self > 0 && c3_self >= 0 && c3_wait >= 0.8 * (g_self + c3_self),
          "wait4 rusage %.3fs for a child that used %.3fs and reaped a grandchild that used %.3fs",
          c3_wait, c3_self, g_self);

    double total = c1_self + c2_self + g_self + c3_self;
    double children_after = rusage_secs(RUSAGE_CHILDREN);
    struct tms tms_after;
    times(&tms_after);
    struct stat_cpu proc_after = read_stat_cpu("/proc/self/stat");
    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", (int) pid);
    struct stat_cpu task_after = read_stat_cpu(path);
    long tms_delta = (long) ((tms_after.tms_cutime + tms_after.tms_cstime) -
                             (tms_before.tms_cutime + tms_before.tms_cstime));
    test_logf("children: they used %.0f ticks; RUSAGE_CHILDREN +%.0f, times() +%ld, "
              "/proc/self/stat +%lld, task/<main> +%lld\n", ticks(total),
              ticks(children_after - children_before), tms_delta, proc_after.cticks - proc_before.cticks,
              task_after.cticks - task_before.cticks);
    check("rusage_children_includes_grandchildren", children_after - children_before >= 0.8 * total,
          "getrusage(RUSAGE_CHILDREN) grew %.3fs, but the reaped children and grandchild used %.3fs",
          children_after - children_before, total);
    check("times_cutime_includes_grandchildren", tms_delta >= 0.8 * ticks(total),
          "times() tms_cutime+tms_cstime grew %ld ticks, but the children used %.0f", tms_delta,
          ticks(total));
    check("stat_cutime", proc_before.ok && proc_after.ok &&
                             proc_after.cticks - proc_before.cticks >= 0.8 * ticks(total),
          "/proc/self/stat cutime+cstime grew %lld ticks (from %lld), but the children used %.0f",
          proc_after.cticks - proc_before.cticks, proc_before.cticks, ticks(total));
    check("task_stat_cutime_is_the_process", task_before.ok && task_after.ok &&
                             task_after.cticks - task_before.cticks >= 0.8 * ticks(total),
          "/proc/self/task/<main>/stat cutime+cstime grew %lld ticks, but the children used %.0f "
          "-- on Linux both views report the process's children",
          task_after.cticks - task_before.cticks, ticks(total));
    close(rep[0]);
    close(rep[1]);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(test_watchdog_secs(180));
    clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0)
        clk_tck = 100;
    test_threads();
    test_children();
    return finish_suite("proc_pid_stat_cputime");
}
