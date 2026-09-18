// chown(path, -1, -1) -- "change neither the owner nor the group".
//
// It is not a no-op on the path. Linux's chown_common() is reached only after
// the full lookup has already happened, so the call reports everything the
// resolution finds and only then discovers there is no uid and no gid to set.
// AOK ran the lookup from inside the two id blocks:
//
//     if (owner != (uid_t) -1) { ... generic_setattrat ... }
//     if (group != (uid_t) -1) { ... generic_setattrat ... }
//     return 0;
//
// so with both ids -1 neither block ran, nothing was ever resolved, and the
// call returned 0 for any path at all -- including one that does not exist.
// Measured on Linux 6.12 (camd, x86_64 glibc, 64-bit and -m32 identical,
// unprivileged and under sudo) against AOK at 562a4eb5:
//
//   lchown("gone", -1, -1)              ENOENT     AOK returned 0
//   chown("file/", -1, -1)              ENOTDIR    AOK returned 0
//   chown("dangling-link", -1, -1)      ENOENT     AOK returned 0
//   chown("symlink-loop", -1, -1)       ELOOP      AOK returned 0
//   chown("unsearchable/inner", -1, -1) EACCES     AOK returned 0
//   fchownat(file_fd, "rel", -1, -1, 0) ENOTDIR    AOK returned 0
//
// Two parts of the Linux behaviour were measured rather than assumed, and both
// came out against the guess:
//
//   - There is NO ownership check. An unprivileged chown(path, -1, -1) on a
//     root-owned file SUCCEEDS, because setattr_prepare() tests ATTR_UID and
//     ATTR_GID and chown_common() sets neither when both ids are -1. So this
//     is a lookup and nothing else; "every permission check" would have been
//     the wrong fix.
//   - It raises no inotify event, even though it does bump ctime:
//     fsnotify_change() maps ATTR_UID/GID/MODE to IN_ATTRIB and a lone
//     ATTR_CTIME to nothing. Asserted below by watching a real chown fire
//     IN_ATTRIB on the same file in the same watch.
//
// The trailing-slash cases are here because a lookup is exactly what spends a
// trailing slash (562a4eb5), and with no lookup there was nothing to spend it
// on -- but they are asserted against a real-id control on the same path, so a
// regression in the slash rules themselves fails both and is told apart from a
// regression in this one.
//
// Every expectation was measured on the oracle, 64-bit and -m32, and the whole
// file passes there unmodified as an ordinary unprivileged user.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/inotify.h>
#include <poll.h>
#endif

#include "test_common.h"

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

// A flag bit fchownat() does not define. Linux rejects it before any lookup,
// which is the one error that must still beat the resolution.
#define AT_NOT_A_FLAG 0x800

static char dir[192];     // scratch directory for the current leg

static char *at(char *buf, size_t n, const char *name) {
    snprintf(buf, n, "%s/%s", dir, name);
    return buf;
}

static void check(const char *label, int r, int want_errno) {
    int e = r < 0 ? errno : 0;
    if (want_errno == 0 ? r < 0 : !(r < 0 && e == want_errno))
        failf(label, (uint64_t) r, (uint64_t) e, 0,
              (uint64_t) (want_errno == 0 ? 0 : -1), (uint64_t) want_errno, 0);
    test_logf("  %-56s rc=%d errno=%d (want %d)\n", label, r, e, want_errno);
}

static int mkfile(const char *path, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return -1;
    write(fd, "x", 1);
    close(fd);
    return 0;
}

static void rm_rf(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' 2>/dev/null", path);
    if (system(cmd) < 0)
        return;
}

// ------------------------------------------------------------------ fixtures

// file       a regular file
// dir2       a directory
// l-file     -> file        l-dir    -> dir2
// l-dangle   -> nowhere     l-loop   -> l-loop
// nox/inner  a file under a parent with no search permission
static int fixtures(void) {
    char p[256], q[256];
    if (mkdir(dir, 0755) < 0)
        return -1;
    if (mkfile(at(p, sizeof p, "file"), 0644) < 0)
        return -1;
    if (mkdir(at(p, sizeof p, "dir2"), 0755) < 0)
        return -1;
    if (symlink("file", at(p, sizeof p, "l-file")) < 0)
        return -1;
    if (symlink("dir2", at(p, sizeof p, "l-dir")) < 0)
        return -1;
    if (symlink("nowhere", at(p, sizeof p, "l-dangle")) < 0)
        return -1;
    if (symlink("l-loop", at(p, sizeof p, "l-loop")) < 0)
        return -1;
    if (mkdir(at(p, sizeof p, "nox"), 0755) < 0)
        return -1;
    if (mkfile(at(q, sizeof q, "nox/inner"), 0644) < 0)
        return -1;
    return 0;
}

// ------------------------------------------------- the lookup must happen

// Everything here is a path error, and every one of them was reported as
// success before the fix. Each is paired with the same call carrying REAL ids,
// which already resolved: the pair is what says "the lookup is missing"
// rather than "the lookup is wrong".
static void lookup_errors(uid_t me, gid_t mg) {
    char p[256];

    test_logf("  -- a path that does not resolve, with nothing to change\n");
    check("chown(\"gone\", -1, -1)",
          chown(at(p, sizeof p, "gone"), (uid_t) -1, (gid_t) -1), ENOENT);
    check("chown(\"gone\", me, mg)          [control]",
          chown(at(p, sizeof p, "gone"), me, mg), ENOENT);
    check("lchown(\"gone\", -1, -1)",
          lchown(at(p, sizeof p, "gone"), (uid_t) -1, (gid_t) -1), ENOENT);
    check("fchownat(AT_FDCWD, \"gone\", -1, -1, 0)",
          fchownat(AT_FDCWD, at(p, sizeof p, "gone"), (uid_t) -1, (gid_t) -1, 0), ENOENT);
    check("fchownat(AT_FDCWD, \"gone\", -1, -1, NOFOLLOW)",
          fchownat(AT_FDCWD, at(p, sizeof p, "gone"), (uid_t) -1, (gid_t) -1,
                   AT_SYMLINK_NOFOLLOW), ENOENT);

    test_logf("  -- a component that is not a directory\n");
    check("chown(\"file/inner\", -1, -1)",
          chown(at(p, sizeof p, "file/inner"), (uid_t) -1, (gid_t) -1), ENOTDIR);
    check("chown(\"file/inner\", me, mg)    [control]",
          chown(at(p, sizeof p, "file/inner"), me, mg), ENOTDIR);

    test_logf("  -- a symlink loop, and a symlink that goes nowhere\n");
    check("chown(\"l-loop\", -1, -1)",
          chown(at(p, sizeof p, "l-loop"), (uid_t) -1, (gid_t) -1), ELOOP);
    check("lchown(\"l-loop\", -1, -1)       [the link itself]",
          lchown(at(p, sizeof p, "l-loop"), (uid_t) -1, (gid_t) -1), 0);
    check("chown(\"l-dangle\", -1, -1)",
          chown(at(p, sizeof p, "l-dangle"), (uid_t) -1, (gid_t) -1), ENOENT);
    check("lchown(\"l-dangle\", -1, -1)     [the link itself]",
          lchown(at(p, sizeof p, "l-dangle"), (uid_t) -1, (gid_t) -1), 0);
    check("fchownat(\"l-dangle\", -1, -1, NOFOLLOW)",
          fchownat(AT_FDCWD, at(p, sizeof p, "l-dangle"), (uid_t) -1, (gid_t) -1,
                   AT_SYMLINK_NOFOLLOW), 0);

    test_logf("  -- a trailing slash, which only a lookup can spend\n");
    check("chown(\"file/\", -1, -1)",
          chown(at(p, sizeof p, "file/"), (uid_t) -1, (gid_t) -1), ENOTDIR);
    check("chown(\"file/\", me, mg)         [control]",
          chown(at(p, sizeof p, "file/"), me, mg), ENOTDIR);
    check("chown(\"dir2/\", -1, -1)",
          chown(at(p, sizeof p, "dir2/"), (uid_t) -1, (gid_t) -1), 0);
    check("lchown(\"l-file/\", -1, -1)",
          lchown(at(p, sizeof p, "l-file/"), (uid_t) -1, (gid_t) -1), ENOTDIR);
    check("lchown(\"l-file/\", me, mg)      [control]",
          lchown(at(p, sizeof p, "l-file/"), me, mg), ENOTDIR);
    check("lchown(\"l-dir/\", -1, -1)       [slash follows it]",
          lchown(at(p, sizeof p, "l-dir/"), (uid_t) -1, (gid_t) -1), 0);
    check("lchown(\"l-dangle/\", -1, -1)",
          lchown(at(p, sizeof p, "l-dangle/"), (uid_t) -1, (gid_t) -1), ENOENT);
    check("chown(\"gone/\", -1, -1)",
          chown(at(p, sizeof p, "gone/"), (uid_t) -1, (gid_t) -1), ENOENT);
}

// -------------------------------------------------- the at-family's own rules

// These already worked, because at_fd_for_path() and the flag check sit above
// the id blocks. They are here so a fix that moves the "nothing to change"
// shortcut ABOVE them -- the obvious wrong place for it -- is caught.
static void at_rules(void) {
    char p[256];

    check("fchownat(999, \"relative\", -1, -1, 0)",
          fchownat(999, "no-such-relative-name", (uid_t) -1, (gid_t) -1, 0), EBADF);
    check("fchownat(AT_FDCWD, \"\", -1, -1, 0)",
          fchownat(AT_FDCWD, "", (uid_t) -1, (gid_t) -1, 0), ENOENT);
    check("fchownat(AT_FDCWD, \"\", -1, -1, AT_EMPTY_PATH)",
          fchownat(AT_FDCWD, "", (uid_t) -1, (gid_t) -1, AT_EMPTY_PATH), 0);
    check("fchownat(999, \"\", -1, -1, AT_EMPTY_PATH)",
          fchownat(999, "", (uid_t) -1, (gid_t) -1, AT_EMPTY_PATH), EBADF);

    // An unknown flag is refused before the lookup, so a path that would
    // otherwise be ENOENT still answers EINVAL.
    check("fchownat(AT_FDCWD, \"gone\", -1, -1, <bad flag>)",
          fchownat(AT_FDCWD, at(p, sizeof p, "gone"), (uid_t) -1, (gid_t) -1,
                   AT_NOT_A_FLAG), EINVAL);

    int ffd = open(at(p, sizeof p, "file"), O_RDONLY);
    if (ffd < 0) {
        failf("open(file) for the dirfd cases", (uint64_t) ffd, (uint64_t) errno, 0, 0, 0, 0);
        return;
    }
    check("fchownat(file_fd, \"relative\", -1, -1, 0)",
          fchownat(ffd, "rel", (uid_t) -1, (gid_t) -1, 0), ENOTDIR);
    check("fchownat(file_fd, \"\", -1, -1, AT_EMPTY_PATH)",
          fchownat(ffd, "", (uid_t) -1, (gid_t) -1, AT_EMPTY_PATH), 0);
    // An absolute path ignores the dirfd entirely, bad or not.
    check("fchownat(999, \"<abs>/file\", -1, -1, 0)",
          fchownat(999, at(p, sizeof p, "file"), (uid_t) -1, (gid_t) -1, 0), 0);
    close(ffd);

    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) {
        failf("open(dir) for the dirfd cases", (uint64_t) dfd, (uint64_t) errno, 0, 0, 0, 0);
        return;
    }
    check("fchownat(dir_fd, \"gone\", -1, -1, 0)",
          fchownat(dfd, "gone", (uid_t) -1, (gid_t) -1, 0), ENOENT);
    check("fchownat(dir_fd, \"file\", -1, -1, 0)",
          fchownat(dfd, "file", (uid_t) -1, (gid_t) -1, 0), 0);
    close(dfd);

    int fd = open(at(p, sizeof p, "file"), O_RDONLY);
    if (fd >= 0) {
        check("fchown(good fd, -1, -1)", fchown(fd, (uid_t) -1, (gid_t) -1), 0);
        close(fd);
    }
    check("fchown(999, -1, -1)", fchown(999, (uid_t) -1, (gid_t) -1), EBADF);
}

// ------------------------------------------------------- it changes nothing

// A successful chown(-1, -1) must leave the owner alone. Trivially true while
// the call did nothing at all; the point is that it stays true now that the
// call does something.
static void changes_nothing(void) {
    char p[256];
    struct stat before, after;
    at(p, sizeof p, "file");
    if (stat(p, &before) < 0) {
        failf("stat before chown(-1, -1)", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
        return;
    }
    check("chown(\"file\", -1, -1)", chown(p, (uid_t) -1, (gid_t) -1), 0);
    if (stat(p, &after) < 0) {
        failf("stat after chown(-1, -1)", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
        return;
    }
    if (before.st_uid != after.st_uid || before.st_gid != after.st_gid)
        failf("chown(-1, -1) changed the owner",
              (uint64_t) after.st_uid, (uint64_t) after.st_gid, 0,
              (uint64_t) before.st_uid, (uint64_t) before.st_gid, 0);
    test_logf("  %-56s uid %d->%d gid %d->%d\n", "owner after chown(-1, -1)",
              (int) before.st_uid, (int) after.st_uid,
              (int) before.st_gid, (int) after.st_gid);
    if ((before.st_mode & 07777) != (after.st_mode & 07777))
        failf("chown(-1, -1) changed the mode",
              (uint64_t) (after.st_mode & 07777), 0, 0,
              (uint64_t) (before.st_mode & 07777), 0, 0);
}

#ifdef __linux__
// Linux bumps ctime but raises no inotify event for it, so a watch that sees
// IN_ATTRIB from a real chown must see nothing from chown(-1, -1). The real
// chown is the positive control: without it, a broken watch would "pass".
static void no_inotify_event(uid_t me, gid_t mg) {
    char p[256];
    at(p, sizeof p, "file");
    int ifd = inotify_init1(IN_NONBLOCK);
    if (ifd < 0) {
        test_logf("  SKIP inotify leg (inotify_init1: %d)\n", errno);
        return;
    }
    if (inotify_add_watch(ifd, p, IN_ATTRIB) < 0) {
        test_logf("  SKIP inotify leg (add_watch: %d)\n", errno);
        close(ifd);
        return;
    }

    char buf[4096];
    struct pollfd pfd = { .fd = ifd, .events = POLLIN };

    chown(p, (uid_t) -1, (gid_t) -1);
    int n = poll(&pfd, 1, 200);
    if (n > 0) {
        ssize_t len = read(ifd, buf, sizeof buf);
        failf("chown(-1, -1) raised an inotify event",
              (uint64_t) n, (uint64_t) len, 0, 0, 0, 0);
    }
    test_logf("  %-56s events=%d (want 0)\n", "inotify after chown(-1, -1)", n < 0 ? 0 : n);

    // positive control: a real chown on the same file, same watch
    chown(p, me, mg);
    n = poll(&pfd, 1, 1000);
    if (n <= 0) {
        failf("the watch never fired for a real chown", (uint64_t) n, 0, 0, 1, 0, 0);
    } else {
        read(ifd, buf, sizeof buf);
    }
    test_logf("  %-56s events=%d (want >0)\n", "inotify after chown(me, mg)", n < 0 ? 0 : n);
    close(ifd);
}
#else
static void no_inotify_event(uid_t me, gid_t mg) { (void) me; (void) mg; }
#endif

// ------------------------------------------------- what only a normal user sees

// Root walks through a directory with no search bit and owns the answer to
// every permission question, so these two say nothing at all when euid is 0.
static void unprivileged_cases(void) {
    char p[256], q[256];

    // An unsearchable parent. The EACCES comes from the walk, so it exists
    // only if the walk does.
    at(p, sizeof p, "nox");
    if (chmod(p, 0644) < 0) {
        test_logf("  SKIP unsearchable-parent case (chmod: %d)\n", errno);
    } else {
        check("chown(\"nox/inner\", -1, -1)",
              chown(at(q, sizeof q, "nox/inner"), (uid_t) -1, (gid_t) -1), EACCES);
        check("chown(\"nox/inner\", me, mg)    [control]",
              chown(at(q, sizeof q, "nox/inner"), getuid(), getgid()), EACCES);
        chmod(p, 0755);
    }

    // A file we do not own. chown(-1, -1) runs NO ownership check and
    // succeeds; the same call with real ids is EPERM. /etc/passwd is the one
    // root-owned, non-setuid, world-readable file every root here has --
    // and nothing below writes to it. A setuid file would be EPERM instead,
    // because stripping its setuid bit is a mode change and THAT is checked;
    // that is a separate behaviour AOK does not implement yet, so this
    // deliberately uses a file with no such bits.
    struct stat st;
    if (stat("/etc/passwd", &st) < 0 || st.st_uid == getuid() ||
        (st.st_mode & (S_ISUID | S_ISGID)) != 0) {
        test_logf("  SKIP not-the-owner case (/etc/passwd unsuitable)\n");
        return;
    }
    check("chown(\"/etc/passwd\", -1, -1)     [not ours]",
          chown("/etc/passwd", (uid_t) -1, (gid_t) -1), 0);
    check("chown(\"/etc/passwd\", 0, 0)       [control]",
          chown("/etc/passwd", 0, 0), EPERM);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    char base[120];
    snprintf(base, sizeof base, "/tmp/chown_no_change.%d", (int) getpid());

    // The shared roots carry /tmp debris from earlier runs, and a leftover
    // directory owned by a different uid turns every case below into EPERM.
    snprintf(dir, sizeof dir, "%s.w", base);
    rm_rf(dir);
    if (fixtures() < 0) {
        printf("fs_chown_no_change: SKIP (cannot set up %s: %s)\n", dir, strerror(errno));
        rm_rf(dir);
        return 0;
    }

    uid_t me = getuid();
    gid_t mg = getgid();
    lookup_errors(me, mg);
    at_rules();
    changes_nothing();
    no_inotify_event(me, mg);
    rm_rf(dir);

    // The permission legs need a uid that owns nothing and may search nothing.
    // Drop to one in a child when we are root; an already unprivileged caller
    // just runs them, which is how this runs on the oracle.
    char odir[160];
    snprintf(odir, sizeof odir, "%s.o", base);
    rm_rf(odir);
    if (geteuid() != 0) {
        snprintf(dir, sizeof dir, "%s", odir);
        if (fixtures() < 0)
            printf("fs_chown_no_change: SKIP permission leg (cannot set up %s)\n", odir);
        else
            unprivileged_cases();
        rm_rf(odir);
        return finish_suite("fs_chown_no_change");
    }

    if (mkdir(odir, 0777) < 0 || chmod(odir, 0777) < 0) {
        printf("fs_chown_no_change: SKIP permission leg (cannot set up %s)\n", odir);
        return finish_suite("fs_chown_no_change");
    }
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        if (setgid(1000) < 0 || setuid(1000) < 0)
            _exit(70);
        snprintf(dir, sizeof dir, "%s/u", odir);
        if (fixtures() < 0)
            _exit(71);
        unprivileged_cases();
        fflush(NULL);   // _exit skips the flush; the diagnosis lives here
        _exit(failures_total != 0 ? 1 : 0);
    }
    int st = 0;
    while (waitpid(c, &st, 0) < 0 && errno == EINTR)
        continue;
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (code == 70)
        printf("fs_chown_no_change: SKIP permission leg (cannot drop privileges)\n");
    else if (code == 71)
        printf("fs_chown_no_change: SKIP permission leg (cannot set up fixtures)\n");
    else if (code != 0)
        failf("chown(-1, -1) permission rules, unprivileged",
              (uint64_t) code, (uint64_t) st, 0, 0, 0, 0);
    rm_rf(odir);

    return finish_suite("fs_chown_no_change");
}
