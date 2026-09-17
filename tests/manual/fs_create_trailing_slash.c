// Creating a NON-directory through a name spelled with a trailing slash.
//
// "d/fifo/" asks for a directory called fifo. mknod, mkfifo, symlink and link
// do not make directories, so Linux refuses: filename_create() suppresses
// LOOKUP_CREATE when the name carries a trailing slash and a directory was not
// requested, then turns the negative dentry that comes back into ENOENT --
// "you had / on the end, you've been asking for (non-existent) directory".
// AOK dropped the slash during path normalization and cheerfully created the
// name WITHOUT it, reporting success, so a guest had no way to tell a request
// for a directory from a request for a fifo.
//
// Three things are asserted, because getting the errno right is only part of
// it:
//
//  - the ERRNO. ENOENT for the create-a-node family, EISDIR for open(O_CREAT)
//    (Linux answers that one in open_last_lookups(), before the final lookup,
//    so it does not depend on the name existing), and plain EEXIST for a name
//    that is already there, whatever kind of thing it is.
//  - that NOTHING WAS CREATED. The bug's damage is the leftover entry, not the
//    return value.
//  - the ORDER. The trailing-slash answer comes after everything the parent
//    walk can raise (a missing parent, a parent that is a file, one with no
//    search permission) and BEFORE the parent's write permission -- Linux only
//    asks may_create() afterwards, inside vfs_mknod/vfs_symlink/vfs_link. So
//    an unwritable parent reports ENOENT here, not EACCES. That half needs an
//    unprivileged caller to mean anything, which is what the second leg is.
//
// mkdir is the control throughout: a trailing slash asks for exactly what it
// creates, so it must keep working, and in an unwritable parent it is the one
// member of the family that still reports EACCES.
//
// Every expectation below was measured on the Linux 6.12 oracle (Devuan 6,
// x86_64 glibc), 64-bit and -m32, both identical.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "test_common.h"

static char dir[256];        // writable scratch directory
static char src[320];        // a regular file used as link()'s source

static void check(const char *label, int r, int want_errno) {
    int e = r < 0 ? errno : 0;
    if (!(r < 0 && e == want_errno))
        failf(label, (uint64_t) r, (uint64_t) e, 0, (uint64_t) -1, (uint64_t) want_errno, 0);
    test_logf("  %-52s rc=%d errno=%d (want %d)\n", label, r, e, want_errno);
}

static void check_ok(const char *label, int r) {
    if (r < 0)
        failf(label, (uint64_t) r, (uint64_t) errno, 0, 0, 0, 0);
    test_logf("  %-52s rc=%d (want 0)\n", label, r);
}

// The point of the whole exercise: a refused create must not have left the
// un-slashed name behind.
static void check_absent(const char *label, const char *d, const char *name) {
    char p[512];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", d, name);
    if (lstat(p, &st) == 0) {
        failf(label, (uint64_t) st.st_mode, 0, 0, 0, 0, 0);
        printf("  %s: %s was created anyway (mode %06o)\n", label, p, (unsigned) st.st_mode);
        if (S_ISDIR(st.st_mode))
            rmdir(p);
        else
            unlink(p);
    }
}

static int open_rc(const char *path, int flags, int mode) {
    int fd = open(path, flags, mode);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

// ---------------------------------------------------------------- writable

// A name that is not there, spelled with a trailing slash, in a directory the
// caller CAN write. Nothing may be created and every call must say so.
static void writable_missing_name(const char *sfx) {
    char t[512], label[160];
    snprintf(t, sizeof t, "%s/gone%s", dir, sfx);

#define CASE(op, call, want)                                                   \
    do {                                                                       \
        snprintf(label, sizeof label, "%s(gone%s)", op, sfx);                  \
        errno = 0;                                                             \
        check(label, (call), (want));                                          \
        check_absent(label, dir, "gone");                                      \
    } while (0)

    CASE("mknod",   mknod(t, S_IFIFO | 0644, 0),                    ENOENT);
    CASE("mkfifo",  mkfifo(t, 0644),                                ENOENT);
    CASE("symlink", symlink("target", t),                           ENOENT);
    CASE("link",    link(src, t),                                   ENOENT);
    // open() cannot create a directory, so this one is EISDIR rather than
    // ENOENT -- Linux decides it before it ever looks the name up.
    CASE("open(O_CREAT)",        open_rc(t, O_CREAT | O_WRONLY, 0644),          EISDIR);
    CASE("open(O_CREAT|O_EXCL)", open_rc(t, O_CREAT | O_EXCL | O_WRONLY, 0644), EISDIR);
    CASE("open(O_CREAT|O_RDONLY)", open_rc(t, O_CREAT | O_RDONLY, 0644),        EISDIR);
    // Without O_CREAT there is nothing to create and the name is simply absent.
    CASE("open(O_WRONLY)", open_rc(t, O_WRONLY, 0), ENOENT);
#undef CASE

    // mkdir asks for precisely what the trailing slash describes.
    snprintf(label, sizeof label, "mkdir(gone%s)", sfx);
    errno = 0;
    check_ok(label, mkdir(t, 0755));
    {
        char p[512];
        struct stat st;
        snprintf(p, sizeof p, "%s/gone", dir);
        if (lstat(p, &st) < 0 || !S_ISDIR(st.st_mode))
            failf("mkdir through a trailing slash must create the directory",
                  (uint64_t) (lstat(p, &st) == 0 ? st.st_mode : 0), 0, 0, 0, 0, 0);
        rmdir(p);
    }
}

// The same calls without the slash: all of this must still work.
static void writable_controls(void) {
    char t[512];
    snprintf(t, sizeof t, "%s/gone", dir);

    errno = 0; check_ok("mknod(gone)",   mknod(t, S_IFIFO | 0644, 0));  unlink(t);
    errno = 0; check_ok("mkfifo(gone)",  mkfifo(t, 0644));              unlink(t);
    errno = 0; check_ok("symlink(gone)", symlink("target", t));         unlink(t);
    errno = 0; check_ok("link(gone)",    link(src, t));                 unlink(t);
    errno = 0; check_ok("open(gone, O_CREAT)", open_rc(t, O_CREAT | O_WRONLY, 0644)); unlink(t);
    errno = 0; check_ok("mkdir(gone)",   mkdir(t, 0755));               rmdir(t);
}

// A name that IS there. The lookup gives a positive dentry and filename_create
// answers EEXIST before the trailing-slash test ever runs, so the kind of
// thing occupying the name does not matter -- file, directory, or a symlink
// pointing at nothing.
static void writable_existing_name(const char *base, const char *sfx) {
    char t[512], label[160];
    snprintf(t, sizeof t, "%s/%s%s", dir, base, sfx);

#define CASE(op, call, want)                                                   \
    do {                                                                       \
        snprintf(label, sizeof label, "%s(%s%s)", op, base, sfx);              \
        errno = 0;                                                             \
        check(label, (call), (want));                                          \
    } while (0)

    CASE("mknod",   mknod(t, S_IFIFO | 0644, 0), EEXIST);
    CASE("mkfifo",  mkfifo(t, 0644),             EEXIST);
    CASE("symlink", symlink("target", t),        EEXIST);
    CASE("link",    link(src, t),                EEXIST);
    CASE("mkdir",   mkdir(t, 0755),              EEXIST);
    // ...but open(O_CREAT) still says EISDIR, because its check does not look
    // the name up at all.
    CASE("open(O_CREAT)", open_rc(t, O_CREAT | O_WRONLY, 0644), EISDIR);
#undef CASE
}

// Whatever the parent walk has to say, it says first.
static void writable_parent_errors(void) {
    char t[512];

    snprintf(t, sizeof t, "%s/exists-dir/no-such-dir/x/", dir);
    errno = 0; check("mknod(missing-parent/x/)",  mknod(t, S_IFIFO | 0644, 0), ENOENT);
    errno = 0; check("symlink(missing-parent/x/)", symlink("target", t),       ENOENT);
    errno = 0; check("open(missing-parent/x/, O_CREAT)",
                     open_rc(t, O_CREAT | O_WRONLY, 0644),                     ENOENT);

    snprintf(t, sizeof t, "%s/exists-file/x/", dir);
    errno = 0; check("mknod(file-as-parent/x/)",  mknod(t, S_IFIFO | 0644, 0), ENOTDIR);
    errno = 0; check("open(file-as-parent/x/, O_CREAT)",
                     open_rc(t, O_CREAT | O_WRONLY, 0644),                     ENOTDIR);

    // ".." is resolved first, so this is the ordinary missing-name case wearing
    // a longer spelling.
    snprintf(t, sizeof t, "%s/exists-dir/../gone/", dir);
    errno = 0; check("mknod(dir/../gone/)", mknod(t, S_IFIFO | 0644, 0), ENOENT);
    check_absent("mknod(dir/../gone/)", dir, "gone");

    // A trailing slash on a name that is a plain file is ENOTDIR for an open
    // that is not creating anything -- a different rule from the EISDIR above,
    // and it must not have been disturbed.
    snprintf(t, sizeof t, "%s/exists-file/", dir);
    errno = 0; check("open(exists-file/, O_WRONLY)", open_rc(t, O_WRONLY, 0), ENOTDIR);
}

static void run_writable(void) {
    test_logf("writable parent %s:\n", dir);
    writable_missing_name("/");
    writable_missing_name("//");
    writable_controls();
    static const char *bases[] = {"exists-file", "exists-dir", "exists-dangling"};
    for (unsigned b = 0; b < 3; b++) {
        writable_existing_name(bases[b], "/");
        writable_existing_name(bases[b], "//");
    }
    writable_parent_errors();
}

// -------------------------------------------------------------- unwritable

// The ordering leg. `d` is searchable but NOT writable by the caller, and
// holds a regular file called "exists-file" plus a mode-0644 subdirectory
// called "nosearch".
static int run_unwritable(const char *d) {
    char t[512];
    test_logf("unwritable parent %s (uid %d):\n", d, (int) geteuid());

    // A missing name with a trailing slash: the spelling answers, not the
    // permission. This is the whole reason the check sits before
    // N_PARENT_DIR_WRITE.
    snprintf(t, sizeof t, "%s/gone/", d);
    errno = 0; check("unwritable mknod(gone/)",   mknod(t, S_IFIFO | 0644, 0), ENOENT);
    errno = 0; check("unwritable mkfifo(gone/)",  mkfifo(t, 0644),             ENOENT);
    errno = 0; check("unwritable symlink(gone/)", symlink("target", t),        ENOENT);
    errno = 0; check("unwritable link(gone/)",    link(src, t),                ENOENT);
    errno = 0; check("unwritable open(gone/, O_CREAT)",
                     open_rc(t, O_CREAT | O_WRONLY, 0644),                     EISDIR);
    // mkdir is asking for a directory, so the slash disqualifies nothing and
    // the permission is what refuses it.
    errno = 0; check("unwritable mkdir(gone/)",   mkdir(t, 0755),              EACCES);

    // Without the slash, every one of them is the permission error.
    snprintf(t, sizeof t, "%s/gone", d);
    errno = 0; check("unwritable mknod(gone)",   mknod(t, S_IFIFO | 0644, 0),  EACCES);
    errno = 0; check("unwritable mkfifo(gone)",  mkfifo(t, 0644),              EACCES);
    errno = 0; check("unwritable symlink(gone)", symlink("target", t),         EACCES);
    errno = 0; check("unwritable link(gone)",    link(src, t),                 EACCES);
    errno = 0; check("unwritable open(gone, O_CREAT)",
                     open_rc(t, O_CREAT | O_WRONLY, 0644),                     EACCES);
    errno = 0; check("unwritable mkdir(gone)",   mkdir(t, 0755),               EACCES);

    // An existing name reports EEXIST, still ahead of the permission.
    snprintf(t, sizeof t, "%s/exists-file/", d);
    errno = 0; check("unwritable mknod(exists-file/)",   mknod(t, S_IFIFO | 0644, 0), EEXIST);
    errno = 0; check("unwritable mkfifo(exists-file/)",  mkfifo(t, 0644),             EEXIST);
    errno = 0; check("unwritable symlink(exists-file/)", symlink("target", t),        EEXIST);
    errno = 0; check("unwritable link(exists-file/)",    link(src, t),                EEXIST);
    errno = 0; check("unwritable mkdir(exists-file/)",   mkdir(t, 0755),              EEXIST);
    errno = 0; check("unwritable open(exists-file/, O_CREAT)",
                     open_rc(t, O_CREAT | O_WRONLY, 0644),                            EISDIR);

    // ...and a parent with no search permission still answers before any of
    // it, because that error comes out of the walk itself.
    snprintf(t, sizeof t, "%s/nosearch/x/", d);
    errno = 0; check("nosearch mknod(x/)",  mknod(t, S_IFIFO | 0644, 0),       EACCES);
    errno = 0; check("nosearch symlink(x/)", symlink("target", t),             EACCES);
    errno = 0; check("nosearch open(x/, O_CREAT)",
                     open_rc(t, O_CREAT | O_WRONLY, 0644),                     EACCES);
    errno = 0; check("nosearch mkdir(x/)",  mkdir(t, 0755),                    EACCES);

    return failures_total == 0 ? 0 : 1;
}

// --------------------------------------------------------------------- main

static int make_fixtures(const char *d, mode_t mode) {
    char t[512];
    if (mkdir(d, 0755) < 0)
        return -1;
    snprintf(t, sizeof t, "%s/exists-file", d);
    int fd = open(t, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0)
        close(fd);
    snprintf(t, sizeof t, "%s/exists-dir", d);
    mkdir(t, 0755);
    snprintf(t, sizeof t, "%s/exists-dangling", d);
    symlink("nowhere-at-all", t);
    snprintf(t, sizeof t, "%s/nosearch", d);
    mkdir(t, 0644);            // readable and writable, but not searchable
    return chmod(d, mode);
}

static void drop_fixtures(const char *d) {
    char t[512];
    chmod(d, 0755);
    snprintf(t, sizeof t, "%s/exists-file", d);     unlink(t);
    snprintf(t, sizeof t, "%s/exists-dir", d);      rmdir(t);
    snprintf(t, sizeof t, "%s/exists-dangling", d); unlink(t);
    snprintf(t, sizeof t, "%s/nosearch", d);        chmod(t, 0755); rmdir(t);
    snprintf(t, sizeof t, "%s/gone", d);            unlink(t); rmdir(t);
    rmdir(d);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    char base[192];
    snprintf(base, sizeof base, "/tmp/create_tslash.%d", (int) getpid());
    snprintf(src, sizeof src, "%s.src", base);
    // 0666 so it stays a legal hardlink source for an unprivileged caller:
    // Linux's protected_hardlinks refuses a link to a file the caller can
    // neither read nor write, and that EPERM would answer before the errors
    // this test is about.
    int fd = open(src, O_CREAT | O_WRONLY, 0666);
    if (fd < 0) {
        printf("fs_create_trailing_slash: SKIP (cannot create %s: %s)\n", src, strerror(errno));
        return 0;
    }
    close(fd);
    chmod(src, 0666);

    snprintf(dir, sizeof dir, "%s.w", base);
    if (make_fixtures(dir, 0755) < 0) {
        printf("fs_create_trailing_slash: SKIP (cannot set up %s: %s)\n", dir, strerror(errno));
        unlink(src);
        return 0;
    }
    run_writable();
    drop_fixtures(dir);

    // The ordering leg needs a caller that the parent's mode bits actually
    // bind. Root is not one -- it bypasses the write check entirely, and the
    // ENOENT-before-EACCES question is invisible to it -- so drop to an
    // unprivileged uid in a child. An already-unprivileged caller can just
    // clear its own write bit instead, which is how this runs on the Linux
    // oracle.
    char udir[256];
    snprintf(udir, sizeof udir, "%s.r", base);
    int rc = 0;
    if (geteuid() == 0) {
        if (make_fixtures(udir, 0755) < 0) {
            printf("fs_create_trailing_slash: SKIP unwritable leg (cannot set up %s)\n", udir);
        } else {
            fflush(NULL);
            pid_t c = fork();
            if (c == 0) {
                if (setgid(1000) < 0 || setuid(1000) < 0)
                    _exit(70);
                int crc = run_unwritable(udir);
                fflush(NULL);   // _exit skips the flush; the diagnosis lives here
                _exit(crc);
            }
            int st = 0;
            while (waitpid(c, &st, 0) < 0 && errno == EINTR)
                continue;
            int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            if (code == 70)
                printf("fs_create_trailing_slash: SKIP unwritable leg (cannot drop privileges)\n");
            else if (code != 0)
                failf("unprivileged create through a trailing slash",
                      (uint64_t) code, (uint64_t) st, 0, 0, 0, 0);
            drop_fixtures(udir);
        }
    } else {
        if (make_fixtures(udir, 0555) < 0) {
            printf("fs_create_trailing_slash: SKIP unwritable leg (cannot set up %s)\n", udir);
        } else {
            rc = run_unwritable(udir);
            (void) rc;
            drop_fixtures(udir);
        }
    }

    unlink(src);
    return finish_suite("fs_create_trailing_slash");
}
