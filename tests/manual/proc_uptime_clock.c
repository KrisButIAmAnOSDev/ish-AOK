/*
 * proc_uptime_clock -- /proc/uptime runs on a monotonic clock with
 * sub-second resolution, and agrees with btime and sysinfo(2).
 *
 * AOK computed uptime as whole seconds of WALL clock since boot: `(now.tv_sec
 * - boot_time) * 100`. So /proc/uptime only ever read N.0, it would jump if
 * the host clock were set, and the /proc/stat idle time derived from it went
 * backward between two reads in the same second (see proc_stat_monotonic).
 *
 * Checked against Linux, which is the oracle for every expectation here:
 *   - consecutive readings never decrease, parsed the way every consumer
 *     parses the file (as a decimal number);
 *   - a second of polling sees many distinct values (Linux: 100 per second;
 *     the assertion only asks for better than whole seconds);
 *   - btime is stable across reads, and btime + uptime lands within a second
 *     of the wall clock, which is what "the machine booted at btime" means;
 *   - sysinfo(2)'s uptime is whole seconds rounded UP (Linux's do_sysinfo adds
 *     one for any fraction), so it is never below /proc/uptime read just
 *     before it, and never more than a second above a reading just after.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static double now_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static double now_realtime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static int read_uptime(double *up, char *raw, size_t raw_size) {
    FILE *f = fopen("/proc/uptime", "r");
    if (f == NULL)
        return -1;
    char buf[128];
    int ok = fgets(buf, sizeof(buf), f) != NULL;
    fclose(f);
    if (!ok)
        return -1;
    if (raw != NULL) {
        snprintf(raw, raw_size, "%s", buf);
        raw[strcspn(raw, "\n")] = '\0';
    }
    char *end;
    *up = strtod(buf, &end);
    return end == buf ? -1 : 0;
}

static long read_btime(void) {
    FILE *f = fopen("/proc/stat", "r");
    if (f == NULL)
        return -1;
    char line[512];
    long btime = -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "btime ", 6) == 0) {
            btime = strtol(line + 6, NULL, 10);
            break;
        }
    }
    fclose(f);
    return btime;
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    char raw[128], prev_raw[128] = "";
    double prev = -1;
    int distinct = 0, reads = 0, backward = 0;
    double start = now_monotonic();
    while (now_monotonic() - start < 1.2) {
        double up;
        if (read_uptime(&up, raw, sizeof(raw)) != 0) {
            failf("read /proc/uptime", 0, 0, 0, 1, 0, 0);
            break;
        }
        reads++;
        if (up != prev)
            distinct++;
        if (prev >= 0 && up < prev) {
            backward++;
            test_log_if(backward <= 5, "  uptime went backward: \"%s\" -> \"%s\"\n", prev_raw, raw);
        }
        prev = up;
        snprintf(prev_raw, sizeof(prev_raw), "%s", raw);
        usleep(2000);
    }
    test_logf("%d reads in 1.2s, %d distinct values, %d backward, last \"%s\"\n",
              reads, distinct, backward, prev_raw);
    if (backward != 0)
        failf("/proc/uptime went backward", (uint64_t) backward, 0, 0, 0, 0, 0);
    // Whole-second resolution gives at most 2 or 3 values in 1.2s.
    if (reads >= 20 && distinct < 5)
        failf("/proc/uptime has sub-second resolution", (uint64_t) distinct, 0, 0, 5, 0, 0);

    // btime: stable, and consistent with the wall clock minus uptime.
    long btime0 = read_btime();
    if (btime0 <= 0) {
        failf("read btime from /proc/stat", 0, 0, 0, 1, 0, 0);
    } else {
        int changed = 0;
        double worst = 0;
        for (int i = 0; i < 40; i++) {
            long b = read_btime();
            double up;
            double real = now_realtime();
            if (read_uptime(&up, NULL, 0) != 0)
                break;
            if (b != btime0)
                changed++;
            // real - up is the moment of boot; btime is that moment in whole
            // seconds, so the difference is in [0, 1), plus read skew.
            double off = real - up - (double) b;
            if (off < 0 ? -off > worst : off > worst)
                worst = off;
            if (off < -0.05 || off > 1.05) {
                test_log_if(1, "  btime %ld, realtime %.3f, uptime %.2f: offset %.3f\n",
                            b, real, up, off);
                failf("btime + uptime matches the wall clock (ms)",
                      (uint64_t) (off * 1000), 0, 0, 0, 0, 0);
                break;
            }
            usleep(25000);
        }
        test_logf("btime %ld, changed in %d of 40 reads, worst offset %.3f\n",
                  btime0, changed, worst);
        if (changed != 0)
            failf("btime is stable", (uint64_t) changed, 0, 0, 0, 0, 0);
    }

    // sysinfo(2) rounds up.
    for (int i = 0; i < 20; i++) {
        double before, after;
        struct sysinfo si;
        if (read_uptime(&before, NULL, 0) != 0 || sysinfo(&si) != 0 ||
                read_uptime(&after, NULL, 0) != 0) {
            failf("read uptime and sysinfo", 0, 0, 0, 1, 0, 0);
            break;
        }
        if ((double) si.uptime < before || (double) si.uptime > after + 1.0) {
            test_log_if(1, "  /proc/uptime %.2f, sysinfo %ld, /proc/uptime %.2f\n",
                        before, (long) si.uptime, after);
            failf("sysinfo uptime is /proc/uptime rounded up",
                  (uint64_t) si.uptime, 0, 0, (uint64_t) before, 0, 0);
            break;
        }
        usleep(37000);
    }

    return finish_suite("proc_uptime_clock");
}
