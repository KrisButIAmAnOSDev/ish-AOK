#ifndef FS_FAKE_SNAPSHOT_H
#define FS_FAKE_SNAPSHOT_H

#include <stddef.h>

// How long to wait for in-flight fakefs transactions to drain before copying.
// The app's suspension handler uses 2000 ms against an iOS deadline; a snapshot
// has no such deadline, but waiting much longer would mean a guest with one
// genuinely stuck transaction hangs the caller instead of reporting it.
#define FAKEFS_SNAPSHOT_QUIESCE_MS 3000

struct fakefs_snapshot_stats {
    // Whether the quiesce reached zero in-flight transactions. False means the
    // copy went ahead anyway with `quiesce_stragglers` still open -- see the
    // reasoning in fake-snapshot.c.
    bool quiesced;
    unsigned quiesce_stragglers;
    // Directory entries walked. Zero on Darwin, where clonefile does the walk
    // internally and does not report a count.
    unsigned long entries;
    unsigned long clone_ms;     // the data directory
    unsigned long db_ms;        // meta.db, through sqlite3_backup_*
    unsigned long total_ms;     // including the quiesce wait
    char error[256];
};

// Snapshot the fakefs root whose data directory is `src_data` (that is,
// `<root>/data`) into the new root directory `dst_root`, which must not exist.
//
// Returns 0 or a guest _E* code (which are negative in this tree). On failure
// the destination is left with an INCOMPLETE marker file in it rather than
// being removed, because there is no recursive remove here to call and a
// half-finished root that looks bootable is the worse outcome.
//
// Cost is proportional to the number of directory entries, not to the number of
// bytes: see the measurements at the top of fake-snapshot.c before assuming
// this is instant.
int fakefs_snapshot(const char *src_data, const char *dst_root,
                    struct fakefs_snapshot_stats *stats);

#endif
