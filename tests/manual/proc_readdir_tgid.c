/*
 * proc_readdir_tgid -- /proc lists processes, not threads.
 *
 * Linux lists only thread-group leaders when /proc is read as a directory
 * (proc_pid_readdir walks tgids). A thread's own /proc/<tid> still opens by
 * name, and /proc/<pid>/task lists every thread, but ps, pgrep and top build
 * their process lists from the /proc listing. AOK listed every live task, so
 * each thread of a program showed up as another process: waybar's 13 threads
 * were 13 rows in ps, and pgrep -x waybar printed 13 ids for one process.
 *
 * Checked against Linux:
 *   - a process's own pid is listed, and a forked child's is too;
 *   - its threads' ids are not listed, yet /proc/<tid>/status opens and names
 *     the process as its Tgid, and /proc/self/task lists them all;
 *   - a process whose main thread has exited while another thread runs is still
 *     listed under its pid, and that thread's id still is not.
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define NTHREADS 3

static void check(const char *label, int cond) {
    if (cond) {
        test_logf("ok   %s\n", label);
    } else {
        printf("FAIL %s\n", label);
        failures_total++;
    }
}

// Whether `id` appears as an entry when /proc is read as a directory.
static int proc_lists(pid_t id) {
    DIR *dir = opendir("/proc");
    if (dir == NULL)
        return -1;
    char want[16];
    snprintf(want, sizeof(want), "%d", (int) id);
    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, want) == 0) {
            found = 1;
            break;
        }
    }
    closedir(dir);
    return found;
}

// The Tgid line of /proc/<tid>/status, or -1.
static pid_t status_tgid(pid_t tid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int) tid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    char line[256];
    pid_t tgid = -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "Tgid:", 5) == 0) {
            tgid = (pid_t) strtol(line + 5, NULL, 10);
            break;
        }
    }
    fclose(f);
    return tgid;
}

static int task_dir_lists(pid_t tid) {
    DIR *dir = opendir("/proc/self/task");
    if (dir == NULL)
        return -1;
    char want[16];
    snprintf(want, sizeof(want), "%d", (int) tid);
    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, want) == 0) {
            found = 1;
            break;
        }
    }
    closedir(dir);
    return found;
}

static int gate[2];                 // the threads block reading this
static pid_t tids[NTHREADS];
static pthread_mutex_t tid_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tid_cond = PTHREAD_COND_INITIALIZER;
static int started;

static void *parked_thread(void *arg) {
    int slot = (int) (long) arg;
    pthread_mutex_lock(&tid_lock);
    tids[slot] = (pid_t) syscall(SYS_gettid);
    started++;
    pthread_cond_broadcast(&tid_cond);
    pthread_mutex_unlock(&tid_lock);
    char byte;
    while (read(gate[0], &byte, 1) < 0 && errno == EINTR)
        ;
    return NULL;
}

static void test_threads_not_listed(void) {
    if (pipe(gate) != 0) {
        check("pipe", 0);
        return;
    }
    pthread_t threads[NTHREADS];
    for (long i = 0; i < NTHREADS; i++) {
        if (pthread_create(&threads[i], NULL, parked_thread, (void *) i) != 0) {
            check("pthread_create", 0);
            return;
        }
    }
    pthread_mutex_lock(&tid_lock);
    while (started < NTHREADS)
        pthread_cond_wait(&tid_cond, &tid_lock);
    pthread_mutex_unlock(&tid_lock);

    char label[160];
    snprintf(label, sizeof(label), "own pid %d is listed in /proc", (int) getpid());
    check(label, proc_lists(getpid()) == 1);
    for (int i = 0; i < NTHREADS; i++) {
        snprintf(label, sizeof(label), "thread %d is not listed in /proc", (int) tids[i]);
        check(label, proc_lists(tids[i]) == 0);
        pid_t tgid = status_tgid(tids[i]);
        snprintf(label, sizeof(label), "/proc/%d/status still opens, Tgid %d (got %d)",
                 (int) tids[i], (int) getpid(), (int) tgid);
        check(label, tgid == getpid());
        snprintf(label, sizeof(label), "/proc/self/task lists thread %d", (int) tids[i]);
        check(label, task_dir_lists(tids[i]) == 1);
    }

    for (int i = 0; i < NTHREADS; i++) {
        if (write(gate[1], "x", 1) != 1)
            check("release a thread", 0);
    }
    for (int i = 0; i < NTHREADS; i++)
        pthread_join(threads[i], NULL);
    close(gate[0]);
    close(gate[1]);
}

// A child whose main thread exits while a second thread keeps the process
// alive. The child reports that thread's id through `report`, then the main
// thread calls pthread_exit; the thread waits on `release`.
static int child_release;

static void *child_thread(void *arg) {
    int report = (int) (long) arg;
    pid_t tid = (pid_t) syscall(SYS_gettid);
    if (write(report, &tid, sizeof(tid)) != sizeof(tid))
        _exit(3);
    char byte;
    while (read(child_release, &byte, 1) < 0 && errno == EINTR)
        ;
    _exit(0);
}

static void test_leader_exited_still_listed(void) {
    int report[2], release[2];
    if (pipe(report) != 0 || pipe(release) != 0) {
        check("pipes", 0);
        return;
    }
    pid_t child = fork();
    if (child < 0) {
        check("fork", 0);
        return;
    }
    if (child == 0) {
        close(report[0]);
        close(release[1]);
        child_release = release[0];
        pthread_t thread;
        if (pthread_create(&thread, NULL, child_thread, (void *) (long) report[1]) != 0)
            _exit(2);
        pthread_exit(NULL);
    }
    close(report[1]);
    close(release[0]);

    pid_t tid = -1;
    ssize_t n = read(report[0], &tid, sizeof(tid));
    char label[160];
    snprintf(label, sizeof(label), "child %d reported its thread (%zd bytes, tid %d)",
             (int) child, n, (int) tid);
    check(label, n == sizeof(tid) && tid > 0);
    if (n == sizeof(tid) && tid > 0) {
        // Give the child's main thread time to finish exiting.
        usleep(300000);
        snprintf(label, sizeof(label), "child %d is listed although its main thread exited",
                 (int) child);
        check(label, proc_lists(child) == 1);
        snprintf(label, sizeof(label), "the child's surviving thread %d is not listed", (int) tid);
        check(label, proc_lists(tid) == 0);
    }

    if (write(release[1], "x", 1) != 1)
        check("release the child", 0);
    int status = 0;
    waitpid(child, &status, 0);
    snprintf(label, sizeof(label), "child exited cleanly (status 0x%x)", status);
    check(label, WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(report[0]);
    close(release[1]);
}

static void test_child_listed(void) {
    int hold[2];
    if (pipe(hold) != 0) {
        check("pipe", 0);
        return;
    }
    pid_t child = fork();
    if (child == 0) {
        close(hold[1]);
        char byte;
        while (read(hold[0], &byte, 1) < 0 && errno == EINTR)
            ;
        _exit(0);
    }
    close(hold[0]);
    char label[160];
    snprintf(label, sizeof(label), "forked child %d is listed in /proc", (int) child);
    check(label, child > 0 && proc_lists(child) == 1);
    close(hold[1]);
    waitpid(child, NULL, 0);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    test_child_listed();
    test_threads_not_listed();
    test_leader_exited_still_listed();

    return finish_suite("proc_readdir_tgid");
}
