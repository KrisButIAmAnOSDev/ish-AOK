// Phase 0 for guest-memory compression: measure it before building it.
//
// THE QUESTION, and it is not the obvious one. Darwin already compresses idle
// anonymous memory with no memory pressure at all -- a process that dirties
// 1 GiB and waits has essentially all of it in the compressor within 25
// seconds. So every cold guest page is already compressed today, for free.
//
// What that buys AOK is NOTHING, because `phys_footprint` -- the ledger jetsam
// kills on (platform/darwin.c) -- does not move when the compressor takes a
// page. Measured, interleaved, two rounds: 1 GiB of a repeating byte and 1 GiB
// of random both sit at 1025.4 MB of footprint whether compressed or not. See
// docs/TODO.md, "Darwin compresses our memory already, and it buys us nothing".
//
// So AOK-level compression is not redundant with the host's; it is the only
// kind that can help, because AOK compresses into its OWN smaller buffer and
// then releases the originals -- and released pages do move the footprint.
// That is zram's model. This file measures whether it is worth it, on real
// guest pages rather than synthetic ones, which is the gap that measurement
// left open.
//
// WHAT IT REPORTS, per algorithm: the ratio, the distribution of ratios (an
// average hides the shape -- a workload where half the pages are incompressible
// behaves very differently from one where every page compresses 2x), the cost
// per page in each direction, and the number of pages that got BIGGER, which a
// real implementation has to store uncompressed and must budget for.
//
// It verifies every round trip. A compression ratio with no decompress-and-
// compare behind it measures nothing: a compressor that dropped the data
// entirely would score best.
//
// It is a measurement, not a feature. It never modifies guest memory, never
// evicts anything, and holds no lock across the compression work beyond the mm
// reference that keeps the address space alive.
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/memcomp.h"
#include "emu/memory.h"

// Gated on the build system finding the library, not merely on __APPLE__: the
// header and the dylib are what matter, and meson reports both together
// (ISH_HAVE_LIBCOMPRESSION). A Darwin host without it still compiles, and
// memcomp_available() then says there is nothing to measure.
#if defined(ISH_HAVE_LIBCOMPRESSION)
#include <compression.h>
#define MEMCOMP_HAVE_COMPRESSION 1
#endif

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t) t.tv_sec * 1000000000ull + (uint64_t) t.tv_nsec;
}

#if defined(MEMCOMP_HAVE_COMPRESSION)

// Which ratio bucket a page lands in. Buckets rather than an average because
// the shape is the decision: zram's win comes from pages that compress a lot,
// and its cost is paid on every page including the ones that do not.
static int ratio_bucket(size_t out, size_t in) {
    if (out * 8 <= in) return 0;        // <= 12.5%  (8x or better)
    if (out * 4 <= in) return 1;        // <= 25%    (4x)
    if (out * 2 <= in) return 2;        // <= 50%    (2x)
    if (out * 4 <= in * 3) return 3;    // <= 75%
    return 4;                           // barely, or not at all
}

static void measure_one(struct memcomp_algo *a, compression_algorithm alg,
                        const uint8_t *page, size_t page_size,
                        uint8_t *cbuf, size_t cbuf_size,
                        uint8_t *dbuf, void *scratch) {
    uint64_t t0 = now_ns();
    size_t out = compression_encode_buffer(cbuf, cbuf_size, page, page_size,
                                           scratch, alg);
    uint64_t t1 = now_ns();

    // 0 means "did not fit in dst" -- for a page-sized dst that is an
    // incompressible page, which a real implementation stores raw. Charge it at
    // full size so the totals stay honest rather than skipping it.
    if (out == 0) {
        a->incompressible++;
        a->bytes_out += page_size;
        a->buckets[4]++;
        a->ns_compress += t1 - t0;
        return;
    }

    uint64_t t2 = now_ns();
    size_t back = compression_decode_buffer(dbuf, page_size, cbuf, out,
                                            scratch, alg);
    uint64_t t3 = now_ns();

    // The whole measurement is worthless without this. A round trip that does
    // not reproduce the page exactly is a defect in the candidate, not a ratio.
    if (back != page_size || memcmp(dbuf, page, page_size) != 0) {
        a->verify_failures++;
        return;
    }

    a->bytes_out += out;
    a->buckets[ratio_bucket(out, page_size)]++;
    a->ns_compress += t1 - t0;
    a->ns_decompress += t3 - t2;
    a->pages_verified++;
}
#endif // MEMCOMP_HAVE_COMPRESSION

#if defined(MEMCOMP_HAVE_COMPRESSION)
struct memcomp_ctx {
    struct memcomp_result *out;
    uint8_t *cbuf;
    size_t cbuf_size;
    uint8_t *dbuf;
    uint8_t *srcbuf;    // stable copy of the page under test; see the visitor
    void *scratch;
    size_t page_size;
};

static void memcomp_visit_page(const void *page, void *vctx) {
    struct memcomp_ctx *c = vctx;
    c->out->pages++;
    c->out->bytes_in += c->page_size;

    // Take a private copy FIRST, and measure that.
    //
    // This is guest memory of a RUNNING process, so the page can be written
    // between the compress and the compare, and then the round trip "fails"
    // against bytes that are no longer the ones that were compressed. That is
    // exactly what the first run reported: one verify failure per algorithm,
    // all three the same, which is not how three independent compressors fail.
    // A real defect would not land on precisely one page in 16,557 in lz4,
    // lzfse and zlib alike.
    //
    // Copying also makes the three algorithms measure the SAME bytes, which
    // they otherwise might not -- the page was free to change between them, so
    // even the ratios were being taken from three slightly different pages.
    memcpy(c->srcbuf, page, c->page_size);
    const uint8_t *p = c->srcbuf;
    measure_one(&c->out->lz4, COMPRESSION_LZ4, p, c->page_size,
                c->cbuf, c->cbuf_size, c->dbuf, c->scratch);
    measure_one(&c->out->lzfse, COMPRESSION_LZFSE, p, c->page_size,
                c->cbuf, c->cbuf_size, c->dbuf, c->scratch);
    measure_one(&c->out->zlib, COMPRESSION_ZLIB, p, c->page_size,
                c->cbuf, c->cbuf_size, c->dbuf, c->scratch);
}
#endif

bool memcomp_available(void) {
#if defined(MEMCOMP_HAVE_COMPRESSION)
    return true;
#else
    return false;
#endif
}

int memcomp_measure_mem(struct mem *mem, struct memcomp_result *out) {
    memset(out, 0, sizeof *out);
#if !defined(MEMCOMP_HAVE_COMPRESSION)
    (void) mem;
    return _ENOSYS;
#else
    if (mem == NULL)
        return _EINVAL;

    const size_t page_size = PAGE_SIZE;
    // LZ4 can expand slightly; give the destination room so "did not fit" means
    // the page is genuinely incompressible rather than the buffer being tight.
    const size_t cbuf_size = page_size * 2;
    uint8_t *cbuf = malloc(cbuf_size);
    uint8_t *dbuf = malloc(page_size);
    uint8_t *srcbuf = malloc(page_size);
    // Scratch sized for the most demanding algorithm we ask for; reused across
    // every page so the measurement does not include an allocation per page.
    size_t scratch_size = compression_encode_scratch_buffer_size(COMPRESSION_LZFSE);
    size_t zscratch = compression_encode_scratch_buffer_size(COMPRESSION_ZLIB);
    if (zscratch > scratch_size)
        scratch_size = zscratch;
    void *scratch = scratch_size > 0 ? malloc(scratch_size) : NULL;
    if (cbuf == NULL || dbuf == NULL || srcbuf == NULL ||
            (scratch_size > 0 && scratch == NULL)) {
        free(cbuf); free(dbuf); free(srcbuf); free(scratch);
        return _ENOMEM;
    }

    out->page_size = page_size;
    uint64_t started = now_ns();

    // The walk itself lives in emu/memory.c (mem_walk_resident_pages): the
    // page-table internals are private there, and a second copy of that
    // traversal would be a second thing to keep correct.
    struct memcomp_ctx ctx = {
        .out = out, .cbuf = cbuf, .cbuf_size = cbuf_size,
        .dbuf = dbuf, .srcbuf = srcbuf, .scratch = scratch,
        .page_size = page_size,
    };
    mem_walk_resident_pages(mem, memcomp_visit_page, &ctx);

    out->wall_ns = now_ns() - started;
    free(cbuf); free(dbuf); free(srcbuf); free(scratch);
    return 0;
#endif
}
