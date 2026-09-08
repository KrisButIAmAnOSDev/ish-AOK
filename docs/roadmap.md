# iSH-AOK roadmap

What the project intends to do next, and in what order. Written 2026-09-07,
during the 554 release run.

**This is the only document in the tree that says what happens next.** The
others deliberately do not:

- [docs/TODO.md](TODO.md) is a lab notebook. It records what is *known* about
  work that is not done -- the measurement, the rejected designs, the reason --
  and says nothing about whether anyone will do it.
- `docs/build_<N>_musts.md` is a commitment for exactly one release: the work
  deferred out of the last one with the diagnosis already made.
- [docs/book/ch42-where-it-could-go.md](book/ch42-where-it-could-go.md) is the
  narrative version, and it is careful to separate *proven possible* from
  *scheduled* from *thought experiment*. This document is the scheduled column.

A roadmap in a project like this one is a claim that can be checked, and it
should be revised when it is wrong rather than quietly outlived. Every item
below says what is **established** today, what the **next step** is, and how to
**prove** it -- the same shape the rest of the tree uses, for the same reason.

---

## Where 554 leaves things

Not roadmap, but the roadmap does not make sense without it. 554 is 115 commits
past 553 and closes three things that were open questions for a cycle or more:

**Simulated swap**, phases 0 through 2, finishing in this release. It ships off
by default, enabled in Settings with a user-specified size. Phase 3 device
validation is where the value has been: a 3 GB iPhone SE reaches states a 16 GB
iPad never does, and it found that the growth guard did not cover the page-fault
path at all -- a guest could commit 704 MiB past the point where the same total
in separate `mmap`s was refused, and the app was jetsam-killed while every
number AOK watched said it was fine.

**Memory truth.** `mem_resident_page_count` is real, and `/proc/meminfo`
describes the guest instead of the phone. This one matters to the roadmap
directly: it is the prerequisite the swap plan deferred `MemTotal` behind, and
it is also the thing that makes a *resident* page distinguishable from a
*mapped* one -- which is what any checkpoint of a guest has to know.

**The amd64 JIT.** A full amd64 regression suite runs with zero interpreter
fallbacks.

The through-line for what follows: **AOK now has a pager that can take a guest's
memory away and give it back, and knows which pages are really there.** Two of
the three 555 items are that capability pointed somewhere new.

---

## 555 -- persistence

The theme is that a session and a machine should survive things they do not
survive today: the app being killed, and a mistake.

### OS snapshot

Take a point-in-time copy of a machine, and go back to it.

**Established.** A root is a directory in the App Group container:
`<roots>/<name>/data/` holding the host files, and `<roots>/<name>/meta.db`, a
SQLite database holding the uid/gid/mode/device-node metadata the host
filesystem cannot carry. `Roots` (app/Roots.h) already implements import from an
archive, export to an archive, destroy, rename and expose-at-`/AOK/roots/<name>`.

So a snapshot is *expressible* today -- export to a tar and import it back under
a new name -- and nobody uses it that way, because a full archive round trip of
a multi-gigabyte root is minutes of work and a second full copy of the bytes.

**What makes it cheap is a host primitive the tree does not use at all.** APFS
clones: `clonefile(2)` copies a directory tree copy-on-write, so the clone is
near-instant and costs no space until the two copies diverge. Source and
destination must be on the same volume, which they are -- both are inside the
container. There are **zero** uses of `clonefile` or `COPYFILE_CLONE` anywhere
in the tree today, so this is greenfield, but it is greenfield on a well-
supported system call rather than a research question.

**The correctness problem is `meta.db`, not `data/`.** It is SQLite, and cloning
a live database while a guest is writing to it produces a snapshot that may not
open. That has a known answer here: AOK already quiesces the fakefs at suspend
-- `fakefs_quiesce_begin(2000, &stragglers)` in `applicationDidEnterBackground:`
(app/AppDelegate.m) -- for very nearly this reason, being mid-write when the
state is frozen. A snapshot of a *running* root takes the same quiesce, clones
`data/`, and takes `meta.db` through SQLite's own backup rather than a raw
clone.

**Restore of the booted root is a relaunch, and must be.** `Roots.h` already
carries the note explaining why: renaming or deleting the running root moves `/`
out from under the live guest. Restoring one is strictly worse. So restore
follows `defaultRoot` -- it takes effect at the next launch, and the UI says so
rather than pretending otherwise.

**Next step.** Prototype on the CLI first, where a root is an ordinary directory
and there is no app lifecycle in the way: clone a quiesced root, boot the clone,
and diff it against the original. Then the app side.

**Prove it.** Snapshot a multi-gigabyte root and have it complete in under a
second with no meaningful change in container size; boot the snapshot; write to
both copies and confirm they diverge without corrupting each other; and take a
snapshot of a root that is *running* a build, restore it, and find a filesystem
that fsck's clean.

**Where it lives in the UI is [#575](https://github.com/emkey1/ish-AOK/issues/575).**
That issue asks for a delete button for machines and the capability already
exists behind it (`destroyRootNamed:`), so the Machines screen is being touched
regardless. Snapshot, restore and delete belong in one place, and shipping the
delete button alone would mean touching that screen twice.

### Suspend to disk

Save a running guest and bring it back after the app is gone.

**Why this is newly plausible, and it is not ambition.** The swap pager is most
of the machinery already, built and shipping in 554:

- a backing store with a slot allocator, and frames that own their slots;
- exact mapping ownership, with per-frame entry counts on `struct data`;
- eviction that writes a guest frame out, and a fault path that brings it back;
- `mem_resident_page_count` and a per-entry state byte, so *which* pages are
  real is a measurement rather than a guess;
- an address-space barrier that quiesces every thread of a process;
- a suspension gate already wired to the iOS lifecycle, next to
  `fakefs_quiesce_begin` and `sockrestart_on_suspend`.

Suspend to disk is that pager told to evict *everything*, plus the state the
pager does not carry: page tables, task and thread state, and the file
descriptor table.

**What it is not.** Not CRIU, and the scope has to say so out loud, because the
gap between "checkpoint a process" and "checkpoint *any* process" is where this
class of feature usually dies. CRIU on real Linux, with a real kernel's
cooperation, is still partial after a decade. The v1 that is worth having is
narrower and more useful than the general one: **survive the app being killed.**
iOS terminates this app routinely -- jetsam, memory pressure, a user swiping it
away -- and today that loses the session unconditionally. Same device, same
root, same build, back where you were. That is the whole promise.

**The three hard parts, honestly.**

*Host file descriptors.* Every `struct fd` carries a `real_fd` into the host,
and on restore that number means nothing. There are **eighteen** `fd_ops`
families in the tree -- realfs, tmpfs, aokfs, fuse, procfs, sysfs, proc_ns,
devpts, socket, fscontext, opath_link, epoll, eventfd, inotify, memfd, pidfd,
signalfd, timerfd -- and each needs its own re-materialisation rule. A regular
file re-opens by path and seeks to its offset, and fakefs paths are stable, so
the common case is genuinely easy. An
unlinked file, a pipe with bytes in it, a pty, a live TCP connection: each is
its own decision, and for some the honest answer is that it cannot come back.
**`sockrestart` is the precedent and the right model** -- it does not restore a
listening socket, it records enough to *rebuild* one, because iOS destroys the
original either way.

*Native programs are host code with a C stack.* This is the deepest one, and the
project already has the rule that names it: a native program is a function call.
There is no serialising a host C stack, and the shell the user is typing at is
usually native bash. But the answer already exists in miniature: bash knows how
to describe itself and re-launch -- `AOK_BASH_DUMP_STATE`
(`deps/bash/aok_fork.c:823`), written for a different reason entirely. So the
rule generalises: **a native program either knows how to dump its own state, or
the checkpoint refuses while it is running.** That is a capability boundary that
can be stated and reported, which is the difference between a limit and a lie.

*What a quiet point is.* v1 checkpoints at a boundary -- every guest task at a
syscall, no native program on the stack -- rather than at an arbitrary
instruction. That is a real restriction and it should be written into the design
rather than discovered.

**Next step is phase 0, and phase 0 is a gate, not a feature.** Two things, in
order. First, an inventory: walk a real booted guest and enumerate everything
held that cannot be trivially serialised, per `fd_ops` family, with counts --
because the interesting number is not whether a pty is hard, it is how many of
the fds in a normal session are the easy kind. Second, the narrowest possible
proof: checkpoint and restore a single-process guest, no native program, one
open file, on the CLI harness.

**Prove it.** The restored guest continues from the instruction after the
checkpoint, reads the same bytes from the same open file at the same offset, and
the four-arch suite passes on a guest that has been through a checkpoint/restore
cycle mid-run.

**The risk, stated in advance.** This is the shape of feature that is 80% done
quickly and then spends a long time on the last 20%, because the last 20% is
every program anyone actually runs. If phase 0's inventory says the common
session is mostly regular files and a pty, it is worth doing. If it says the
common session is full of things with no restore rule, that is a result, and the
right response is to publish the inventory and stop -- not to spend 556 and 557
discovering it slowly.

### Desktop groundwork

The Wayland work is shipped, both tiers. `DisplayRFBClient`, `DisplayRFBView`,
Metal shaders, `opt/AOK/tools/setup-wayland.sh` and `start-wayland.sh` are all in
the tree, and the applet runs a real wlroots compositor over VNC. **So the
desktop theme through 555--557 is polish and reach, not construction** -- which
is worth saying because the open issues read like the feature does not exist.

Two of them are the app's own chrome and both hurt a windowed session
disproportionately:
[#580](https://github.com/emkey1/ish-AOK/issues/580) puts the window controls
over the terminal content in windowed mode on iPadOS while fullscreen is correct
-- a layout-guide bug, not anything deep -- and
[#579](https://github.com/emkey1/ish-AOK/issues/579) loses keyboard focus after
a Magic Keyboard trackpad selection. A desktop that drops the keyboard when you
click is not a desktop.

And [#483](https://github.com/emkey1/ish-AOK/issues/483)'s second half: the
client still requests a fixed resolution pair rather than deriving it from the
window. The mechanism is already there -- `d60437caf` drives per-orientation
resize through RFB `SetDesktopSize`.
[#482](https://github.com/emkey1/ish-AOK/issues/482) is a screenshot with no
text and is very likely the same root cause; **confirm that before treating them
as two jobs**, because if they are one, this is a smaller item than the issue
count suggests.

---

## 556 -- the desktop is the product

555's persistence work continues here (suspend to disk phase 1: the fd
re-materialisation rules, and more than one process), but the headline moves to
making the graphical session something to recommend rather than something that
works.

**[#574](https://github.com/emkey1/ish-AOK/issues/574), desktop environments.**
The request is for something the tree can already install --
`setup-wayland.sh` puts the stack in place -- so the gap is packaging and
documentation, not capability. That makes it cheap and high-leverage: a
supported DE choice, a one-command setup, and a page of documentation that says
what works. Users do not know AOK does this. That sentence is true of several
things here and it is a product problem, not an engineering one.

**Pixman v2 coverage.** `pixman_accel_plan.md` has phases 0--2 done and verified
end-to-end against unmodified labwc and foot, with mask/`OVER_MASK_A8` named as
the biggest remaining gap and an app Settings toggle still missing. This is
directly the desktop's frame rate, and the measurement that justified it (~23.5%
of an interactive redraw window inside raw pixman) was taken on exactly the
workload 556 is about.

**Input, seriously.** Pointer, keyboard, modifiers, scroll, and what a trackpad
gesture means to a Wayland client. #579 is the first symptom rather than the
whole job.

---

## 557 -- reach

**3D acceleration, [#484](https://github.com/emkey1/ish-AOK/issues/484) -- as a
feasibility gate, not as a feature.** This is the largest open request and the
one most likely to consume a release without producing anything. virglrenderer
needs a host GL or GLES implementation to render against; iOS has Metal and
deprecated OpenGL ES years ago, so the only plausible path is ANGLE over Metal,
and that is a substantial dependency to carry into an app binary. Book ch42
notes it would need the Wayland work to land first -- it has landed, which is
why this is now a question worth asking rather than a deflection.

Treat it exactly like the Metal sgemm study and the JIT code cache study: a
scoped investigation with a **go/no-go gate and a number attached**, before any
estimate. The JIT code cache is the precedent worth remembering -- phase 0
measured translation at 3.5--6% of wall time against a 30% gate and the project
correctly did not build it. A no-go here is a good outcome, not a wasted
release.

**Suspend to disk ships**, behind a Settings switch and off by default, on the
same reasoning swap ships that way: a feature that spends the user's storage and
can lose their session is one they opt into.

---

## Carried, not headlined

Work that fills the space between the items above. None of it is scheduled to a
release; all of it is ready to pick up, and the conformance items in particular
are what the release-run regression sweeps keep landing on.

**The debugging tools do not work on this kernel, and that taxes everything
else.** This entry said `strace` and `gdb` kill *the thread they attach to*,
because threads here are children of their creator rather than of the leader's
parent, so a wait after attaching to a non-leader resolves to the wrong task.
**Re-measured 2026-09-08, and that is not the bug.** The attach is fine and the
tracing is fine; the *clean detach* kills the target, and it does so whether the
target is a thread or a group leader -- while a tracer that is SIGKILLed and
never detaches leaves it alive. Separately, `waitpid(<tid>, __WALL)` on a traced
non-leader hangs where `waitpid(-1, __WALL)` returns correctly. Both are in
[docs/build_555_musts.md](build_555_musts.md) with the measurements, and
[#503](https://github.com/emkey1/ish-AOK/issues/503) -- the amd64 cousin -- is
now closed. The cost is not the bug, it is that every future diagnosis is done
without the two tools that would answer it fastest -- the swap investigation had
to settle a CPU-spin question from `/proc/<pid>/io` counters for exactly this
reason. **This is the item most likely to be worth more than its place in the
list**, and 555 promotes it out of this section: it is the prerequisite for the
suspend-to-disk inventory, not a parallel track.

**The conformance long tail**, all in TODO.md with measurements: `PROT_EXEC` is
never enforced, so guest W^X is decorative -- a contained project with two
candidate designs, an identified obstacle in fault delivery across four dispatch
loops, and a note that it deserves its own before/after benchmark run rather
than being folded into a sweep. PI futexes are ENOSYS, and the lock half is
implementable while the priority-inheritance half is not -- so the honest shape
is documented up front. `pipe(2)` cannot honour PIPE_BUF atomicity while it
delegates to a host pipe. `tmpfs size=` is accepted and not enforced, which on a
device with a jetsam budget is host memory. `fcntl(F_GETFL)` reports an
`O_NONBLOCK` the guest never set. `/proc/locks` does not exist. FUSE has no
attribute cache, and three separate absences follow from that one gap.

**The reported-issue burn-down.** Eighteen open, and two of them are the same
network question from different angles:
[#568](https://github.com/emkey1/ish-AOK/issues/568) reports slow throughput and
[#523](https://github.com/emkey1/ish-AOK/issues/523)'s reproducible half is a
15.3 s TLS handshake tail against a sub-second median. That is a wait not being
woken rather than work being slow, which puts it in the poll/quiesce
neighbourhood -- and it reproduces with plain `curl`, so it needs neither Go nor
the AUR to chase.

**Closing issues is part of fixing bugs.**
[#541](https://github.com/emkey1/ish-AOK/issues/541) has been fixed since
2026-08-20 and is still open on GitHub. A fix the reporter never hears about did
not fully happen.

---

## Not on the roadmap, and why

Naming these is the point of the document. A "future directions" list that
contains everything commits to nothing, and an unmerged branch quietly becomes a
promise if nobody says otherwise.

**External display / AirPlay ([#540](https://github.com/emkey1/ish-AOK/issues/540)).**
Work exists on `worktree-external-display-540` and is deliberately unmerged --
the maintainer judged it flawed. It stays fenced. It is not "coming in a future
release", and it must not be swept into one by accident.

**DriverKit and raw USB.** M-series iPad only, an entitlement Apple grants per
app against a specific hardware justification and is unlikely to grant for a
terminal, and it vanishes in unsigned builds -- so it would split the user base
for a feature most users could not run. The survey is in TODO.md under *Host
capabilities worth exposing*. The useful half of that survey is that `iosfs`
already mounts USB storage through the document picker and **nobody knows**,
which is a documentation task, not a driver.

**Namespaces.** Architectural, not a gap -- and the Bedrock-AOK experience is
the evidence that it is the right call: the two capabilities that community
project actually needed were `bind_mount` and FUSE, both of which now exist,
and namespaces explicitly were not the ask.

**A guest address space for native programs.** Settled by measurement: it would
not produce `fork` anyway, and the memory lock costs 33--39x on tight access.

**A persistent JIT code cache.** Phase 0 ran and returned NO-GO with numbers:
translation is 3.5--6% of wall time against a 30% gate. Recorded so it is not
re-proposed.

**The WebKit/Wasm architecture.** A thought experiment, filed as one, and
valuable for what it revealed rather than as a plan: the moment syscalls are
answered by browser storage APIs, fakefs's uid/gid/mode/device-node model has
nowhere to live.

---

## Bluetooth LE -- unscheduled, and the best unclaimed item here

Not placed in a release because nothing above depends on it and it has no
deadline, but it is the highest ratio of *genuinely new capability* to *known
cost* on the list, and it should not get lost between the scheduled items.

CoreBluetooth's central role needs no MFi programme, no Apple-granted
entitlement and no vendor agreement: scan, connect, discover, read/write/notify
against any BLE peripheral, for the price of one Info.plist key and a user
prompt. `/dev/bluetooth` follows the `/dev/url` recipe that is already proven in
the tree, and app/LocationDevice.m is 190 lines for a read-only device -- this
one is read/write and stateful, so budget several hundred, plus the same five
registration points.

Two things decide whether it is done well, both in TODO.md: **do not fake
BlueZ** -- synthesising HCI-level events from a GATT-level API reports
controller states no real controller produces -- and App Review will ask why a
terminal wants Bluetooth, for which the precedent is already shipping in
`/dev/location`.

---

## Revising this document

Three rules, learned from the documents this one sits beside.

**Dates and numbers, not adjectives.** Every claim above is checkable. When one
of them turns out to be wrong, the correction is more valuable than the original
-- `docs/historical/build_553_musts.md` is kept precisely because the next cycle
found its diagnosis wrong in the optimistic direction.

**Status sections go stale faster than plans.** The swap plan's "still open"
lists were describing `/proc/swaps`, `swapon`/`swapoff` and `mincore` as
outstanding after all three had landed. If a status paragraph here is more than
a release old, distrust it and read the commits.

**A release that changes the roadmap is not a failure of the roadmap.** Swap's
phase 3 found a gap that defeated the feature's core promise, and finding it was
worth more than the schedule it broke.
