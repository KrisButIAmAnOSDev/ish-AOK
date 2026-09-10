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
//
// Returns 0 if the request was taken, or a guest _E* code if this guest cannot
// be checkpointed at all. The check is here as WELL as in checkpoint_save
// because some refusals mean the deferred save would never run: a native
// program is dispatched by native_exec_run_pending and never comes back to
// task_run_current's loop, so a request made from one would simply sit there
// -- an image that never appears and no error anywhere, which is worse than
// either outcome.
// `and_halt` makes it a SUSPEND rather than a checkpoint: the image is
// written and the guest then stops, so the next launch resumes it instead of
// booting. A checkpoint is a copy and the guest carries on; a suspend is a
// departure.
int checkpoint_request(const char *host_path, bool and_halt);

// Take a checkpoint from a thread that is NOT a guest task -- the app's
// backgrounding path, which runs on the UI thread and has to know the image is
// on disk before iOS freezes it.
//
// Synchronous, unlike checkpoint_request: there is nothing to defer to,
// because the caller is not a task that will come back round a loop, and
// nothing useful to return to if the save has not happened. Every guest task
// is frozen, including the ones the deferred path would have left running.
//
// Returns 0, or a guest _E* code with /proc/ish/checkpoint's last_refusal
// naming the cause.
int checkpoint_save_external(const char *host_path);
void checkpoint_run_pending(void);

// The session file the entry point was given, if any: restored at startup and
// written at suspend. Empty means neither. kernel/checkpoint.c owns the
// string so both halves name the same file.
// ---- the freezer ---------------------------------------------------------
//
// Stopping the machine, which AOK can do because it owns the scheduler.
//
// A task running guest code parks at the top of task_run_current's loop. A
// task blocked INSIDE a syscall is woken the way a signal wakes it, its wait
// returns EINTR, and the dispatcher turns that into a RESTART -- the program
// counter is rewound over the syscall instruction, so the task arrives at the
// loop top about to re-execute the call it was in. That is what makes a
// blocked read() checkpointable: the image says "about to call read", and the
// restored guest calls it.
//
// checkpoint_freeze_pending is read by kernel/calls.c on every syscall return,
// so it is deliberately one relaxed load of one global in the common case.
bool checkpoint_freeze_pending(void);
// Called at the top of task_run_current's loop, with no lock held.
void checkpoint_park_if_frozen(void);
// The same, for a NATIVE program, from its parking place in
// native_checkpoint(). It never reaches task_run_current's loop -- it is a C
// function on a host thread -- so this is where it stops, and where it is
// asked to describe itself, because its state exists on this thread and
// nowhere else.
void checkpoint_native_park(void);

void checkpoint_set_session(const char *host_path);

// Whether the guest may drive this itself through /proc/ish/checkpoint.
//
// The app publishes its Settings switch here; the CLI has ISH_GUEST_CHECKPOINT
// as well. Writing to a /proc file is how a guest reaches every other AOK
// control (swap_evict, snapshot, the JIT knobs), and there is no reason for
// this one to be the exception -- but it hands a guest process a HOST path to
// write, so it is gated rather than open.
void checkpoint_set_guest_control(bool allowed);
bool checkpoint_guest_control(void);
const char *checkpoint_session(void);

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
    unsigned long tasks;      // processes in it
};
void checkpoint_get_status(struct checkpoint_status *out);

#endif
