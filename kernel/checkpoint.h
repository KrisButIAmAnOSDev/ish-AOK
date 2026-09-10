// checkpoint.h -- save a running guest to a file and bring it back.
//
// The design, the scope and the honest limits are at the top of checkpoint.c.
// This is only the seam the rest of the kernel calls through.

#ifndef KERNEL_CHECKPOINT_H
#define KERNEL_CHECKPOINT_H

#include <stdbool.h>
#include <stddef.h>

// Written from the guest's own thread, at the top of task_run_current's loop,
// where the task is at a clean boundary: the syscall that asked for the
// checkpoint has already stored its return value and the program counter names
// the instruction AFTER it. That is what makes a restore continue rather than
// re-run.
//
// Returns 0, or a guest _E* code. The guest keeps running either way -- a
// checkpoint is a copy, not a departure.
int checkpoint_save(const char *host_path);

// Rebuild the guest described by the file onto `current`, which must be a
// freshly constructed init (kernel/init.c's become_first_process). The caller
// then lets it run, exactly as it would after do_execve.
int checkpoint_restore(const char *host_path);

// Requested by a write to /proc/ish/checkpoint; performed at the next loop top.
// Consumed by checkpoint_run_pending, which is the only caller of
// checkpoint_save.
void checkpoint_request(const char *host_path);
void checkpoint_run_pending(void);

// What /proc/ish/checkpoint reports. `restored` is how a guest program tells
// the two sides of a checkpoint apart: the write that took it returns
// normally, and so does the same write in the restored guest, because it is
// the SAME instruction stream continuing.
struct checkpoint_status {
    bool restored;            // this guest came back from a file
    unsigned long generation; // how many restores this image has been through
    unsigned long saves;      // checkpoints taken since boot
    int last_err;             // guest _E* code of the last save, 0 if fine
    char last_path[256];
    char last_refusal[256];   // why the last save refused, if it did
    unsigned long long bytes; // size of the last file written
    unsigned long pages;      // guest pages in it
    unsigned long fds;        // descriptors in it
};
void checkpoint_get_status(struct checkpoint_status *out);

#endif
