// The compressed cache in front of the swap area. See kernel/zswap.h for what
// it is, why it is worth it, and the measurements behind both.
//
// LOCKING. One mutex covers the pool and the slot table. Raw pthread rather
// than the tree's lock_t for the reason kernel/swap.c's quiesce gate gives:
// this runs on eviction and fault paths where `current` may be any task, and
// the critical sections are a memcpy and a compress -- microseconds, no I/O, no
// blocking call, and nothing else taken underneath.
//
// WHAT IS DELIBERATELY *NOT* DONE HERE. No quiesce-gate participation: the gate
// exists so the app is never mid-WRITE when iOS freezes it, and a store here is
// a memcpy into our own heap. Storing while suspended is safe, and refusing
// would give up the one path that avoids flash entirely.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/zswap.h"
#include "kernel/zpool.h"
#include "debug.h"

#if defined(ISH_HAVE_LIBCOMPRESSION)
#include <compression.h>
#define ZSWAP_HAVE_COMPRESSION 1
// lz4, and the choice is measured rather than assumed. Against lzfse and zlib
// on the same real workloads it gave 2.2-2.8x for 1.9 us (M4) to 3.1 us (A9)
// per decompress, where the denser two cost 5-7x that for 45-60% more ratio.
// Ratio is not the figure to maximise here: decompress lands on the fault path,
// and a frame that is slow to bring back is a frame the pager should have left
// alone.
#define ZSWAP_ALGO COMPRESSION_LZ4
#endif

static pthread_mutex_t zswap_lock = PTHREAD_MUTEX_INITIALIZER;
static struct zpool *zswap_pool;
static zpool_handle_t *zswap_slots;     // slot -> handle, or ZPOOL_HANDLE_NONE
static uint32_t zswap_slot_count;
static size_t zswap_slot_size;
static uint64_t zswap_max_bytes;
static void *zswap_scratch;             // compressor scratch, reused
static uint8_t *zswap_cbuf;             // compression destination

static struct zswap_stats zswap_stats_live;

bool zswap_enabled(void) {
#if defined(ZSWAP_HAVE_COMPRESSION)
    // Relaxed read of a pointer that only changes under the lock, in a
    // configure call the pager quiesces around. Worth not taking the mutex:
    // this is asked on every slot write and read.
    return __atomic_load_n(&zswap_pool, __ATOMIC_RELAXED) != NULL;
#else
    return false;
#endif
}

// Free everything. Caller holds the lock. Returns false and changes NOTHING if
// the pool still holds frames.
//
// THE TRAP THIS CLOSES, and it is silent corruption rather than a crash. A slot
// stored here was never written to the swap file -- that is the entire point.
// If the pool goes away while a slot still names an object in it, zswap_load
// declines, swap_slot_read falls through to the file, and the guest is handed
// whatever those blocks happened to contain. The frame is not lost loudly; it
// comes back as plausible garbage.
//
// So teardown is refused while anything is live. Every legitimate caller
// already reaches this with an empty pool -- swap_disable faults every frame
// back before the geometry changes, and the startup path configures the size
// before any area exists -- so a refusal here means a caller that did not
// quiesce, which is a bug worth seeing rather than absorbing.
static bool zswap_teardown_locked(void) {
    if (zswap_pool != NULL) {
        struct zpool_stats ps;
        zpool_get_stats(zswap_pool, &ps);
        if (ps.objects > 0) {
            printk("zswap: REFUSING to tear down with %llu frames still held -- "
                   "the caller did not fault them back first, and dropping them "
                   "would hand the guest garbage from the swap file\n",
                   (unsigned long long) ps.objects);
            return false;
        }
    }
    if (zswap_pool != NULL) {
        zpool_destroy(zswap_pool);
        __atomic_store_n(&zswap_pool, NULL, __ATOMIC_RELEASE);
    }
    free(zswap_slots);
    zswap_slots = NULL;
    free(zswap_scratch);
    zswap_scratch = NULL;
    free(zswap_cbuf);
    zswap_cbuf = NULL;
    return true;
}

// Build the pool and the slot table, if the geometry and the cap are both
// known. Caller holds the lock. Any allocation failure leaves the tier OFF
// rather than half-built: it is an optimisation, so declining is always a
// correct answer and there is never a reason to fail an eviction over it.
static void zswap_build_locked(void) {
#if defined(ZSWAP_HAVE_COMPRESSION)
    if (zswap_pool != NULL || zswap_max_bytes == 0 ||
            zswap_slot_count == 0 || zswap_slot_size == 0)
        return;
    if (zswap_slot_size > ZPOOL_OBJECT_MAX || zswap_slot_size % ZPOOL_GRANULE != 0) {
        printk("zswap: slot size %zu is not a pool object size; staying off\n",
               zswap_slot_size);
        return;
    }
    struct zpool *pool = zpool_create(zswap_max_bytes, zswap_slot_size);
    if (pool == NULL)
        return;
    zpool_handle_t *slots = calloc(zswap_slot_count, sizeof *slots);
    // The compressor can expand slightly; give it room so a failure to fit
    // means "incompressible" rather than "buffer too small".
    uint8_t *cbuf = malloc(zswap_slot_size * 2);
    size_t scratch_size = compression_encode_scratch_buffer_size(ZSWAP_ALGO);
    void *scratch = scratch_size > 0 ? malloc(scratch_size) : NULL;
    if (slots == NULL || cbuf == NULL || (scratch_size > 0 && scratch == NULL)) {
        zpool_destroy(pool);
        free(slots); free(cbuf); free(scratch);
        return;
    }
    zswap_slots = slots;
    zswap_cbuf = cbuf;
    zswap_scratch = scratch;
    __atomic_store_n(&zswap_pool, pool, __ATOMIC_RELEASE);
    printk("zswap: on, cap %llu MB, %u slots of %zu bytes\n",
           (unsigned long long) (zswap_max_bytes / (1024 * 1024)),
           zswap_slot_count, zswap_slot_size);
#endif
}

void zswap_configure(uint32_t max_mb) {
    pthread_mutex_lock(&zswap_lock);
    if (!zswap_teardown_locked()) {
        pthread_mutex_unlock(&zswap_lock);
        return;                 // live frames; the existing pool stays
    }
    zswap_max_bytes = (uint64_t) max_mb * 1024 * 1024;
    zswap_stats_live.max_bytes = zswap_max_bytes;
    zswap_build_locked();
    pthread_mutex_unlock(&zswap_lock);
}

void zswap_set_slot_geometry(uint32_t slot_count, size_t slot_size) {
    pthread_mutex_lock(&zswap_lock);
    // The area was rebuilt, so anything the pool held describes slots that no
    // longer mean the same thing. Drop it rather than reinterpret it -- but
    // only if it is empty; see zswap_teardown_locked.
    if (!zswap_teardown_locked()) {
        pthread_mutex_unlock(&zswap_lock);
        return;
    }
    zswap_slot_count = slot_count;
    zswap_slot_size = slot_size;
    zswap_build_locked();
    pthread_mutex_unlock(&zswap_lock);
}

bool zswap_store(uint32_t slot, const void *buf, size_t len) {
#if !defined(ZSWAP_HAVE_COMPRESSION)
    (void) slot; (void) buf; (void) len;
    return false;
#else
    if (!zswap_enabled())
        return false;
    pthread_mutex_lock(&zswap_lock);
    if (zswap_pool == NULL || zswap_slots == NULL ||
            slot >= zswap_slot_count || len != zswap_slot_size) {
        pthread_mutex_unlock(&zswap_lock);
        return false;
    }
    // A slot is allocated per eviction and freed on fault-in, so a second store
    // to a live slot should not happen. Drop the old object rather than leak it
    // if it ever does -- a leak here is permanent, since nothing else knows the
    // handle.
    if (zswap_slots[slot] != ZPOOL_HANDLE_NONE) {
        zpool_free(zswap_pool, zswap_slots[slot]);
        zswap_slots[slot] = ZPOOL_HANDLE_NONE;
    }

    size_t out = compression_encode_buffer(zswap_cbuf, zswap_slot_size * 2,
                                           buf, len, zswap_scratch, ZSWAP_ALGO);
    // 0 is "did not fit", and anything not smaller than a slot saves nothing --
    // both go to the file, where they cost what they always did.
    if (out == 0 || out >= len) {
        zswap_stats_live.store_declined++;
        pthread_mutex_unlock(&zswap_lock);
        return false;
    }
    zpool_handle_t h = zpool_store(zswap_pool, zswap_cbuf, out);
    if (h == ZPOOL_HANDLE_NONE) {       // pool at its cap
        zswap_stats_live.store_declined++;
        pthread_mutex_unlock(&zswap_lock);
        return false;
    }
    zswap_slots[slot] = h;
    zswap_stats_live.stores++;
    zswap_stats_live.bytes_not_written += len;
    pthread_mutex_unlock(&zswap_lock);
    return true;
#endif
}

bool zswap_load(uint32_t slot, void *buf, size_t len) {
#if !defined(ZSWAP_HAVE_COMPRESSION)
    (void) slot; (void) buf; (void) len;
    return false;
#else
    if (!zswap_enabled())
        return false;
    pthread_mutex_lock(&zswap_lock);
    if (zswap_pool == NULL || zswap_slots == NULL ||
            slot >= zswap_slot_count || len != zswap_slot_size ||
            zswap_slots[slot] == ZPOOL_HANDLE_NONE) {
        pthread_mutex_unlock(&zswap_lock);
        return false;
    }
    // Decompress straight into the caller's frame. The compressed bytes stay in
    // the pool: the pager frees the slot when the frame is faulted back for
    // good, and until then a second reader must still find it.
    size_t stored = 0;
    uint8_t *comp = zswap_cbuf;
    if (!zpool_load(zswap_pool, zswap_slots[slot], comp, zswap_slot_size * 2, &stored)) {
        // A handle we issued must always load. If it does not, the table and
        // the pool disagree, and serving the frame from the file is wrong
        // because nothing was ever written there -- say so loudly rather than
        // hand the guest a stale page.
        printk("zswap: BUG -- slot %u has a handle the pool will not load\n", slot);
        zswap_slots[slot] = ZPOOL_HANDLE_NONE;
        pthread_mutex_unlock(&zswap_lock);
        return false;
    }
    size_t back = compression_decode_buffer(buf, len, comp, stored,
                                            zswap_scratch, ZSWAP_ALGO);
    bool ok = back == len;
    if (!ok)
        printk("zswap: BUG -- slot %u decompressed to %zu, wanted %zu\n",
               slot, back, len);
    else
        zswap_stats_live.loads++;
    pthread_mutex_unlock(&zswap_lock);
    return ok;
#endif
}

void zswap_forget(uint32_t slot) {
#if defined(ZSWAP_HAVE_COMPRESSION)
    if (!zswap_enabled())
        return;
    pthread_mutex_lock(&zswap_lock);
    if (zswap_pool != NULL && zswap_slots != NULL && slot < zswap_slot_count &&
            zswap_slots[slot] != ZPOOL_HANDLE_NONE) {
        zpool_free(zswap_pool, zswap_slots[slot]);
        zswap_slots[slot] = ZPOOL_HANDLE_NONE;
        zswap_stats_live.frees++;
    }
    pthread_mutex_unlock(&zswap_lock);
#else
    (void) slot;
#endif
}

void zswap_get_stats(struct zswap_stats *out) {
    pthread_mutex_lock(&zswap_lock);
    *out = zswap_stats_live;
    out->enabled = zswap_pool != NULL;
    if (zswap_pool != NULL) {
        struct zpool_stats ps;
        zpool_get_stats(zswap_pool, &ps);
        out->pool_bytes = ps.pool_bytes;
        out->stored_bytes = ps.stored_bytes;
        out->objects = ps.objects;
        // LIVE original bytes, derived from the live object count, because
        // every object here is exactly one slot. Taking a cumulative
        // "original" counter and dividing it by the current pool size gave a
        // ratio of 9.97x on a run that had already freed everything -- a
        // number with no meaning that looked like a great result.
        out->original_bytes = (uint64_t) ps.objects * zswap_slot_size;
    } else {
        out->pool_bytes = out->stored_bytes = out->original_bytes = out->objects = 0;
    }
    pthread_mutex_unlock(&zswap_lock);
}
