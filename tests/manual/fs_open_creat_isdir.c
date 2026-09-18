// open(O_CREAT) on a name that is already a DIRECTORY.
//
// open() cannot create a directory, so asking it to is a request that can
// never be granted, and Linux refuses it in one line of do_open():
//
//     if (open_flag & O_CREAT) {
//         if ((open_flag & O_EXCL) && !(file->f_mode & FMODE_CREATED))
//             return -EEXIST;
//         if (d_is_dir(nd->path.dentry))
//             return -EISDIR;
//
// -- with no regard for the access mode. AOK refused only the write-mode form,
// and it refused it much later, from the S_ISDIR check after the open: so
// `open("dir", O_CREAT|O_RDONLY, 0644)` succeeded and handed back a working
// fd on the directory, where every Linux says EISDIR.
//
// Three orderings are asserted alongside the errno, because that one line sits
// in the middle of a sequence and EISDIR is the right answer only where it is:
//
//   - O_EXCL answers FIRST. open("dir", O_CREAT|O_EXCL|O_RDONLY) is EEXIST,
//     not EISDIR.
//   - the target's own permissions answer LAST. Measured on a directory the
//     caller cannot read, open(O_CREAT|O_RDONLY) is EISDIR where the same open
//     without O_CREAT is EACCES -- which needs an unprivileged caller to mean
//     anything, and is what the second leg is for.
//   - O_PATH is exempt, because it ignores O_CREAT altogether: measured,
//     open("dir", O_CREAT|O_PATH) and open("dir/", O_CREAT|O_PATH) both
//     succeed.
//
// This is about what the name HOLDS. The neighbouring rule about how the name
// is SPELLED -- a trailing slash is EISDIR whatever it holds and even if it
// holds nothing, fs_create_trailing_slash.c -- is decided earlier, inside path
// resolution, and the two are pinned here together because their ORDERS
// differ: open("dir", O_CREAT|O_EXCL) is EEXIST while open("dir/",
// O_CREAT|O_EXCL) is EISDIR.
//
// Every expectation below was measured on the Linux 6.12 oracle (Devuan 6,
// x86_64 glibc), 64-bit and -m32, both identical.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "test_common.h"

#ifndef O_PATH
#define O_PATH 010000000
#endif

static char dir[256];        // writable scratch directory

static int open_rc(const char *path, int flags, int mode) {
    errno = 0;
    int fd = open(path, flags, mode);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

static void check(const char *label, int r, int want_errno) {
    int e = r < 0 ? errno : 0;
    if (!(r < 0 && e == want_errno))
        failf(label, (uint64_t) r, (uint64_t) e, 0, (uint64_t) -1, (uint64_t) want_errno, 0);
    test_logf("  %-54s rc=%d errno=%d (want %d)\n", label, r, e, want_errno);
}

static void check_ok(const char *label, int r) {
    if (r < 0)
        failf(label, (uint64_t) r, (uint64_t) errno, 0, 0, 0, 0);
    test_logf("  %-54s rc=%d (want 0)\n", label, r);
}

// A refused open must not have created anything, which for a directory target
// means the directory is still there and still a directory.
static void check_still_a_dir(const char *label, const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0 || !S_ISDIR(st.st_mode))
        failf(label, (uint64_t) (lstat(path, &st) == 0 ? st.st_mode : 0), (uint64_t) errno,
              0, 0, 0, 0);
}

#define CASE(label, call, want) do { errno = 0; check((label), (call), (want)); } while (0)
#define OKAY(label, call)       do { errno = 0; check_ok((label), (call)); } while (0)

// The target is a directory: every O_CREAT open of it is EISDIR, whatever the
// access mode, and whether the name reaches it directly or through a symlink.
static void directory_targets(void) {
    char d[512], sym[512];
    snprintf(d, sizeof d, "%s/adir", dir);
    snprintf(sym, sizeof sym, "%s/symdir", dir);

    CASE("open(dir, O_CREAT|O_RDONLY)",       open_rc(d, O_CREAT | O_RDONLY, 0644), EISDIR);
    CASE("open(dir, O_CREAT|O_WRONLY)",       open_rc(d, O_CREAT | O_WRONLY, 0644), EISDIR);
    CASE("open(dir, O_CREAT|O_RDWR)",         open_rc(d, O_CREAT | O_RDWR, 0644),   EISDIR);
    CASE("open(dir, O_CREAT|O_WRONLY|O_TRUNC)",
         open_rc(d, O_CREAT | O_WRONLY | O_TRUNC, 0644),                            EISDIR);
    CASE("open(dir, O_CREAT|O_RDONLY|O_NOFOLLOW)",
         open_rc(d, O_CREAT | O_RDONLY | O_NOFOLLOW, 0644),                         EISDIR);
    check_still_a_dir("open(O_CREAT) must not disturb the directory", d);

    // O_EXCL is answered one line earlier.
    CASE("open(dir, O_CREAT|O_EXCL|O_RDONLY)",
         open_rc(d, O_CREAT | O_EXCL | O_RDONLY, 0644),                             EEXIST);
    CASE("open(dir, O_CREAT|O_EXCL|O_WRONLY)",
         open_rc(d, O_CREAT | O_EXCL | O_WRONLY, 0644),                             EEXIST);

    // Reached through a symlink, the rule follows the link: what matters is
    // what the resolution LANDS on. Except under O_NOFOLLOW, where it lands on
    // the symlink instead and that is ELOOP.
    CASE("open(symdir, O_CREAT|O_RDONLY)",    open_rc(sym, O_CREAT | O_RDONLY, 0644), EISDIR);
    CASE("open(symdir, O_CREAT|O_WRONLY)",    open_rc(sym, O_CREAT | O_WRONLY, 0644), EISDIR);
    CASE("open(symdir, O_CREAT|O_EXCL|O_RDONLY)",
         open_rc(sym, O_CREAT | O_EXCL | O_RDONLY, 0644),                             EEXIST);
    CASE("open(symdir, O_CREAT|O_RDONLY|O_NOFOLLOW)",
         open_rc(sym, O_CREAT | O_RDONLY | O_NOFOLLOW, 0644),                         ELOOP);

    // "." and ".." name a directory as surely as any other spelling does, and
    // so does the root.
    CASE("open(., O_CREAT|O_RDONLY)",   open_rc(".", O_CREAT | O_RDONLY, 0644),  EISDIR);
    CASE("open(.., O_CREAT|O_RDONLY)",  open_rc("..", O_CREAT | O_RDONLY, 0644), EISDIR);
    CASE("open(/, O_CREAT|O_RDONLY)",   open_rc("/", O_CREAT | O_RDONLY, 0644),  EISDIR);
    CASE("open(/, O_CREAT|O_WRONLY)",   open_rc("/", O_CREAT | O_WRONLY, 0644),  EISDIR);
    CASE("open(/, O_CREAT|O_EXCL|O_RDONLY)",
         open_rc("/", O_CREAT | O_EXCL | O_RDONLY, 0644),                        EEXIST);

    // O_PATH ignores O_CREAT: Linux keeps only O_CLOEXEC, O_DIRECTORY and
    // O_NOFOLLOW with it, so neither this rule nor the trailing-slash one
    // applies.
    OKAY("open(dir, O_CREAT|O_PATH)",  open_rc(d, O_CREAT | O_RDONLY | O_PATH, 0644));
    {
        char slashed[520];
        snprintf(slashed, sizeof slashed, "%s/", d);
        OKAY("open(dir/, O_CREAT|O_PATH)", open_rc(slashed, O_CREAT | O_RDONLY | O_PATH, 0644));
    }
}

// The controls. None of this may change: the rule is about directories, and
// O_CREAT on everything else still means what it always did.
static void non_directory_targets(void) {
    char f[512], symf[512], gone[512];
    snprintf(f, sizeof f, "%s/afile", dir);
    snprintf(symf, sizeof symf, "%s/symfile", dir);
    snprintf(gone, sizeof gone, "%s/not-here", dir);

    OKAY("open(file, O_CREAT|O_RDONLY)",  open_rc(f, O_CREAT | O_RDONLY, 0644));
    OKAY("open(file, O_CREAT|O_WRONLY)",  open_rc(f, O_CREAT | O_WRONLY, 0644));
    OKAY("open(file, O_CREAT|O_WRONLY|O_TRUNC)",
         open_rc(f, O_CREAT | O_WRONLY | O_TRUNC, 0644));
    CASE("open(file, O_CREAT|O_EXCL|O_WRONLY)",
         open_rc(f, O_CREAT | O_EXCL | O_WRONLY, 0644),                          EEXIST);
    OKAY("open(symlink-to-file, O_CREAT|O_WRONLY)",
         open_rc(symf, O_CREAT | O_WRONLY, 0644));
    CASE("open(symlink-to-file, O_CREAT|O_EXCL|O_WRONLY)",
         open_rc(symf, O_CREAT | O_EXCL | O_WRONLY, 0644),                       EEXIST);

    // Creating a new name is still exactly that.
    OKAY("open(missing, O_CREAT|O_WRONLY)", open_rc(gone, O_CREAT | O_WRONLY, 0644));
    {
        struct stat st;
        if (lstat(gone, &st) < 0 || !S_ISREG(st.st_mode))
            failf("open(O_CREAT) must still create a regular file", 0, 0, 0, 0, 0, 0);
    }
    OKAY("open(created-name, O_CREAT|O_RDONLY)", open_rc(gone, O_CREAT | O_RDONLY, 0644));
    unlink(gone);

    // Opening a directory WITHOUT O_CREAT is the ordinary opendir and must
    // keep working -- the whole point of the bug is that it looked like this.
    {
        char d[512];
        snprintf(d, sizeof d, "%s/adir", dir);
        OKAY("open(dir, O_RDONLY)",              open_rc(d, O_RDONLY, 0));
        OKAY("open(dir, O_RDONLY|O_DIRECTORY)",  open_rc(d, O_RDONLY | O_DIRECTORY, 0));
        CASE("open(dir, O_WRONLY)",              open_rc(d, O_WRONLY, 0),        EISDIR);
    }
}

// How the name is SPELLED, decided before the name is looked up at all. Here
// only to pin that the two rules do not collide: the answers agree except
// under O_EXCL, where they disagree because their orders do.
static void spelling_rule_still_holds(void) {
    char d[512], f[512], gone[512];
    snprintf(d, sizeof d, "%s/adir/", dir);
    snprintf(f, sizeof f, "%s/afile/", dir);
    snprintf(gone, sizeof gone, "%s/not-here/", dir);

    CASE("open(dir/, O_CREAT|O_RDONLY)",  open_rc(d, O_CREAT | O_RDONLY, 0644),    EISDIR);
    CASE("open(file/, O_CREAT|O_WRONLY)", open_rc(f, O_CREAT | O_WRONLY, 0644),    EISDIR);
    CASE("open(missing/, O_CREAT|O_WRONLY)",
         open_rc(gone, O_CREAT | O_WRONLY, 0644),                                  EISDIR);
    // ...and the orders differ: with a slash the spelling answers before the
    // lookup, so O_EXCL never gets to say EEXIST.
    CASE("open(dir/, O_CREAT|O_EXCL|O_RDONLY)",
         open_rc(d, O_CREAT | O_EXCL | O_RDONLY, 0644),                            EISDIR);
}

// ------------------------------------------------------------ unprivileged

// `d` holds a directory the caller cannot read ("noread", 0311) and one it
// cannot write ("nowrite", 0555) which itself holds a directory. Root is not
// bound by either, so this leg only means something unprivileged.
static int run_unprivileged(const char *d) {
    char p[512];
    test_logf("unprivileged leg in %s (uid %d):\n", d, (int) geteuid());

    // The whole ordering claim in one pair: EISDIR is decided before the
    // target's own permissions are consulted.
    snprintf(p, sizeof p, "%s/noread", d);
    CASE("open(unreadable-dir, O_CREAT|O_RDONLY)", open_rc(p, O_CREAT | O_RDONLY, 0644), EISDIR);
    CASE("open(unreadable-dir, O_RDONLY)",         open_rc(p, O_RDONLY, 0),              EACCES);
    CASE("open(unreadable-dir, O_CREAT|O_EXCL|O_RDONLY)",
         open_rc(p, O_CREAT | O_EXCL | O_RDONLY, 0644),                                  EEXIST);

    // An existing directory inside a parent the caller cannot write: still
    // EISDIR, because nothing is being created and the parent is never asked.
    snprintf(p, sizeof p, "%s/nowrite/inner", d);
    CASE("open(dir-in-unwritable-parent, O_CREAT|O_RDONLY)",
         open_rc(p, O_CREAT | O_RDONLY, 0644),                                           EISDIR);
    CASE("open(dir-in-unwritable-parent, O_CREAT|O_WRONLY)",
         open_rc(p, O_CREAT | O_WRONLY, 0644),                                           EISDIR);
    // ...while a name that is NOT there in the same parent is the permission
    // error, which is what says the deferral granted nothing.
    snprintf(p, sizeof p, "%s/nowrite/gone", d);
    CASE("open(missing-in-unwritable-parent, O_CREAT|O_WRONLY)",
         open_rc(p, O_CREAT | O_WRONLY, 0644),                                           EACCES);

    return failures_total == 0 ? 0 : 1;
}

// --------------------------------------------------------------------- main

static int make_fixtures(const char *d) {
    char t[512];
    if (mkdir(d, 0755) < 0)
        return -1;
    snprintf(t, sizeof t, "%s/adir", d);
    if (mkdir(t, 0755) < 0)
        return -1;
    snprintf(t, sizeof t, "%s/afile", d);
    int fd = open(t, O_CREAT | O_WRONLY, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    snprintf(t, sizeof t, "%s/symdir", d);
    symlink("adir", t);
    snprintf(t, sizeof t, "%s/symfile", d);
    symlink("afile", t);
    return 0;
}

static void drop_fixtures(const char *d) {
    char t[512];
    chmod(d, 0755);
    snprintf(t, sizeof t, "%s/noread", d);       chmod(t, 0755); rmdir(t);
    snprintf(t, sizeof t, "%s/nowrite", d);      chmod(t, 0755);
    snprintf(t, sizeof t, "%s/nowrite/inner", d); rmdir(t);
    snprintf(t, sizeof t, "%s/nowrite/gone", d);  unlink(t);
    snprintf(t, sizeof t, "%s/nowrite", d);      rmdir(t);
    snprintf(t, sizeof t, "%s/adir", d);         rmdir(t);
    snprintf(t, sizeof t, "%s/afile", d);        unlink(t);
    snprintf(t, sizeof t, "%s/symdir", d);       unlink(t);
    snprintf(t, sizeof t, "%s/symfile", d);      unlink(t);
    snprintf(t, sizeof t, "%s/not-here", d);     unlink(t);
    rmdir(d);
}

// The unprivileged leg's own fixtures: a directory with no read permission,
// and an unwritable directory holding one.
static int make_unpriv_fixtures(const char *d) {
    char t[512];
    if (make_fixtures(d) < 0)
        return -1;
    snprintf(t, sizeof t, "%s/noread", d);
    if (mkdir(t, 0755) < 0)
        return -1;
    snprintf(t, sizeof t, "%s/nowrite", d);
    if (mkdir(t, 0755) < 0)
        return -1;
    snprintf(t, sizeof t, "%s/nowrite/inner", d);
    if (mkdir(t, 0755) < 0)
        return -1;
    snprintf(t, sizeof t, "%s/noread", d);
    if (chmod(t, 0311) < 0)                  // searchable, not readable
        return -1;
    snprintf(t, sizeof t, "%s/nowrite", d);
    return chmod(t, 0555);                   // searchable, not writable
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    char base[192];
    snprintf(base, sizeof base, "/tmp/open_creat_isdir.%d", (int) getpid());
    snprintf(dir, sizeof dir, "%s.w", base);
    if (make_fixtures(dir) < 0) {
        printf("fs_open_creat_isdir: SKIP (cannot set up %s: %s)\n", dir, strerror(errno));
        return 0;
    }

    // "." and ".." are tested as spellings of a directory, so stand somewhere
    // known rather than wherever the runner left us.
    char cwd[512];
    if (getcwd(cwd, sizeof cwd) == NULL)
        strcpy(cwd, "/");
    if (chdir(dir) < 0) {
        printf("fs_open_creat_isdir: SKIP (cannot chdir to %s)\n", dir);
        drop_fixtures(dir);
        return 0;
    }
    directory_targets();
    non_directory_targets();
    spelling_rule_still_holds();
    if (chdir(cwd) < 0) {
        /* the fixtures below use absolute paths either way */
    }
    drop_fixtures(dir);

    // The ordering half needs a caller the mode bits actually bind. Root is
    // not one, so drop to an unprivileged uid in a child; an already
    // unprivileged caller just runs it, which is how this runs on the oracle.
    char udir[256];
    snprintf(udir, sizeof udir, "%s.u", base);
    if (make_unpriv_fixtures(udir) < 0) {
        printf("fs_open_creat_isdir: SKIP unprivileged leg (cannot set up %s: %s)\n",
               udir, strerror(errno));
    } else if (geteuid() == 0) {
        fflush(NULL);
        pid_t c = fork();
        if (c == 0) {
            if (setgid(1000) < 0 || setuid(1000) < 0)
                _exit(70);
            int crc = run_unprivileged(udir);
            fflush(NULL);       // _exit skips the flush; the diagnosis lives here
            _exit(crc);
        }
        int st = 0;
        while (waitpid(c, &st, 0) < 0 && errno == EINTR)
            continue;
        int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (code == 70)
            printf("fs_open_creat_isdir: SKIP unprivileged leg (cannot drop privileges)\n");
        else if (code != 0)
            failf("unprivileged open(O_CREAT) on a directory",
                  (uint64_t) code, (uint64_t) st, 0, 0, 0, 0);
        drop_fixtures(udir);
    } else {
        run_unprivileged(udir);
        drop_fixtures(udir);
    }

    return finish_suite("fs_open_creat_isdir");
}
