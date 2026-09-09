// A compressed frame shared by a fork must stay private to each side.
//
// WHAT THIS ACTUALLY LOCKS IN, which is not what it was written to test.
//
// It began as a corruption test: evict a frame shared by a fork, have the child
// write to it, and check the parent's copy did not change. That failure mode --
// a copy-on-write break-out against a compressed frame writing through to the
// shared page -- would be silent corruption in a forked child, and every
// process a shell starts is a forked child. It was the one gap worth closing
// before shipping the compressed tier.
//
// IT CANNOT HAPPEN, and the reason is one line in emu/memory.c's
// swap_frame_eligible:
//
//     if (!(pt->flags & P_ANONYMOUS) || (pt->flags & P_SHARED) || (pt->flags & P_COW))
//         return false;
//
// A COW frame is refused for eviction outright, so a forked page never reaches
// the pager, let alone the compressor. The first version of this test proved it
// by failing honestly: after a fork it reported "nothing was stored compressed
// in 4 sweeps", because nothing was eligible.
//
// So this now asserts the SAFETY PROPERTY rather than chasing the bug:
//
//   1. With a fork outstanding, a forced eviction sweep must store NOTHING.
//      That is the invariant the whole fork/COW question rests on.
//   2. Once the sharing is gone -- the child has exited and the parent has
//      written through the region to collapse the COW chain -- the same frames
//      become evictable and must still round-trip byte-for-byte.
//
// If anyone later relaxes that eligibility check to evict shared frames without
// teaching the tier about sharing, (1) is what catches it, and it catches it as
// a test failure rather than as wrong data in someone's shell.
//
// The pattern is compressible (64-byte runs, so lz4 has something to work with
// and frames actually enter the pool) and position-dependent (the run value
// hashes the block index, so a frame restored to the wrong offset mismatches).
// A per-byte hash would be incompressible and the tier would decline every
// frame, leaving this testing the file path instead -- the same trap
// zswap_roundtrip.c documents.
//
//     ISH_GUEST_SWAP_MB=256 ISH_GUEST_ZSWAP_MB=128 \
//         ./build/ish -f <root> /AOK/tests/zswap_fork_cow
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#define REGION_MB      16
#define SWEEPS         4
#define SETTLE_SECONDS 12
#define RUN_BYTES      64
// The child rewrites every Nth block, so the region ends up a mix of pages it
// broke out of the sharing and pages it still shares with the parent.
#define CHILD_STRIDE   7
#define CHILD_MARK     0xC5

static unsigned char run_byte(size_t block) {
    uint64_t x = (uint64_t) block * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    return (unsigned char) (x & 0xff);
}

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

// Verify the region against what this side should see. `mine_marked` says
// whether the strided blocks should carry the child's mark.
static size_t verify(const unsigned char *base, size_t region, int mine_marked,
                     size_t *first_bad) {
    size_t bad = 0;
    for (size_t block = 0; block * RUN_BYTES < region; block++) {
        int marked = mine_marked && (block % CHILD_STRIDE == 0);
        unsigned char want = marked ? CHILD_MARK : run_byte(block);
        const unsigned char *p = base + block * RUN_BYTES;
        for (size_t i = 0; i < RUN_BYTES; i++) {
            if (p[i] != want) {
                if (bad == 0)
                    *first_bad = block * RUN_BYTES + i;
                bad++;
            }
        }
    }
    return bad;
}

// Force eviction of this process, up to `sweeps` times, stopping early if
// `stores` moves past `baseline`. Returns the errno from the control write.
static int evict_self(int sweeps, unsigned long long baseline, int *unused) {
    (void) unused;
    int err = 0;
    for (int sweep = 0; sweep < sweeps; sweep++) {
        FILE *ev = fopen("/proc/ish/swap_evict", "w");
        if (ev == NULL)
            return errno;
        errno = 0;
        fprintf(ev, "%d\n", (int) getpid());
        if (fclose(ev) != 0)
            err = errno;
        if (err == EPERM)
            return err;
        for (int t = 0; t < SETTLE_SECONDS; t++) {
            int ok = 0;
            if (zswap_field("stores", &ok) > baseline && ok)
                return err;
            struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
            nanosleep(&ts, NULL);
        }
    }
    return err;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(300));

    if (!zswap_is_on()) {
        printf("zswap_fork_cow: SKIP (the compressed tier is off -- it is "
               "opt-in; set ISH_GUEST_ZSWAP_MB with ISH_GUEST_SWAP_MB)\n");
        return 0;
    }

    size_t region = (size_t) REGION_MB * 1024 * 1024;
    unsigned char *shared = mmap(NULL, region, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        printf("zswap_fork_cow: SKIP (could not map %d MB: %s)\n",
               REGION_MB, strerror(errno));
        return 0;
    }
    for (size_t block = 0; block * RUN_BYTES < region; block++)
        memset(shared + block * RUN_BYTES, run_byte(block), RUN_BYTES);

    int ok = 0;
    unsigned long long base_stores = zswap_field("stores", &ok);
    if (!ok) {
        printf("zswap_fork_cow: SKIP (/proc/ish/zswap has no stores line)\n");
        return 0;
    }

    int to_child[2];
    if (pipe(to_child) != 0) {
        printf("zswap_fork_cow: SKIP (pipe: %s)\n", strerror(errno));
        return 0;
    }

    fflush(NULL);
    pid_t child = fork();
    if (child < 0) {
        printf("zswap_fork_cow: SKIP (fork: %s)\n", strerror(errno));
        return 0;
    }
    if (child == 0) {
        // Hold the sharing open, verify our own view, and get out of the way.
        char c;
        close(to_child[1]);
        if (read(to_child[0], &c, 1) != 1) _exit(3);
        size_t first = 0;
        _exit(verify(shared, region, 0, &first) == 0 ? 0 : 4);
    }
    close(to_child[0]);

    // ---- (1) the invariant: a COW frame must never be evicted -------------
    int evict_err = evict_self(SWEEPS, base_stores, NULL);
    unsigned long long shared_stores = zswap_field("stores", &ok);
    if (evict_err == EPERM) {
        printf("zswap_fork_cow: SKIP (/proc/ish/swap_evict is EPERM: the "
               "forced-eviction control is a CLI/Xcode-only gate)\n");
        char go = 'g';
        if (write(to_child[1], &go, 1) != 1) { }
        waitpid(child, NULL, 0);
        return 0;
    }
    if (shared_stores > base_stores) {
        printf("zswap_fork_cow: FAIL (a frame shared with a forked child was "
               "stored compressed: stores %llu -> %llu. swap_frame_eligible is "
               "supposed to refuse P_COW outright, and the compressed tier has "
               "no handling for a frame reachable from two address spaces)\n",
               base_stores, shared_stores);
        char go = 'g';
        if (write(to_child[1], &go, 1) != 1) { }
        waitpid(child, NULL, 0);
        return 1;
    }
    test_log_if(0, "with a fork outstanding, %d sweeps stored nothing "
                "(stores stayed %llu) -- COW frames refused, as they must be\n",
                SWEEPS, base_stores);

    // Release the child and let the sharing end.
    char go = 'g';
    if (write(to_child[1], &go, 1) != 1) {
        printf("zswap_fork_cow: SKIP (could not signal the child)\n");
        kill(child, SIGKILL); waitpid(child, NULL, 0);
        return 0;
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("zswap_fork_cow: FAIL (the child read the shared region back "
               "wrong: exit %d)\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return 1;
    }

    // ---- (2) sharing gone: the same frames must round-trip ----------------
    // Write through the region to collapse the COW chain. The flag persists
    // after the sharer is gone until a write fault breaks it, so without this
    // the frames stay ineligible and the second half would silently test
    // nothing -- which is the failure this whole file exists to avoid.
    for (size_t block = 0; block * RUN_BYTES < region; block++)
        memset(shared + block * RUN_BYTES, run_byte(block), RUN_BYTES);

    unsigned long long before2 = zswap_field("stores", &ok);
    evict_err = evict_self(SWEEPS, before2, NULL);
    unsigned long long after2 = zswap_field("stores", &ok);
    if (after2 <= before2) {
        printf("zswap_fork_cow: FAIL (after the child exited and the COW chain "
               "was collapsed, %d sweeps still stored nothing -- stores stayed "
               "%llu, so the round trip below would prove nothing)\n",
               SWEEPS, before2);
        return 1;
    }

    size_t first = 0;
    size_t bad = verify(shared, region, 0, &first);
    if (bad != 0) {
        printf("zswap_fork_cow: FAIL (%zu bytes came back wrong after a "
               "post-fork round trip, first at offset %zu)\n", bad, first);
        return 1;
    }

    test_log_if(0, "after the fork ended: stores %llu -> %llu, %d MB back "
                "byte-for-byte\n", before2, after2, REGION_MB);
    munmap(shared, region);
    return finish_suite("zswap_fork_cow");
}
