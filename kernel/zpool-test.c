// Unit test for the compressed-page pool. Runs on the host: `meson test -C
// build zpool`.
//
// The pool holds guest memory that has been taken away from the guest, so a bug
// here is silent data corruption in a process that has no way to notice. The
// tests are weighted accordingly: round-tripping is the easy half, and most of
// what follows is about handles that are wrong, pools that are full, and space
// that must be reused rather than leaked.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/zpool.h"

// Run everything at the frame size the pager actually uses on Apple Silicon.
// A pool sized for a 4 KiB guest page would pass every test here and truncate
// every real handle on the hardware this is for.
#define OBJ 16384

static int failures;

static void check(bool ok, const char *what) {
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

// Fill a buffer with a deterministic, position-dependent pattern, so a
// misplaced copy shows up as wrong content rather than as plausible bytes.
static void fill(uint8_t *buf, size_t n, unsigned seed) {
    unsigned s = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        buf[i] = (uint8_t) (s >> 24);
    }
}

static void test_roundtrip(void) {
    struct zpool *p = zpool_create(0, OBJ);
    check(p != NULL, "create");

    // Every size class, plus the boundaries either side of each granule.
    for (size_t size = 1; size < OBJ; size++) {
        if (size > 600 && size % 97 != 0 && size < OBJ - 3)
            continue;   // thin out the middle, keep the edges
        uint8_t in[OBJ], out[OBJ];
        fill(in, size, (unsigned) size);
        zpool_handle_t h = zpool_store(p, in, size, OBJ);
        if (h == ZPOOL_HANDLE_NONE) {
            printf("FAIL store size=%zu\n", size);
            failures++;
            continue;
        }
        memset(out, 0, sizeof out);
        size_t got = 0;
        check(zpool_load(p, h, out, sizeof out, &got), "load");
        check(got == size, "load size");
        check(memcmp(in, out, size) == 0, "load content");
        zpool_free(p, h);
    }
    struct zpool_stats st;
    zpool_get_stats(p, &st);
    check(st.objects == 0, "all freed");
    zpool_destroy(p);
}

static void test_rejects(void) {
    struct zpool *p = zpool_create(0, OBJ);
    uint8_t buf[OBJ];
    fill(buf, sizeof buf, 1);

    // An object at the ceiling cannot be stored: it saves nothing, and the
    // 14-bit size field could not represent it. The caller keeps those raw.
    check(zpool_store(p, buf, OBJ, OBJ) == ZPOOL_HANDLE_NONE,
          "reject object at the ceiling");
    check(zpool_store(p, buf, OBJ + 1, OBJ) == ZPOOL_HANDLE_NONE,
          "reject oversize");
    check(zpool_store(p, buf, 0, OBJ) == ZPOOL_HANDLE_NONE,
          "reject zero");

    // Handles that were never issued must be refused, not followed. These are
    // the shapes a corrupted page-table entry would take.
    size_t got;
    check(!zpool_load(p, ZPOOL_HANDLE_NONE, buf, sizeof buf, &got), "reject none handle");
    check(!zpool_load(p, 0xFFFFFFFFFFFFFFFFull, buf, sizeof buf, &got), "reject huge handle");
    check(!zpool_load(p, ((zpool_handle_t) 999 << 24) | 100, buf, sizeof buf, &got),
          "reject unknown slab");

    zpool_handle_t h = zpool_store(p, buf, 100, OBJ);
    check(h != ZPOOL_HANDLE_NONE, "store 100");
    // An entry index past the end of the slab, with an otherwise valid handle.
    // Entry occupies bits 23..14, so set them all: 1023 entries is past any
    // slab this pool builds.
    check(!zpool_load(p, h | ((zpool_handle_t) 0x3FF << 14), buf, sizeof buf, &got),
          "reject bad entry index");
    // A size larger than the class this handle's slab was cut for. Size is
    // bits 13..0; 0x3FFF is 16383, far past the 256-byte class holding a
    // 100-byte object.
    check(!zpool_load(p, (h & ~(zpool_handle_t) 0x3FFF) | 0x3FFF, buf, sizeof buf, &got),
          "reject oversized size field");
    // A destination that is too small must fail rather than overflow.
    check(!zpool_load(p, h, buf, 10, &got), "reject short destination");

    // Freeing rubbish must not corrupt the pool.
    zpool_free(p, ZPOOL_HANDLE_NONE);
    zpool_free(p, 0xFFFFFFFFFFFFFFFFull);
    struct zpool_stats st;
    zpool_get_stats(p, &st);
    check(st.objects == 1, "bad frees did not touch the live object");
    zpool_destroy(p);
}

// Space must be reused. A pool that leaks a slab per fill/empty cycle would
// pass every round-trip test and still exhaust memory on a device.
static void test_reuse(void) {
    struct zpool *p = zpool_create(0, OBJ);
    uint8_t buf[512];
    fill(buf, sizeof buf, 7);

    zpool_handle_t hs[4096];
    for (int i = 0; i < 4096; i++)
        hs[i] = zpool_store(p, buf, sizeof buf, OBJ);
    struct zpool_stats first;
    zpool_get_stats(p, &first);
    for (int i = 0; i < 4096; i++)
        zpool_free(p, hs[i]);
    for (int i = 0; i < 4096; i++)
        hs[i] = zpool_store(p, buf, sizeof buf, OBJ);
    struct zpool_stats second;
    zpool_get_stats(p, &second);

    check(second.slabs == first.slabs, "second fill allocated no new slabs");
    for (int i = 0; i < 4096; i++)
        zpool_free(p, hs[i]);
    zpool_destroy(p);
}

// The cap is what makes this safe to ship as a sized, opt-in feature: past it,
// stores fail and the caller falls back rather than the pool eating the device.
static void test_cap(void) {
    struct zpool *p = zpool_create(16 * OBJ, OBJ);   // 16 slabs
    uint8_t buf[256];
    fill(buf, sizeof buf, 3);
    int stored = 0;
    for (int i = 0; i < 10000; i++)
        if (zpool_store(p, buf, sizeof buf, OBJ) != ZPOOL_HANDLE_NONE)
            stored++;
    struct zpool_stats st;
    zpool_get_stats(p, &st);
    check(st.pool_bytes <= 16 * OBJ, "pool respected its cap");
    check(stored > 0, "stored something before the cap");
    check(st.store_failures > 0, "reported the refusals");
    zpool_destroy(p);
}

// Mixed sizes, interleaved store and free, with content verified at the end --
// the shape that catches a free list threaded through the wrong class.
static void test_mixed(void) {
    struct zpool *p = zpool_create(0, OBJ);
    enum { N = 3000 };
    static zpool_handle_t h[N];
    static size_t sz[N];
    static uint8_t ref[N][64];

    for (int i = 0; i < N; i++) {
        sz[i] = (size_t) (17 + (i * 131) % (OBJ - 100));
        uint8_t in[OBJ];
        fill(in, sz[i], (unsigned) i);
        memcpy(ref[i], in, 64 < sz[i] ? 64 : sz[i]);
        h[i] = zpool_store(p, in, sz[i], OBJ);
        check(h[i] != ZPOOL_HANDLE_NONE, "mixed store");
        if (i % 3 == 0 && i > 0) {           // churn
            zpool_free(p, h[i - 1]);
            h[i - 1] = ZPOOL_HANDLE_NONE;
        }
    }
    for (int i = 0; i < N; i++) {
        if (h[i] == ZPOOL_HANDLE_NONE)
            continue;
        uint8_t out[OBJ];
        size_t got = 0;
        check(zpool_load(p, h[i], out, sizeof out, &got), "mixed load");
        check(got == sz[i], "mixed size");
        size_t n = 64 < sz[i] ? 64 : sz[i];
        check(memcmp(ref[i], out, n) == 0, "mixed content");
    }
    zpool_destroy(p);
}

int main(void) {
    test_roundtrip();
    test_rejects();
    test_reuse();
    test_cap();
    test_mixed();
    if (failures != 0) {
        printf("zpool: FAIL failures=%d\n", failures);
        return 1;
    }
    printf("zpool: PASS\n");
    return 0;
}
