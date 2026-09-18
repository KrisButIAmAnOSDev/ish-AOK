// A successful chown drops the file's setuid bit, and usually its setgid bit
// with it. AOK dropped nothing, at either privilege, for either id form.
//
// A chown is how a setuid binary changes hands, and the bits cannot survive
// the handover -- the program would go on running as whoever owns it now. So
// a user who could get a setuid-root binary handed to them kept a setuid-root
// binary. Linux has closed this in chown_common() since forever; AOK had the
// matching rule for the WRITE path (file_remove_privs) and nothing here.
//
// Measured on Linux 6.12.101, ext4 and tmpfs, 64-bit and -m32, unprivileged
// and under sudo -- every leg identical, and this file passes there unmodified
// at both privileges. chown(f, -1, -1), which sets no id at all, strips
// exactly as much as a chown with real ids: chown_common() adds ATTR_KILL_SUID
// for any non-directory before it has looked at the ids.
//
//   4755 -> 0755   suid always goes...
//   4644 -> 0644   ...with no execute bit anywhere, too
//   2711 -> 0711   sgid goes when the group-execute bit is set
//   2644 -> 2644   ...but not without it: that combination is a mandatory
//                  locking marker rather than a privilege
//   6644 -> 2644   the two bits are decided separately
//   7755 -> 1755   the sticky bit is not a privilege and is never touched
//   DIR            exempt whatever it is set to
//   FIFO 4644 -> 0644   "not a directory" is the test, not "regular file"
//
// Three things here were measured rather than assumed, and all three are the
// opposite of the obvious guess:
//
//   - Root is NOT exempt. The CAP_FSETID exemption everybody remembers lives
//     in the write path, not this one: chown_common() sets the kill flags with
//     no capability test at all. A root chown of /usr/bin/passwd really does
//     clear its setuid bit -- measured on the real file, which is why the
//     oracle's passwd needed putting back afterwards.
//   - "sgid without group-x is kept" holds only for a caller who is IN the
//     file's group (or privileged); for anyone else it goes too. That is
//     setattr_should_drop_sgid(), and it is why root keeps a 2644 file's sgid
//     where an unprivileged stranger strips it.
//   - The gid it asks about is the file's gid BEFORE the chown -- and a chgrp
//     is exactly what moves a file out of the group being asked about. A 2644
//     file owned by me with a group I am not in comes back 0644 when I chgrp
//     it to my own group; decided from the new gid it would have kept the bit.
//
// And the consequence that is easiest to miss: once the bits are going, the
// call is a mode change, so setattr_prepare() runs its chmod arm as well and
// an unprivileged chown of a setuid file the caller does not own is EPERM --
// where the same call on a plain root-owned file returns 0. That is the only
// way a chown(-1, -1) can be refused at all, since with no id set there is
// nothing else left to check. fs_chown_no_change.c deliberately used a
// non-setuid file to stay clear of this; the legs below are the other half.
//
// The privileged legs need root to build fixtures and to drop into them, and
// skip when this runs unprivileged -- but the strip table above does not, and
// is asserted at whatever privilege this is run with.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

// Any nonzero ids do; nothing below needs them to exist in /etc/passwd.
// FOREIGN_GID is one the unprivileged child is deliberately NOT in.
#define UNPRIV_UID 1000
#define UNPRIV_GID 1000
#define FOREIGN_GID 1001

static char base[160];

static const char *P(const char *sub) {
    static char buf[8][320];
    static int i;
    char *p = buf[i++ & 7];
    snprintf(p, 320, "%s/%s", base, sub);
    return p;
}

// Two of these can appear in one printf, so they must not share a buffer.
static const char *M(mode_t m) {
    static char buf[8][16];
    static int i;
    char *p = buf[i++ & 7];
    snprintf(p, 16, "%04o", (unsigned) (m & 07777));
    return p;
}

static mode_t mode_of(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0)
        return (mode_t) -1;
    return st.st_mode & 07777;
}

static void mkfile(const char *path, mode_t mode) {
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, "x", 1) != 1) {
        failf("mkfile", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
        if (fd >= 0)
            close(fd);
        return;
    }
    close(fd);
    // chmod after the write: the open mode is masked by umask, and the setuid
    // bits are the whole point of the fixture.
    if (chmod(path, mode) < 0)
        failf("mkfile chmod", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
}

// One chown, checked for its result AND for what it left behind.
static void chown_leg(const char *label, const char *path, uid_t u, gid_t g,
                      int want_errno, mode_t want_mode) {
    mode_t before = mode_of(path);
    errno = 0;
    int rc = chown(path, u, g);
    int e = rc < 0 ? errno : 0;
    mode_t after = mode_of(path);
    if (e != want_errno || (rc < 0) != (want_errno != 0) || after != want_mode)
        failf(label, (uint64_t) rc, (uint64_t) e, (uint64_t) after,
              want_errno != 0 ? (uint64_t) -1 : 0, (uint64_t) want_errno,
              (uint64_t) want_mode);
    test_logf("  %-52s %s -> rc=%d errno=%-2d %s (want %s)\n",
              label, M(before), rc, e, M(after), M(want_mode));
}

// The strip itself, which is identical as root and as an ordinary user: every
// file here is one we own, in a group we are in.
static void strip_table(const char *who) {
    test_logf("-- strip table, real ids (%s) --\n", who);
    uid_t me = getuid();
    gid_t mg = getgid();
    struct { const char *name; mode_t before, after; } t[] = {
        { "t4755", 04755, 00755 },  // suid always goes
        { "t4644", 04644, 00644 },  // ...with no execute bit anywhere, too
        { "t2711", 02711, 00711 },  // sgid goes when group-execute is set
        { "t2755", 02755, 00755 },
        { "t2644", 02644, 02644 },  // ...but not without it, for a member
        { "t2664", 02664, 02664 },
        { "t6644", 06644, 02644 },  // decided separately
        { "t6755", 06755, 00755 },
        { "t1755", 01755, 01755 },  // sticky is not a privilege
        { "t7755", 07755, 01755 },
        { "t0755", 00755, 00755 },  // nothing to take
    };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
        char label[96];
        const char *p = P(t[i].name);
        mkfile(p, t[i].before);
        snprintf(label, sizeof label, "%s chown(me,mg)", M(t[i].before));
        chown_leg(label, p, me, mg, 0, t[i].after);
    }

    test_logf("-- the same table for chown(-1, -1), which sets no id (%s) --\n", who);
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
        char label[96];
        char name[64];
        snprintf(name, sizeof name, "n%s", t[i].name);
        const char *p = P(name);
        mkfile(p, t[i].before);
        snprintf(label, sizeof label, "%s chown(-1,-1)", M(t[i].before));
        chown_leg(label, p, (uid_t) -1, (gid_t) -1, 0, t[i].after);
    }

    test_logf("-- one id at a time (%s) --\n", who);
    mkfile(P("u4755"), 04755);
    chown_leg("4755 chown(me,-1)", P("u4755"), me, (gid_t) -1, 0, 00755);
    mkfile(P("g4755"), 04755);
    chown_leg("4755 chown(-1,mg)", P("g4755"), (uid_t) -1, mg, 0, 00755);

    test_logf("-- directories are exempt (%s) --\n", who);
    struct { const char *name; mode_t mode; } d[] = {
        { "d4755", 04755 }, { "d2755", 02755 }, { "d6755", 06755 },
    };
    for (unsigned i = 0; i < sizeof d / sizeof *d; i++) {
        const char *p = P(d[i].name);
        rmdir(p);
        if (mkdir(p, 0755) < 0 || chmod(p, d[i].mode) < 0) {
            failf("mkdir fixture", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
            continue;
        }
        char label[96];
        snprintf(label, sizeof label, "DIR %s chown(me,mg)", M(d[i].mode));
        chown_leg(label, p, me, mg, 0, d[i].mode);
    }

    test_logf("-- a fifo is not a directory, so it strips (%s) --\n", who);
    {
        const char *p = P("fifo");
        unlink(p);
        if (mkfifo(p, 0644) < 0 || chmod(p, 04644) < 0) {
            test_logf("  SKIP fifo leg (mkfifo: %d)\n", errno);
        } else {
            chown_leg("FIFO 4644 chown(me,mg)", p, me, mg, 0, 00644);
        }
    }

    test_logf("-- lchown on a symlink leaves the target alone (%s) --\n", who);
    {
        // Both paths in locals: every check here is about the TARGET's mode
        // while the call names the LINK, so the two must not be confused --
        // chown_leg() lstat()s what it is given and would read the link's own
        // 0777 instead of the file the question is about.
        char target[320], link[320];
        snprintf(target, sizeof target, "%s", P("sym-target"));
        snprintf(link, sizeof link, "%s", P("sym-link"));
        mkfile(target, 04755);
        unlink(link);
        if (symlink(target, link) < 0) {
            test_logf("  SKIP symlink leg (symlink: %d)\n", errno);
        } else {
            errno = 0;
            int rc = lchown(link, me, mg);
            mode_t after = mode_of(target);
            if (rc < 0 || after != 04755)
                failf("lchown(symlink) must not touch the target",
                      (uint64_t) rc, (uint64_t) errno, (uint64_t) after,
                      0, 0, 04755);
            test_logf("  %-52s target %s -> rc=%d target %s\n",
                      "lchown(link,me,mg)", M(04755), rc, M(after));
            // ...and following it DOES strip the target, which is the control
            // that says the leg above measured the link rather than nothing.
            errno = 0;
            rc = chown(link, me, mg);
            after = mode_of(target);
            if (rc < 0 || after != 00755)
                failf("chown(symlink) strips the target",
                      (uint64_t) rc, (uint64_t) errno, (uint64_t) after,
                      0, 0, 00755);
            test_logf("  %-52s target %s -> rc=%d target %s\n",
                      "chown(link,me,mg) follows", M(04755), rc, M(after));
        }
    }

    test_logf("-- by descriptor (%s) --\n", who);
    {
        const char *p = P("f4755");
        mkfile(p, 04755);
        int fd = open(p, O_RDONLY);
        if (fd < 0) {
            failf("open f4755", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
        } else {
            errno = 0;
            int rc = fchown(fd, me, mg);
            mode_t after = mode_of(p);
            if (rc < 0 || after != 00755)
                failf("fchown(fd, me, mg) strips", (uint64_t) rc,
                      (uint64_t) errno, (uint64_t) after, 0, 0, 00755);
            test_logf("  %-52s %s -> rc=%d %s\n", "fchown(fd,me,mg) 4755",
                      M(04755), rc, M(after));
            close(fd);
        }
    }
    {
        const char *p = P("n4755fd");
        mkfile(p, 04755);
        int fd = open(p, O_RDONLY);
        if (fd < 0) {
            failf("open n4755fd", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
        } else {
            errno = 0;
            int rc = fchown(fd, (uid_t) -1, (gid_t) -1);
            mode_t after = mode_of(p);
            if (rc < 0 || after != 00755)
                failf("fchown(fd, -1, -1) strips", (uint64_t) rc,
                      (uint64_t) errno, (uint64_t) after, 0, 0, 00755);
            test_logf("  %-52s %s -> rc=%d %s\n", "fchown(fd,-1,-1) 4755",
                      M(04755), rc, M(after));
            close(fd);
        }
    }
    {
        const char *p = P("e4755");
        mkfile(p, 04755);
        int fd = open(p, O_RDONLY);
        if (fd < 0) {
            failf("open e4755", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
        } else {
            errno = 0;
            int rc = fchownat(fd, "", me, mg, AT_EMPTY_PATH);
            mode_t after = mode_of(p);
            if (rc < 0 || after != 00755)
                failf("fchownat(fd, \"\", AT_EMPTY_PATH) strips",
                      (uint64_t) rc, (uint64_t) errno, (uint64_t) after,
                      0, 0, 00755);
            test_logf("  %-52s %s -> rc=%d %s\n",
                      "fchownat(fd,\"\",me,mg,EMPTY) 4755", M(04755), rc, M(after));
            close(fd);
        }
    }
}

// Everything below needs root to build the fixtures, and an unprivileged child
// to observe them: as root the two interesting answers -- the EPERM, and the
// sgid a non-member loses -- are both invisible, because root is the owner of
// everything and CAP_FSETID makes it a member of every group.
static void unprivileged_legs(void) {
    test_logf("-- a file we do not own --\n");
    // A chown(-1, -1) that has nothing to strip runs no check and succeeds;
    // the same call on a setuid file is EPERM, because taking the bit off is a
    // mode change and THAT is checked. The plain file is the control: without
    // it, a blanket EPERM would look like the rule working.
    chown_leg("root's 0644 chown(-1,-1)  [nothing to strip]",
              P("o0644"), (uid_t) -1, (gid_t) -1, 0, 00644);
    chown_leg("root's 4755 chown(-1,-1)", P("o4755"), (uid_t) -1, (gid_t) -1,
              EPERM, 04755);
    chown_leg("root's 2755 chown(-1,-1)", P("o2755"), (uid_t) -1, (gid_t) -1,
              EPERM, 02755);
    // 2644 in root's group: no group-execute, but we are not in the group
    // either, so the marker goes -- and the mode change is refused.
    chown_leg("root's 2644 chown(-1,-1)  [not in its group]",
              P("o2644"), (uid_t) -1, (gid_t) -1, EPERM, 02644);
    // A directory is exempt, so nothing becomes a mode change and it succeeds
    // exactly like the plain file.
    chown_leg("root's DIR 2755 chown(-1,-1)  [exempt]",
              P("odir2755"), (uid_t) -1, (gid_t) -1, 0, 02755);
    // A chown that fails takes nothing with it.
    chown_leg("root's 4755 chown(me,-1)  [refused]", P("o4755b"), getuid(),
              (gid_t) -1, EPERM, 04755);

    test_logf("-- sgid without group-x: kept only for a member of the group --\n");
    // Both files are ours, so the mode change is permitted either way; the
    // only difference is whether we are in the group the bit belongs to.
    chown_leg("our 2644, our group     [marker kept]",
              P("m2644-mine"), (uid_t) -1, (gid_t) -1, 0, 02644);
    chown_leg("our 2644, foreign group [marker goes]",
              P("m2644-foreign"), (uid_t) -1, (gid_t) -1, 0, 00644);
    // The group-execute form goes either way -- that one is a real privilege.
    chown_leg("our 2755, our group",
              P("m2755-mine"), (uid_t) -1, (gid_t) -1, 0, 00755);
    chown_leg("our 2755, foreign group",
              P("m2755-foreign"), (uid_t) -1, (gid_t) -1, 0, 00755);
}

// Run a block as an unprivileged user, folding its failures back into ours.
#define AS_USER(...) do {                                                      \
        fflush(NULL);                                                          \
        pid_t c_ = fork();                                                     \
        if (c_ == 0) {                                                         \
            /* Drop the supplementary set too, or root's groups follow us in   \
             * and "not in the file's group" stops being true. */              \
            if (setgroups(0, NULL) != 0 || setgid(UNPRIV_GID) != 0 ||          \
                    setuid(UNPRIV_UID) != 0) {                                 \
                printf("FAIL could not drop to uid %d: %s\n",                  \
                       UNPRIV_UID, strerror(errno));                           \
                fflush(NULL);                                                  \
                _exit(1);                                                      \
            }                                                                  \
            failures_total = 0;                                                \
            __VA_ARGS__;                                                       \
            fflush(NULL);                                                      \
            _exit(failures_total > 250 ? 250 : (int) failures_total);          \
        }                                                                      \
        int st_;                                                               \
        if (waitpid(c_, &st_, 0) != c_) { failures_total++; break; }           \
        if (WIFSIGNALED(st_)) {                                                \
            printf("FAIL child died on signal %d\n", WTERMSIG(st_));           \
            failures_total++;                                                  \
        } else                                                                 \
            failures_total += (unsigned) WEXITSTATUS(st_);                     \
    } while (0)

static void rm_rf(const char *path) {
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
    if (system(cmd) < 0)
        return;
}

static void build_unpriv_fixtures(void) {
    struct { const char *name; mode_t mode; uid_t uid; gid_t gid; } f[] = {
        // Root's, so the child owns none of them.
        { "o0644",         00644, 0,          0 },
        { "o4755",         04755, 0,          0 },
        { "o4755b",        04755, 0,          0 },
        { "o2755",         02755, 0,          0 },
        { "o2644",         02644, 0,          0 },
        // The child's, so the mode change is permitted and only the group
        // question is left.
        { "m2644-mine",    02644, UNPRIV_UID, UNPRIV_GID },
        { "m2644-foreign", 02644, UNPRIV_UID, FOREIGN_GID },
        { "m2755-mine",    02755, UNPRIV_UID, UNPRIV_GID },
        { "m2755-foreign", 02755, UNPRIV_UID, FOREIGN_GID },
    };
    for (unsigned i = 0; i < sizeof f / sizeof *f; i++) {
        const char *p = P(f[i].name);
        mkfile(p, f[i].mode);
        // chown before chmod would be fine here, but chown is the thing under
        // test: set the owner first, then stamp the mode it must start from.
        if (chown(p, f[i].uid, f[i].gid) < 0)
            failf("fixture chown", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
        if (chmod(p, f[i].mode) < 0)
            failf("fixture chmod", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
    }
    const char *d = P("odir2755");
    rmdir(d);
    if (mkdir(d, 0755) < 0 || chown(d, 0, 0) < 0 || chmod(d, 02755) < 0)
        failf("fixture dir", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    snprintf(base, sizeof base, "/tmp/chown-kills-privs.%d", (int) getpid());
    rm_rf(base);
    if (mkdir(base, 0755) != 0) {
        printf("FAIL mkdir %s: %s\n", base, strerror(errno));
        return finish_suite("fs_chown_kills_privs");
    }
    // The child has to walk in here, and it owns nothing above it.
    if (chmod(base, 0755) != 0)
        failf("chmod base", (uint64_t) -1, (uint64_t) errno, 0, 0, 0, 0);

    strip_table(geteuid() == 0 ? "as root -- NOT exempt" : "unprivileged");

    if (geteuid() == 0) {
        // Root keeps a 2644 file's marker where the stranger below loses it:
        // CAP_FSETID is what in_group_or_capable() asks about, and it is the
        // only place in this whole rule where privilege changes the answer.
        test_logf("-- root keeps the sgid marker (CAP_FSETID) --\n");
        mkfile(P("r2644"), 02644);
        chown_leg("root's own 2644 chown(-1,-1)", P("r2644"), (uid_t) -1,
                  (gid_t) -1, 0, 02644);

        build_unpriv_fixtures();
        AS_USER(unprivileged_legs());
    } else {
        test_logf("  SKIP not-the-owner and foreign-group legs "
                  "(need root to build the fixtures and to drop into them)\n");
    }

    rm_rf(base);
    return finish_suite("fs_chown_kills_privs");
}
