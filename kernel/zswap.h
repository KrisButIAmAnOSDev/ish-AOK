#ifndef KERNEL_ZSWAP_H
#define KERNEL_ZSWAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A compressed cache in front of the swap area.
//
// WHAT IT IS. The pager already treats slots as a backing store:
// swap_slot_write puts a frame's bytes somewhere, swap_slot_read gets them
// back, swap_slot_free releases the space. This intercepts those three, and for
// a frame that compresses well it keeps the bytes in RAM instead of on flash.
// Nothing about eviction eligibility, the fault path, fork/COW handling or the
// address-space barrier changes -- the pager cannot tell the difference, which
// is the whole reason to put it here rather than deeper.
//
// WHY IT IS WORTH IT, measured (docs/TODO.md, "Guest-memory compression"):
//
//   - The host already compresses idle memory for free, and it buys AOK
//     nothing: `phys_footprint`, the ledger jetsam kills on, charges for a page
//     whether or not the compressor has taken it. Confirmed on an iPad 5th gen,
//     where 250 MB of a single repeating byte stayed charged for three minutes.
//     Only compression AOK does itself -- into its own smaller buffer, with the
//     original frame released -- moves that number.
//   - Real workloads compress 2.2-2.8x with lz4 (cc1, a Python heap), and a
//     database's mostly-untouched buffer pool far more.
//   - lz4 decompresses a page in 1.9 us on an M4 and 3.1 us on an A9 -- the
//     oldest device AOK supports -- against the hundreds of microseconds a read
//     from flash costs. So serving a fault from the pool is roughly two orders
//     of magnitude cheaper than serving it from the swap file.
//   - Every frame served from the pool is a frame NOT written to flash, which
//     is the wear question users actually ask about swap. It comes straight off
//     the 24-hour write budget.
//
// WHAT IT IS NOT. This is Linux's zswap, not zram: it is a cache in FRONT of a
// swap area, so it does nothing unless swap is enabled. A pool that replaces
// the swap file entirely would need the pager to evict without allocating a
// slot, which is a deeper change than this one.
//
// OFF BY DEFAULT, and sized, for the same reason swap is: it spends the user's
// memory, and how much is their decision.

// Record what Settings asked for. RECORDS ONLY -- nothing allocates until
// zswap_startup() acts on it, so this is safe to call from a KVO observer, for
// the same reason swap_set_preference is (kernel/swap.h). Settings take effect
// at the next launch: resizing a live pool would mean faulting everything back
// first, which is deliberately not offered.
void zswap_set_preference(bool enabled, unsigned size_mb);
// Apply whatever was recorded. Called once from the boot path, after
// swap_startup, because the tier is meaningless without an area to front.
void zswap_startup(void);
// What Settings asked for, in MiB, or 0 if the tier was not requested. Read by
// the boot path to decide whether a RAM-only area is wanted at all.
unsigned zswap_requested_mb(void);

// Turn the tier on with a cap in MiB, or off with 0. Safe to call at any time;
// disabling frees the pool, which forces every slot it held back to the file on
// its next read -- so it is only called where the pager is quiesced.
void zswap_configure(uint32_t max_mb);
bool zswap_enabled(void);

// Called once the swap area's geometry is known, so the slot table can be
// sized. Until then every store declines.
void zswap_set_slot_geometry(uint32_t slot_count, size_t slot_size);

// Try to keep this slot's bytes in RAM. True means stored -- the caller must
// NOT write the file, and must not count the bytes against the write budget.
// False means "not mine": no compressor, tier off, does not compress, or the
// pool is at its cap. The caller falls through to the file exactly as before.
bool zswap_store(uint32_t slot, const void *buf, size_t len);

// Serve this slot from RAM if it is held here. False means the caller should
// read the file.
bool zswap_load(uint32_t slot, void *buf, size_t len);

// Release whatever is held for this slot. Safe for a slot that was never
// stored, so the pager can call it unconditionally on free.
void zswap_forget(uint32_t slot);

struct zswap_stats {
    bool enabled;
    uint64_t max_bytes;
    uint64_t pool_bytes;        // what the pool occupies
    uint64_t stored_bytes;      // compressed bytes live in it
    uint64_t original_bytes;    // what those frames occupied before
    uint64_t objects;
    uint64_t stores;            // frames kept in RAM
    uint64_t store_declined;    // did not compress, or pool full -- went to disk
    uint64_t loads;             // faults served from RAM
    uint64_t frees;
    uint64_t bytes_not_written; // flash writes avoided, i.e. the wear saving
};
void zswap_get_stats(struct zswap_stats *out);

#endif
