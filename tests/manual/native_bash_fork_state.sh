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
