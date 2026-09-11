# Suspend to disk

Save the whole session and get it back on the next launch.

iOS ends this app routinely — memory pressure, or you swiping it away — and
without this that always loses the session. With it on you get the same one
back: same processes, same open files, same shell.

**It is off by default.** Turn it on in the iOS Settings app, under iSH-AOK →
**Suspend to Disk**. It spends storage, and a session it cannot describe is one
it will not save, so it is not something to switch on for somebody.

## Once it is on

Nothing to do. The session is saved when the app goes to the background and
comes back on the next launch.

To save one *now*, without waiting to be backgrounded:

- **Workspace**: the ☰ menu, **Save Session**.
- **iPad**, anywhere: the ⤓ button on the accessory bar above the keyboard. It
  turns into a checkmark when the session is on disk. It appears only while
  Suspend to Disk is on.
- **Workspace → Utilities… → Workspace → Sessions** has the same thing on a
  card, with the last save's size and process count.
- From the shell:

    /AOK/tools/suspend.sh

That writes the session and stops the machine, so the next launch resumes it.
There is also `--save FILE`, which takes a copy and lets the session carry on,
and `--status`, which reports what the last attempt did.

The control underneath is a `/proc` file, like every other AOK knob:

    echo suspend             > /proc/ish/checkpoint
    echo save /host/path     > /proc/ish/checkpoint
    cat /proc/ish/checkpoint

`cat` is worth reading. Above the blank line is an inventory of what this guest
is holding that a save would have to deal with — how many processes, how many
descriptors, and how many of them are the awkward kind. Below it is what the
last attempt actually did.

## What comes back

Every process and its place in the tree, with its pid unchanged. A shell
blocked in `wait` and the child it is waiting for both resume. A pipeline comes
back with the bytes still in it. A file comes back at the offset it was read to,
and two processes that shared a descriptor still share it.

Your zsh session comes back with its variables, functions and aliases. A native
program is host code on a host thread, so it cannot be photographed the way an
emulated process can — instead it is asked to write down its own state, and zsh
knows how. It is **re-launched** from that description rather than resumed
mid-instruction, which for an interactive shell means a prompt with your session
still in it.

The terminal comes back too, and it is the one you are looking at. A terminal
cannot be restored -- the one you had belonged to an app process that no longer
exists -- so a fresh one is made and the session is re-attached to it, with the
same session, the same foreground job and the same line settings. In the app
that means the window you resume into is your session, not a new shell beside
it. Your hostname, your background jobs and your job table are all still there.

The system consoles are re-attached the same way, each to its own: a getty on
`tty3` comes back on `tty3`, not alongside everything else on the console.

## What it will not save, and how it tells you

It refuses rather than writing something that will not come back. Every refusal
names the process and the reason, in `/proc/ish/checkpoint`:

- **A native program that cannot describe itself.** `/AOK/native/zsh` can.
  `/AOK/native/dash` cannot — it has no way to write its shell functions back
  out as text — so a save refuses while one is running. Nothing reaches native
  dash unless you ask for it; it is not `/bin/sh`.
- **A socket**, or any other descriptor with no rule for rebuilding it. Regular
  files, directories, terminals, pipes and the standard streams all have one.
- **A native program that is not making any system calls**, because there is
  nowhere to stop it. A shell at a prompt is waiting on a read and is fine; a
  native program in a tight compute loop is not.

A refusal costs nothing: the session carries on exactly as it was. A save is a
copy, and the machine is stopped only for as long as it takes to write one.

An image from a **different build** is refused on the way back in. The register
file travels as bytes, and reinterpreting one from another build would be worse
than declining it — so after an update, the first launch boots normally.

## What it is not

It is not a snapshot of your filesystem — that is
[`/proc/ish/snapshot`](proc-ish.md), and the two are independent. A session
saved against a root you have since changed will resume against the changed
one, exactly as it would if the app had simply stayed running.
