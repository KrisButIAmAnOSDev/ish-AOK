// A trailing slash on a name whose FINAL component the caller resolves
// WITHOUT following a symlink there -- lstat, readlink, open(O_NOFOLLOW),
// lchown, utimensat(AT_SYMLINK_NOFOLLOW), link's source -- and on the names
// the create/remove family never resolves at all.
//
// Linux draws one line through all of it, in lookup_last():
//
//     if (nd->last_type == LAST_NORM && nd->last.name[nd->last.len])
//             nd->flags |= LOOKUP_FOLLOW | LOOKUP_DIRECTORY;
//
// The slash turns the last component into "follow it, and what you land on is
// a directory" -- but lookup_last() is reached only from path_lookupat(), a
// FULL lookup. A LOOKUP_PARENT walk never resolves the final component at all,
// and unlink, rmdir, rename, mkdir, mknod, symlink, link's destination and a
// unix bind all use that one; they spend the slash with their own rules, and
// those rules are not the same rule:
//
//   filename_create()  ENOENT for a missing name, EEXIST for one that is there
//                      (fs_create_trailing_slash.c)
//   open_last_lookups() EISDIR for O_CREAT, whatever is there
//                      (fs_open_creat_isdir.c)
//   do_unlinkat()      the `slashes:` label -- EISDIR for a directory, ENOENT
//                      for a missing name, ENOTDIR for anything else
//   do_renameat2()     ENOTDIR whenever the SOURCE is not a directory and
//                      EITHER name carries a slash
//   do_rmdir()         no rule: a trailing slash changes nothing
//
// AOK had one flag, N_SYMLINK_NOFOLLOW, doing both jobs, and it served the
// second: the final component was skipped entirely, so the must-be-a-directory
// check that spends the slash never ran for ANY of these callers. Measured
// against Linux 6.12 (camd, x86_64 glibc, 64-bit and -m32 identical):
//
//   lstat("file/")                   ENOTDIR        AOK succeeded
//   lstat("link-to-dir/")            the DIRECTORY  AOK the link (120777)
//   readlink("link-to-dir/")         EINVAL         AOK succeeded
//   readlink("link-to-file/")        ENOTDIR        AOK succeeded
//   open("link-to-dir/", O_NOFOLLOW) succeeds       AOK ELOOP
//   unlink("file/") / unlink("link/") ENOTDIR       AOK DELETED IT
//   rename(src, "gone/")             ENOTDIR        AOK CREATED IT
//   rename(src, "dir/")              ENOTDIR        AOK EISDIR
//
// The two halves are asserted together on purpose, because the fix is one
// flag drawing the line between them and either half alone can be made to
// pass by moving the line to the wrong place. Running the final-component
// block for every nofollow caller would fix the first half and break the
// second: mkdir("dangling-link/") would follow the link and create its target
// where Linux says EEXIST. The create/remove controls below are that half.
//
// Every expectation was measured on the oracle, 64-bit and -m32, both
// identical, and the whole file passes there unmodified.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>

#include "test_common.h"

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

static char dir[192];        // scratch directory, rebuilt before every case

// ------------------------------------------------------------------ helpers

static char *at(char *buf, size_t n, const char *name) {
    snprintf(buf, n, "%s/%s", dir, name);
    return buf;
}

static void check(const char *label, int r, int want_errno) {
    int e = r < 0 ? errno : 0;
    if (want_errno == 0 ? r < 0 : !(r < 0 && e == want_errno))
        failf(label, (uint64_t) r, (uint64_t) e, 0,
              (uint64_t) (want_errno == 0 ? 0 : -1), (uint64_t) want_errno, 0);
    test_logf("  %-52s rc=%d errno=%d (want %d)\n", label, r, e, want_errno);
}

// What is at `name` right now: 'r' regular, 'd' directory, 'l' symlink,
// 'p' fifo, '-' nothing there, '?' anything else. An lstat, so a dangling
// symlink is 'l' and not '-'.
static char kind_of(const char *name) {
    char p[512];
    struct stat st;
    if (lstat(at(p, sizeof p, name), &st) < 0)
        return errno == ENOENT ? '-' : '?';
    if (S_ISREG(st.st_mode)) return 'r';
    if (S_ISDIR(st.st_mode)) return 'd';
    if (S_ISLNK(st.st_mode)) return 'l';
    if (S_ISFIFO(st.st_mode)) return 'p';
    return '?';
}

// The damage a wrong answer does is the leftover state, not the return value:
// unlink("file/") reported success AND removed the file.
static void check_kind(const char *label, const char *name, char want) {
    char got = kind_of(name);
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-52s is '%c' (want '%c')\n", label, got, want);
}

// O_NONBLOCK throughout: one fixture is a fifo, and an ordinary open of one
// waits for a peer that never comes.
static int open_rc(const char *path, int flags) {
    errno = 0;
    int fd = open(path, flags | O_NONBLOCK, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

// open(), but reporting what the fd turned out to be pointing at.
static int open_kind(const char *path, int flags, char *kind) {
    errno = 0;
    *kind = '-';
    int fd = open(path, flags | O_NONBLOCK, 0644);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) == 0)
        *kind = S_ISDIR(st.st_mode) ? 'd' : S_ISREG(st.st_mode) ? 'r'
              : S_ISLNK(st.st_mode) ? 'l' : S_ISFIFO(st.st_mode) ? 'p' : '?';
    close(fd);
    return 0;
}

static int readlink_rc(const char *path) {
    char buf[256];
    errno = 0;
    return readlink(path, buf, sizeof buf) < 0 ? -1 : 0;
}

static int utimensat_nofollow_rc(const char *path) {
    struct timespec ts[2] = {{1000000, 0}, {1000000, 0}};
    errno = 0;
    return utimensat(AT_FDCWD, path, ts, AT_SYMLINK_NOFOLLOW);
}

static int renameat2_noreplace(const char *from, const char *to) {
    errno = 0;
#ifdef SYS_renameat2
    return (int) syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to,
                         RENAME_NOREPLACE);
#else
    (void) from; (void) to; errno = ENOSYS; return -1;
#endif
}

// ----------------------------------------------------------------- fixtures

static void rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0)
        return;
    if (!S_ISDIR(st.st_mode)) {
        unlink(path);
        return;
    }
    chmod(path, 0700);       // an unwritable fixture has to be unlocked first
    DIR *d = opendir(path);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char child[512];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            rm_rf(child);
        }
        closedir(d);
    }
    rmdir(path);
}

// Every case runs against a pristine copy: half of them delete or create.
static int fixtures(void) {
    char p[512];
    rm_rf(dir);
    if (mkdir(dir, 0755) < 0)
        return -1;
    int fd = open(at(p, sizeof p, "file"), O_CREAT | O_WRONLY, 0666);
    if (fd < 0)
        return -1;
    if (write(fd, "x", 1) != 1) { close(fd); return -1; }
    close(fd);
    if (chmod(p, 0666) < 0)                      // a legal hardlink source
        return -1;
    if (mkdir(at(p, sizeof p, "dir"), 0755) < 0)
        return -1;
    if (mkdir(at(p, sizeof p, "full"), 0755) < 0)
        return -1;
    snprintf(p, sizeof p, "%s/full/e", dir);
    fd = open(p, O_CREAT | O_WRONLY, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    if (symlink("file", at(p, sizeof p, "l2f")) < 0)
        return -1;
    if (symlink("dir", at(p, sizeof p, "l2d")) < 0)
        return -1;
    if (symlink("ghost", at(p, sizeof p, "dang")) < 0)
        return -1;
    if (mkfifo(at(p, sizeof p, "fifo"), 0644) < 0)
        return -1;
    return 0;
}

// Rebuild, and say so loudly if it ever fails mid-run: a case that runs
// against a half-built directory reports the wrong errno for the right reason.
static void reset(void) {
    if (fixtures() < 0)
        failf("rebuild fixtures", (uint64_t) errno, 0, 0, 0, 0, 0);
}

// ------------------------------- what a slashed name resolves to, per subject

// Once the slash has done its work (follow, then demand a directory), a name
// has landed in one of three places -- and they are three different answers,
// reached by different routes, so a fix that produces one of them everywhere
// is still wrong.
enum lands { LANDS_DIR, LANDS_OTHER, LANDS_NOWHERE };

static const struct subject {
    const char *name;
    enum lands lands;    // where "<name>/" ends up
    char kind;           // what "<name>" itself is -- the un-slashed control
} subjects[] = {
    {"file", LANDS_OTHER,   'r'},
    {"dir",  LANDS_DIR,     'd'},
    {"full", LANDS_DIR,     'd'},
    {"l2f",  LANDS_OTHER,   'l'},   // the link is followed, and lands on a file
    {"l2d",  LANDS_DIR,     'l'},   // ...and here on the directory
    {"dang", LANDS_NOWHERE, 'l'},   // the name it names is not there
    {"fifo", LANDS_OTHER,   'p'},
    {"gone", LANDS_NOWHERE, '-'},
};
#define NSUBJECTS (sizeof subjects / sizeof subjects[0])

// The common shape: a directory satisfies the slash, anything else is ENOTDIR,
// and nothing at all is ENOENT.
static int want_for(enum lands l) {
    return l == LANDS_DIR ? 0 : l == LANDS_OTHER ? ENOTDIR : ENOENT;
}

// ------------------------------------ the callers that DO resolve the final
//                                      component, just without following it

static void resolving_callers(void) {
    for (unsigned i = 0; i < NSUBJECTS; i++) {
        const struct subject *s = &subjects[i];
        char p[400], q[512], label[200];
        reset();
        at(p, sizeof p, s->name);
        snprintf(q, sizeof q, "%s/", p);

#define ROW(op, call, want) do {                                               \
            snprintf(label, sizeof label, "%s(%s/)", (op), s->name);           \
            errno = 0;                                                         \
            check(label, (call), (want));                                      \
        } while (0)

        struct stat st;
        ROW("lstat", lstat(q, &st), want_for(s->lands));
        // Landing on the directory is not enough: it has to be the DIRECTORY
        // that gets reported. lstat("link-to-dir/") is 040755 on Linux, and
        // AOK gave 120777 -- the link, which is the one thing the slash says
        // it is not.
        if (s->lands == LANDS_DIR) {
            snprintf(label, sizeof label, "lstat(%s/) is the directory", s->name);
            errno = 0;
            if (lstat(q, &st) < 0 || !S_ISDIR(st.st_mode))
                failf(label, (uint64_t) st.st_mode, (uint64_t) errno, 0, 0, 0, 0);
            test_logf("  %-52s mode=%06o\n", label, (unsigned) st.st_mode);
        }

        // readlink lands the same way and then reports on what it found: a
        // directory is EINVAL, because a directory is not a symlink.
        ROW("readlink", readlink_rc(q),
            s->lands == LANDS_DIR ? EINVAL : want_for(s->lands));

        // O_NOFOLLOW is overruled by the slash -- the slash sets LOOKUP_FOLLOW
        // -- so a link to a directory OPENS, where AOK said ELOOP.
        ROW("open.NOFOLLOW", open_rc(q, O_RDONLY | O_NOFOLLOW), want_for(s->lands));
        ROW("open.NOFOLLOW|PATH", open_rc(q, O_PATH | O_NOFOLLOW), want_for(s->lands));
        if (s->lands == LANDS_DIR) {
            char k = '-';
            snprintf(label, sizeof label, "open(%s/, O_NOFOLLOW) is the directory",
                     s->name);
            errno = 0;
            if (open_kind(q, O_RDONLY | O_NOFOLLOW, &k) < 0 || k != 'd')
                failf(label, (uint64_t) k, (uint64_t) errno, 0, (uint64_t) 'd', 0, 0);
            test_logf("  %-52s fd is '%c'\n", label, k);
        }

        ROW("utimensat.NOFOLLOW", utimensat_nofollow_rc(q), want_for(s->lands));
        ROW("lchown", lchown(q, getuid(), getgid()), want_for(s->lands));

        // link's SOURCE is a full lookup too (do_linkat calls filename_lookup
        // on it, filename_create only on the destination), so the slash
        // follows there as well -- and a directory it lands on is then EPERM
        // from vfs_link, not ENOTDIR. The destination must not appear either
        // way.
        {
            char dst[512];
            at(dst, sizeof dst, "made");
            ROW("link.src", link(q, dst),
                s->lands == LANDS_DIR ? EPERM : want_for(s->lands));
            snprintf(label, sizeof label, "link(%s/) created nothing", s->name);
            check_kind(label, "made", '-');
        }

        // Two slashes are one slash; a trailing "/." says the same thing
        // again; and a name AFTER the slash was always handled correctly,
        // which is what says the FINAL component is where this went wrong.
        {
            char qq[512];
            snprintf(qq, sizeof qq, "%s//", p);
            snprintf(label, sizeof label, "lstat(%s//)", s->name);
            errno = 0;
            check(label, lstat(qq, &st), want_for(s->lands));
            snprintf(qq, sizeof qq, "%s/x", p);
            snprintf(label, sizeof label, "lstat(%s/x)", s->name);
            errno = 0;
            check(label, lstat(qq, &st),
                  s->lands == LANDS_DIR ? ENOENT : want_for(s->lands));
        }
#undef ROW

        // ...and the un-slashed spelling is untouched: it still reports the
        // name itself, symlink and all. Every one of these already worked and
        // has to keep working -- they are how you tell "the slash is honoured"
        // from "the final component is now followed".
        snprintf(label, sizeof label, "lstat(%s) unchanged", s->name);
        check_kind(label, s->name, s->kind);
        if (s->kind == 'l') {
            snprintf(label, sizeof label, "readlink(%s) still reads the link",
                     s->name);
            errno = 0;
            check(label, readlink_rc(p), 0);
            snprintf(label, sizeof label, "open(%s, O_NOFOLLOW) is ELOOP", s->name);
            errno = 0;
            check(label, open_rc(p, O_RDONLY | O_NOFOLLOW), ELOOP);
        }
    }
}

// ------------------------------------------------------------------- unlink

// do_unlinkat's `slashes:` label. The dentry is the one a LOOKUP_PARENT walk
// produced, so no symlink was followed to reach it: a link to a directory is
// ENOTDIR, not EISDIR, because the LINK is what the name names.
static void unlink_slash(void) {
    for (unsigned i = 0; i < NSUBJECTS; i++) {
        const struct subject *s = &subjects[i];
        char p[400], q[512], label[200];
        reset();
        at(p, sizeof p, s->name);
        snprintf(q, sizeof q, "%s/", p);

        int want = s->kind == 'd' ? EISDIR : s->kind == '-' ? ENOENT : ENOTDIR;
        snprintf(label, sizeof label, "unlink(%s/)", s->name);
        errno = 0;
        check(label, unlink(q), want);
        // The point of the whole rule: it is still there.
        snprintf(label, sizeof label, "unlink(%s/) removed nothing", s->name);
        check_kind(label, s->name, s->kind);

        snprintf(label, sizeof label, "unlink(%s//)", s->name);
        snprintf(q, sizeof q, "%s//", p);
        errno = 0;
        check(label, unlink(q), want);
        snprintf(label, sizeof label, "unlink(%s//) removed nothing", s->name);
        check_kind(label, s->name, s->kind);
    }

    // The un-slashed control: those same names DO go away, symlinks included
    // -- unlink has never followed the final component and still must not.
    reset();
    char p[512];
    check("unlink(file)", unlink(at(p, sizeof p, "file")), 0);
    check_kind("unlink(file) removed it", "file", '-');
    check("unlink(l2d)", unlink(at(p, sizeof p, "l2d")), 0);
    check_kind("unlink(l2d) removed the link", "l2d", '-');
    check_kind("unlink(l2d) left the directory", "dir", 'd');
    check("unlink(dir)", unlink(at(p, sizeof p, "dir")), EISDIR);
}

// -------------------------------------------------------------------- rmdir

// The member of the family with no trailing-slash rule at all: a slash asks
// for precisely what rmdir removes. Here so that a change which gives every
// remove caller the same rule is caught.
static void rmdir_slash(void) {
    for (unsigned i = 0; i < NSUBJECTS; i++) {
        const struct subject *s = &subjects[i];
        char p[400], q[512], label[200];
        int want = s->kind == 'd' ? (strcmp(s->name, "full") == 0 ? ENOTEMPTY : 0)
                 : s->kind == '-' ? ENOENT : ENOTDIR;

        reset();
        at(p, sizeof p, s->name);
        snprintf(label, sizeof label, "rmdir(%s)", s->name);
        errno = 0;
        check(label, rmdir(p), want);

        reset();
        snprintf(q, sizeof q, "%s/", at(p, sizeof p, s->name));
        snprintf(label, sizeof label, "rmdir(%s/) says the same", s->name);
        errno = 0;
        check(label, rmdir(q), want);
    }
}

// ------------------------------------------------------------------- rename

// "unless the source is a directory trailing slashes give -ENOTDIR" -- the
// comment is Linux's own, above the test in do_renameat2. Both slashes are
// spent on the SOURCE's type, which is why a slashed destination is ENOTDIR
// even when a directory is sitting there and the un-slashed spelling is
// EISDIR.
static void rename_slash(void) {
    char src[512], dst[512], q[600], label[200];

    // (1) the slash on the SOURCE
    for (unsigned i = 0; i < NSUBJECTS; i++) {
        const struct subject *s = &subjects[i];
        reset();
        at(src, sizeof src, s->name);
        snprintf(q, sizeof q, "%s/", src);
        at(dst, sizeof dst, "moved");

        int want = s->kind == 'd' ? 0 : s->kind == '-' ? ENOENT : ENOTDIR;
        snprintf(label, sizeof label, "rename(%s/, moved)", s->name);
        errno = 0;
        check(label, rename(q, dst), want);
        snprintf(label, sizeof label, "rename(%s/, moved) moved %s", s->name,
                 want == 0 ? "it" : "nothing");
        check_kind(label, s->name, want == 0 ? '-' : s->kind);
        snprintf(label, sizeof label, "rename(%s/, moved) destination", s->name);
        check_kind(label, "moved", want == 0 ? 'd' : '-');
    }

    // (2) the slash on the DESTINATION, from a source that is NOT a directory.
    // Always ENOTDIR, whatever the destination is or is not -- including a
    // name that does not exist, where AOK simply created it.
    for (unsigned i = 0; i < NSUBJECTS; i++) {
        const struct subject *s = &subjects[i];
        reset();
        at(src, sizeof src, "file");
        snprintf(q, sizeof q, "%s/%s/", dir, s->name);

        snprintf(label, sizeof label, "rename(file, %s/)", s->name);
        errno = 0;
        check(label, rename(src, q), ENOTDIR);
        snprintf(label, sizeof label, "rename(file, %s/) left the source", s->name);
        check_kind(label, "file", 'r');
        snprintf(label, sizeof label, "rename(file, %s/) left the destination",
                 s->name);
        check_kind(label, s->name, s->kind);
    }

    // ...and the un-slashed spellings of the same two, which are the answers
    // the slash replaces: a directory destination is EISDIR, and a missing one
    // is an ordinary successful move.
    reset();
    check("rename(file, dir)", rename(at(src, sizeof src, "file"),
                                      at(dst, sizeof dst, "dir")), EISDIR);
    reset();
    check("rename(file, gone)", rename(at(src, sizeof src, "file"),
                                       at(dst, sizeof dst, "gone")), 0);
    check_kind("rename(file, gone) created it", "gone", 'r');

    // (3) a DIRECTORY source spends the slash on nothing: the destination's
    // slash is only ENOTDIR because the source was not a directory, so with
    // one that is, every answer is the un-slashed answer.
    reset();
    check("rename(dir, gone/)", rename(at(src, sizeof src, "dir"),
                                       at(dst, sizeof dst, "gone/")), 0);
    check_kind("rename(dir, gone/) created the directory", "gone", 'd');
    reset();
    check("rename(dir, full/)", rename(at(src, sizeof src, "dir"),
                                       at(dst, sizeof dst, "full/")), ENOTEMPTY);
    reset();
    check("rename(dir, file/)", rename(at(src, sizeof src, "dir"),
                                       at(dst, sizeof dst, "file/")), ENOTDIR);
    check_kind("rename(dir, file/) left the file", "file", 'r');
    reset();
    check("rename(dir/, gone)", rename(at(src, sizeof src, "dir/"),
                                       at(dst, sizeof dst, "gone")), 0);
    check_kind("rename(dir/, gone) moved the directory", "gone", 'd');

    // (4) RENAME_NOREPLACE answers EEXIST first, so only a destination that is
    // NOT there reaches the trailing-slash rule.
    reset();
    int r = renameat2_noreplace(at(src, sizeof src, "file"),
                                at(dst, sizeof dst, "dir/"));
    if (r < 0 && errno == ENOSYS) {
        printf("fs_nofollow_trailing_slash: SKIP renameat2 (ENOSYS)\n");
    } else {
        check("renameat2(file, dir/, NOREPLACE)", r, EEXIST);
        reset();
        check("renameat2(file, gone/, NOREPLACE)",
              renameat2_noreplace(at(src, sizeof src, "file"),
                                  at(dst, sizeof dst, "gone/")), ENOTDIR);
        check_kind("renameat2(file, gone/, NOREPLACE) created nothing", "gone", '-');
    }
}

// ---------------------------------------------- the create family, unchanged
//
// The other half of the line. These resolve the final component not at all,
// and the slash means something else entirely to each of them. They already
// answered correctly (3641d5b7, b0efd05e) and the point here is that they
// still do: give the final-component block to every nofollow caller and
// mkdir("dang/") follows the link and creates "ghost", where Linux says
// EEXIST because the NAME is taken.
static void create_family_controls(void) {
    char p[512];

    reset();
    check("mkdir(dang/)", mkdir(at(p, sizeof p, "dang/"), 0755), EEXIST);
    check_kind("mkdir(dang/) left the link alone", "dang", 'l');
    check_kind("mkdir(dang/) did not create the target", "ghost", '-');

    reset();
    check("mkdir(l2d/)", mkdir(at(p, sizeof p, "l2d/"), 0755), EEXIST);
    check_kind("mkdir(l2d/) left the link alone", "l2d", 'l');

    reset();
    check("mkdir(gone/)", mkdir(at(p, sizeof p, "gone/"), 0755), 0);
    check_kind("mkdir(gone/) made a directory", "gone", 'd');

    reset();
    check("mknod(gone/, FIFO)", mknod(at(p, sizeof p, "gone/"), S_IFIFO | 0644, 0),
          ENOENT);
    check_kind("mknod(gone/) created nothing", "gone", '-');

    reset();
    check("symlink(x, gone/)", symlink("x", at(p, sizeof p, "gone/")), ENOENT);
    check_kind("symlink(x, gone/) created nothing", "gone", '-');

    reset();
    {
        char src[512];
        check("link(file, gone/)", link(at(src, sizeof src, "file"),
                                        at(p, sizeof p, "gone/")), ENOENT);
        check_kind("link(file, gone/) created nothing", "gone", '-');
    }

    reset();
    check("open(gone/, O_CREAT)", open_rc(at(p, sizeof p, "gone/"),
                                          O_CREAT | O_WRONLY), EISDIR);
    check_kind("open(gone/, O_CREAT) created nothing", "gone", '-');

    reset();
    check("open(dir/, O_CREAT)", open_rc(at(p, sizeof p, "dir/"),
                                         O_CREAT | O_RDONLY), EISDIR);
}

// --------------------------------------------------------- what answers first
//
// The trailing-slash rules sit between the parent walk and the parent's own
// write permission: Linux looks the final component up after filename_parentat
// and before may_delete()/may_create(), which only run inside vfs_unlink() and
// vfs_rename(). So an unsearchable parent still reports EACCES and beats the
// slash rule, while an UNWRITABLE one does not -- it reports the slash rule.
// That needs a caller the mode bits actually bind, so this leg runs
// unprivileged.
static int order_cases(const char *base) {
    unsigned before = failures_total;
    char p[600], q[600];

#define SETUP() do {                                                           \
        rm_rf(base);                                                           \
        if (mkdir(base, 0755) < 0) return -1;                                  \
        snprintf(p, sizeof p, "%s/file", base);                                \
        int _fd = open(p, O_CREAT | O_WRONLY, 0644);                           \
        if (_fd < 0) return -1;                                                \
        close(_fd);                                                            \
        snprintf(p, sizeof p, "%s/d2", base);                                  \
        if (mkdir(p, 0755) < 0) return -1;                                     \
        snprintf(p, sizeof p, "%s/nowrite", base);                             \
        if (mkdir(p, 0755) < 0) return -1;                                     \
        snprintf(p, sizeof p, "%s/nowrite/f", base);                           \
        _fd = open(p, O_CREAT | O_WRONLY, 0644);                               \
        if (_fd < 0) return -1;                                                \
        close(_fd);                                                            \
        snprintf(p, sizeof p, "%s/nowrite/d", base);                           \
        if (mkdir(p, 0755) < 0) return -1;                                     \
        snprintf(p, sizeof p, "%s/nowrite/s", base);                           \
        if (symlink("f", p) < 0) return -1;                                    \
        snprintf(p, sizeof p, "%s/nosearch", base);                            \
        if (mkdir(p, 0755) < 0) return -1;                                     \
        snprintf(p, sizeof p, "%s/nosearch/f", base);                          \
        _fd = open(p, O_CREAT | O_WRONLY, 0644);                               \
        if (_fd < 0) return -1;                                                \
        close(_fd);                                                            \
        snprintf(p, sizeof p, "%s/nosearch", base);                            \
        if (chmod(p, 0644) < 0) return -1;                                     \
        snprintf(p, sizeof p, "%s/nowrite", base);                             \
        if (chmod(p, 0555) < 0) return -1;                                     \
    } while (0)

#define AT(buf, rel) (snprintf((buf), sizeof (buf), "%s/%s", base, (rel)), (buf))
#define ORDER(label, call, want) do { SETUP(); errno = 0; check((label), (call), (want)); } while (0)

    struct stat st;

    // The parent WALK still answers first: an unsearchable parent is EACCES,
    // a parent that is a regular file is ENOTDIR, a missing one is ENOENT.
    ORDER("unlink(nosearch/f/)",  unlink(AT(p, "nosearch/f/")),  EACCES);
    ORDER("lstat(nosearch/f/)",   lstat(AT(p, "nosearch/f/"), &st), EACCES);
    ORDER("unlink(file/x/)",      unlink(AT(p, "file/x/")),      ENOTDIR);
    ORDER("unlink(gone/x/)",      unlink(AT(p, "gone/x/")),      ENOENT);
    ORDER("rename(gone/x/, dst)", rename(AT(p, "gone/x/"), AT(q, "dst")), ENOENT);

    // The parent's WRITE permission does not: may_delete runs afterwards, so
    // the slash rule reports instead of EACCES.
    ORDER("unlink(nowrite/f)",    unlink(AT(p, "nowrite/f")),    EACCES);
    ORDER("unlink(nowrite/f/)",   unlink(AT(p, "nowrite/f/")),   ENOTDIR);
    ORDER("unlink(nowrite/s/)",   unlink(AT(p, "nowrite/s/")),   ENOTDIR);
    ORDER("unlink(nowrite/d/)",   unlink(AT(p, "nowrite/d/")),   EISDIR);
    ORDER("unlink(nowrite/gone/)", unlink(AT(p, "nowrite/gone/")), ENOENT);
    ORDER("lstat(nowrite/f/)",    lstat(AT(p, "nowrite/f/"), &st), ENOTDIR);

    ORDER("rename(nowrite/f, dst)",  rename(AT(p, "nowrite/f"), AT(q, "dst")), EACCES);
    ORDER("rename(nowrite/f/, dst)", rename(AT(p, "nowrite/f/"), AT(q, "dst")), ENOTDIR);
    ORDER("rename(file, nowrite/x)",  rename(AT(p, "file"), AT(q, "nowrite/x")), EACCES);
    ORDER("rename(file, nowrite/x/)", rename(AT(p, "file"), AT(q, "nowrite/x/")), ENOTDIR);
    ORDER("rename(file, nowrite/f/)", rename(AT(p, "file"), AT(q, "nowrite/f/")), ENOTDIR);

    // mkdir has no slash rule, so it is still the parent's EACCES -- the same
    // control fs_create_trailing_slash pins, kept here because this change is
    // what could take it away.
    ORDER("mkdir(nowrite/x/)",    mkdir(AT(p, "nowrite/x/"), 0755), EACCES);
    ORDER("symlink(t, nowrite/x/)", symlink("t", AT(p, "nowrite/x/")), ENOENT);

#undef ORDER
#undef AT
#undef SETUP
    rm_rf(base);
    return failures_total == before ? 0 : 1;
}

// --------------------------------------------------------------------- main

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(240));

    char base[160];
    snprintf(base, sizeof base, "/tmp/nofollow_tslash.%d", (int) getpid());
    snprintf(dir, sizeof dir, "%s.w", base);
    if (fixtures() < 0) {
        printf("fs_nofollow_trailing_slash: SKIP (cannot set up %s: %s)\n",
               dir, strerror(errno));
        rm_rf(dir);
        return 0;
    }

    resolving_callers();
    unlink_slash();
    rmdir_slash();
    rename_slash();
    create_family_controls();
    rm_rf(dir);

    // Root bypasses the write check entirely, so the ordering questions are
    // invisible to it. Drop to an unprivileged uid in a child; an already
    // unprivileged caller just runs them, which is how this runs on the oracle.
    char odir[192];
    snprintf(odir, sizeof odir, "%s.o", base);
    if (geteuid() == 0) {
        if (mkdir(odir, 0777) < 0 || chmod(odir, 0777) < 0) {
            printf("fs_nofollow_trailing_slash: SKIP order leg (cannot set up %s)\n",
                   odir);
        } else {
            char inner[256];
            snprintf(inner, sizeof inner, "%s/o", odir);
            fflush(NULL);
            pid_t c = fork();
            if (c == 0) {
                if (setgid(1000) < 0 || setuid(1000) < 0)
                    _exit(70);
                int crc = order_cases(inner);
                fflush(NULL);   // _exit skips the flush; the diagnosis lives here
                _exit(crc < 0 ? 71 : crc);
            }
            int st = 0;
            while (waitpid(c, &st, 0) < 0 && errno == EINTR)
                continue;
            int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            if (code == 70)
                printf("fs_nofollow_trailing_slash: SKIP order leg (cannot drop privileges)\n");
            else if (code == 71)
                printf("fs_nofollow_trailing_slash: SKIP order leg (cannot set up fixtures)\n");
            else if (code != 0)
                failf("trailing slash before the parent's write permission",
                      (uint64_t) code, (uint64_t) st, 0, 0, 0, 0);
            rm_rf(odir);
        }
    } else {
        if (order_cases(odir) < 0)
            printf("fs_nofollow_trailing_slash: SKIP order leg (cannot set up %s)\n",
                   odir);
        rm_rf(odir);
    }

    return finish_suite("fs_nofollow_trailing_slash");
}
