// A frame that goes out through the COMPRESSED tier must come back
// byte-for-byte, and the test must fail if it never went through it.
//
// This is the sibling of swap_roundtrip, and it exists because that test cannot
// exercise this path. Its pattern is a per-byte position hash -- deliberately,
// so a frame restored to the wrong offset is a mismatch rather than a
// coincidence -- and a per-byte hash is incompressible. Run with the tier on,
// it reports 4096 frames DECLINED and zero stored, passes, and proves nothing
// about compression at all. That is exactly what happened the first time.
//
// So the pattern here has to be two things at once:
//
//   - COMPRESSIBLE, or the tier declines every frame and we are back to
//     testing the file path. 64-byte runs give lz4 something to work with.
//   - POSITION-DEPENDENT, or a frame handed back at the wrong offset, or the
//     same frame handed back twice, still compares equal. The run VALUE is a
//     hash of (page index, block index), so it varies both between pages and
//     within one.
//
// 64-byte granularity is fine for catching misplacement: frames are
// mem_frame_size(), 16 KiB on Apple Silicon, so a frame-level error moves the
// data by 256 blocks.
//
// AND IT ASSERTS THE TIER ENGAGED. `stores` and `loads` from /proc/ish/zswap
// must both move. Without that this would pass while the tier sat idle, which
// is the failure mode that motivated writing it.
//
// To run it:
//
//     ISH_GUEST_SWAP_MB=256 ISH_GUEST_ZSWAP_MB=128 \
//         ./build/ish -f <root> /AOK/tests/zswap_roundtrip
//
// On an installed app it SKIPs, for the same reason swap_roundtrip does: the
// forced-eviction control is a CLI/Xcode development gate.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#define REGION_MB      24
#define SWEEPS         4
#define SETTLE_SECONDS 12
#define RUN_BYTES      64

// Value for the run at `block`, where block counts 64-byte runs from the start
// of the region. Distinct for neighbouring blocks and for the same block index
// in different pages, so a misplaced frame mismatches.
static unsigned char run_byte(size_t block) {
    uint64_t x = (uint64_t) block * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    return (unsigned char) (x & 0xff);
}

// One numeric field out of /proc/ish/zswap.
static unsigned long long zswap_field(const char *name, int *ok) {
    *ok = 0;
    FILE *f = fopen("/proc/ish/zswap", "r");
    if (f == NULL)
        return 0;
    char line[256];
    unsigned long long value = 0;
    size_t namelen = strlen(name);
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, name, namelen) != 0 ||
                (line[namelen] != ' ' && line[namelen] != '\t'))
            continue;
        const char *p = line + namelen;
        while (*p == ' ' || *p == '\t')
            p++;
        value = strtoull(p, NULL, 10);
        *ok = 1;
        break;
    }
    fclose(f);
    return value;
}

static int zswap_is_on(void) {
    FILE *f = fopen("/proc/ish/zswap", "r");
    if (f == NULL)
        return 0;
    char line[256];
    int on = 0;
    if (fgets(line, sizeof line, f) != NULL && strncmp(line, "on", 2) == 0)
        on = 1;
    fclose(f);
    return on;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(240));

    if (!zswap_is_on()) {
        printf("zswap_roundtrip: SKIP (the compressed tier is off -- it is "
               "opt-in; set ISH_GUEST_ZSWAP_MB with ISH_GUEST_SWAP_MB)\n");
        return 0;
    }

    size_t region = (size_t) REGION_MB * 1024 * 1024;
    unsigned char *cold = mmap(NULL, region, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cold == MAP_FAILED) {
        printf("zswap_roundtrip: SKIP (could not map %d MB: %s)\n",
               REGION_MB, strerror(errno));
        return 0;
    }

    for (size_t block = 0; block * RUN_BYTES < region; block++)
        memset(cold + block * RUN_BYTES, run_byte(block), RUN_BYTES);

    int ok = 0;
    unsigned long long stores_before = zswap_field("stores", &ok);
    if (!ok) {
        printf("zswap_roundtrip: SKIP (/proc/ish/zswap has no stores line)\n");
        munmap(cold, region);
        return 0;
    }
    unsigned long long loads_before = zswap_field("loads", &ok);

    // Force the eviction. Three sweeps minimum: the clock is second-chance, so
    // a frame must survive two passes untouched before the third may take it.
    // Deliberately not touching `cold` in between -- a read resets its chance.
    unsigned long long stores_after = stores_before;
    int evict_err = 0;
    for (int sweep = 0; sweep < SWEEPS && stores_after <= stores_before; sweep++) {
        FILE *ev = fopen("/proc/ish/swap_evict", "w");
        if (ev == NULL) {
            printf("zswap_roundtrip: SKIP (no /proc/ish/swap_evict: %s)\n",
                   strerror(errno));
            munmap(cold, region);
            return 0;
        }
        errno = 0;
        fprintf(ev, "%d\n", (int) getpid());
        if (fclose(ev) != 0)
            evict_err = errno;
        if (evict_err == EPERM)
            break;
        for (int s = 0; s < SETTLE_SECONDS; s++) {
            stores_after = zswap_field("stores", &ok);
            if (ok && stores_after > stores_before)
                break;
            struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
            nanosleep(&ts, NULL);
        }
        test_log_if(0, "sweep %d: zswap stores %llu\n", sweep + 1, stores_after);
    }

    if (stores_after <= stores_before) {
        if (evict_err == EPERM) {
            printf("zswap_roundtrip: SKIP (/proc/ish/swap_evict is EPERM: the "
                   "forced-eviction control is a CLI/Xcode-only gate)\n");
        } else {
            // NOT a pass. Reading the region back would verify nothing, because
            // nothing went through the tier -- which is precisely the hole this
            // test exists to close.
            printf("zswap_roundtrip: FAIL (no frame was stored compressed in %d "
                   "sweeps; stores stayed at %llu, so the compressed round trip "
                   "was never exercised)\n", SWEEPS, stores_before);
            munmap(cold, region);
            return 1;
        }
        munmap(cold, region);
        return 0;
    }

    // Read it all back and compare. This is what faults the frames in, so the
    // decompress path runs here.
    size_t mismatches = 0;
    size_t first_bad = 0;
    for (size_t block = 0; block * RUN_BYTES < region; block++) {
        unsigned char want = run_byte(block);
        const unsigned char *p = cold + block * RUN_BYTES;
        for (size_t i = 0; i < RUN_BYTES; i++) {
            if (p[i] != want) {
                if (mismatches == 0)
                    first_bad = block * RUN_BYTES + i;
                mismatches++;
            }
        }
    }

    unsigned long long loads_after = zswap_field("loads", &ok);
    unsigned long long declined = zswap_field("declined", &ok);

    if (mismatches != 0) {
        printf("zswap_roundtrip: FAIL (%zu bytes came back wrong, first at "
               "offset %zu)\n", mismatches, first_bad);
        munmap(cold, region);
        return 1;
    }
    if (loads_after <= loads_before) {
        // The bytes are right but nothing was served from the pool, so they
        // came from somewhere else and this proved nothing about decompression.
        printf("zswap_roundtrip: FAIL (content is correct but zswap loads "
               "stayed at %llu -- the frames did not come back through the "
               "compressed tier)\n", loads_before);
        munmap(cold, region);
        return 1;
    }

    test_log_if(0, "%d MB survived a compressed round trip byte-for-byte; "
                "%llu stored, %llu loaded, %llu declined\n",
                REGION_MB, stores_after - stores_before,
                loads_after - loads_before, declined);
    munmap(cold, region);
    return finish_suite("zswap_roundtrip");
}
