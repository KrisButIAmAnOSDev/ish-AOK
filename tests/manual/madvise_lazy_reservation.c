// A large anonymous mapping is MAPPED even before anything touches it.
//
// AOK does not build page-table entries for a big anonymous mmap up front; it
// records a reservation and materialises on first fault (emu/memory.h, struct
// mem_lazy_map -- one struct pt_entry per page is ~65 bytes, so an untouched
// GiB used to cost ~16.6 MB the instant it was asked for). Every reader that
// treats "no page-table entry" as "no mapping" therefore has to consult the
// reservation list too.
//
// mincore did. madvise and msync did not, so both reported ENOMEM for a range
// the guest had legitimately mapped and never written. InnoDB MADV_DONTDUMPs
// its whole buffer pool the moment it allocates it, so every MariaDB start
// logged:
//
//   [Warning] InnoDB: Failed to set memory to MADV_DONTDUMP:
//             Cannot allocate memory ptr 0x7fff90000000 size 134217728
//
// The mapping was fine; the answer was wrong.
//
// The size here is not arbitrary: reservations only kick in above
// MEM_LAZY_MIN_PAGES (64 MB), so a smaller mapping takes the eager path and
// tests nothing. 128 MB is what InnoDB's default buffer pool asks for.
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define BIG (128UL * 1024 * 1024)

static void ck_errno(const char *what, int rc, int want) {
    int got = rc < 0 ? errno : 0;
    if (got != want)
        failf(what, (uint64_t) got, (uint64_t) rc, 0, (uint64_t) want, 0, 0);
    test_logf("  %-52s %s (%s)\n", what, got == want ? "ok" : "FAIL",
              got ? strerror(got) : "success");
}

static char *map_big(void) {
    char *p = mmap(NULL, BIG, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

// Returns 1 if the child saw zeroes, 0 if it saw the parent's data, -1 on error.
static int child_sees_zeroes(char *p) {
    p[0] = 42;
    p[BIG - 1] = 43;
    pid_t kid = fork();
    if (kid < 0)
        return -1;
    if (kid == 0)
        _exit(p[0] == 0 && p[BIG - 1] == 0 ? 0 : 1);
    int status = 0;
    if (waitpid(kid, &status, 0) < 0 || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status) == 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    char *p = map_big();
    if (p == NULL) {
        // Not a kernel defect -- a device too small to hold the mapping cannot
        // answer the question this test asks.
        printf("madvise_lazy_reservation: SKIP (cannot map %luMB anonymous)\n",
               BIG / (1024 * 1024));
        return 0;
    }

    // Untouched, so entirely reservation and no page-table entries at all.
    ck_errno("MADV_DONTDUMP over an untouched 128MB map",
             madvise(p, BIG, MADV_DONTDUMP), 0);
    ck_errno("MADV_DODUMP over an untouched 128MB map",
             madvise(p, BIG, MADV_DODUMP), 0);
    // DONTNEED and FREE promise the pages read back as zero afterwards. A
    // reservation has never been written, so that already holds and there is
    // nothing to discard -- but the call still has to succeed.
    ck_errno("MADV_DONTNEED over an untouched 128MB map",
             madvise(p, BIG, MADV_DONTNEED), 0);
    ck_errno("MADV_FREE over an untouched 128MB map",
             madvise(p, BIG, MADV_FREE), 0);
    ck_errno("msync(MS_SYNC) over an untouched 128MB map",
             msync(p, BIG, MS_SYNC), 0);
    // A private mapping has no shared backing to punch a hole in, so this is
    // EINVAL on Linux -- and must stay EINVAL rather than becoming the ENOMEM
    // the missing reservation check produced.
    ck_errno("MADV_REMOVE on a private map is EINVAL",
             madvise(p, BIG, MADV_REMOVE), EINVAL);

    // WIPEONFORK records itself in per-page flags, so unlike the hints above it
    // has to materialise the reservation first. Applied while still untouched.
    ck_errno("MADV_WIPEONFORK on an untouched map",
             madvise(p, BIG, MADV_WIPEONFORK), 0);
    {
        int saw = child_sees_zeroes(p);
        if (saw != 1)
            failf("WIPEONFORK actually wipes across fork", (uint64_t) saw, 0, 0, 1, 0, 0);
        test_logf("  %-52s %s\n", "WIPEONFORK actually wipes across fork",
                  saw == 1 ? "ok" : "FAIL");
    }
    // Now materialised: the same advice must keep working on real entries.
    ck_errno("MADV_DONTDUMP after the map is materialised",
             madvise(p, BIG, MADV_DONTDUMP), 0);
    munmap(p, BIG);

    // KEEPONFORK, the other half of the pair, also on an untouched reservation.
    p = map_big();
    if (p != NULL) {
        ck_errno("MADV_KEEPONFORK on an untouched map",
                 madvise(p, BIG, MADV_KEEPONFORK), 0);
        int saw = child_sees_zeroes(p);
        if (saw != 0)
            failf("KEEPONFORK keeps the data across fork", (uint64_t) saw, 0, 0, 0, 0, 0);
        test_logf("  %-52s %s\n", "KEEPONFORK keeps the data across fork",
                  saw == 0 ? "ok" : "FAIL");
        munmap(p, BIG);
    }

    // The control, and the reason this is not just "return 0 everywhere": a
    // range with a REAL hole in it is still ENOMEM, for both calls.
    p = map_big();
    if (p != NULL) {
        munmap(p + BIG / 2, 4096);
        ck_errno("madvise over a range with a real hole is ENOMEM",
                 madvise(p, BIG, MADV_DONTDUMP), ENOMEM);
        ck_errno("msync over a range with a real hole is ENOMEM",
                 msync(p, BIG, MS_SYNC), ENOMEM);
        munmap(p, BIG);
    }

    return finish_suite("madvise_lazy_reservation");
}
