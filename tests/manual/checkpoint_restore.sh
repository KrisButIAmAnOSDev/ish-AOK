#!/bin/sh
# checkpoint_restore.sh -- the phase-0 proof for suspend to disk, on the CLI.
#
# The roadmap's wording is the test: "checkpoint and restore a single-process
# guest, no native program, one open file ... The restored guest continues from
# the instruction after the checkpoint, reads the same bytes from the same open
# file at the same offset."
#
# So this runs a shell that reads two lines of a file, checkpoints itself,
# reads three more, and then runs a SEPARATE ish process from the image. The
# restored run's output must be exactly the tail of the original's -- not
# similar to it, and in particular not starting again from the top.
#
#     tests/manual/checkpoint_restore.sh [root]
#
# `root` defaults to build/devuan-arm64-test. It has to be a root the CLI can
# boot with a working /bin/dash or /bin/sh.
set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=$REPO/build/ish
ROOT=${1:-$REPO/build/devuan-arm64-test}
IMG=${TMPDIR:-/tmp}/aok-checkpoint-$$.img
SH=/bin/dash
[ -d "$ROOT" ] || { echo "no root at $ROOT" >&2; exit 2; }
[ -x "$ISH" ] || { echo "build/ish first" >&2; exit 2; }
trap 'rm -f "$IMG"' EXIT

# The `pre` line is printed BEFORE the checkpoint and the `post`/`then` lines
# after it, so the restored run must print the second two and not the first.
PROG='
i=1; : > /tmp/ckpt-lines.txt
while [ $i -le 8 ]; do echo "LINE-$i-payload" >> /tmp/ckpt-lines.txt; i=$((i+1)); done
exec 3< /tmp/ckpt-lines.txt
read -r a <&3; read -r b <&3
MARK=a-shell-variable-that-must-survive
echo "pre : $a / $b / $MARK"
echo save IMAGE > /proc/ish/checkpoint
read -r c <&3; read -r d <&3
echo "post: $c / $d / $MARK"
read -r e <&3
echo "then: $e"
'

save_out=$(ISH_GUEST_CHECKPOINT=1 "$ISH" -f "$ROOT" $SH -c "$(printf '%s' "$PROG" | sed "s|IMAGE|$IMG|")" 2>&1)
echo "$save_out" | sed 's/^/  save    | /'

[ -s "$IMG" ] || { echo "FAIL: no image written"; exit 1; }

restore_out=$(ISH_RESTORE="$IMG" "$ISH" -f "$ROOT" 2>&1)
echo "$restore_out" | sed 's/^/  restore | /'

# 1. It continued rather than re-ran: the line printed before the checkpoint
#    must NOT appear in the restored run.
if echo "$restore_out" | grep -q '^pre :'; then
    echo "FAIL: the restored guest re-ran the checkpoint request"
    exit 1
fi

# 2. Same bytes, same file, same offset -- the restored run is exactly the
#    tail of the original. Compared whole rather than grepped for a substring,
#    because "it printed something plausible" is what a wrong offset looks
#    like.
expect=$(echo "$save_out" | sed -n '2,$p')
if [ "$restore_out" != "$expect" ]; then
    echo "FAIL: restored output is not the original's tail"
    echo "  expected: $expect"
    echo "  got     : $restore_out"
    exit 1
fi

# 3. And it really did come back from a file rather than boot fresh, which the
#    two checks above would not distinguish if the shell were somehow re-run
#    with the same state by accident.
case $restore_out in
    *"LINE-3-payload"*"LINE-4-payload"*"LINE-5-payload"*) ;;
    *) echo "FAIL: the restored guest did not continue the open file"; exit 1;;
esac

# 4. And the guest can TELL the two lives apart. This is what a program uses
#    to decide whether it is running for the first time; without it a restore
#    is indistinguishable from a continuation and nothing can react to one.
gen_out=$(ISH_GUEST_CHECKPOINT=1 "$ISH" -f "$ROOT" $SH -c "
echo save $IMG > /proc/ish/checkpoint
while read -r l; do case \$l in restored*) echo \"\$l\";; esac; done < /proc/ish/checkpoint
" 2>&1)
gen_back=$(ISH_RESTORE="$IMG" "$ISH" -f "$ROOT" 2>&1)
case $gen_out in
    *"restored        no"*) ;;
    *) echo "FAIL: a fresh guest reported itself restored: $gen_out"; exit 1;;
esac
case $gen_back in
    *"restored        yes"*) ;;
    *) echo "FAIL: a restored guest did not report itself restored: $gen_back"; exit 1;;
esac

# ---- and the refusals ------------------------------------------------------
#
# A capability boundary that never fires is not a boundary, it is a comment.
# Each of these is a case v1 cannot do, and each has to SAY so rather than
# writing an image that would not come back.
refuse() {   # refuse <what> <shell> <program> <expected substring>
    out=$(ISH_GUEST_CHECKPOINT=1 "$ISH" -f "$ROOT" "$2" -c "$3" 2>&1 || true)
    case $out in
        *"$4"*) echo "  refused | $1" ;;
        *) echo "FAIL: no refusal for $1"; echo "  got: $out"; exit 1;;
    esac
    rm -f "$IMG"
}
READ_REFUSAL='while read -r l; do case $l in last_refusal*) echo "$l";; esac; done < /proc/ish/checkpoint'

refuse "a native program on the stack" /AOK/native/dash \
    "echo save $IMG > /proc/ish/checkpoint 2>/dev/null; $READ_REFUSAL" \
    "is a native program"
refuse "a second task still running" $SH \
    "/bin/sleep 5 & echo save $IMG > /proc/ish/checkpoint 2>/dev/null; $READ_REFUSAL" \
    "is also running"
refuse "a pipe with no restore rule" $SH \
    "mkfifo /tmp/ckpt-fifo 2>/dev/null; exec 4<> /tmp/ckpt-fifo
     echo save $IMG > /proc/ish/checkpoint; $READ_REFUSAL" \
    "is a pipe"

echo "PASS: continued from the instruction after the checkpoint, same file, same offset"
