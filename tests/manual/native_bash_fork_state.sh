#!/bin/sh
# native_bash_fork_state.sh -- what must survive a native bash subshell.
# Run inside the guest (device or CLI root):
#
#   /AOK/tests/native_bash_fork_state.sh
#
# WHY THIS EXISTS, and why it is separate from the zsh one. Native bash uses
# the same fork-by-re-launch design (deps/bash/aok_fork.c) but reaps its
# children differently, so the two shells fail in different ways from the same
# kernel change. The case that motivated this file proves the point:
#
#   kernel/fork.c gave a native-spawned task SIGCHLD as its exit signal. It had
#   to -- a native ZSH waits for a job by sleeping in sigsuspend until its
#   handler reaps, so with no exit signal the child became a zombie and the
#   shell hung forever on its first external command. bash does not wait that
#   way, but it does install sigchld_handler, and waitchld() reaps with
#   waitpid(-1, WNOHANG) keeping a status only for pids in its OWN jobs table.
#   A re-launch child is not one, so the status was discarded, the explicit
#   waitpid returned ECHILD, and `x=$(sh -c "exit 6"); echo $?` printed 0.
#
# So a fix that is REQUIRED by one native shell silently broke the other, and
# nothing in the zsh suite could have caught it. That is the class this file
# guards: kernel-level changes whose blast radius crosses shells.

B=/AOK/native/bash
pass=0; fail=0; skip=0

# A case whose ORACLE is missing from this root is not a failure. Three of the
# eight test roots (alpine i386/amd64/riscv64) have no /bin/bash at all, so a
# case that compares native bash against the guest's own bash has nothing to
# compare against and used to report FAIL there -- a red result that says
# nothing about the code under test.
ck_skip() {
    skip=$((skip+1)); printf '  skip  %-30s %s\n' "$1" "$2"
}

ck() {
    name=$1; want=$2; shift 2
    got=$("$B" -c "$*" 2>&1 | tail -1)
    if [ "$got" = "$want" ]; then
        pass=$((pass+1)); printf '  ok    %s\n' "$name"
    else
        fail=$((fail+1)); printf '  FAIL  %-30s want [%s] got [%s]\n' "$name" "$want" "$got"
    fi
}

echo "== command substitution must carry the exit status =="
# The regression above. An EXTERNAL command is the case that broke; a builtin
# never spawns a task for the handler to steal, so test both.
ck cmdsubst-external 6      'x=$(sh -c "exit 6"); echo $?'
ck cmdsubst-builtin  1      'x=$(false); echo $?'
ck cmdsubst-value    hi     'x=$(echo hi); echo $x'
ck cmdsubst-nested   deep   'echo $(echo $(echo deep))'

echo "== the shapes that were NOT broken, so they stay that way =="
# Each of these collects a status by a different route than $( ) does.
ck subshell-rc       6      '( sh -c "exit 6" ); echo $?'
ck direct-external   6      'sh -c "exit 6"; echo $?'
ck pipestatus        6      'sh -c "exit 6" | cat; echo ${PIPESTATUS[0]}'
ck wait-status       4      'sh -c "exit 4" & wait $!; echo $?'

echo "== job control still works =="
ck bg-wait           ok     'sleep 1 & wait; echo ok'
ck lastpid           ok     'sleep 1 & [ -n "$!" ] && echo ok; wait'
ck jobs-builtin      ok     'sleep 1 & jobs >/dev/null && echo ok; wait'

echo "== recursion must not take the app down =="
# bash is the worse case of the two shells: FUNCNEST is UNSET by default, so
# bash's own limit never fires and the stack guard is the ONLY thing between a
# runaway recursive function and the end of a guest task thread's stack --
# which, for a native program, is the end of the APP. If one of these regresses
# the app dies and this script reports nothing at all, which is the point.
# These assert the MESSAGE, not a later command: the guard ends the -c script,
# so expecting anything printed afterwards would assert behaviour the shell does
# not have. A message at all proves the guard fired instead of the app dying --
# and the app dying is what this case exists to catch.
ck recurse-unbounded "environment: line 1: r: maximum function nesting level exceeded (out of stack)" 'r(){ r; }; r'
ck recurse-funcnest  "environment: line 1: r: maximum function nesting level exceeded (out of stack)" 'FUNCNEST=5000; r(){ r; }; r'
ck recurse-subshell   "rc=1 alive" 'r(){ r; }; x=$(r) 2>/dev/null; echo rc=$? alive'
ck recurse-legal-400  deep-ok  'f(){ [ $1 -gt 0 ] && f $(($1-1)) || echo deep-ok; }; f 400'
# bash's own FUNCNEST must still work where it applies.
ck funcnest-still-works 1 'FUNCNEST=10; r(){ r; }; r 2>&1 | grep -c "exceeded (10)"'

echo "== a null command must not re-launch itself for ever =="
# `{v}>file` with no command is a SIMPLE COMMAND WITH NO WORDS, and bash forks
# for it -- execute_null_command's forcefork -- so that the descriptor variable
# and the redirection land somewhere the shell will not see them. A fork gives
# the child a process that is already past the parse; it runs do_redirections
# and exits. A re-launch does not: the child is a fresh shell handed the
# command as TEXT.
#
# So the text matters. This site used to hand down the printed command, and the
# printed command is `{v}>file` again -- and forcefork is a property of that
# TEXT, not of the pipes the child was given, so the child took the same
# decision and re-launched a child of its own. Measured: 6141 nested shells for
# one `{v}>/tmp/x`, "shell level (1000) too high" six times over, a spawn
# failure at the bottom, exit 254, and no /tmp/x at all -- not one of those
# shells ever reached the redirection. It is the same trap the subshell site
# documents for `( ... )`, in the one other place where a command's own text
# routes it back through the fork.
#
# The child is now handed `: ` plus the redirections, which has a word to run
# and so never enters execute_null_command. These cases assert the redirection
# actually HAPPENED, which is the part the runaway lost, and that it happened
# quietly -- a regression here announces itself on stderr long before it
# announces itself in the exit status.
ck nullcmd-varassign  ok   'f=/tmp/aok-nc-1; rm -f $f; {v}>$f; [ -f $f ] && echo ok'
ck nullcmd-status     0    '{v}>/tmp/aok-nc-2; echo $?'
ck nullcmd-quiet      ok   'r=$( { {v}>/tmp/aok-nc-3; } 2>&1 ); [ -z "$r" ] && echo ok || echo "noise: $r"'
# The variable is bound in the child and discarded with it, as a fork gave.
ck nullcmd-var-local  ok   '{v}>/tmp/aok-nc-4; [ -z "$v" ] && echo ok || echo "leaked: $v"'
# The redirection is a real one, not a no-op that happens to exit 0.
ck nullcmd-truncates  0    'f=/tmp/aok-nc-5; printf abcdef > $f; {v}>$f; wc -c < $f'
ck nullcmd-fail-rc    1    '{v}>/no-such-dir/x; echo $?' 
# A here-document is the case that decided HOW the word is added: its body is
# printed after the whole command, so there is no position in the finished text
# to paste a word into, and the child's command has to be printed rather than
# spliced.
ck nullcmd-heredoc    0    '{h}<<EOF
hi
EOF
echo $?'
# The other thing that sends a null command here: a redirection of the shell'"'"'s
# own input descriptor.
ck nullcmd-input      ok   'f=/tmp/aok-nc-6; echo data > $f; ${nothing} <$f && echo ok'
ck nullcmd-closefd    ok   '${nothing} <&-; echo ok'
# In a pipeline, in every position. These are also the shapes that would catch
# a live pipe descriptor reaching the spawned child: the reader would never see
# EOF and the case would HANG rather than fail.
ck nullcmd-pipe-first ok   'f=/tmp/aok-nc-7; rm -f $f; {v}>$f | cat; [ -f $f ] && echo ok'
ck nullcmd-pipe-last  ok   'f=/tmp/aok-nc-8; rm -f $f; echo x | {v}>$f; [ -f $f ] && echo ok'
ck nullcmd-pipe-mid   ok   'f=/tmp/aok-nc-9; rm -f $f; echo x | {v}>$f | cat; [ -f $f ] && echo ok'
# And the compound shapes that reach it through another spawn.
ck nullcmd-in-func    ok   'f=/tmp/aok-nc-10; rm -f $f; g(){ {v}>$f; }; g | cat; [ -f $f ] && echo ok'
ck nullcmd-in-group   ok   'f=/tmp/aok-nc-11; rm -f $f; { {v}>$f; } | cat; [ -f $f ] && echo ok'
ck nullcmd-in-subsh   ok   'f=/tmp/aok-nc-12; rm -f $f; ( {v}>$f ) | cat; [ -f $f ] && echo ok'
ck nullcmd-async      ok   'f=/tmp/aok-nc-13; rm -f $f; {v}>$f & wait; [ -f $f ] && echo ok'
ck nullcmd-exec-pipe  ok   'f=/tmp/aok-nc-14; rm -f $f; { exec {v}>$f; } | cat; [ -f $f ] && echo ok'

echo "== the locale comes from the guest, not the host =="
# setlocale(cat, "") means "take it from the environment", and a native program
# is a function call inside the app -- so the C library resolved it against the
# HOST's environment, which on a device is whatever iOS is set to. Native and
# emulated bash then disagreed about what a character IS, and length, case and
# collation all follow from that. Asserted as AGREEMENT with the guest's own
# bash rather than a fixed number, so the case holds whatever locale the guest
# is actually in.
if [ -x /bin/bash ]; then
ck locale-agrees agree 'n=$(/AOK/native/bash -c "e=\$(printf \\303\\251); echo \${#e}"); g=$(/bin/bash -c "e=\$(printf \\303\\251); echo \${#e}"); [ "$n" = "$g" ] && echo agree || echo "differ n=$n g=$g"'
else
    ck_skip locale-agrees "no /bin/bash in this root to compare against"
fi

echo "== the special traps must not see the state script =="
# DEBUG, ERR and RETURN are CODE, not state: once armed they run before (DEBUG),
# on failure of (ERR) and on return from (RETURN) every command that follows.
# A forked child starts past the parse with nothing between the fork and the
# command it was forked for. A re-launched one runs ~77 lines of `declare -x`,
# `shopt` and function definitions first -- and used to arm these three traps in
# the MIDDLE of that, so every one of those lines tripped them.
#
# Measured before the fix: `trap 'echo T' DEBUG; ( : )` fired 78 times against a
# forked shell's 1, and the offset was a constant 77 at every re-launch site --
# subshell, command substitution, pipeline element, null command. That makes
# every DEBUG-trap-based tool (bashdb, a `trap ... DEBUG` profiler, a script that
# counts commands) read garbage inside any of them.
#
# Two things fixed it and both are asserted here. The traps are emitted LAST,
# after everything else the state restores; and they are emitted only under the
# option that arms them in a real child -- trap.c's
# reset_or_restore_signal_handlers clears SIG_TRAPPED for DEBUG and RETURN
# unless `set -T', and for ERR unless `set -E', so without those a forked
# subshell runs NO special trap at all and neither may this one.
#
# Asserted as AGREEMENT with the guest's own bash, run on the same script, for
# the same reason the locale case is: the right answer is whatever a fork does,
# not a number written down here.
ck_oracle() {
    n=$("$B" -c "$2" 2>&1 | tr '\n' ' ')
    g=$(/bin/bash -c "$2" 2>&1 | tr '\n' ' ')
    if [ "$n" = "$g" ]; then
        pass=$((pass+1)); printf '  ok    %s\n' "$1"
    else
        fail=$((fail+1))
        printf '  FAIL  %-30s bash [%s] native [%s]\n' "$1" "$g" "$n"
    fi
}

if [ -x /bin/bash ]; then
# The reported shapes. The command substitution is how it was first seen: the
# trap output has to be collected and counted, because a subshell's own stdout
# is the terminal and 78 lines of it scroll past unremarked.
ck_oracle debug-comsub-builtin  'n=$( trap "echo T" DEBUG; ( : ); trap - DEBUG; : ); echo "$n"'
ck_oracle debug-comsub-external 'n=$( trap "echo T" DEBUG; ( /bin/true ); trap - DEBUG; : ); echo "$n"'
ck_oracle debug-comsub-nullcmd  'n=$( trap "echo T" DEBUG; {v}>/tmp/aok-dbg-1; trap - DEBUG; : ); echo "$n"'
# ...and directly, where the count is small enough to read.
ck_oracle debug-subshell        'trap "echo T" DEBUG; ( : ); trap - DEBUG; echo .'
ck_oracle debug-subshell-two    'trap "echo T" DEBUG; ( :; : ); trap - DEBUG; echo .'
ck_oracle debug-nested          'trap "echo T" DEBUG; ( ( : ) ); trap - DEBUG; echo .'
ck_oracle debug-pipeline        'trap "echo T" DEBUG; : | cat; trap - DEBUG; echo .'
ck_oracle debug-async           'trap "echo T" DEBUG; ( : ) & wait; trap - DEBUG; echo .'
ck_oracle debug-group-in-pipe   'trap "echo T" DEBUG; { :; } | cat; trap - DEBUG; echo .'
# $? nonzero at the boundary is a separate path through the state: it is the one
# case that emits a status-restoring command AFTER the traps.
ck_oracle debug-after-failure   'trap "echo T" DEBUG; false; ( : ); trap - DEBUG; echo .'
ck_oracle err-subshell          'trap "echo E" ERR; ( : ); trap - ERR; echo .'
ck_oracle err-after-failure     'trap "echo E" ERR; false; ( : ); trap - ERR; echo .'
ck_oracle err-subshell-fails    'trap "echo E" ERR; ( false ); trap - ERR; echo .'
ck_oracle return-subshell       'trap "echo R" RETURN; f(){ :; }; ( f ); trap - RETURN; echo .'

# The other half: under -T and -E the traps DO cross, and must still not see the
# state. A subshell and a command substitution announce their own commands (the
# parent does not run the trap for them), so these are exact.
ck_oracle T-debug-subshell      'set -T; trap "echo T" DEBUG; ( : ); trap - DEBUG; echo .'
ck_oracle T-debug-subshell-two  'set -T; trap "echo T" DEBUG; ( :; : ); trap - DEBUG; echo .'
ck_oracle T-debug-comsub        'set -T; trap "echo T" DEBUG; n=$( : ); trap - DEBUG; echo "[$n]"'
ck_oracle T-return-subshell     'set -T; trap "echo R" RETURN; f(){ :; }; ( f ); trap - RETURN; echo .'
ck_oracle E-err-subshell-ok     'set -E; trap "echo E" ERR; ( : ); trap - ERR; echo .'
ck_oracle E-err-subshell-fails  'set -E; trap "echo E" ERR; ( false ); trap - ERR; echo .'
ck_oracle E-err-comsub-fails    'set -E; trap "echo E" ERR; n=$( false ); trap - ERR; echo .'

# KNOWN DIVERGENCE, deliberately not asserted: under `set -T', a re-launch from
# execute_simple_command -- a pipeline element, an async simple command -- fires
# DEBUG twice, because that site runs the trap and THEN calls make_child, so the
# parent has announced the command and the re-parsing child announces it again.
# `set -T; trap "echo T" DEBUG; : | cat' is 5 here and 3 in a fork. Closing it
# means the child skipping counted fires on a signal from the parent, and a
# mechanism that can SWALLOW a DEBUG fire is worse for a debugger than one that
# adds one. See the divergence note above aok_serialize_state.

echo "== errexit must not kill the child in its own prologue =="
# The state restores $? with a trailing `(exit N)`, and `set -e' is restored
# three lines above it -- so a subshell entered with a nonzero $? under errexit
# exited in its prologue and never parsed the command it was spawned to run.
# The shape below is the one that gets there: a failing command on the left of
# `&&' leaves $? at 1 without tripping errexit, and every following subshell
# then died. Measured: native bash printed NOTHING for the first two of these,
# where a forked shell prints `hi done' -- the subshell died, and the parent
# died with it under the same errexit. That is silent data loss, and it is why
# the restore is now `(exit N) && :': a command on the left of `&&' is exempt
# from both errexit and the ERR trap, and short-circuits, so $? is still N.
ck_oracle errexit-subshell      'set -e; false && true; ( echo hi ); echo done'
ck_oracle errexit-comsub        'set -e; false && true; n=$( echo hi ); echo "$n done"'
ck_oracle errexit-and-err-trap  'set -e; trap "echo E" ERR; false && true; ( echo hi ); echo done'
ck_oracle errexit-nullcmd       'set -e; false && true; {v}>/tmp/aok-dbg-2; echo done'
# ...and with the ERR trap actually crossing, which is the combination that has
# both halves live in the child: `set -E' arms it there, and the status restore
# is the one command in the state that can fail. The `&&' exemption has to reach
# into the subshell the restore spawns, not just the list in the child.
ck_oracle errtrace-nonzero      'set -E; trap "echo E" ERR; false && true; ( echo hi ); echo done'
ck_oracle errtrace-errexit      'set -Ee; trap "echo E" ERR; false && true; ( echo hi ); echo done'
ck_oracle errtrace-comsub       'set -E; trap "echo E" ERR; false && true; n=$( echo hi ); echo "$n done"'
ck_oracle errtrace-status       'set -E; trap "echo E" ERR; (exit 7) && :; ( echo rc=$? ); echo done'
ck_oracle functrace-errtrace    'set -TE; trap "echo E" ERR; false && true; ( echo hi ); echo done'
# ...while the status itself still crosses, which is what the restore is for.
ck_oracle status-crosses        'false; ( echo rc=$? )'
ck_oracle status-crosses-7      '(exit 7) && :; ( echo rc=$? )'
ck_oracle status-crosses-comsub 'false; n=$( echo rc=$? ); echo "$n"'
ck_oracle status-under-errexit  'set -e; (exit 7) && :; ( echo rc=$? ); echo done'
else
    ck_skip traps-and-errexit "no /bin/bash in this root to compare against"
fi

echo "== the state must not be published in /proc/PID/cmdline =="
# A re-launched subshell used to carry the whole serialized state as argv[2] of
# `bash -c`, and aok_relaunch_state emits `declare -x NAME='value'` for every
# exported variable. So a subshell published its entire environment in
# /proc/PID/cmdline, which is mode 0444 -- any user on the system could read
# another user's environment out of `ps`. Linux keeps that in
# /proc/PID/environ, which is 0400. The state now travels in the environment
# as AOK_BASH_STATE and argv[2] is a fixed bootstrap.
#
# The unset has to be the FIRST thing the eval'd script does, not something the
# bootstrap does after `eval`: a subshell's own commands are appended to that
# script, so an unset placed after the eval runs only after the user's code has
# already seen the variable.
ck cmdline-no-env    clean 'export SEKRIT=SEKRITVALUE; ( sleep 3; : ) & c=$!; sleep 1; r=clean; tr "\0" " " < /proc/$c/cmdline | grep -q SEKRITVALUE && r=LEAKED; kill $c 2>/dev/null; echo $r'
# Length is the blunt instrument that catches a regression this test did not
# anticipate the shape of: the old cmdline ran to thousands of bytes.
ck cmdline-bounded   ok    'p=xxxxxxxxxxxxxxxxxxxx; p=$p$p$p$p$p; p=$p$p$p$p$p; export PAD=$p; ( sleep 3; : ) & c=$!; sleep 1; n=$(wc -c < /proc/$c/cmdline); kill $c 2>/dev/null; [ "$n" -lt 200 ] && echo ok || echo "too long: $n"'
# The carrier must be invisible to the code it carries, in both namespaces.
ck state-var-hidden  0     '( echo ${#AOK_BASH_STATE} )'
ck state-env-hidden  0     '( env | grep -c AOK_BASH_STATE )'
# ...while a real exported variable still reaches a child process untouched.
ck env-still-exported 1    'export SEKRIT=SEKRITVALUE; ( env | grep -c "^SEKRIT=SEKRITVALUE$" )'

echo "== state crossing =="
ck var               outer  'v=outer; ( v=inner ); echo $v'
ck function          fn-ok  'f(){ echo fn-ok; }; echo $(f)'
ck exported          exp    'export E=exp; echo $(echo $E)'

echo
echo "  passed=$pass failed=$fail skipped=$skip"
[ "$fail" -eq 0 ]
