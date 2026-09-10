# Release Notes Since `builds/iSH-AOK_554`

Two large things: a session can survive the app being killed, and there is a
third shell — a small, fast, permissively-licensed one — compiled in.

## Highlights

**Suspend to disk.** iOS ends this app routinely — memory pressure, or a swipe
away — and until now that always lost the session. It can now save the whole
guest to a file when the app is backgrounded and bring it back on the next
launch: same device, same root, same build, back where you were.

Every process, its memory, its open files and its place in the process tree. A
shell blocked in `wait()` and the child it is waiting for come back and carry
on. A pipeline comes back with the bytes still in it. A file comes back at the
offset it was read to, and two processes that shared a descriptor still share
it. A zsh session comes back with its variables, functions and aliases, because
a native shell is *asked to describe itself* rather than photographed — there is
no serialising a host C stack, so the rule is that a native program either knows
how to dump its own state or the checkpoint refuses while it is running.

You come back to the terminal you were looking at, not to a new shell beside
your old one. A terminal cannot be restored — the one you had belonged to an app
process that no longer exists — so the resume makes a fresh one for each session
in the image and re-attaches that session to it: same controlling terminal, same
foreground job, same line settings, same hostname, same job table. In the app,
the window you resume into is the session you suspended.

**It is off by default**, in Settings, for the same reason swap is: it spends
your storage, and a session it cannot describe is one it will not save. When
that happens it says so — `cat /proc/ish/checkpoint` in the guest reports what
the last attempt did and, if it refused, which process and why — and the next
launch simply boots, which is the behaviour you get with the switch off. The
guest is never harmed by the attempt: a checkpoint is a copy, and the machine is
stopped only for as long as it takes to write one.

Two limits worth naming rather than leaving to be discovered. An image from a
different build is refused outright, because the register file travels as bytes
and reinterpreting one would be worse than declining it. And
`/AOK/native/dash` cannot describe itself — it has no way to write its shell
functions back out as text — so a checkpoint refuses by name while one is
running. Nothing reaches native dash unless you ask for it; it is not
`/bin/sh`.

The guest can also take one for itself:

    echo save /path/to/image    > /proc/ish/checkpoint   # a copy; carry on
    echo suspend                > /proc/ish/checkpoint   # save and stop

**Native dash.** `/AOK/native/dash` and `/AOK/native/sh` are dash compiled into
the app and run as host code — a POSIX shell that starts in a fraction of the
time an emulated one does, which matters when a script forks one per line. It is
BSD-licensed and it is the shell most scripts are actually written against.

Its subshells work the way bash's and zsh's do here — a fork becomes a
re-launch, because a native program is a function call and not a process — but
with a difference worth having: dash hands its child the parse TREE rather than
the command's text, so quoting cannot be lost on the way. Measured against the
emulated `/bin/dash` over twenty-five constructs — nesting, `$$`, pipeline exit
status, subshell isolation, inherited functions and aliases, traps, background
jobs, a 51 KB here-document — the output is byte-for-byte identical, and faster:
500 command substitutions in 0.32s against 1.00s emulated.

## Notice: native bash is going away in 556

`/AOK/native/bash` will be removed in the next build. bash is GPLv3, and the
App Store's terms and the GPL have a history that has already cost two
well-known apps their listing — VLC in 2011 and GNU Go in 2010, both over the
same clause. Shipping a GPLv3 interpreter compiled into the binary is not a
risk worth carrying for a shell that now has two alternatives.

Nothing else changes. **Guest bash is untouched** — `apt install bash`,
`/bin/bash`, every script with `#!/bin/bash` in it, all exactly as before. What
goes is the natively-compiled copy at `/AOK/native/bash`.

If your login shell in `/etc/passwd` is `/AOK/native/bash`, this build moves it
to the guest bash for you, and to `/AOK/native/zsh` if there is none. Native zsh
is the interactive shell this project recommends, and it is the one that can be
suspended and restored.

## Compressed memory is no longer lazy

The pager only reclaimed when the guest was already short of memory, which for
a RAM-only compression pool is exactly the wrong rule — there is no file and no
flash cost to ration, so filling the pool with cold pages costs nothing and buys
headroom before it is needed. Measured on a device: 239 MB of recovered headroom
became 433 MB.

## And a long tail

- Two symbols that were silently one: dash's `$SHLVL` counter and zsh's shared
  a variable, as did their line counters, and dash's arithmetic `yylval` shared
  one with bash's. A tentative definition merges with a real one instead of
  colliding, so the build had never said a word about it.
- BSD `sigsetmask` reached the host rather than the guest, so dash's `wait`
  builtin worked once per shell and then blocked for ever.
- `cpu_poke` dereferenced a null pointer for any task whose program is native,
  which took the whole app down with it.
- Both ends of a pipe now share one inode, the way Linux reports them, so `lsof`
  and process-tree viewers can tell that two descriptors are two ends of one
  pipe; and the write end reports itself write-only rather than read-only.
- The guest test gate no longer skips anything. Every test it has, it runs.
