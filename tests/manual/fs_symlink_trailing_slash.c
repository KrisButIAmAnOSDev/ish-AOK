// A trailing slash on a name whose FINAL component is a symlink.
//
// "link/" says two things: follow link, and what you land on is a directory.
// Linux carries both across the link -- link_path_walk() turns the trailing
// slash into LOOKUP_FOLLOW | LOOKUP_DIRECTORY on the last component, and the
// flag is spent on whatever the link resolves to, not on the link. So
// open("link-to-a-file/") is ENOTDIR and open("dangling-link/") is ENOENT,
// where the same two names without the slash open the file and report ENOENT.
//
// AOK dropped the slash while expanding the link: __path_normalize() rebuilds
// the path as <target><rest>, and "rest" is empty for a FINAL component, so
// the recursion resolved a path that had never been spelled with a slash at
// all. Both of the above opened the target, and a caller had no way to insist
// on a directory.
//
// The three kinds of target are three different answers, and they are NOT
// interchangeable -- Linux reaches them by different routes, so a fix that
// produces one of them everywhere is still wrong:
//
//   link -> a directory   the slash is satisfied; everything works as if the
//                         link were the directory
//   link -> a regular file ENOTDIR, from the must-be-a-directory check on
//                         what the link landed on
//   link -> nothing       ENOENT, because the name the link names is not there
//
// The un-slashed spellings are the controls: every one of them must keep
// working exactly as before. So are the same calls on names that are not
// symlinks at all ("file/", "gone/"), which already answered correctly and
// say that the symlink is what was broken.
//
// NOT asserted here, deliberately: lstat("link/"), readlink("link/") and
// open("link/", O_NOFOLLOW). Linux answers those ENOTDIR/ENOENT too, by the
// same LOOKUP_FOLLOW rule, but they reach path resolution as
// N_SYMLINK_NOFOLLOW and AOK skips the whole final-component block for those
// -- a separate divergence with a separate cause (it applies to "file/" as
// much as to "link/"), left for its own change. Measured on the oracle for
// whoever writes it: lstat("link-to-file/") is ENOTDIR and
// lstat("link-to-dir/") stats the DIRECTORY (mode 040755, not 120777),
// readlink("link-to-dir/") is EINVAL, and open("link-to-dir/", O_NOFOLLOW)
// succeeds.
//
// Every expectation below was measured on the Linux 6.12 oracle (Devuan 6,
// x86_64 glibc), 64-bit and -m32, both identical.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "test_common.h"

static char dir[256];        // scratch directory holding every fixture

static void check(const char *label, int r, int want_errno) {
    int e = r < 0 ? errno : 0;
    if (!(r < 0 && e == want_errno))
        failf(label, (uint64_t) r, (uint64_t) e, 0, (uint64_t) -1, (uint64_t) want_errno, 0);
    test_logf("  %-50s rc=%d errno=%d (want %d)\n", label, r, e, want_errno);
}

static void check_ok(const char *label, int r) {
    if (r < 0)
        failf(label, (uint64_t) r, (uint64_t) errno, 0, 0, 0, 0);
    test_logf("  %-50s rc=%d (want 0)\n", label, r);
}

static int open_rc(const char *path, int flags, int mode) {
    errno = 0;
    int fd = open(path, flags, mode);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

static int opendir_rc(const char *path) {
    errno = 0;
    DIR *d = opendir(path);
    if (d == NULL)
        return -1;
    closedir(d);
    return 0;
}

// execve in a child, reporting the child's errno through a pipe. A successful
// exec is a success; anything else is the errno the exec failed with.
static int execve_rc(const char *path, const char *argv0) {
    int pfd[2];
    if (pipe(pfd) < 0)
        return -1;
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        close(pfd[0]);
        // argv[0] is the program's own name rather than the path we are
        // testing: busybox dispatches on argv[0], and handing it the scratch
        // path made a SUCCESSFUL exec print "applet not found" -- noise from
        // the control, in the one case where the exec is supposed to work.
        char *av[] = {(char *) argv0, NULL};
        char *ev[] = {NULL};
        execve(path, av, ev);
        int e = errno;
        if (write(pfd[1], &e, sizeof e) != (ssize_t) sizeof e) {}
        _exit(1);
    }
    close(pfd[1]);
    int e = 0;
    ssize_t n = read(pfd[0], &e, sizeof e);
    close(pfd[0]);
    int st = 0;
    while (waitpid(c, &st, 0) < 0 && errno == EINTR)
        continue;
    if (n == (ssize_t) sizeof e) {
        errno = e;
        return -1;
    }
    return 0;
}

static char *at(char *buf, size_t n, const char *name) {
    snprintf(buf, n, "%s/%s", dir, name);
    return buf;
}

#define CASE(label, call, want) do { errno = 0; check((label), (call), (want)); } while (0)
#define OKAY(label, call)       do { errno = 0; check_ok((label), (call)); } while (0)

// --------------------------------------------------- link -> a regular file

// Everything that resolves the final component must refuse it: the link lands
// on a file, and a file is not the directory the slash asked for.
static void link_to_file(const char *name) {
    char p[400], q[512], label[160];
    at(p, sizeof p, name);
    snprintf(q, sizeof q, "%s/", p);
    struct stat st;

#define ROW(op, call, want)                                                    \
    do {                                                                       \
        snprintf(label, sizeof label, "%s(%s/)", op, name);                    \
        errno = 0;                                                             \
        check(label, (call), (want));                                          \
    } while (0)

    ROW("stat",     stat(q, &st),                        ENOTDIR);
    ROW("open",     open_rc(q, O_RDONLY, 0),             ENOTDIR);
    ROW("open_wr",  open_rc(q, O_WRONLY, 0),             ENOTDIR);
    ROW("opendir",  opendir_rc(q),                       ENOTDIR);
    ROW("access",   access(q, F_OK),                     ENOTDIR);
    ROW("chmod",    chmod(q, 0644),                      ENOTDIR);
    ROW("chdir",    chdir(q),                            ENOTDIR);
    ROW("truncate", truncate(q, 0),                      ENOTDIR);
    // Two slashes are one slash, and a "." after it changes nothing.
    {
        char qq[512];
        snprintf(qq, sizeof qq, "%s//", p);
        ROW("stat//", stat(qq, &st),                     ENOTDIR);
        snprintf(qq, sizeof qq, "%s/.", p);
        ROW("stat/.", stat(qq, &st),                     ENOTDIR);
        snprintf(qq, sizeof qq, "%s/x", p);
        ROW("stat/x", stat(qq, &st),                     ENOTDIR);
    }
    // O_CREAT is EISDIR instead, decided before any of this from the spelling
    // alone (fs_create_trailing_slash.c). Pinned here because it is the one
    // answer the slash gives that does NOT depend on what the link points at.
    ROW("open_creat", open_rc(q, O_CREAT | O_WRONLY, 0644), EISDIR);
    ROW("open_creat_rd", open_rc(q, O_CREAT | O_RDONLY, 0644), EISDIR);
#undef ROW

    // ...and without the slash it is an ordinary file, as it always was.
    snprintf(label, sizeof label, "stat(%s)", name);
    errno = 0;
    if (stat(p, &st) < 0 || !S_ISREG(st.st_mode))
        failf(label, (uint64_t) st.st_mode, (uint64_t) errno, 0, 0, 0, 0);
    snprintf(label, sizeof label, "open(%s, O_RDONLY)", name);
    OKAY(label, open_rc(p, O_RDONLY, 0));
    snprintf(label, sizeof label, "lstat(%s) is still a symlink", name);
    errno = 0;
    if (lstat(p, &st) < 0 || !S_ISLNK(st.st_mode))
        failf(label, (uint64_t) st.st_mode, (uint64_t) errno, 0, 0, 0, 0);
}

// ----------------------------------------------------- link -> a directory

// The case everything actually relies on: the slash is satisfied and the link
// behaves as the directory it names.
static void link_to_dir(const char *name) {
    char p[400], q[512], label[160];
    at(p, sizeof p, name);
    snprintf(q, sizeof q, "%s/", p);
    struct stat st;

    snprintf(label, sizeof label, "stat(%s/) is the directory", name);
    errno = 0;
    if (stat(q, &st) < 0 || !S_ISDIR(st.st_mode))
        failf(label, (uint64_t) st.st_mode, (uint64_t) errno, 0, 0, 0, 0);
    test_logf("  %-50s mode=%06o\n", label, (unsigned) st.st_mode);

    snprintf(label, sizeof label, "open(%s/, O_RDONLY)", name);
    OKAY(label, open_rc(q, O_RDONLY, 0));
    snprintf(label, sizeof label, "opendir(%s/)", name);
    OKAY(label, opendir_rc(q));
    snprintf(label, sizeof label, "access(%s/, F_OK)", name);
    OKAY(label, access(q, F_OK));
    snprintf(label, sizeof label, "chmod(%s/, 0755)", name);
    OKAY(label, chmod(q, 0755));
    // A directory refuses a write-mode open and a truncate, as any directory
    // does -- the point is that it got that far.
    snprintf(label, sizeof label, "open(%s/, O_WRONLY)", name);
    CASE(label, open_rc(q, O_WRONLY, 0), EISDIR);
    snprintf(label, sizeof label, "truncate(%s/)", name);
    CASE(label, truncate(q, 0), EISDIR);
    // Something INSIDE it, reached through the slashed link.
    {
        char inner[512];
        snprintf(inner, sizeof inner, "%s/inner", p);
        snprintf(label, sizeof label, "stat(%s/inner)", name);
        errno = 0;
        if (stat(inner, &st) < 0 || !S_ISREG(st.st_mode))
            failf(label, (uint64_t) st.st_mode, (uint64_t) errno, 0, 0, 0, 0);
        snprintf(inner, sizeof inner, "%s//inner", p);
        snprintf(label, sizeof label, "stat(%s//inner)", name);
        errno = 0;
        check_ok(label, stat(inner, &st));
    }
    // ...and a name that is not in it is still just missing.
    {
        char miss[512];
        snprintf(miss, sizeof miss, "%s/no-such-entry", p);
        snprintf(label, sizeof label, "stat(%s/no-such-entry)", name);
        CASE(label, stat(miss, &st), ENOENT);
    }
    // chdir last: it moves us, and the caller puts us back.
    snprintf(label, sizeof label, "chdir(%s/)", name);
    OKAY(label, chdir(q));
}

// ------------------------------------------------------- link -> nothing

// A dangling link is ENOENT, not ENOTDIR: there is no target to fail the
// must-be-a-directory test, the target simply is not there. Getting this one
// wrong is invisible until something tries to tell the two apart.
static void link_to_nothing(const char *name, const char *target_name) {
    char p[400], q[512], label[160];
    at(p, sizeof p, name);
    snprintf(q, sizeof q, "%s/", p);
    struct stat st;

#define ROW(op, call, want)                                                    \
    do {                                                                       \
        snprintf(label, sizeof label, "%s(%s/)", op, name);                    \
        errno = 0;                                                             \
        check(label, (call), (want));                                          \
    } while (0)

    ROW("stat",     stat(q, &st),                        ENOENT);
    ROW("open",     open_rc(q, O_RDONLY, 0),             ENOENT);
    ROW("open_wr",  open_rc(q, O_WRONLY, 0),             ENOENT);
    ROW("opendir",  opendir_rc(q),                       ENOENT);
    ROW("access",   access(q, F_OK),                     ENOENT);
    ROW("chmod",    chmod(q, 0644),                      ENOENT);
    ROW("chdir",    chdir(q),                            ENOENT);
    ROW("truncate", truncate(q, 0),                      ENOENT);
    // O_CREAT cannot rescue it either, and for a different reason: the slash
    // is EISDIR before the name is looked up at all.
    ROW("open_creat", open_rc(q, O_CREAT | O_WRONLY, 0644), EISDIR);
#undef ROW

    // Nothing above may have created the target the link names.
    {
        char t[512];
        at(t, sizeof t, target_name);
        if (lstat(t, &st) == 0) {
            failf("a refused open through a dangling link created its target",
                  (uint64_t) st.st_mode, 0, 0, 0, 0, 0);
            unlink(t);
        }
    }
    // Un-slashed, the same name is the ordinary dangling-link behaviour.
    snprintf(label, sizeof label, "stat(%s)", name);
    CASE(label, stat(p, &st), ENOENT);
    snprintf(label, sizeof label, "lstat(%s)", name);
    errno = 0;
    if (lstat(p, &st) < 0 || !S_ISLNK(st.st_mode))
        failf(label, (uint64_t) st.st_mode, (uint64_t) errno, 0, 0, 0, 0);
    snprintf(label, sizeof label, "readlink(%s)", name);
    {
        char buf[256];
        errno = 0;
        check_ok(label, (int) readlink(p, buf, sizeof buf));
    }
}

// ---------------------------------------------------------------- controls

// Names that are not symlinks. Every one of these already answered correctly,
// and says that the symlink expansion is what lost the slash.
static void non_symlink_controls(void) {
    char p[512];
    struct stat st;

    CASE("stat(file/)",   stat(at(p, sizeof p, "file/"), &st),        ENOTDIR);
    CASE("open(file/)",   open_rc(at(p, sizeof p, "file/"), O_RDONLY, 0), ENOTDIR);
    CASE("stat(gone/)",   stat(at(p, sizeof p, "gone/"), &st),        ENOENT);
    CASE("open(gone/)",   open_rc(at(p, sizeof p, "gone/"), O_RDONLY, 0), ENOENT);
    OKAY("stat(adir/)",   stat(at(p, sizeof p, "adir/"), &st));
    OKAY("opendir(adir/)", opendir_rc(at(p, sizeof p, "adir/")));
    // A symlink in a NON-final position always kept its slash, because
    // something followed it.
    OKAY("stat(symdir/inner)", stat(at(p, sizeof p, "symdir/inner"), &st));
    CASE("stat(symfile/inner)", stat(at(p, sizeof p, "symfile/inner"), &st), ENOTDIR);
}

// execve resolves its path the same way, so the same rule reaches it.
static void exec_through_a_slash(void) {
    static const char *candidates[] = {"/bin/true", "/usr/bin/true", NULL};
    const char *prog = NULL;
    for (unsigned i = 0; candidates[i] != NULL; i++)
        if (access(candidates[i], X_OK) == 0) {
            prog = candidates[i];
            break;
        }
    if (prog == NULL) {
        printf("fs_symlink_trailing_slash: SKIP exec leg (no true(1) to link to)\n");
        return;
    }
    char p[400], q[512];
    at(p, sizeof p, "symexec");
    unlink(p);
    if (symlink(prog, p) < 0) {
        printf("fs_symlink_trailing_slash: SKIP exec leg (cannot link to %s)\n", prog);
        return;
    }
    snprintf(q, sizeof q, "%s/", p);
    const char *base = strrchr(prog, '/');
    base = base != NULL ? base + 1 : prog;
    OKAY("execve(symlink-to-a-program)",  execve_rc(p, base));
    CASE("execve(symlink-to-a-program/)", execve_rc(q, base), ENOTDIR);
    unlink(p);
}

// --------------------------------------------------------------------- main

static int make_fixtures(void) {
    char t[512];
    if (mkdir(dir, 0755) < 0)
        return -1;
    if (mkdir(at(t, sizeof t, "adir"), 0755) < 0)
        return -1;
    // "inner" exists in the scratch directory AND in adir, because symdot
    // points at the scratch directory itself and symdir at adir: both slashed
    // links must be able to reach a name inside what they resolve to.
    {
        char inner[512];
        snprintf(inner, sizeof inner, "%s/adir/inner", dir);
        int fd = open(inner, O_CREAT | O_WRONLY, 0644);
        if (fd < 0)
            return -1;
        close(fd);
        snprintf(inner, sizeof inner, "%s/inner", dir);
        fd = open(inner, O_CREAT | O_WRONLY, 0644);
        if (fd < 0)
            return -1;
        close(fd);
    }
    int fd = open(at(t, sizeof t, "file"), O_CREAT | O_WRONLY, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    if (symlink("adir", at(t, sizeof t, "symdir")) < 0)
        return -1;
    if (symlink("file", at(t, sizeof t, "symfile")) < 0)
        return -1;
    if (symlink("ghost", at(t, sizeof t, "symgone")) < 0)
        return -1;
    if (symlink("symfile", at(t, sizeof t, "symchain")) < 0)
        return -1;
    {
        char abs[512], link[512];
        snprintf(abs, sizeof abs, "%s/file", dir);
        if (symlink(abs, at(link, sizeof link, "symabs")) < 0)
            return -1;
    }
    if (symlink(".", at(t, sizeof t, "symdot")) < 0)
        return -1;
    return 0;
}

static void drop_fixtures(void) {
    char t[512];
    static const char *names[] = {"symdir", "symfile", "symgone", "symchain",
                                  "symabs", "symdot", "symexec", "file", "ghost",
                                  "inner", NULL};
    for (unsigned i = 0; names[i] != NULL; i++)
        unlink(at(t, sizeof t, names[i]));
    snprintf(t, sizeof t, "%s/adir/inner", dir);
    unlink(t);
    rmdir(at(t, sizeof t, "adir"));
    rmdir(dir);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    snprintf(dir, sizeof dir, "/tmp/sym_tslash.%d", (int) getpid());
    if (make_fixtures() < 0) {
        printf("fs_symlink_trailing_slash: SKIP (cannot set up %s: %s)\n",
               dir, strerror(errno));
        drop_fixtures();
        return 0;
    }
    char cwd[512];
    if (getcwd(cwd, sizeof cwd) == NULL)
        strcpy(cwd, "/");

    link_to_file("symfile");
    link_to_file("symchain");    // a chain: every level carries the slash
    link_to_file("symabs");      // an absolute target, re-anchored on the way
    link_to_nothing("symgone", "ghost");
    link_to_dir("symdir");
    if (chdir(cwd) < 0) {}
    link_to_dir("symdot");       // -> ".", the scratch directory itself
    if (chdir(cwd) < 0) {}
    non_symlink_controls();
    exec_through_a_slash();

    drop_fixtures();
    return finish_suite("fs_symlink_trailing_slash");
}
