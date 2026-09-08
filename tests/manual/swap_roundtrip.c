// A page that goes out to swap must come back byte-for-byte.
//
// 554 ships a pager, and nothing in the suite exercised it. That is the wrong
// gap to have: the failure mode of a pager is not a crash, it is SILENT DATA
// CORRUPTION. emu/memory.c's own header says so -- on Darwin a released frame
// stays mapped, readable and byte-identical under MADV_FREE_REUSABLE, so a
// missed pointer holder reads plausible old data and a write is quietly
// reverted by the swap-in. Every other outcome announces itself; this one does
// not.
//
// What this asserts, in order:
//   1. Swap is on, and the guest can see it. Otherwise SKIP -- swap is off by
//      default and that is deliberate.
//   2. Eviction actually happened, measured from /proc/ish/swap's bytes_written
//      moving. Without this the read-back below would pass against a build
//      where nothing was ever paged, which proves nothing at all. If nothing is
//      evicted the verdict is SKIP, not PASS.
//   3. Every byte of the evicted region reads back exactly what was written.
//
// Eviction is FORCED rather than waited for: `echo <pid> > /proc/ish/swap_evict`
// sweeps that process. Waiting for kswapd instead would make this a function of
// host memory pressure, which on a Mac or an idle iPad is simply absent --
// kswapd's loop `continue`s before sweeping when swap_kswapd_should_reclaim()
// is false, so with memory to spare it never sweeps, never ages anything, and
// reclaims nothing. Measured on an M4 iPad Pro with swap on: 4060 passes, 0
// bytes reclaimed. A test that only runs when the machine happens to be short
// of memory is a test that never runs.
//
// It takes THREE sweeps, not one, and that is the design rather than a wait for
// something asynchronous: the clock is second-chance with SWAP_AGE_CANDIDATE=2
// (emu/memory.c), so a frame has to survive two sweeps untouched before the
// third may take it. Measured here, on 64 MB freshly written:
//
//     sweep 1   frames_evicted 0     16559 resident, 0 out
//     sweep 2   frames_evicted 0     16561 resident, 0 out
//     sweep 3   frames_evicted 4096    177 resident, 16384 out   64 MB written
//
// A one-sweep version of this test reported "nothing was evicted" and would
// have been read as a broken pager.
//
// The pattern is POSITION-DEPENDENT (a hash of the byte's offset), not a
// constant fill and not zeros. A constant fill cannot tell a correct swap-in
// from a frame restored at the wrong offset, from the same frame handed back
// twice, or from a page that was never evicted and never touched -- and all
// three are live failure modes for a clock-based pager that stores a slot per
// frame. Zeros are worse still, since a fresh anonymous page is already zero.
//
// To run it with swap on, on a build that has no Settings screen:
//
//     ISH_GUEST_SWAP_MB=256 ./build/ish -f <root> /AOK/tests/swap_roundtrip
//
// ON AN INSTALLED APP THIS TEST SKIPS, and that is correct rather than a gap in
// the gate. /proc/ish/swap_evict is gated on swap_guest_control, which
// kernel/swap.c sets only on the ISH_GUEST_SWAP_MB branch -- the CLI and Xcode
// path -- and never on the Settings branch an App Store install takes. The
// comment on the handler gives the reason: forcing another process's memory out
// to storage would let any guest root process spend the user's flash write
// budget on any pid it liked. So a device run reports SKIP with EPERM, and the
// round trip has to be exercised from a CLI or Xcode launch. Verified on both:
// SKIP with `Operation not permitted` on an M4 iPad Pro with swap on, PASS from
// the CLI.

#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

// Big enough that the clock has cold pages worth taking, small enough to be
// polite on a phone. Both regions are anonymous and private, which is all the
// pager evicts today.
#define REGION_MB     64
// One more than SWAP_AGE_CANDIDATE (emu/memory.c): two sweeps to age the region
// past the second-chance bar, and a third to actually take it.
#define SWEEPS 3
// Seconds to let the eviction counter catch up before concluding nothing moved.
#define SETTLE_SECONDS 12

static unsigned char pattern_byte(size_t off) {
    // Cheap position hash. Distinct for neighbouring pages and for the same
    // offset within different pages, so a frame restored to the wrong place is
    // a mismatch rather than a coincidence.
    uint64_t x = (uint64_t) off * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    return (unsigned char) (x & 0xff);
}

// One field out of /proc/ish/swap. Returns 0 and sets *ok=0 if absent.
static unsigned long long swap_field(const char *name, int *ok) {
    *ok = 0;
    FILE *f = fopen("/proc/ish/swap", "r");
    if (f == NULL)
        return 0;
    char line[256];
    unsigned long long value = 0;
    size_t namelen = strlen(name);
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, name, namelen) != 0 || (line[namelen] != ' ' && line[namelen] != '\t'))
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

static int swap_is_on(void) {
    FILE *f = fopen("/proc/ish/swap", "r");
    if (f == NULL)
        return 0;
    char line[256];
    int on = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "state", 5) == 0 && strstr(line, "on") != NULL)
            on = 1;
    }
    fclose(f);
    return on;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(180));

    if (!swap_is_on()) {
        printf("swap_roundtrip: SKIP (swap is off -- it is opt-in; set "
               "ISH_GUEST_SWAP_MB or enable it in Settings)\n");
        return 0;
    }

    size_t region = (size_t) REGION_MB * 1024 * 1024;
    unsigned char *cold = mmap(NULL, region, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cold == MAP_FAILED) {
        printf("swap_roundtrip: SKIP (could not map %d MB: %s)\n",
               REGION_MB, strerror(errno));
        return 0;
    }

    for (size_t i = 0; i < region; i++)
        cold[i] = pattern_byte(i);

    int ok = 0;
    unsigned long long written_before = swap_field("bytes_written", &ok);
    if (!ok) {
        printf("swap_roundtrip: SKIP (/proc/ish/swap has no bytes_written line)\n");
        munmap(cold, region);
        return 0;
    }

    // Force the eviction rather than waiting for memory pressure that may never
    // come. This is the documented control: /proc/ish/swap_evict says
    //     echo <pid> > /proc/ish/swap_evict   # evict every eligible frame
    // Deliberately NOT touching `cold` between sweeps -- a read would reset its
    // second chance and it would never age into candidacy.
    unsigned long long written_after = written_before;
    int evict_err = 0;
    for (int sweep = 0; sweep < SWEEPS && written_after <= written_before; sweep++) {
        FILE *ev = fopen("/proc/ish/swap_evict", "w");
        if (ev == NULL) {
            printf("swap_roundtrip: SKIP (no /proc/ish/swap_evict to force an "
                   "eviction with: %s)\n", strerror(errno));
            munmap(cold, region);
            return 0;
        }
        errno = 0;
        fprintf(ev, "%d\n", (int) getpid());
        if (fclose(ev) != 0)
            evict_err = errno;
        if (evict_err == EPERM)
            break;   // a gate, not a slow sweep: no point trying twice more

        // The write returns once the sweep is done, but read the counter in a
        // short loop anyway: an implementation that queued the work would
        // otherwise be reported as "nothing evicted" when it is merely not
        // finished yet.
        for (int s = 0; s < SETTLE_SECONDS; s++) {
            written_after = swap_field("bytes_written", &ok);
            if (ok && written_after > written_before)
                break;
            struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
            nanosleep(&ts, NULL);
        }
        test_log_if(0, "sweep %d: bytes_written %llu\n", sweep + 1, written_after);
    }

    if (written_after <= written_before) {
        // Nothing was paged out, so reading the region back would prove
        // nothing. A pass here would be a lie -- see the header.
        if (evict_err == EPERM) {
            // The expected answer on an installed app -- see the header. Say so
            // explicitly, because "nothing was evicted" in a device gate log
            // otherwise reads as a broken pager.
            printf("swap_roundtrip: SKIP (/proc/ish/swap_evict is EPERM: the "
                   "forced-eviction control is a CLI/Xcode-only development "
                   "gate, not reachable from an installed app)\n");
        } else {
            printf("swap_roundtrip: SKIP (nothing was evicted in %d sweeps%s; "
                   "bytes_written stayed at %llu, so the round trip was never "
                   "exercised)\n", SWEEPS,
                   evict_err ? " (the swap_evict write failed)" : "",
                   written_before);
        }
        munmap(cold, region);
        return 0;
    }
    test_log_if(0, "evicted: bytes_written %llu -> %llu\n",
                written_before, written_after);

    // The actual assertion. Walk every byte, and report the FIRST mismatch with
    // its offset -- which page it is in is the most useful thing a failure can
    // say about a pager.
    size_t bad = 0;
    size_t first_bad = 0;
    unsigned char got = 0, want = 0;
    for (size_t i = 0; i < region; i++) {
        unsigned char expect = pattern_byte(i);
        if (cold[i] != expect) {
            if (bad == 0) {
                first_bad = i;
                got = cold[i];
                want = expect;
            }
            bad++;
        }
    }

    if (bad != 0) {
        printf("FAIL swap round trip corrupted %zu of %zu bytes; first at "
               "offset %zu (page %zu, offset %zu within it): got 0x%02x, "
               "expected 0x%02x\n",
               bad, region, first_bad, first_bad / 4096, first_bad % 4096,
               got, want);
        failures_total++;
    } else {
        test_log_if(0, "%d MB survived the round trip byte-for-byte\n", REGION_MB);
    }

    munmap(cold, region);
    return finish_suite("swap_roundtrip");
}
