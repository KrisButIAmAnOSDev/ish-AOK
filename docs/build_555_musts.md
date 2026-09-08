# build 555 musts

Work carried out of 554, with the diagnosis already done so nobody has to
re-derive it. Each entry says what is **established**, what the **next step**
is, and how to **prove** it afterwards.

Started 2026-09-08, immediately after `builds/iSH-AOK_554` was tagged.
Supersedes [docs/historical/build_554_musts.md](historical/build_554_musts.md).

**Three of that document's ten open entries were already fixed when it was read
at the top of this cycle**, two of them by work that never mentioned them — see
*Closed during 554 while the doc still listed them* at the bottom. A fourth had
a diagnosis that does not reproduce at all. That is a 40% staleness rate on a
six-day-old document, and it is the reason every entry below was re-measured
before being carried rather than copied forward.

The release's **theme** is in [docs/roadmap.md](roadmap.md) and it is
persistence. This document is the other half: the work that has to happen
around it.

---

## Order for the cycle

The roadmap says of the debugging tools that this is "the item most likely to be
worth more than its place in the list". 555's headline is a checkpoint/restore
feature whose phase 0 is *an inventory of what a live guest is holding* — which
is precisely the work a functioning `strace` and `gdb` make cheap and an absent
one makes archaeology. So:

1. **The debugging tools** (§1 below). It is a prerequisite for the headline,
   not a parallel track, and the re-measurement below has already made it a much
   smaller job than the carried entry suggested.
2. **OS snapshot** — roadmap. The cheaper and more certain of the two
   persistence items, and it lands #575 on the way past.
3. **Suspend to disk phase 0** — roadmap, and *a gate*. Publish the fd
   inventory; if it says the common session is full of things with no restore
   rule, that is the result and the right move is to stop.
4. **Desktop chrome** — #580, #579, and #483/#482 (confirm they are one bug
   before scheduling two).
5. **The rest of this document**, as fill.

§§2–4 below are cheap, bounded, and each has a written next step; they are the
right things to pick up when a headline item is blocked on a device run.

---

## 1. A clean ptrace detach kills the tracee

**This replaces the carried entry "strace and gdb kill the process they attach
to", whose diagnosis does not reproduce.** That entry said the cause was
"a wait after attaching to a non-leader thread resolves to the wrong task". It
is not, and the symptom is not about non-leader threads either.

**Established, measured 2026-09-08** on `build/alpine-arm64-test`, with the same
script run against the Devuan 6 / Linux 6.12 oracle:

| case | AOK | Linux |
|---|---|---|
| `strace -p <non-leader tid>`, SIGINT to make it detach | target **DEAD** | alive |
| `strace -p <leader>` of a multithreaded process, SIGINT | target **DEAD** | alive |
| `strace -f -p <leader>`, SIGINT | target **DEAD** | alive |
| `strace -p <non-leader tid>`, tracer **SIGKILLed** | target ALIVE | alive |

Four things follow, and each narrows the search:

- **It is not about non-leader threads.** Tracing the group leader kills it too.
- **It is not the attach, and it is not the tracing.** The target was checked
  alive mid-trace in every case, and `strace -c` produced a correct profile —
  260 `nanosleep` calls counted, "Process N attached" and "Process N detached"
  both printed. The trace works.
- **It is the detach.** The one case that survives is the one where the tracer
  is SIGKILLed and never detaches. AOK's *implicit* detach-on-tracer-death path
  (`ptrace_detach_from_tracer`, kernel/exit.c:194) is correct; the *explicit*
  `PTRACE_DETACH` path is not.
- **And it is not plain `PTRACE_ATTACH` + `PTRACE_DETACH`**, which a C probe
  runs cleanly against both a thread and a leader with the target surviving. So
  the fault is in what `strace` does that the probe does not: `PTRACE_SEIZE`,
  `PTRACE_INTERRUPT`, `PTRACE_SETOPTIONS`, or the detach of an
  interrupt-stopped tracee.

**One structural difference is already visible and is the first thing to look
at.** `PTRACE_DETACH_` (kernel/ptrace.c:1139) calls
`ptrace_resume_child_locked(child, sig, false, false, true)`, which clears
`traced`/`tracer`/`options` — and **never removes the tracee from the tracer's
`ptracees` list**. `list_add(&tracer->ptracees, ...)` happens at
kernel/ptrace.c:111 and 780; there is no matching `list_remove` anywhere in
`kernel/ptrace.c`. Only the exit-time path removes it
(`list_remove_safe(&tracee->ptrace_siblings)`, kernel/exit.c:218) — which is
exactly the path that does **not** kill the target. So after a clean detach the
tracer still lists a task it no longer traces, and `do_wait`'s ptracees loop
(kernel/exit.c) and `do_exit`'s ptracees sweep (kernel/exit.c:615) both still
walk it.

**A second, separate bug found on the way, and it is the gdb one.**
`waitpid(<tid>, ..., __WALL)` on a traced non-leader thread does not return —
`waitpid(-1, ..., __WALL)` on the same stop returns the right tid immediately
(status `0x137f`, byte-identical to the oracle). `do_wait`'s `P_PID_` branch
admits the thread (`id_is_thread && traced_by_us`) and then does
`task = task->group->leader;` before testing for a stop, so it inspects the
leader's state and the thread's ptrace-stop is never seen. gdb's
`linux_nat_post_attach_wait` waits on the specific pid it attached to, which is
this path.

**Next step.** Three, in order, and the first two are cheap:

1. Bisect the strace detach sequence with a C probe — `SEIZE` alone, then
   `SEIZE`+`INTERRUPT`, then each with `SETOPTIONS` — until one of them turns
   `ALIVE` into `DEAD`. (`tests/manual/` has `ptrace_attach.c`,
   `ptrace_group_stop.c` and `ptrace_thread_follow.c` to build from.) Use
   `sigaction` without `SA_RESTART` for the probe's alarm, or a hung case blocks
   the whole probe instead of reporting.
2. Fix the `P_PID_` thread resolution in `do_wait`: a traced non-leader must be
   tested for a ptrace-stop as itself, not through `task->group->leader`.
3. Make `PTRACE_DETACH` unlink `ptrace_siblings` the way the exit path does,
   and re-measure. This may be the whole bug or merely a real defect beside it;
   both are worth closing.

**Prove it.** A `tests/manual` test — the natural name is
`ptrace_detach_survives` — that attaches to a thread and to a leader, by
`ATTACH` and by `SEIZE`+`INTERRUPT`, detaches cleanly each time, and requires
the target alive afterwards; plus `waitpid(<tid>, __WALL)` returning that tid.
Then the tools themselves: `strace -p <tid>` for ten seconds followed by a clean
detach leaves the target running, and `gdb -p <tid>` on a live multithreaded
guest process prints a backtrace and detaches with the process still running.

**Why this is first.** [#503](https://github.com/emkey1/ish-AOK/issues/503) is
closed and riscv64 single-step is fixed, so the debugger works right up until
you let go of it. Every diagnosis in this cycle — the fd inventory above all —
is done with the tools or without them.

---

## 2. `lock not` and `lock neg` are SIGILL on an i386 guest

**Established, and re-verified 2026-09-08.** The i386 `LOCK` table in
`emu/decode.h` (the `case 0xf0:` block, ending at the `default: UNDEFINED` near
line 1689) has the ALU pairs, the `80/81/83` immediate group, the `0F` atomics,
`86/87` xchg and `FE/FF` inc/dec — **and still no group-3 entry**. There is no
`case 0xf6` or `0xf7` in it. So `lock notl (mem)` and `lock negl (mem)` fall
through to `UNDEFINED` and kill the guest with SIGILL. Real Linux runs both.
Found by `tests/manual/x86/atomic_lock_contended.c`, which skips those two forms
on i386 for exactly this reason and says so.

**Next step.** Unchanged from the carried entry, which was correct: add `not`
and `neg` to the `.irp` list in `do_op_size_atomic`
(`jit/gadgets-aarch64/math.S:2217`) — `not` is `mvn` with **no** flag changes,
`neg` is `0 - operand` with the full sub flag rule — then add `case 0xf6`/`0xf7`
with a group-3 switch to the LOCK table.

**Prove it.** Un-skip the two forms in `atomic_lock_contended` and require the
i386 leg to pass, plus the whole i386 atomics set: `do_op_size_atomic` is shared
by every i386 atomic, so a mistake there breaks all of them.

---

## 3. An iosfs mount made through the new mount API does not persist

**Established, and re-verified 2026-09-08**: there is still no `relocated` hook
in `struct fs_ops` and no caller of one — `grep relocated kernel/fs.h fs/mount.c
fs/iosfs.m` is empty. Carried unchanged through 553 and 554.

iosfs keys its security-scoped bookmark on `mount->point` at mount time, and a
mount made through `fsopen`/`fsconfig`/`fsmount`/`move_mount` is created at a
private staging path (`/.ish-fsmount/<n>`) and relocated later, so the key is
wrong. 552 stopped persisting staging-path keys because persisting them
resurrected a permanent phantom mount on every launch; the cost is that such a
mount no longer survives a relaunch.

**Next step.** Re-key at relocation. `mount_relocate` (`fs/mount.c`) has the
mount and both paths; an optional `relocated(mount, old_point)` in
`struct fs_ops` (`kernel/fs.h`) lets iosfs move the bookmark to the real path.
Call it after unlocking `mounts_lock`. Note the bookmark is not merely
*mis-keyed* at mount time, it is **not stored at all**, so iosfs also needs a
non-persisted side table keyed by the staging path to move from — or it must
re-derive the bookmark, which needs the security-scoped URL it only holds during
`iosfs_mount`.

**Prove it.** Mount an iCloud directory with a util-linux `mount(8)` new enough
to use the new API, relaunch the app, and require the mount back at the path the
user asked for — with nothing under `/.ish-fsmount/` in `/proc/mounts` either
before or after. Needs a device and an app relaunch, which is why it has now
been deferred out of two releases; if it is not scheduled against a device run
in 555 it should be moved to TODO.md and stop being called a must.

---

## 4. POLLHUP without POLLIN on a closed socket

**Established, and *not* re-measured — measure before fixing.** On a unix
socketpair whose peer has closed, `poll(POLLIN)` returned `revents=0x10`
(POLLHUP alone) under AOK against `0x11` (POLLIN|POLLHUP) on Linux 6.12,
measured by `tests/manual/poll_idle_cpu.c`, which accepts either because it is
testing something else. Linux sets POLLIN as well because a closed socket **is**
readable: a read returns 0 for EOF. A program that waits for POLLIN before
reading, and treats POLLHUP as informational, never reads the EOF it is being
told about.

**The measurement is a cycle old and `sock_poll` changed underneath it.**
554 added the `conn_dead` arm (`fs/sock.c:8709`), which returns
`POLL_ERR | POLL_HUP` and **deliberately excludes POLL_READ**, with a comment
explaining that a poll loop told "readable" and then handed an error by every
`recv` is a 100%-CPU spin. That is a different case — an iOS-killed connection,
where there is no EOF to read — but it is close enough that a fix here must not
be pattern-matched onto it.

**Next step.** Re-measure the socketpair case against the oracle first. If it
still diverges, find where the peer-closed result is composed and add POLL_READ
alongside POLL_HUP *for that case only*. Check the half-close case separately:
`fs/sock.c:8724` already has careful reasoning about EPOLLRDHUP vs EPOLLHUP,
paid for with a zero-length send, and it must not be disturbed.

**Prove it.** A test asserting `revents == POLLIN|POLLHUP` after the peer
closes, checked against the oracle first, plus `poll_idle_cpu` still passing —
including its CPU-cost assertion, which is what the `conn_dead` arm exists to
protect.

---

## 5. `tty_hangup_signal` failed once on device, under suite load

Carried unchanged, and deliberately not dismissed. It failed in the 553 device
suite run — "still alive 6s after the hangup" — then passed 3 of 3 standalone on
the same device minutes later, and passes on all five local legs. The test gives
the hangup a 6-second budget and the device was running the rest of a 188-test
suite at the time.

**"Passes alone, fails in the suite" is NOT by itself proof of a load flake** —
that exact shape was a real bug once (GH #542, `ptrace_group_stop`). If it
recurs, A/B the suspected cause in one binary before re-running anything.

**Next step.** Nothing, unless it recurs. This entry is the date it was not yet
a regression.

---

## 6. The Launcher applets do not appear in `top`

Carried unchanged, and still a **design decision rather than a bug**. Programs
under `/AOK/native` do appear in `ps`, `top` and `ktop` with correct state,
`%CPU` and — since 2026-09-02 — their own `ARGUMENTS` rather than the parent's.
What does not appear is the Launcher applets: File Manager, MotePad, LLM Chat,
Markdown, Clock, Music, Settings, Wayland. Those are not guest processes at all.
They are iOS UI running in the app, with no pid, no `/proc` entry and no guest
address space.

Showing them means synthesising `/proc/<pid>` entries for app-side work —
inventing pids that no guest syscall can act on, so `kill` on one has to mean
something or be refused. Worth doing only if the goal is "the user can see what
the app is doing", in which case a distinct presentation (a separate section, or
an `ARCH` value reading `applet`) is more honest than pretending they are
processes.

This is also a **capability-lie risk**: a synthesised process that accepts a
signal and does nothing reports a state a real system never produces.

**Prove it.** Whatever is decided, `kill -9` on such an entry must do something
defensible and must not corrupt the process table.

---

## 7. RLIMIT_STACK is not pushed down for a third party

Carried unchanged from 553 and 554, and still deliberate. `prlimit64` against
another process updates that process's limits without updating its address
space, so a lowered `RLIMIT_STACK` takes effect at its next `exec` rather than
immediately. Reading another task's `->mm` needs `general_lock`, and the stack
stays bounded by the guard gap meanwhile, so the failure mode is "bounded less
tightly than asked", never unbounded.

**Next step.** None, unless something real depends on it. Three cycles carried
is enough to say so: if 555 ends without a consumer, this belongs in TODO.md
rather than in a musts document.

---

## 8. Issue hygiene, which is part of fixing bugs

[#541](https://github.com/emkey1/ish-AOK/issues/541) — *ptraceomatic does not
run: tracee reaped during setup* — has been **fixed since 2026-08-20** and is
still open on GitHub, confirmed 2026-09-08. Close it, with the commit named. A
fix the reporter never hears about did not fully happen.

It is also in §1's neighbourhood, so re-run ptraceomatic as part of the ptrace
work rather than closing it blind.

**Two issues are one investigation.**
[#568](https://github.com/emkey1/ish-AOK/issues/568) (slow throughput) and
[#523](https://github.com/emkey1/ish-AOK/issues/523)'s reproducible half (a
15.3 s TLS handshake tail against a sub-second median) are a wait not being
woken rather than work being slow, which puts them in the poll/quiesce
neighbourhood. It reproduces with plain `curl`, so it needs neither Go nor the
AUR to chase. Schedule them together or not at all.

---

## Closed during 554 while the doc still listed them

Recorded because two of the three were closed by work that never mentioned the
entry, which is how a musts document goes stale without anyone noticing.

**A NULL dereference in the ptrace memory-write path.** Fixed.
`__user_write_task_mem` (kernel/user.c:135) now checks `mem_ptr`'s result in
*both* places it mints a pointer — before the trace hook at line 146, and again
after it at line 152, because the hook may drop the read lock and free the
mapping underneath. The second check is the interesting one and was not part of
the carried diagnosis.

**`mem_mapped_page_count` walks a page table without the lock.** Fixed, and
with a better answer than the entry proposed. `task_maxrss_kb`
(kernel/resource.c:245) now holds `general_lock` **across** the walk — by
`trylock`, returning the latched value on failure, and skipping the lock
entirely when this thread already holds it, which is `do_exit`'s own call. The
comment there records why a reference-count approach was rejected: retaining
the mm would make the timer thread the last referrer and run `mem_destroy` on a
thread with no `current`.

**The amd64 JIT still bridges locked instructions.** Closed. The amd64 JIT now
has its own `ldaxr`/`stlxr` fast path with an alignment check and a bridge
fallback — `jit/gadgets-aarch64/math.S:4598` documents it, and notes explicitly
that "the 'lock (atomic path bridges)' comments described a gap, not a design".
`LOCK <alu> [mem],imm`, `XCHG [mem]` and `CMPXCHG [mem],reg` all compile
natively. This landed as part of the zero-fallback amd64 work rather than
against this entry.

**And one whose diagnosis was wrong rather than stale:** the strace/gdb entry.
See §1. The mechanism it named was fixed by the `__WALL` work in `do_wait` that
shipped for `strace -f`, and the symptom it described has a different cause.

---

## Also open, and tracked elsewhere

Not repeated here, but a reader of this document should know where they are.

**554's own known gaps** (`docs/release-notes-since-iSH-AOK_554.md`) — two of
them are directly the 555 theme and should be read before the suspend-to-disk
design, not after: **swap does not survive a guest reboot** (the area is
recreated empty, and `swapoff` tears it down rather than parking it), and
**swap does not evict pages shared by a forked family**. A checkpoint feature
built on a pager whose store is deliberately non-persistent needs to say which
of the two changes. Also there: `signal_child_burst` on the A9 iPad,
unresolved; a single unreproduced `wayland_scm_shm` failure; and the macOS-only
FileProvider limitation, which is a protocol port and affects no iOS user.

**The conformance long tail** is in [TODO.md](TODO.md), each entry with its
measurement: `PROT_EXEC` never enforced (guest W^X is decorative — two candidate
designs, both rejected on cost, and it deserves its own before/after benchmark
rather than being folded into a sweep); PI futexes ENOSYS; `pipe(2)` cannot
honour PIPE_BUF atomicity while it delegates to a host pipe; `tmpfs size=`
accepted and not enforced; `fcntl(F_GETFL)` reporting an `O_NONBLOCK` the guest
never set; `/proc/locks` absent; FUSE with no attribute cache and three absences
following from it; and `SEEK_DATA`/`SEEK_HOLE` still `EINVAL` on tmpfs, now the
odd one out rather than one of a pair since realfs and FUSE both answer it.

**atop's accounting daemon wedges boot** (TODO.md) is the one in that list with
a device report behind it and an unverified half: AOK's netlink socket appears
never to become readable and never to report an error after a failed family
lookup, so `atopacc` blocks in `ppoll` forever and every init script after
`S01atop` — sshd included — never runs. Implementing the `netatop` family is
**not** the fix.
