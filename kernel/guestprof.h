#ifndef KERNEL_GUESTPROF_H
#define KERNEL_GUESTPROF_H

// Guest-PC sampling profiler: where does a guest workload's wall time actually
// go, split across the shared objects it has mapped?
//
// The question this exists to answer is whether library-level native
// interposition (running a host libz/libcrypto in place of the guest's) would
// pay. The engine costs ~6.8 ns per guest instruction dispatch
// (docs/perf_benchmarks_2026_08.md section 3), so a native stand-in is worth
// (guest instructions it replaces) x 6.8 ns minus the trampoline -- and that
// product is only large for functions that burn a lot of instructions per
// call. Guessing which ones those are is how the idea stayed open for months;
// this measures it instead.
//
// WHY A SLOT TABLE AND NOT A WALK OF THE TASK LIST. The sampler runs on its
// own host thread and must not dereference a struct task, which can be freed
// (and is, through a deferred queue) while it reads. So every task publishes
// into a slot it owns in a fixed global array, and the sampler reads nothing
// else: plain atomic words, no pointers, no locks, no lifetime to get wrong.
// The cost on the guest's side is one relaxed store at each of a handful of
// state changes, none of them per-instruction.
//
// WHAT THE PC MEANS. The i386/amd64/arm64 gadget engines keep the guest PC in
// a HOST REGISTER while they run (eip is w28 in jit/gadgets-aarch64/gadgets.h),
// so cpu.eip in memory is stale mid-chain and cannot be sampled from outside.
// What is published instead is the block address at each dispatch through
// cpu_run_to_interrupt's C loop. The engines return to that loop at every
// unchained edge and, at the latest, when frame->chain_budget (8192 dispatches)
// expires -- so a published PC is at most one chain-group old. For a hot loop
// that is exact, because the loop's own blocks are what the budget expires on.
// For long straight-line runs it names where the chain started rather than
// where it is now, which blurs symbol attribution within a DSO and does not
// move the DSO totals, since a chain does not leave its object without an
// indirect branch that exits to C anyway.
//
// TIME OUTSIDE GUEST CODE IS MEASURED TOO, and separated into running-kernel
// and blocked: a workload that spends its wall time waiting on a socket has
// nothing for a codec accelerator to win back, and without that split a big
// "not in the guest" number reads as if it did.
//
//   ISH_GUEST_PROFILE=1            sample at the default 1000 us
//   ISH_GUEST_PROFILE=<interval>   sample every <interval> us
//   ISH_GUEST_PROFILE_OUT=<path>   write the report there instead of stderr
//
// The report is written at exit, alongside lockstats' (main.c).

#include <stdbool.h>
#include <stdint.h>

struct task;

extern bool guestprof_on;

void guestprof_init(void);
void guestprof_dump(void);

// Slot states, published by the owning task and read only by the sampler.
#define GUESTPROF_IDLE    0u  // created, not yet running anything we track
#define GUESTPROF_GUEST   1u  // inside cpu_run_to_interrupt, PC is meaningful
#define GUESTPROF_KERNEL  2u  // in a syscall/fault handler, on-CPU
#define GUESTPROF_BLOCKED 3u  // parked in a wait; on-CPU time is not being used
#define GUESTPROF_NATIVE  4u  // running a natively-implemented program

// Publish. Each is one relaxed store to this task's own slot; all are no-ops
// unless guestprof_on. Called from the task's own thread only.
void guestprof_state(unsigned state);
// Same, for a task other than `current` -- do_exit is called with the task it
// is exiting, which is not always the caller.
void guestprof_state_for(struct task *task, unsigned state);
void guestprof_state_syscall(unsigned long nr);
void guestprof_pc(uint64_t pc, uint64_t mm_id);

// Slot lifetime. Claimed lazily on the first publish from a task's own thread;
// released when the struct is finally freed (kernel/task.c task_free_final),
// which is after that thread is gone.
void guestprof_slot_release(struct task *task);

// Called on the task's own thread at a kernel transition, where `current` is
// set and no address-space lock is held. Snapshots this task's file-backed
// executable mappings if the sampler has asked for them; a no-op otherwise.
// This is why the sampler never touches a struct task: the only thing that
// needs guest structures is done BY the guest thread that owns them.
void guestprof_maps_checkpoint(void);

#endif
