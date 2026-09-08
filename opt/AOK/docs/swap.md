# Swap

iOS gives an app a memory ceiling and kills it without warning when it is
crossed. A guest that allocates too much therefore does not get an `ENOMEM` —
the whole of iSH-AOK disappears, terminals and all. Swap gives the emulator
somewhere to put pages the guest is not using, so that a large, mostly idle
working set can exist without spending real memory on it.

It is a file, not a partition, and it lives inside the app's own storage. Paging
to it costs flash writes on your device, which is why it is **off by default and
has to be turned on deliberately**.

## Turning it on

In the **iOS Settings app** — not the settings inside iSH-AOK — open
**Settings → iSH-AOK → Simulated Swap**. There are two controls:

- **Enable Swap**, off by default.
- **Swap Size**, which starts at *Not chosen (swap stays off)* and offers
  256 MB, 512 MB, 1 GB, 2 GB, 4 GB, 8 GB and 16 GB.

**Both are needed.** Turning the switch on without choosing a size leaves swap
off, deliberately: picking a size for you would be iSH-AOK deciding how much of
your flash to write to. If that is the state you are in, the guest says so —
`cat /proc/ish/swap` reports it rather than just `off`.

**Both take effect the next time iSH-AOK starts**, not when you flip the switch.
The area is created when the guest boots, at its full size, and does not grow.

The sizes offered do not know how much room your device actually has. If there
is not enough free space for the size you picked, iSH-AOK leaves swap off rather
than quietly making a smaller area than you asked for — and again,
`/proc/ish/swap` says which of the two happened.

Pick a size the way you would on any machine: big enough to hold the cold parts
of what you run, small enough that you are not writing gigabytes to flash for
nothing. 256 MB or 512 MB is a reasonable starting point.

From the command-line build there is no Settings app, so use the environment
variable instead — see `ISH_GUEST_SWAP_MB` in
[tuning-knobs.md](tuning-knobs.md).

## Seeing what it is doing

The guest sees swap the way it sees anything else:

```sh
free                      # SwapTotal / SwapFree, as on any Linux
cat /proc/meminfo         # the same numbers, plus the rest
cat /proc/swaps           # the area as a device, like swapon --show
vmstat 1                  # si/so columns: pages in and out per second
```

`/dev/aokswap0` is the block device behind it. It is real enough to read and to
name in `/proc/swaps`, but it is the app's own file rather than a disk
partition, and it is not a general-purpose block device: it carries a swap
header and its data, and nothing else.

`swapon` and `swapoff` work on it, and answer as they would anywhere -- running
`swapon /dev/aokswap0` while swap is already on reports the device busy, as
Linux does. **`swapoff` tears the area down rather than parking it**, though, so
it is not a pause: once off, swap stays off until the guest boots again with the
switch on.

For what the pager itself is doing — how full the area is, how much has been
written, whether the clock is finding cold pages, whether it has had to pause —
read `/proc/ish/swap`:

```sh
cat /proc/ish/swap
```

Every counter there is described in [proc-ish.md](proc-ish.md).

## Things worth knowing

- **It is off by default, and that is deliberate.** Paging spends your flash.
  Nothing turns it on for you.
- **There is a write budget.** The pager stops evicting once it has written a
  fixed amount within 24 hours, so a runaway guest cannot quietly grind through
  the write endurance of your device. `/proc/ish/swap` shows the window and how
  much of it is spent.
- **It pauses itself when paging is not helping.** If pages come straight back
  after being evicted — thrashing — the background sweep backs off for a while
  rather than churning. That is the `thrashing` line.
- **Pages shared by a forked family are not evicted.** Only memory reachable
  from a single address space is paged out today, so a process that forked and
  did not exec keeps its shared image resident. Fork-heavy workloads therefore
  benefit less than the totals suggest.
- **Swapping is not free speed.** It buys you the ability to run something that
  otherwise could not run at all. Anything actively touched should stay
  resident, and if it does not you will feel it.
- **It does not survive a reboot of the guest.** The area is recreated empty.

## When it will not turn on

`/proc/ish/swap` says why rather than just reporting `off` — no space for the
area, a size of zero, or a build that was not offered guest control. If the
switch is on and the guest still reports no swap, read that file first.

## See also

- [tuning-knobs.md](tuning-knobs.md) — `ISH_GUEST_SWAP_MB` and the memory-guard
  knobs, for the command-line build.
- [proc-ish.md](proc-ish.md) — `/proc/ish/swap` and the other emulator files.
- [ktop.md](ktop.md) — watching memory and paging live from inside the guest.
