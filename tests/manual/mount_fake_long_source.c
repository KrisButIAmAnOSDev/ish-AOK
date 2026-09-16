// mount_fake_long_source.c — a guest-reachable buffer overflow in fakefs_mount.
//
// fakefs_mount() derives the metadata database path from the mount source by
// swapping the trailing "data" component for "meta.db". It built that path in a
// host `char db_path[PATH_MAX]` and wrote "meta.db" in with an unchecked
// strncpy. "meta.db" is three bytes longer than the "data" it replaces, so a
// source that just fits PATH_MAX overruns db_path by up to three bytes and
// aborts the whole host process via __stack_chk_fail -- and the guest chooses
// the source, since `mount -t fake <source> <target>` needs only CAP_SYS_ADMIN
// (uid 0 in a default AOK session) and mount(2) accepts a source up to
// MAX_PATH (4096) bytes.
//
// The limit is the HOST's PATH_MAX, not the guest's: 1024 on the iOS/macOS app
// and the macOS CLI, 4096 on a Linux CLI host. A ".../data" source of length
// `len` makes a db path of len + 3 bytes plus its NUL, so it fits a host buffer
// of P bytes exactly when len <= P - 4. The fix refuses anything longer with
// ENAMETOOLONG (a truncated path would name a different database). This test
// pins both sides of that boundary:
//
//   - len 1020 fits a 1024 buffer exactly, so it must reach the host on every
//     host (the host then fails it: the path does not exist). Rejecting it is
//     an off-by-one in the check.
//   - len 4093..4095 cannot fit a 4096 buffer, so it must be ENAMETOOLONG on
//     every host.
//   - The rest depends on P, which a guest cannot read. It is inferred from a
//     2048-byte source, which fits a 4096 buffer and not a 1024 one, so the
//     answer names the host rather than the boundary: that one length cannot
//     hide an off-by-one. Then P-4 must reach the host and P-3..P-1 must be
//     ENAMETOOLONG.
//
// On the unfixed binary the first overflowing length aborts ish outright, so
// no verdict is printed at all. Where the overrun does not abort, the
// ENAMETOOLONG checks still catch it: on a 1024 host the old code cut a longer
// source short and returned EINVAL, and on a 4096 host a source of 4093..4095
// bytes went on to realpath() and came back ENOENT.
//
// Needs root (mount(2) wants CAP_SYS_ADMIN). Skips cleanly otherwise. The
// positive control -- that an ordinary fake mount still works -- re-mounts the
// booted root's own fakefs backing dir, whose path /proc/ish/snapshot reports;
// it is skipped (not failed) where that is unavailable (a realfs CLI root, or a
// foreign chroot), so the length assertions still run there.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_common.h"

#ifndef ENAMETOOLONG
#define ENAMETOOLONG 36
#endif

static void check(const char *label, int cond) {
    if (cond) {
        test_logf("ok   %s\n", label);
    } else {
        printf("FAIL %s\n", label);
        failures_total++;
    }
}

// Build "/aaa.../aaa/data" of total length `len` into `out` (which must hold
// len+1 bytes). The final component is exactly "data", so fakefs_mount accepts
// the shape and reaches the meta.db path construction.
static void make_data_source(char *out, size_t len) {
    // Layout: '/' <fill> "/data"  ->  1 + fill + 5 == len, so fill = len - 6.
    size_t fill = len - 6;
    size_t i = 0;
    out[i++] = '/';
    // Short components: one component over NAME_MAX makes the host's realpath()
    // fail ENAMETOOLONG by itself, which would satisfy the checks below without
    // the length check in fakefs_mount ever running.
    for (size_t j = 0; j < fill; j++)
        out[i++] = (j % 16 == 15 && j + 1 < fill) ? '/' : 'a';
    memcpy(out + i, "/data", 5);
    i += 5;
    out[i] = '\0';
}

// Mount a `len`-byte ".../data" source on `point`. Returns 0 if the mount
// succeeded (it never should: the path does not exist), else the errno.
static int mount_len(const char *point, size_t len) {
    char *src = malloc(len + 1);
    if (src == NULL) {
        check("alloc source", 0);
        return -1;
    }
    make_data_source(src, len);
    char label[64];
    snprintf(label, sizeof label, "len=%zu source length as intended", len);
    check(label, strlen(src) == len);

    // Printed before the call and without -v: the bug takes the whole host
    // down inside mount(), and this line is then the only record of the length.
    printf("mount len=%zu\n", len);
    errno = 0;
    int r = mount(src, point, "fake", 0, NULL);
    int e = r == 0 ? 0 : errno;
    free(src);
    if (r == 0)
        umount(point);
    // Getting here at all is half the test: an unfixed binary aborts inside
    // that mount() and never returns.
    test_logf("len=%zu -> ret=%d errno=%d (%s)\n", len, r, e, strerror(e));
    return e;
}

// len <= P - 4: the db path fits, so fakefs_mount must pass the source on to
// the host, which fails it. Which errno the host picks is its business (ENOENT
// on a Mac; a sandboxed device could say otherwise); only ENAMETOOLONG would
// mean the length check refused a path that fits.
static void expect_fits(const char *point, size_t len, const char *why) {
    int e = mount_len(point, len);
    char label[160];
    snprintf(label, sizeof label, "len=%zu %s: reaches the host, not ENAMETOOLONG (errno=%d)",
             len, why, e);
    check(label, e > 0 && e != ENAMETOOLONG);
}

// len > P - 4: the db path does not fit, so fakefs_mount must refuse it.
static void expect_too_long(const char *point, size_t len, const char *why) {
    int e = mount_len(point, len);
    char label[160];
    snprintf(label, sizeof label, "len=%zu %s: ENAMETOOLONG (errno=%d)", len, why, e);
    check(label, e == ENAMETOOLONG);
}

static void test_lengths(const char *point) {
    expect_fits(point, 1020, "fits a 1024 buffer exactly");

    size_t host_path_max = 0;
    int e = mount_len(point, 2048);
    if (e == ENAMETOOLONG) {
        host_path_max = 1024;
    } else if (e == ENOENT) {
        host_path_max = 4096;
    } else {
        char label[128];
        snprintf(label, sizeof label,
                 "len=2048 names the host limit: ENAMETOOLONG (1024) or ENOENT (4096) (errno=%d)", e);
        check(label, 0);
    }
    test_logf("host PATH_MAX inferred: %zu\n", host_path_max);

    static const size_t lens[] = {1021, 1022, 1023, 4092, 4093, 4094, 4095};
    for (size_t k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
        size_t len = lens[k];
        if (len >= 4093) {
            expect_too_long(point, len, "cannot fit a 4096 buffer");
        } else if (host_path_max == 0) {
            // Host unknown: this length is legal on one host and not the other,
            // so only require that it fails without taking the host down.
            char label[64];
            snprintf(label, sizeof label, "len=%zu fails cleanly", len);
            check(label, mount_len(point, len) > 0);
        } else if (len + 4 <= host_path_max) {
            expect_fits(point, len, "fits this host's buffer");
        } else {
            expect_too_long(point, len, "overflows this host's buffer");
        }
    }
}

// Read "source <path>" from /proc/ish/snapshot: the booted root's fakefs
// backing dir when / is a fakefs, or a "(not a fakefs root...)" line otherwise.
// Returns 1 and fills `out` on success.
static int booted_fakefs_source(char *out, size_t outlen) {
    FILE *f = fopen("/proc/ish/snapshot", "r");
    if (f == NULL)
        return 0;
    char line[PATH_MAX + 64];
    int got = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "source ", 7) != 0)
            continue;
        char *p = line + 7;
        while (*p == ' ')
            p++;
        size_t n = strlen(p);
        while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == ' '))
            p[--n] = '\0';
        // The not-a-fakefs line begins with '(' where a real path begins '/'.
        if (p[0] == '/' && n + 1 <= outlen) {
            memcpy(out, p, n + 1);
            got = 1;
        }
        break;
    }
    fclose(f);
    return got;
}

// Positive control: an ordinary-length fake mount still succeeds and umounts.
static void test_normal_mount_works(const char *point) {
    char src[PATH_MAX];
    if (!booted_fakefs_source(src, sizeof src)) {
        test_logf("skip positive control: / is not a fakefs root "
                  "(/proc/ish/snapshot names no source)\n");
        return;
    }
    test_logf("positive control source: %s\n", src);

    errno = 0;
    int r = mount(src, point, "fake", 0, NULL);
    check("normal fake mount succeeds", r == 0);
    if (r != 0) {
        test_logf("normal mount errno=%d (%s)\n", errno, strerror(errno));
        return;
    }
    struct stat st;
    check("mounted root stats as a directory", stat(point, &st) == 0 && S_ISDIR(st.st_mode));
    check("normal fake mount umounts", umount(point) == 0);
}

int main(int argc, char **argv) {
    // Unbuffered: the bug this catches takes the whole host process down, and
    // buffered output would die with it. mount_len prints each length first.
    setvbuf(stdout, NULL, _IONBF, 0);
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    char base[64];
    snprintf(base, sizeof base, "/tmp/mfls_XXXXXX");
    if (mkdtemp(base) == NULL) {
        printf("mount_fake_long_source: SKIP (mkdtemp: %s)\n", strerror(errno));
        return 0;
    }
    char point[128];
    snprintf(point, sizeof point, "%s/mnt", base);
    if (mkdir(point, 0755) != 0) {
        printf("mount_fake_long_source: SKIP (mkdir point: %s)\n", strerror(errno));
        return 0;
    }

    // Privilege probe: mount(2) needs CAP_SYS_ADMIN. An unprivileged session
    // gets EPERM before the source is ever inspected, so skip rather than fail.
    // The source does not end in "data", so a privileged mount is refused with
    // EINVAL before any host call -- a host errno can't pass for EPERM here.
    errno = 0;
    if (mount("/probe/notdata", point, "fake", 0, NULL) != 0 &&
            (errno == EPERM || errno == EACCES)) {
        printf("mount_fake_long_source: SKIP (mount not permitted: %s)\n", strerror(errno));
        rmdir(point);
        rmdir(base);
        return 0;
    }

    test_lengths(point);
    test_normal_mount_works(point);

    umount(point); // harmless if nothing is mounted
    rmdir(point);
    rmdir(base);
    return finish_suite("mount_fake_long_source");
}
