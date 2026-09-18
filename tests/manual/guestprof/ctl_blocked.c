// POSITIVE CONTROL 3: time parked in a wait must land in the BLOCKED bucket,
// not in "kernel" -- that split is what keeps a network-bound workload from
// reading as emulator overhead.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv) {
    int ms = argc > 1 ? atoi(argv[1]) : 4000;
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long) (ms % 1000) * 1000000L };
    if (nanosleep(&ts, NULL) != 0) { perror("nanosleep"); return 2; }
    printf("CTL_BLOCKED_OK slept_ms=%d\n", ms);
    return 0;
}
