// Compressed memory must fill its pool WITHOUT waiting for memory pressure.
//
// WHAT THIS EXISTS TO CATCH, which is a policy regression rather than a crash.
//
// Every gate around eviction was built for a swap FILE: reclaim writes to the
// user's flash, that is metered against a 24-hour budget, and flash wears out,
// so waiting until the app is nearly dead before evicting is right. RAM-only
// mode (zram) inherited that caution and it was wrong -- swap_write_frame takes
// the ram_only branch and returns _ENOSPC before it reaches a file descriptor,
// so nothing is written and none of that cost exists. Measured on an A9 device
// before the fix:
//
//     kswapd  running, 864 passes, 0 bytes reclaimed
//     pool    0 KB of a 494 MB cap        headroom 931 MB
//     write_window  0 of 4294967296 bytes used in the last 24h
//
// 864 background passes, a half-gigabyte pool the user had asked for in
// Settings, and not one frame ever compressed. So RAM-only reclaim no longer
// consults headroom at all: it runs while the pool has room and stops when it
// is full (kernel/swap.c, swap_kswapd_should_reclaim).
//
// THE ASSERTION IS DELIBERATELY MADE WHERE THERE IS NO PRESSURE. The test
// refuses to run unless headroom is comfortably ABOVE the file-backed watermark
// -- the point at which the old rule would have started reclaiming anyway --
// and fails if nothing is stored while it stays there. A pass therefore means
// "compressed cold memory with the machine nowhere near its ceiling", which is
// the whole behaviour change; testing under pressure would have passed before
// the fix and after it, and proved nothing.
//
// It fails rather than skips when nothing is stored, because "the compressor
// never ran" IS the bug: a pass that reclaims nothing looks exactly like a pass
// that had nothing to reclaim.
//
//     ISH_GUEST_MEM_BUDGET_MB=1200 ISH_GUEST_MEM_HEADROOM_MB=100 \
//     ISH_GUEST_SWAP_MB=0 ISH_GUEST_ZSWAP_MB=128 \
//         ./build/ish -f <root> /AOK/tests/zram_idle_reclaim
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

// The aging clock needs several sweeps at 500 ms before a frame reads cold, so
// this is sweeps-worth of patience, not a guess.
#define SETTLE_SECONDS 60
#define REGION_MB      192
#define RUN_BYTES      64

// Compressible but position-dependent, the same shape zswap_fork_cow.c uses:
// runs give lz4 something to work with so frames really enter the pool, and the
// run value hashes the block index, so a frame restored to the wrong offset
// would not match.
static unsigned char run_byte(size_t block) {
    uint64_t x = (uint64_t) block * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    return (unsigned char) (x & 0xff);
}

static unsigned long long proc_field(const char *path, const char *name, int *ok) {
    *ok = 0;
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 0;
    char line[256];
    unsigned long long value = 0;
    size_t namelen = strlen(name);
    while (fgets(line, sizeof line, f) != NULL) {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncmp(p, name, namelen) != 0 || (p[namelen] != ' ' && p[namelen] != '\t'))
            continue;
        p += namelen;
        while (*p == ' ' || *p == '\t')
            p++;
        value = strtoull(p, NULL, 10);
        *ok = 1;
        break;
    }
    fclose(f);
    return value;
}

// True when the area has no file behind it, which is the mode under test.
static int swap_is_ram_only(void) {
    FILE *f = fopen("/proc/ish/swap", "r");
    if (f == NULL)
        return 0;
    char line[256];
    int ram_only = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "backing", 7) == 0 && strstr(line, "NO file") != NULL) {
            ram_only = 1;
            break;
        }
    }
    fclose(f);
    return ram_only;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(300));

    int ok = 0;
    proc_field("/proc/ish/zswap", "stores", &ok);
    if (!ok) {
        printf("zram_idle_reclaim: SKIP (compressed memory is off -- it is "
               "opt-in; set ISH_GUEST_ZSWAP_MB with ISH_GUEST_SWAP_MB=0)\n");
        return 0;
    }
    if (!swap_is_ram_only()) {
        printf("zram_idle_reclaim: SKIP (there is a swap file, so the "
               "conservative file-backed watermark applies and reclaim without "
               "pressure is not the contract)\n");
        return 0;
    }

    // The watermark is on the PROCESS budget, which mem_guard prints as
    // "ceiling" and "headroom". The "total"/"available" lines above them are
    // the machine's, and reading those would compare against a different
    // quantity entirely.
    int floor_ok = 0;
    unsigned long long floor_mb = proc_field("/proc/ish/mem_guard", "floor", &floor_ok);
    unsigned long long headroom_mb = proc_field("/proc/ish/mem_guard", "headroom", &ok);
    if (!ok || !floor_ok) {
        printf("zram_idle_reclaim: SKIP (/proc/ish/mem_guard has no "
               "floor/headroom)\n");
        return 0;
    }

    // Where the OLD rule would have started. Everything below is asserted to
    // happen while headroom stays above this, so pressure cannot be the reason.
    unsigned long long file_mark = floor_mb * 2;
    size_t region = (size_t) REGION_MB * 1024 * 1024;
    if (headroom_mb < file_mark + REGION_MB + 64) {
        printf("zram_idle_reclaim: SKIP (only %llu MB of headroom; allocating "
               "%d MB would reach the %llu MB file-backed mark, and then "
               "reclaim would prove nothing new)\n",
               headroom_mb, REGION_MB, file_mark);
        return 0;
    }

    unsigned char *buf = mmap(NULL, region, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        printf("zram_idle_reclaim: SKIP (could not map %d MB: %s)\n",
               REGION_MB, strerror(errno));
        return 0;
    }
    for (size_t block = 0; block * RUN_BYTES < region; block++)
        memset(buf + block * RUN_BYTES, run_byte(block), RUN_BYTES);

    unsigned long long before = proc_field("/proc/ish/zswap", "stores", &ok);
    test_log_if(0, "floor %llu MB, so the old rule would wait for %llu MB of "
                "headroom. Touched %d MB and left it cold at %llu MB headroom, "
                "which is %llu MB clear of that mark.\n",
                floor_mb, file_mark, REGION_MB, headroom_mb,
                headroom_mb - file_mark);

    // Leave it alone. Nothing here re-touches the region: the aging clock only
    // offers frames that have read cold across several sweeps, so the test has
    // to actually stop using the memory it just wrote.
    unsigned long long after = before;
    unsigned long long lowest_headroom = headroom_mb;
    int settled = 0;
    for (int t = 0; t < SETTLE_SECONDS; t++) {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        unsigned long long now = proc_field("/proc/ish/mem_guard", "headroom", &ok);
        if (ok && now < lowest_headroom)
            lowest_headroom = now;
        after = proc_field("/proc/ish/zswap", "stores", &ok);
        if (ok && after > before) {
            settled = t + 1;
            break;
        }
    }

    if (after <= before) {
        printf("zram_idle_reclaim: FAIL (nothing was compressed in %d s with "
               "%d MB of cold memory sitting there and a pool with room. stores "
               "stayed %llu. RAM-only reclaim is supposed to fill the pool "
               "rather than wait for pressure -- see swap_kswapd_should_reclaim)\n",
               SETTLE_SECONDS, REGION_MB, before);
        munmap(buf, region);
        return 1;
    }
    if (lowest_headroom < file_mark) {
        printf("zram_idle_reclaim: FAIL (reclaim happened, but headroom fell to "
               "%llu MB -- past the %llu MB mark where the OLD rule would also "
               "have reclaimed -- so this run proves nothing about reclaiming "
               "without pressure. Give the test more headroom to work in.)\n",
               lowest_headroom, file_mark);
        munmap(buf, region);
        return 1;
    }

    test_log_if(0, "stores %llu -> %llu after %d s, with headroom never below "
                "%llu MB against a %llu MB file-backed mark -- compressed with "
                "no pressure at all\n",
                before, after, settled, lowest_headroom, file_mark);
    munmap(buf, region);
    return finish_suite("zram_idle_reclaim");
}
