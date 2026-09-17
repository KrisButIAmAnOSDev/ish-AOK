#!/bin/sh
# checkpoint_freeze_restart.sh -- a checkpoint must be invisible to a task
# blocked in a syscall: no EINTR, and no early return.
#
# The freezer wakes every blocked task, the wait comes back EINTR, and the
# dispatcher turns that into a restart. On x86_64 most syscalls skipped the
# conversion (kernel/calls.c's backstop after handle_amd64_native_memory_syscall
# asked only about the restart codes), so readv, write, recvfrom, nanosleep and
# fourteen more failed with EINTR at the moment of the checkpoint. Wakes the
# freezer left behind then did the same on every ABI to whichever call ran
# first after the thaw.
#
# checkpoint_freeze_restart.c blocks one child in each of 31 syscalls and grades
# every one: right errno, not early, and the freeze really landed inside the
# call (see its header). This drives it four ways per root:
#   control      no checkpoint -- every case must already be right without one
#   host         ISH_CHECKPOINT_AFTER, the path the app's backgrounding save takes
#   lost wakes   the same with ISH_CHECKPOINT_LOSE_WAKES=1, the way a device fails
#   guest        `save` written to /proc/ish/checkpoint from inside the guest
#
#     tests/manual/checkpoint_freeze_restart.sh [root...]
#
# Roots default to build/alpine-amd64-test and build/devuan-amd64-test; any root
# with a C compiler works. The probe is compiled INTO each root, so point this
# at clones (cp -c -R) rather than roots you want left untouched.
set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
[ -x "$ISH" ] || { echo "build/ish first" >&2; exit 2; }
if [ $# -eq 0 ]; then
    set -- "$REPO/build/alpine-amd64-test" "$REPO/build/devuan-amd64-test"
fi
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-freeze-restart.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
PROBE=/tmp/checkpoint_freeze_restart
# The calls the bug was reported against. Each must be present AND pass, so a
# probe edited down to fewer cases cannot pass by no longer asking.
NAMED="readv-udp write-unix recvfrom-udp nanosleep"
failures=0

# run_guarded OUT CMD... -- run with no stdin and a host-side watchdog (macOS has
# no timeout(1)); leaves the exit status in $rc, 124 on a timeout.
run_guarded() {
    out=$1; shift
    "$@" < /dev/null > "$out" 2>&1 &
    pid=$!
    ticks=0
    while kill -0 "$pid" 2>/dev/null; do
        ticks=$((ticks + 1))
        if [ $ticks -gt 1200 ]; then
            kill -9 "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            rc=124
            return
        fi
        sleep 0.1
    done
    wait "$pid"
    rc=$?
}

fail() {
    echo "FAIL: $*"
    failures=$((failures + 1))
}

# check_leg NAME OUT IMG_LOG -- grade one probe run.
check_leg() {
    leg=$1 out=$2 log=$3
    sed 's/^/    | /' "$out"
    if [ "$rc" = 124 ]; then
        fail "$leg: the probe never finished"
        return
    fi
    if [ -n "$log" ]; then
        # Proof the save happened, independent of the probe's own witness.
        case $(cat "$log" 2>/dev/null) in
            "written (0)"*) ;;
            *) fail "$leg: the checkpoint was not written: $(cat "$log" 2>/dev/null)"; return;;
        esac
    fi
    summary=$(grep '^SUMMARY ' "$out")
    case $summary in
        "SUMMARY pass="*" fail=0 inconclusive=0") ;;
        *) fail "$leg: ${summary:-no summary (exit $rc)}"; return;;
    esac
    for name in $NAMED; do
        grep -q "^CASE $name  *PASS " "$out" || fail "$leg: $name did not pass"
    done
    [ "$rc" = 0 ] || fail "$leg: probe exit $rc"
}

for ROOT in "$@"; do
    [ -d "$ROOT" ] || { echo "no root at $ROOT" >&2; exit 2; }
    echo "==== $ROOT"

    run_guarded "$WORK/cc.out" env ISH_REAL_MNT="$REPO/tests/manual" "$ISH" -f "$ROOT" \
        /bin/sh -c "cc -O2 -o $PROBE /realmnt/checkpoint_freeze_restart.c && echo COMPILED"
    if ! grep -q '^COMPILED$' "$WORK/cc.out"; then
        sed 's/^/    | /' "$WORK/cc.out"
        fail "could not compile the probe in $ROOT"
        continue
    fi

    echo "  -- control: no checkpoint"
    run_guarded "$WORK/control.out" "$ISH" -f "$ROOT" $PROBE none
    check_leg control "$WORK/control.out" ""

    # Saved without ISH_REAL_MNT: a checkpoint is taken of the guest as it
    # would be in the app, and the host mount is not part of that.
    echo "  -- host save (the app's path), 2s in"
    img=$WORK/host.img
    run_guarded "$WORK/host.out" env ISH_CHECKPOINT_AFTER=2:"$img" "$ISH" -f "$ROOT" $PROBE
    check_leg host "$WORK/host.out" "$img.log"
    rm -f "$img" "$img.log"

    echo "  -- host save with the freezer's wakes lost"
    img=$WORK/lost.img
    run_guarded "$WORK/lost.out" env ISH_CHECKPOINT_LOSE_WAKES=1 ISH_CHECKPOINT_AFTER=2:"$img" \
        "$ISH" -f "$ROOT" $PROBE
    check_leg "lost wakes" "$WORK/lost.out" "$img.log"
    rm -f "$img" "$img.log"

    echo "  -- guest save through /proc/ish/checkpoint"
    img=$WORK/guest.img
    run_guarded "$WORK/guest.out" env ISH_GUEST_CHECKPOINT=1 "$ISH" -f "$ROOT" $PROBE save "$img"
    check_leg guest "$WORK/guest.out" ""
    grep -q '^SAVE-REQUEST written$' "$WORK/guest.out" || fail "guest: the save request was refused"
    [ -s "$img" ] || fail "guest: no image written"
    rm -f "$img"
done

if [ $failures -ne 0 ]; then
    echo "checkpoint_freeze_restart: FAIL ($failures)"
    exit 1
fi
echo "checkpoint_freeze_restart: PASS"
