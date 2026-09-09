#ifndef KERNEL_MEMCOMP_H
#define KERNEL_MEMCOMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mem;

// Per-algorithm results of one measurement pass. See kernel/memcomp.c for why
// this reports a distribution and a verification count rather than one ratio.
struct memcomp_algo {
    uint64_t bytes_out;         // charged at full page size for incompressible pages
    uint64_t pages_verified;    // round-tripped and compared equal
    uint64_t incompressible;    // did not fit in a 2-page destination
    uint64_t verify_failures;   // decompressed to something else -- must be 0
    // Ratio distribution: <=12.5%, <=25%, <=50%, <=75%, worse.
    uint64_t buckets[5];
    uint64_t ns_compress;
    uint64_t ns_decompress;
};

struct memcomp_result {
    uint64_t pages;             // resident pages sampled
    uint64_t bytes_in;
    size_t page_size;
    uint64_t wall_ns;
    struct memcomp_algo lz4;
    struct memcomp_algo lzfse;
    struct memcomp_algo zlib;
};

// Whether this host has a compressor to measure at all. False everywhere but
// Darwin today: libcompression ships in the OS on macOS 10.11+ and iOS 9+, and
// the decision this measures is an iOS one.
bool memcomp_available(void);

// Walk every RESIDENT page of `mem`, compress and decompress it with each
// candidate, and fill `out`. Read-only with respect to guest memory: it never
// evicts, never faults a swapped page back in, and never writes.
//
// The caller must hold a reference to the mm (mm_retain) for the duration --
// see proc_ish_update_mem_compress for the pinning discipline, which is the
// same one swap_evict needs and for the same reason.
//
// Returns 0, or _ENOSYS where no compressor exists.
int memcomp_measure_mem(struct mem *mem, struct memcomp_result *out);

#endif
