// POSITIVE CONTROL 2: a tight loop in a syscall that cannot be served from
// userspace, so the profiler must put the samples OUTSIDE guest code.
// syscall(2) directly, not getpid(), because a libc may answer that itself.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 3000000;
    long acc = 0;
    for (long i = 0; i < n; i++)
        acc += syscall(SYS_getpid);
    printf("CTL_SYSCALL_OK n=%ld acc=%ld\n", n, acc);
    return 0;
}
