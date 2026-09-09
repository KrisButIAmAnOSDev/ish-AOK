#ifndef KERNEL_ZPOOL_H
#define KERNEL_ZPOOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A pool of compressed guest pages.
//
// WHY THIS EXISTS. Compressing guest memory only helps if AOK holds the
// compressed bytes in its OWN storage and releases the original pages, because
// `phys_footprint` -- the ledger jetsam kills on -- charges for a page whether
// or not the host compressor has taken it. Measured on macOS and confirmed on
// an iPad 5th gen: 250 MB of a single repeating byte stayed charged for three
// minutes of idle. See docs/TODO.md, "Guest-memory compression: phase 0".
//
// THE PROBLEM THIS SOLVES. Compressed pages are variable-sized -- the same
// measurement found real pages landing anywhere from a few dozen bytes to
// slightly over a full page. A page-granular allocator (which is what
// kernel/swap.c's slot bitmap is) would store a 300-byte page in 4096 bytes and
// give back none of the ratio. Linux solves this with zsmalloc, which is a
// large piece of machinery. This is the small version of the same idea.
//
// THE DESIGN, and its cost, stated up front. Fixed size classes at 256-byte
// granularity: an object is rounded up to the next multiple of 256, and each
// class allocates page-sized slabs carved into equal entries. Internal
// fragmentation is therefore under 256 bytes per page, which against a measured
// mean compressed size of roughly 1.7 KB is about 7%. So a raw 2.4x becomes an
// effective ~2.2x. That is the price of not writing zsmalloc, and it is
// reported rather than assumed -- see zpool_stats::stored_bytes against
// ::pool_bytes.
//
// NOT THREAD-SAFE by itself. The caller serialises; the pager already holds an
// address-space barrier where this will be used.

#define ZPOOL_PAGE_SIZE     4096
#define ZPOOL_GRANULE       256
// Classes for 256, 512, ... 4096. An object bigger than a page is never stored:
// the caller keeps those uncompressed, which the phase 0 numbers say is 22
// pages in 23,184.
#define ZPOOL_CLASSES       (ZPOOL_PAGE_SIZE / ZPOOL_GRANULE)

// Opaque handle. ZPOOL_HANDLE_NONE is never returned by a successful store, so
// it is safe as the "nothing here" value in a page-table entry.
typedef uint64_t zpool_handle_t;
#define ZPOOL_HANDLE_NONE   ((zpool_handle_t) 0)

struct zpool;

struct zpool_stats {
    uint64_t objects;         // live stored objects
    uint64_t stored_bytes;    // sum of their compressed sizes
    uint64_t pool_bytes;      // what the pool actually occupies (slabs)
    uint64_t original_bytes;  // sum of their uncompressed sizes
    uint64_t stores;
    uint64_t loads;
    uint64_t frees;
    uint64_t store_failures;  // pool full, or object too large
    uint64_t slabs;
};

// `max_bytes` caps the slab memory the pool will allocate; a store that would
// exceed it fails rather than growing, so the feature can be given a size the
// way swap is. 0 means unlimited, which is for tests only.
struct zpool *zpool_create(uint64_t max_bytes);
void zpool_destroy(struct zpool *pool);

// Copy `size` bytes into the pool. Returns ZPOOL_HANDLE_NONE if the object does
// not fit a page, or the pool is at its cap and no slab has room.
// `original_size` is recorded for the ratio only.
zpool_handle_t zpool_store(struct zpool *pool, const void *data, size_t size,
                           size_t original_size);

// Copy the object back out. `out_size` must be at least the stored size; the
// stored size is written to *size_out. False if the handle is not live.
bool zpool_load(struct zpool *pool, zpool_handle_t handle,
                void *out, size_t out_size, size_t *size_out);

// Release. Freeing ZPOOL_HANDLE_NONE is a no-op, so a caller tearing down a
// page table does not need to check first.
void zpool_free(struct zpool *pool, zpool_handle_t handle);

void zpool_get_stats(struct zpool *pool, struct zpool_stats *out);

#endif
