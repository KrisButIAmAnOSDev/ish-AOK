project('ish-tests', 'c')
add_project_arguments(['-m32', '-fno-stack-protector', '-I' + meson.current_source_dir()], language: 'c')
add_project_link_arguments('-m32', language: 'c')

# hello world
executable('hello', ['hello.c'], link_args: ['-static', '-nostdlib'])
executable('hello-libc-static', ['hello-clib.c'], link_args: ['-static'])
executable('hello-libc', ['hello-clib.c'])

# simple benchmark
executable('looper', ['looper.c'])
executable('fibbonaci', ['fibbonaci.c'])

# filesystem
executable('cat', ['cat.c'])
executable('stat', ['stat.c'], c_args: ['-D_FILE_OFFSET_BITS=64'])
executable('getdents', ['getdents.c'])

executable('signal', ['signal.c'], link_args: ['-static'])
executable('forkexec', ['forkexec.c'])

executable('thread', ['thread.c'], dependencies: dependency('threads'))
executable('atomics32', ['x86/atomics32.c'], dependencies: dependency('threads'))
executable('atomic_xadd32', ['x86/atomic_xadd32.c'], dependencies: dependency('threads'))
executable('atomic_cmpxchg32', ['x86/atomic_cmpxchg32.c'], dependencies: dependency('threads'))
executable('atomic_cmpxchg8b', ['x86/atomic_cmpxchg8b.c'], dependencies: dependency('threads'))
executable('atomic_logic32', ['x86/atomic_logic32.c'])
executable('signal_core', ['signal_core.c'], dependencies: dependency('threads'))
executable('signal_restart', ['signal_restart.c'], dependencies: dependency('threads'))
executable('signal_realtime', ['signal_realtime.c'], dependencies: dependency('threads'))
executable('signal_altstack', ['signal_altstack.c'], dependencies: dependency('threads'))
executable('signal_poll', ['signal_poll.c'], dependencies: dependency('threads'))
executable('eventfd_interrupt', ['eventfd_interrupt.c'], dependencies: dependency('threads'))
executable('futex_core', ['futex_core.c'], dependencies: dependency('threads'))
executable('process_lifecycle', ['process_lifecycle.c'], dependencies: dependency('threads'))
executable('pthread_sync', ['pthread_sync.c'], dependencies: dependency('threads'))
executable('fcntl_lock', ['fcntl_lock.c'])
executable('fcntl_ofd', ['fcntl_ofd.c'])
executable('at_empty_path', ['at_empty_path.c'])
executable('copy_file_range', ['copy_file_range.c'])
executable('name_to_handle_at', ['name_to_handle_at.c'])
executable('sendfile_vhangup', ['sendfile_vhangup.c'])
executable('pidfd_open', ['pidfd_open.c'])
executable('ptrace_group_stop', ['ptrace_group_stop.c'])
executable('ptrace_group_stop_report', ['ptrace_group_stop_report.c'])
executable('ptrace_thread_follow', ['ptrace_thread_follow.c'], dependencies: dependency('threads'))
executable('epoll_mod_wake', ['epoll_mod_wake.c'], dependencies: dependency('threads'))
executable('epoll_oneshot_rearm', ['epoll_oneshot_rearm.c'])
executable('ptrace_exit_kill', ['ptrace_exit_kill.c'])

# various tests for code that modifies itself
executable('modify', ['modify.c'], link_args: ['-zexecstack'])

# qemu test program
executable('qemu-test', ['qemu-test.c'], link_args: ['-lm'])

run_target('busybox',
    command: ['get-busybox.sh'])
