# guestprof positive controls

Three programs whose answer is known in advance, for checking that
`ISH_GUEST_PROFILE` (kernel/guestprof.c) actually fires and attributes time to
the right place. A profiler that silently records nothing looks exactly like a
workload with no hot spot, so none of its output means anything until these
have been seen to produce the expected answer.

They are deliberately NOT tier0 tests: they have no pass/fail criterion of
their own (that lives in the profile report), and tier0 discovery only picks up
top-level `tests/manual/*.c` that include `test_common.h`.

Build them inside a guest root and run each under the profiler:

```sh
ISH_REAL_MNT=tests/manual/guestprof ./build/ish -f build/devuan-arm64-test /bin/sh -c \
    'for f in ctl_inflate ctl_syscall ctl_blocked; do gcc -O2 -o /tmp/$f /realmnt/$f.c -ldl; done'

ISH_GUEST_PROFILE=1 ./build/ish -f build/devuan-arm64-test /bin/sh -c '/tmp/ctl_inflate libz.so.1 120'
```

| program | what it must show |
|---|---|
| `ctl_inflate` | nearly all on-CPU samples in `libz`, hot in the unexported function after `inflateBackEnd` (`inflate_fast`) |
| `ctl_syscall` | a substantial `[kernel]` share — samples landing OUTSIDE guest code |
| `ctl_blocked` | nearly all samples in `[blocked]`, not `[kernel]` |

Each prints a `CTL_*_OK` line; a run without it did not do the work, and its
profile says nothing. Measured values are in
`docs/guest_pc_sampling_2026_09.md`.
