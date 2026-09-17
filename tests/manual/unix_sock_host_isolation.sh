#!/bin/sh
# unix_sock_host_isolation.sh -- CLI guests running at once must not share the
# host sockets behind their unix sockets.
#
# Every ish process backed its guests' bound unix sockets with host sockets at
# /tmp/ishsock.<id>, ids restarting at 1 in each process, and bind unlinked the
# path first. Two CLI guests at once therefore took each other's sockets, and a
# client in one reached the other's server, or nothing: concurrent D-Bus, X and
# Wayland tests failed spuriously. fs/sock.c now gives each process a private
# directory under $TMPDIR (or /tmp), removes it at exit, and sweeps up the
# directories of processes that died without removing theirs.
#
# This is a HOST test -- a guest cannot see host paths -- so it is not in the
# guest suite. The guest half, unix_sock_host_isolation.c, is; this script
# compiles it into the root and drives it:
#
#   1. Three CLI guests at once, each serving and connecting to its own
#      abstract, filesystem-stream and datagram sockets for several seconds.
#      Every connection must reach its own process. (Unfixed: they collide.)
#   2. Their socket directories are gone once they exit.
#   3. A guest killed with SIGKILL leaves its directory; the next guest that
#      makes one removes it.
#   4. A $TMPDIR too long for sun_path is not used, and sockets still work.
#
#     tests/manual/unix_sock_host_isolation.sh [root]
#
# `root` defaults to build/alpine-arm64-test and needs a C compiler. $ISH
# overrides the binary (default build/ish).
set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/alpine-arm64-test}
[ -d "$ROOT" ] || { echo "no root at $ROOT" >&2; exit 2; }
[ -x "$ISH" ] || { echo "no ish at $ISH" >&2; exit 2; }

# A short, private $TMPDIR for the guests, so their socket directories are the
# only ones in it and can be counted.
BASE=$(mktemp -d /tmp/aok-sockiso.XXXXXX) || exit 2
PROG=/tmp/aok-sockiso-$$
cleanup() {
    rm -rf "$BASE"
    "$ISH" -f "$ROOT" /bin/sh -c "rm -f $PROG" >/dev/null 2>&1
}
trap cleanup EXIT

failures=0
fail() {
    echo "FAIL: $*"
    failures=$((failures + 1))
}

socket_dirs() {
    find "$BASE" -mindepth 1 -maxdepth 1 -type d -name 'ishsock.*' | wc -l | tr -d ' '
}

echo "+ compiling the guest half into $ROOT"
if ! ISH_REAL_MNT="$REPO/tests/manual" "$ISH" -f "$ROOT" /bin/sh -c \
        "cc -O2 -I/realmnt -o $PROG /realmnt/unix_sock_host_isolation.c"; then
    echo "unix_sock_host_isolation.sh: could not compile the guest program" >&2
    exit 2
fi

# 1. Three at once.
echo "+ three guests at once"
pids=
for tag in a b c; do
    TMPDIR=$BASE "$ISH" -f "$ROOT" "$PROG" --hold "$tag" 4 >"$BASE/out.$tag" 2>&1 &
    pids="$pids $!"
done
for pid in $pids; do
    wait "$pid"
done
for tag in a b c; do
    if grep -q "^unix_sock_host_isolation: PASS $tag" "$BASE/out.$tag"; then
        echo "  $tag: $(grep '^unix_sock_host_isolation:' "$BASE/out.$tag")"
    else
        fail "guest $tag:"
        sed 's/^/    /' "$BASE/out.$tag"
    fi
done

# 2. Gone at exit.
n=$(socket_dirs)
[ "$n" -eq 0 ] || fail "$n socket directories left in $BASE after the guests exited"

# 3. A guest killed with SIGKILL leaves its directory; the next one sweeps it.
echo "+ a killed guest's directory is swept by the next guest"
TMPDIR=$BASE "$ISH" -f "$ROOT" "$PROG" --hold killed 60 >"$BASE/out.killed" 2>&1 &
victim=$!
i=0
while [ "$i" -lt 600 ] && ! grep -q '^READY' "$BASE/out.killed" 2>/dev/null; do
    sleep 0.1
    i=$((i + 1))
done
grep -q '^READY' "$BASE/out.killed" || fail "the guest to be killed never became ready"
kill -9 "$victim" 2>/dev/null
wait "$victim" 2>/dev/null
n=$(socket_dirs)
[ "$n" -eq 1 ] || fail "expected the killed guest's directory to remain, found $n"
TMPDIR=$BASE "$ISH" -f "$ROOT" "$PROG" --hold sweeper 1 >"$BASE/out.sweeper" 2>&1 ||
    fail "the sweeping guest: $(cat "$BASE/out.sweeper")"
n=$(socket_dirs)
[ "$n" -eq 0 ] || fail "$n socket directories left after the sweeping guest exited"

# 4. A $TMPDIR that cannot fit is passed over.
echo "+ a \$TMPDIR too long for sun_path"
long=$BASE/$(printf '%0120d' 0)
mkdir -p "$long"
if TMPDIR=$long "$ISH" -f "$ROOT" "$PROG" --hold long 1 >"$BASE/out.long" 2>&1; then
    echo "  $(grep '^unix_sock_host_isolation:' "$BASE/out.long")"
else
    fail "with a long \$TMPDIR:"
    sed 's/^/    /' "$BASE/out.long"
fi
n=$(find "$long" -mindepth 1 | wc -l | tr -d ' ')
[ "$n" -eq 0 ] || fail "the too-long \$TMPDIR was used anyway"

if [ "$failures" -ne 0 ]; then
    echo "unix_sock_host_isolation.sh: FAIL failures=$failures"
    exit 1
fi
echo "unix_sock_host_isolation.sh: PASS"
