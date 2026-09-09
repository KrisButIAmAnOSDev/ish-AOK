// The compressed-page pool. See kernel/zpool.h for why it exists and what the
// size-class design costs.
//
// STRUCTURE. One free list per size class, threaded through the free entries
// themselves so an empty pool costs nothing per entry. A slab is one
// ZPOOL_PAGE_SIZE block carved into equal entries of its class's size; slabs
// live in a growable table so a handle can name one by index rather than by
// pointer, which keeps handles stable and 64-bit.
//
// HANDLE ENCODING, and it is deliberately not a pointer:
//
//     bits 63..24  slab index      (40 bits -- 2^40 slabs is far past the cap)
//     bits 23..12  entry index     (12 bits -- at most 16 entries per slab)
//     bits 11..0   stored size     (12 bits -- 0..4095, and 4096 is impossible
//                                   because an object that size is refused)
//
// A handle is therefore self-describing: freeing or loading needs no side
// table, and a zeroed page-table entry reads as ZPOOL_HANDLE_NONE. The +1 on
// the slab index is what makes an all-zero handle mean "nothing", so that
// sentinel does not cost a real slab.
#include <stdlib.h>
#include <string.h>

#include "kernel/zpool.h"

struct zpool_slab {
    uint8_t *mem;
    uint16_t class_index;     // 0-based: entry size is (class_index+1)*GRANULE
    uint16_t entries;
    uint16_t used;
    uint16_t free_head;       // entry index, or ZPOOL_NO_ENTRY
    uint32_t next_partial;    // slab index + 1, or 0
};

#define ZPOOL_NO_ENTRY 0xFFFF

struct zpool {
    struct zpool_slab *slabs;
    uint32_t slab_count;
    uint32_t slab_capacity;
    // Head of each class's partial-slab list: slab index + 1, or 0.
    uint32_t partial[ZPOOL_CLASSES];
    uint64_t max_bytes;
    struct zpool_stats stats;
};

static size_t class_entry_size(uint16_t class_index) {
    return ((size_t) class_index + 1) * ZPOOL_GRANULE;
}

static uint8_t *entry_ptr(struct zpool_slab *slab, uint16_t entry) {
    return slab->mem + (size_t) entry * class_entry_size(slab->class_index);
}

struct zpool *zpool_create(uint64_t max_bytes) {
    struct zpool *pool = calloc(1, sizeof *pool);
    if (pool == NULL)
        return NULL;
    pool->max_bytes = max_bytes;
    return pool;
}

void zpool_destroy(struct zpool *pool) {
    if (pool == NULL)
        return;
    for (uint32_t i = 0; i < pool->slab_count; i++)
        free(pool->slabs[i].mem);
    free(pool->slabs);
    free(pool);
}

// Add a slab for `class_index` and return its index, or UINT32_MAX.
static uint32_t slab_new(struct zpool *pool, uint16_t class_index) {
    if (pool->max_bytes != 0 &&
            pool->stats.pool_bytes + ZPOOL_PAGE_SIZE > pool->max_bytes)
        return UINT32_MAX;

    if (pool->slab_count == pool->slab_capacity) {
        uint32_t want = pool->slab_capacity == 0 ? 16 : pool->slab_capacity * 2;
        struct zpool_slab *grown = realloc(pool->slabs, (size_t) want * sizeof *grown);
        if (grown == NULL)
            return UINT32_MAX;
        pool->slabs = grown;
        pool->slab_capacity = want;
    }

    uint8_t *mem = malloc(ZPOOL_PAGE_SIZE);
    if (mem == NULL)
        return UINT32_MAX;

    uint32_t idx = pool->slab_count++;
    struct zpool_slab *slab = &pool->slabs[idx];
    slab->mem = mem;
    slab->class_index = class_index;
    slab->entries = (uint16_t) (ZPOOL_PAGE_SIZE / class_entry_size(class_index));
    slab->used = 0;
    slab->free_head = 0;
    slab->next_partial = pool->partial[class_index];
    pool->partial[class_index] = idx + 1;

    // Thread the free list through the entries themselves: each free entry
    // holds the index of the next one. Every class is at least GRANULE bytes,
    // so there is always room for the uint16_t.
    for (uint16_t e = 0; e < slab->entries; e++) {
        uint16_t next = (uint16_t) (e + 1 == slab->entries ? ZPOOL_NO_ENTRY : e + 1);
        memcpy(entry_ptr(slab, e), &next, sizeof next);
    }

    pool->stats.pool_bytes += ZPOOL_PAGE_SIZE;
    pool->stats.slabs++;
    return idx;
}

// Drop a slab from its class's partial list. Called when it fills.
static void partial_remove(struct zpool *pool, uint32_t idx) {
    struct zpool_slab *slab = &pool->slabs[idx];
    uint32_t *link = &pool->partial[slab->class_index];
    while (*link != 0) {
        uint32_t cur = *link - 1;
        if (cur == idx) {
            *link = pool->slabs[cur].next_partial;
            pool->slabs[cur].next_partial = 0;
            return;
        }
        link = &pool->slabs[cur].next_partial;
    }
}

zpool_handle_t zpool_store(struct zpool *pool, const void *data, size_t size,
                           size_t original_size) {
    // An object that needs a whole page or more saves nothing and cannot be
    // encoded (the size field is 12 bits). The caller stores those raw.
    if (pool == NULL || size == 0 || size >= ZPOOL_PAGE_SIZE) {
        if (pool != NULL)
            pool->stats.store_failures++;
        return ZPOOL_HANDLE_NONE;
    }

    uint16_t class_index = (uint16_t) ((size + ZPOOL_GRANULE - 1) / ZPOOL_GRANULE - 1);
    uint32_t idx;
    if (pool->partial[class_index] != 0) {
        idx = pool->partial[class_index] - 1;
    } else {
        idx = slab_new(pool, class_index);
        if (idx == UINT32_MAX) {
            pool->stats.store_failures++;
            return ZPOOL_HANDLE_NONE;
        }
    }

    struct zpool_slab *slab = &pool->slabs[idx];
    uint16_t entry = slab->free_head;
    uint16_t next;
    memcpy(&next, entry_ptr(slab, entry), sizeof next);
    slab->free_head = next;
    slab->used++;
    if (slab->free_head == ZPOOL_NO_ENTRY)
        partial_remove(pool, idx);

    memcpy(entry_ptr(slab, entry), data, size);

    pool->stats.objects++;
    pool->stats.stored_bytes += size;
    pool->stats.original_bytes += original_size;
    pool->stats.stores++;

    return ((zpool_handle_t) (idx + 1) << 24) |
           ((zpool_handle_t) entry << 12) |
           (zpool_handle_t) size;
}

// Decode, and validate hard enough that a corrupt handle cannot be turned into
// an out-of-bounds access. A page-table entry is guest-adjacent state, so a bad
// value here has to be an error rather than a memcpy from nowhere.
static bool handle_decode(struct zpool *pool, zpool_handle_t handle,
                          struct zpool_slab **slab_out, uint16_t *entry_out,
                          size_t *size_out) {
    if (pool == NULL || handle == ZPOOL_HANDLE_NONE)
        return false;
    uint64_t slab_id = handle >> 24;
    uint16_t entry = (uint16_t) ((handle >> 12) & 0xFFF);
    size_t size = (size_t) (handle & 0xFFF);
    if (slab_id == 0 || slab_id > pool->slab_count || size == 0)
        return false;
    struct zpool_slab *slab = &pool->slabs[slab_id - 1];
    if (entry >= slab->entries)
        return false;
    if (size > class_entry_size(slab->class_index))
        return false;
    *slab_out = slab;
    *entry_out = entry;
    *size_out = size;
    return true;
}

bool zpool_load(struct zpool *pool, zpool_handle_t handle,
                void *out, size_t out_size, size_t *size_out) {
    struct zpool_slab *slab;
    uint16_t entry;
    size_t size;
    if (!handle_decode(pool, handle, &slab, &entry, &size))
        return false;
    if (out_size < size)
        return false;
    memcpy(out, entry_ptr(slab, entry), size);
    if (size_out != NULL)
        *size_out = size;
    pool->stats.loads++;
    return true;
}

void zpool_free(struct zpool *pool, zpool_handle_t handle) {
    struct zpool_slab *slab;
    uint16_t entry;
    size_t size;
    if (!handle_decode(pool, handle, &slab, &entry, &size))
        return;

    bool was_full = slab->free_head == ZPOOL_NO_ENTRY;
    memcpy(entry_ptr(slab, entry), &slab->free_head, sizeof slab->free_head);
    slab->free_head = entry;
    slab->used--;
    if (was_full) {
        // Back onto its class's partial list, so the space is reused. Without
        // this a pool that fills and then empties would keep allocating new
        // slabs and never shrink in practice.
        slab->next_partial = pool->partial[slab->class_index];
        pool->partial[slab->class_index] = (uint32_t) (slab - pool->slabs) + 1;
    }

    pool->stats.objects--;
    pool->stats.stored_bytes -= size;
    pool->stats.frees++;
}

void zpool_get_stats(struct zpool *pool, struct zpool_stats *out) {
    if (pool == NULL) {
        memset(out, 0, sizeof *out);
        return;
    }
    *out = pool->stats;
}
