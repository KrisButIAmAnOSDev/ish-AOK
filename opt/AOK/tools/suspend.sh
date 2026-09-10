#!/bin/sh
# suspend.sh -- save this iSH-AOK session, or check whether it can be saved.
#
#     /AOK/tools/suspend.sh              save the session and stop
#     /AOK/tools/suspend.sh --save FILE  save a copy and keep running
#     /AOK/tools/suspend.sh --status     what the last attempt did
#
# WHAT SUSPENDING DOES. Every process, its memory, its open files and its place
# in the process tree are written to one file, and the next launch resumes from
# it instead of booting. iOS ends this app routinely -- memory pressure, or a
# swipe away -- and without this that always loses the session.
#
# You do not normally have to run this. With "Suspend to Disk" switched on in
# Settings, iSH-AOK saves the session by itself whenever the app goes to the
# background, which is the case this exists for. This is the manual form, for
# when you want the session saved NOW.
#
# WHAT IT CANNOT SAVE, and it says so rather than saving something that will
# not come back:
#
#   - A native program that cannot describe itself. A native program is host
#     code on a host thread, so it is asked to write down its own state rather
#     than photographed. /AOK/native/zsh can; /AOK/native/dash cannot, because
#     it has no way to write its shell functions back out as text.
#   - A socket, or any other descriptor with no rule for rebuilding it.
#     Regular files, directories, terminals, pipes and the standard streams all
#     have one.
#
# In either case nothing is written, the session carries on untouched, and
# --status names the process and the reason.
set -e

CONTROL=/proc/ish/checkpoint

usage() {
    sed -n '2,/^set -e$/p' "$0" | sed 's/^# \{0,1\}//; /^set -e$/d'
}

status() {
    # The file is the report. Everything above the blank line is the inventory
    # -- what a checkpoint would have to deal with in this guest -- and what
    # follows it is what actually happened.
    cat "$CONTROL"
}

case ${1-} in
    --status|-s|status)
        status
        ;;
    --help|-h|help)
        usage
        ;;
    --save)
        if [ -z "${2-}" ]; then
            echo "suspend.sh: --save needs a file to write" >&2
            exit 2
        fi
        # A COPY: the guest carries on afterwards. The path is a HOST path --
        # somewhere on the device outside the guest filesystem -- because that
        # is where a session has to live to survive the app being killed.
        if echo "save $2" > "$CONTROL" 2>/dev/null; then
            echo "saved to $2"
        else
            echo "suspend.sh: refused. Why:" >&2
            status | sed -n 's/^last_refusal *//p' >&2
            status | sed -n 's/^last_err *//p' | sed 's/^/error /' >&2
            exit 1
        fi
        ;;
    "")
        # A DEPARTURE: the image is written and the machine stops, so the next
        # launch resumes rather than boots.
        if ! echo suspend > "$CONTROL" 2>/dev/null; then
            echo "suspend.sh: refused." >&2
            echo "  Turn on Suspend to Disk in Settings, or on the command" >&2
            echo "  line run iSH-AOK with ISH_GUEST_CHECKPOINT=1 and a" >&2
            echo "  session file (ISH_SESSION=<path>)." >&2
            status | sed -n 's/^last_refusal */  /p' >&2
            exit 1
        fi
        # Not reached: the machine stops inside the write above.
        ;;
    *)
        echo "suspend.sh: unknown argument '$1'" >&2
        usage >&2
        exit 2
        ;;
esac
