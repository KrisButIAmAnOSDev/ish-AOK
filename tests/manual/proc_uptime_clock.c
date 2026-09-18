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
 *   - btime is stable across reads, and a realtime reading taken before and
 *     after a /proc/uptime read brackets it, which is what "the machine booted
 *     at btime" means;
 *   - sysinfo(2)'s uptime is whole seconds rounded UP (Linux's do_sysinfo adds
 *     one for any fraction), so it is never below /proc/uptime read just
 *     before it, and never more than a second above a reading just after;
 *   - the line is "%lu.%02lu %lu.%02lu": both fields have two digits of
 *     hundredths, and the second is the idle time summed over every CPU, not
 *     a copy of the first. AOK printed "%lu.%lu" (12.05 s as "12.5") with the
 *     uptime twice, and rounded uptime to tenths so that format read right;
 *   - resolution is finer than tenths: a 1.2 s poll with enough reads sees more
 *     values than whole tenths allow (Linux: about 120).
 */
#define _GNU_SOURCE

#include <ctype.h>
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

// "N.NN N.NN\n", Linux's exact shape.
static int uptime_line_is_linux_shaped(const char *raw) {
    const char *p = raw;
    for (int field = 0; field < 2; field++) {
        if (!isdigit((unsigned char) *p))
            return 0;
        while (isdigit((unsigned char) *p))
            p++;
        if (*p++ != '.')
            return 0;
        if (!isdigit((unsigned char) p[0]) || !isdigit((unsigned char) p[1]))
            return 0;
        p += 2;
        if (field == 0 && *p++ != ' ')
            return 0;
    }
    return *p == '\0';
}

// failf's fields are unsigned and printed as hex, so a negative value has to
// be handed over as its two's complement -- which reads as ffff...N and is
// unmistakable -- rather than cast from a double, where a negative saturates
// to zero on arm64 and silently reports "0".
static uint64_t signed_ms(double seconds) {
    return (uint64_t) (int64_t) (seconds * 1000.0 + (seconds < 0 ? -0.5 : 0.5));
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
    double prev = -1, prev_idle = -1;
    int distinct = 0, reads = 0, backward = 0, misshapen = 0, idle_is_uptime = 0, idle_backward = 0;
    double start = now_monotonic();
    while (now_monotonic() - start < 1.2) {
        double up;
        if (read_uptime(&up, raw, sizeof(raw)) != 0) {
            failf("read /proc/uptime", 0, 0, 0, 1, 0, 0);
            break;
        }
        reads++;
        if (!uptime_line_is_linux_shaped(raw)) {
            misshapen++;
            test_log_if(misshapen <= 3, "  not \"%%lu.%%02lu %%lu.%%02lu\": \"%s\"\n", raw);
        }
        const char *space = strchr(raw, ' ');
        double idle = space != NULL ? strtod(space + 1, NULL) : -1;
        if (space != NULL && strncmp(raw, space + 1, (size_t) (space - raw)) == 0 &&
                strlen(space + 1) == (size_t) (space - raw))
            idle_is_uptime++;
        if (prev_idle >= 0 && idle >= 0 && idle < prev_idle) {
            idle_backward++;
            test_log_if(idle_backward <= 3, "  idle went backward: \"%s\" -> \"%s\"\n", prev_raw, raw);
        }
        if (idle >= 0)
            prev_idle = idle;
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
    // Whole tenths give at most 13 in 1.2s; hundredths give up to 120.
    if (reads >= 120 && distinct <= 15)
        failf("/proc/uptime resolves hundredths, not tenths", (uint64_t) distinct, 0, 0, 16, 0, 0);
    if (misshapen != 0)
        failf("/proc/uptime is \"%lu.%02lu %lu.%02lu\"", (uint64_t) misshapen, 0, 0, 0, 0, 0);
    // The polling above keeps this process busy, so idle cannot track uptime
    // exactly in every one of these reads unless it is a copy of it.
    if (reads >= 20 && idle_is_uptime == reads)
        failf("/proc/uptime's second field is idle time, not uptime again",
              (uint64_t) idle_is_uptime, 0, 0, 0, 0, 0);
    if (idle_backward != 0)
        failf("/proc/uptime's idle time never goes backward", (uint64_t) idle_backward, 0, 0, 0, 0, 0);

    // btime: stable, and consistent with the wall clock minus uptime.
    //
    // Asserted as a BRACKET, not a tolerance. Realtime is read before AND
    // after both /proc reads, so every sample either file was built from --
    // including the two clock reads /proc/stat makes internally to derive
    // btime -- was taken inside [r0, r1], and the only slack the bounds need
    // is the quantisation each value really carries plus the MEASURED
    // duration of the reads. On a correct kernel it then holds however slow
    // the host is.
    //
    // It used to read realtime ONCE, before the /proc/uptime read, and allow
    // the difference a fixed [-0.05, 1.05]. That -0.05 was not a clock
    // tolerance at all: it was a budget for how long an open+read of
    // /proc/uptime takes, and under fakefs lock contention beside a
    // concurrent build it went straight through it -- the test failed on
    // 2026-09-17 with a `ninja` running alongside and passed on a quiet
    // machine, same binary. clock_boot_origin.c was rewritten away from the
    // same shape; this is the other half of it.
    long btime0 = read_btime();
    if (btime0 <= 0) {
        failf("read btime from /proc/stat", 0, 0, 0, 1, 0, 0);
    } else {
        int changed = 0;
        double slack = 1e9;
        for (int i = 0; i < 40; i++) {
            double r0 = now_realtime();
            long b = read_btime();
            double up;
            if (read_uptime(&up, NULL, 0) != 0)
                break;
            double r1 = now_realtime();
            if (b != btime0) {
                changed++;
                test_log_if(changed <= 3, "  btime changed: %ld -> %ld\n", btime0, b);
            }
            // Where the machine booted, in wall-clock seconds:
            //
            //   the uptime was sampled somewhere in [r0, r1] and /proc/uptime
            //   truncates to hundredths, so boot is in (r0 - up - 0.01, r1 - up];
            //   the kernel truncates its own uptime to a 10 ms tick before
            //   subtracting, which can only put ITS boot instant later;
            //   btime is that instant in WHOLE seconds, so it can sit up to a
            //   second lower and never higher;
            //   and the kernel's two reads are somewhere in the same [r0, r1],
            //   so they can be d = r1 - r0 apart either way.
            //
            // d is measured, not assumed: a slow read widens the window by
            // exactly what the read cost and by nothing else.
            double d = r1 - r0;
            double lo = r0 - up - 0.01 - 1.0 - d;
            double hi = r1 - up + 0.01 + d;
            double margin = (double) b - lo < hi - (double) b ? (double) b - lo : hi - (double) b;
            if (margin < slack)
                slack = margin;
            if ((double) b < lo || (double) b > hi) {
                test_log_if(1, "  btime %ld is outside [%.3f, %.3f]: realtime %.3f..%.3f "
                               "(%.3f s), uptime %.2f\n",
                            b, lo, hi, r0, r1, d, up);
                // Both edges, in signed milliseconds: room below btime, room
                // above it, and the measured read duration that sets the
                // width. Exactly one of the first two is negative, which says
                // which bound was missed. The old line reported
                // (uint64_t) (off * 1000) of a NEGATIVE double, which
                // saturates to 0 on arm64 -- the failure printed
                // got=0000000000000000 and said nothing at all.
                failf("btime is the wall clock minus uptime (ms below, ms above, ms read)",
                      signed_ms((double) b - lo), signed_ms(hi - (double) b),
                      signed_ms(d), 0, 0, 0);
                break;
            }
            usleep(25000);
        }
        test_logf("btime %ld, changed in %d of 40 reads, tightest margin %.3f s\n",
                  btime0, changed, slack);
        // btime is a constant: the machine booted when it booted. It is not
        // read from anywhere, though -- /proc/stat derives it every time from
        // realtime minus uptime -- so it is stable only if that subtraction
        // has room between the gap in its two reads and the next second
        // boundary. Losing uptime's sub-second part left it none, and this
        // caught it: 1 of 40 reads differed under load.
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
