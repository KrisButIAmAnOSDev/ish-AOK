/*
 * proc_loadavg_rises -- a busy task lifts the load average, and the average
 * does not depend on anyone reading it.
 *
 * AOK's /proc/loadavg read 0.00 after 65 seconds of one busy loop (native
 * Linux: 0.66). The guest average counted "alive minus io-blocked minus one",
 * the one being the reader -- but a task inside read(2) is already marked
 * io-blocked, so the reader was subtracted twice and one runnable task
 * counted as none. sysinfo(2) calls the same code without being blocked.
 *
 * The average also used to advance only when somebody read it, applying the
 * reader's instantaneous count to every 5-second step it had missed. So a
 * load that ended just before a read was never seen at all: `uptime` after a
 * build said 0.00. Linux samples on a timer whether or not anyone is looking,
 * and so does this test: it reads once before the busy spell and once after
 * it has ended, with nothing in between.
 *
 * Expectations, from Linux (kernel/sched/loadavg.c; measured on a Devuan 6
 * x86_64 box): the 1-minute average is sampled every 5 seconds and each
 * sample moves it by 1 - exp(-5/60) toward the number of runnable tasks. A
 * 15-second busy spell spans two or three samples, so from a baseline L0 the
 * average reaches at least L0 * 0.92^2 + 0.15; the read two seconds later can
 * have taken one idle sample on top. The assertion is looser than that on
 * both ends -- L0 is allowed to decay by up to four samples and the rise need
 * only be 0.08 -- so that a background load, a slow emulator, or a baseline
 * still decaying from an earlier test cannot fail it, while a kernel that
 * never counts the busy task, or never samples it, reads L0 or less.
 *
 * sysinfo(2) must report the same average: it is read between two reads of
 * /proc/loadavg and has to fall inside them.
 */
#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#define BUSY_SECONDS 15
#define AFTER_SECONDS 2

static int read_loadavg(double *one, char *raw, size_t raw_size) {
    FILE *f = fopen("/proc/loadavg", "r");
    if (f == NULL)
        return -1;
    char buf[256];
    int ok = fgets(buf, sizeof(buf), f) != NULL;
    fclose(f);
    if (!ok || sscanf(buf, "%lf", one) != 1)
        return -1;
    if (raw != NULL) {
        snprintf(raw, raw_size, "%s", buf);
        raw[strcspn(raw, "\n")] = '\0';
    }
    return 0;
}

static void sleep_seconds(double seconds) {
    struct timespec ts = {
        .tv_sec = (time_t) seconds,
        .tv_nsec = (long) ((seconds - (double) (time_t) seconds) * 1e9),
    };
    while (nanosleep(&ts, &ts) != 0)
        ;
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    double l0;
    char raw0[256], raw1[256];
    if (read_loadavg(&l0, raw0, sizeof(raw0)) != 0) {
        failf("read /proc/loadavg", 0, 0, 0, 1, 0, 0);
        return finish_suite("proc_loadavg_rises");
    }

    pid_t kid = fork();
    if (kid < 0) {
        perror("fork");
        return 2;
    }
    if (kid == 0) {
        volatile unsigned long sink = 0;
        for (;;)
            sink++;
    }
    sleep_seconds(BUSY_SECONDS);
    kill(kid, SIGKILL);
    waitpid(kid, NULL, 0);
    sleep_seconds(AFTER_SECONDS);

    double l1;
    if (read_loadavg(&l1, raw1, sizeof(raw1)) != 0) {
        failf("read /proc/loadavg", 0, 0, 0, 1, 0, 0);
        return finish_suite("proc_loadavg_rises");
    }
    // 1884/2048 is one 5-second step of the 1-minute average.
    double decay4 = 1.0;
    for (int i = 0; i < 4; i++)
        decay4 *= 1884.0 / 2048.0;
    double floor_expected = l0 * decay4 + 0.08;
    test_logf("before \"%s\", after %ds busy + %ds idle \"%s\", need 1-min >= %.3f\n",
              raw0, BUSY_SECONDS, AFTER_SECONDS, raw1, floor_expected);
    if (l1 < floor_expected) {
        test_log_if(1, "  before \"%s\" after \"%s\": 1-minute average %.2f, needed %.2f\n",
                    raw0, raw1, l1, floor_expected);
        failf("1-minute load rises under one busy task (x100)",
              (uint64_t) (l1 * 100), 0, 0, (uint64_t) (floor_expected * 100), 0, 0);
    }

    // sysinfo(2) and /proc/loadavg read one average.
    for (int i = 0; i < 5; i++) {
        double a, b;
        struct sysinfo si;
        if (read_loadavg(&a, NULL, 0) != 0 || sysinfo(&si) != 0 ||
                read_loadavg(&b, NULL, 0) != 0) {
            failf("read loadavg and sysinfo", 0, 0, 0, 1, 0, 0);
            break;
        }
        double s = (double) si.loads[0] / 65536.0;
        double lo = a < b ? a : b, hi = a < b ? b : a;
        // /proc/loadavg prints two decimals; allow its rounding.
        if (s < lo - 0.006 || s > hi + 0.006) {
            test_log_if(1, "  /proc/loadavg %.2f, sysinfo %.4f, /proc/loadavg %.2f\n", a, s, b);
            failf("sysinfo load matches /proc/loadavg (x100)",
                  (uint64_t) (s * 100), 0, 0, (uint64_t) (a * 100), 0, 0);
            break;
        }
        usleep(50000);
    }

    return finish_suite("proc_loadavg_rises");
}
