// A final "." or ".." names no entry that can be created or removed -- and
// WHEN that is answered decides which error the caller sees.
//
// Linux answers it in the middle of the operation, never at the start. Every
// one of do_mknodat/do_symlinkat/do_linkat/do_mkdirat calls filename_create(),
// which runs filename_parentat() FIRST and only then reaches
//
//     dentry = ERR_PTR(-EEXIST);
//     ...
//     if (type != LAST_NORM)
//         goto out;
//
// with do_unlinkat (EISDIR), do_rmdir (EINVAL for ".", ENOTEMPTY for "..") and
// do_renameat2 (EBUSY) built the same way. So the parent walk always speaks
// first: mknod("gone/.") is ENOENT because "gone" is not there, mknod("file/.")
// is ENOTDIR, and mknod("unsearchable/.") is EACCES. Only once the walk has
// arrived does the final component's spelling get to answer.
//
// AOK asked path_final_dot() before path_normalize() ran at all, so all three
// of those were EEXIST -- an answer that says "the name is taken" about a
// directory that does not exist.
//
// The rule itself must survive, and the other half of its order with it: an
// existing parent the caller cannot WRITE still reports EEXIST and not EACCES,
// because Linux only asks may_create() afterwards. That is why
// path_final_dot() exists -- `ln -s /bin/sh .` in a user's own home reported
// EACCES (the parent of a normalized "." is its GRANDparent, /home, which is
// root-owned) where root got EEXIST, and GNU ln keys off exactly that EEXIST
// to retry as "link into this directory". So the command worked for root and
// failed for every normal user.
//
// Hence four kinds of parent, and both halves of the order:
//
//   exists, writable      the dot answers
//   exists, unwritable    the dot answers -- ahead of the permission
//   exists, unsearchable  EACCES -- the walk cannot even get there
//   missing / a file      ENOENT / ENOTDIR -- the walk's own error
//
// Every expectation below was measured on the Linux 6.12 oracle (Devuan 6,
// x86_64 glibc), 64-bit and -m32, both identical.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "test_common.h"

static char dir[256];        // writable scratch directory
static char src[320];        // a regular file: link()'s and rename()'s source
static char newname[320];    // rename()'s destination

static void check(const char *label, int r, int want_errno) {
    int e = r < 0 ? errno : 0;
    if (!(r < 0 && e == want_errno))
        failf(label, (uint64_t) r, (uint64_t) e, 0, (uint64_t) -1, (uint64_t) want_errno, 0);
    test_logf("  %-46s rc=%d errno=%d (want %d)\n", label, r, e, want_errno);
}

// Every operation that a final "." can be handed, with the answer it gives
// once the parent walk has arrived. "." and ".." differ only for rmdir.
struct op {
    const char *name;
    int (*fn)(const char *path);
    int dot_err;
    int dotdot_err;
};

static int op_mknod(const char *p)   { return mknod(p, S_IFIFO | 0644, 0); }
static int op_mkfifo(const char *p)  { return mkfifo(p, 0644); }
static int op_symlink(const char *p) { return symlink("target", p); }
static int op_link(const char *p)    { return link(src, p); }
static int op_mkdir(const char *p)   { return mkdir(p, 0755); }
static int op_rmdir(const char *p)   { return rmdir(p); }
static int op_unlink(const char *p)  { return unlink(p); }
static int op_rename_to(const char *p)   { return rename(src, p); }
static int op_rename_from(const char *p) { return rename(p, newname); }

static const struct op ops[] = {
    {"mknod",       op_mknod,       EEXIST, EEXIST},
    {"mkfifo",      op_mkfifo,      EEXIST, EEXIST},
    {"symlink",     op_symlink,     EEXIST, EEXIST},
    {"link",        op_link,        EEXIST, EEXIST},
    {"mkdir",       op_mkdir,       EEXIST, EEXIST},
    {"rmdir",       op_rmdir,       EINVAL, ENOTEMPTY},
    {"unlink",      op_unlink,      EISDIR, EISDIR},
    {"rename(->)",  op_rename_to,   EBUSY,  EBUSY},
    {"rename(<-)",  op_rename_from, EBUSY,  EBUSY},
};

// The damage a wrong answer leaves behind is an entry, so look for one.
static void check_absent(const char *label, const char *path) {
    struct stat st;
    if (lstat(path, &st) == 0) {
        failf(label, (uint64_t) st.st_mode, 0, 0, 0, 0, 0);
        printf("  %s: %s exists (mode %06o)\n", label, path, (unsigned) st.st_mode);
        if (S_ISDIR(st.st_mode))
            rmdir(path);
        else
            unlink(path);
    }
}

// `parent` is a path; `walk_err` is what the walk to it reports, or 0 when it
// arrives and the dot rule is what answers.
static void run_parent(const char *pname, const char *parent, int walk_err, int tails) {
    static const char *all_tails[] = {".", "..", "./", "../"};
    char t[512], label[200];

    test_logf("parent %s (%s):\n", pname, walk_err == 0 ? "reachable" : "the walk fails");
    for (int i = 0; i < tails; i++) {
        const char *tail = all_tails[i];
        bool is_dotdot = tail[1] == '.';
        snprintf(t, sizeof t, "%s/%s", parent, tail);
        for (unsigned j = 0; j < sizeof ops / sizeof *ops; j++) {
            int want = walk_err != 0 ? walk_err
                                     : (is_dotdot ? ops[j].dotdot_err : ops[j].dot_err);
            snprintf(label, sizeof label, "%s(%s/%s)", ops[j].name, pname, tail);
            errno = 0;
            check(label, ops[j].fn(t), want);
        }
        // Nothing may have been created under the name the dotty path would
        // have normalized to, nor may the rename have moved anything.
        check_absent("a refused dotty create left an entry", newname);
    }
    // The source must still be there: a rename that was refused did not move
    // it, and a link that was refused did not consume it.
    {
        struct stat st;
        if (lstat(src, &st) < 0)
            failf("the source of a refused link/rename disappeared",
                  0, (uint64_t) errno, 0, 0, 0, 0);
    }
}

// The controls: an ORDINARY final name in the same parents. These are the
// errors the walk raises on its own, and they must be unchanged -- the dot
// rule is only allowed to answer where they do not.
static void ordinary_names(void) {
    char t[512];

    snprintf(t, sizeof t, "%s/gone/x", dir);
    errno = 0; check("mkdir(missing-parent/x)",  mkdir(t, 0755),                 ENOENT);
    errno = 0; check("mknod(missing-parent/x)",  mknod(t, S_IFIFO | 0644, 0),    ENOENT);
    errno = 0; check("unlink(missing-parent/x)", unlink(t),                      ENOENT);
    errno = 0; check("rmdir(missing-parent/x)",  rmdir(t),                       ENOENT);

    snprintf(t, sizeof t, "%s/file/x", dir);
    errno = 0; check("mkdir(file-as-parent/x)",  mkdir(t, 0755),                 ENOTDIR);
    errno = 0; check("unlink(file-as-parent/x)", unlink(t),                      ENOTDIR);

    snprintf(t, sizeof t, "%s/symgone/x", dir);
    errno = 0; check("mkdir(dangling-link-as-parent/x)", mkdir(t, 0755),         ENOENT);

    // ...and an ordinary name in a parent that IS there still works, which is
    // what says the new walk did not start refusing good paths.
    snprintf(t, sizeof t, "%s/adir/x", dir);
    errno = 0;
    if (mkdir(t, 0755) < 0)
        failf("mkdir in a good parent must still work", 0, (uint64_t) errno, 0, 0, 0, 0);
    errno = 0;
    if (rmdir(t) < 0)
        failf("rmdir in a good parent must still work", 0, (uint64_t) errno, 0, 0, 0, 0);
    // "." and ".." in the MIDDLE of a path are not this rule at all.
    snprintf(t, sizeof t, "%s/adir/./x", dir);
    errno = 0;
    if (mkdir(t, 0755) < 0)
        failf("mkdir through a middle dot must still work", 0, (uint64_t) errno, 0, 0, 0, 0);
    errno = 0;
    if (rmdir(t) < 0)
        failf("rmdir through a middle dot must still work", 0, (uint64_t) errno, 0, 0, 0, 0);
    snprintf(t, sizeof t, "%s/adir/../adir/x", dir);
    errno = 0;
    if (mkdir(t, 0755) < 0)
        failf("mkdir through a middle dotdot must still work", 0, (uint64_t) errno, 0, 0, 0, 0);
    errno = 0;
    if (rmdir(t) < 0)
        failf("rmdir through a middle dotdot must still work", 0, (uint64_t) errno, 0, 0, 0, 0);
}

// ------------------------------------------------------------ unprivileged

// The half of the order that only an unprivileged caller can see: `d` holds
// "nowrite" (0555) and "nosearch" (0644), and root is bound by neither.
static int run_unprivileged(const char *d) {
    char p[400];
    test_logf("unprivileged leg in %s (uid %d):\n", d, (int) geteuid());

    // Unwritable but reachable: the dot answers, ahead of the permission.
    // This is the original bug -- `ln -s /bin/sh .` -- and must not come back.
    snprintf(p, sizeof p, "%s/nowrite", d);
    run_parent("nowrite", p, 0, 2);

    // Unsearchable: the walk cannot arrive, so it answers instead.
    snprintf(p, sizeof p, "%s/nosearch", d);
    run_parent("nosearch", p, EACCES, 2);

    // ...and the deferral grants nothing: an ordinary new name in the
    // unwritable parent is still refused.
    {
        char t[512];
        snprintf(t, sizeof t, "%s/nowrite/x", d);
        errno = 0; check("mkdir(unwritable/x)",   mkdir(t, 0755),              EACCES);
        errno = 0; check("symlink(unwritable/x)", symlink("target", t),        EACCES);
        errno = 0; check("mknod(unwritable/x)",   mknod(t, S_IFIFO | 0644, 0), EACCES);
    }
    return failures_total == 0 ? 0 : 1;
}

// --------------------------------------------------------------------- main

static int make_fixtures(const char *d, int unpriv) {
    char t[512];
    if (mkdir(d, 0755) < 0)
        return -1;
    snprintf(t, sizeof t, "%s/adir", d);
    if (mkdir(t, 0755) < 0)
        return -1;
    snprintf(t, sizeof t, "%s/file", d);
    int fd = open(t, O_CREAT | O_WRONLY, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    snprintf(t, sizeof t, "%s/symdir", d);
    symlink("adir", t);
    snprintf(t, sizeof t, "%s/symfile", d);
    symlink("file", t);
    snprintf(t, sizeof t, "%s/symgone", d);
    symlink("ghost", t);
    if (!unpriv)
        return 0;
    snprintf(t, sizeof t, "%s/nowrite", d);
    if (mkdir(t, 0755) < 0)
        return -1;
    snprintf(t, sizeof t, "%s/nosearch", d);
    if (mkdir(t, 0755) < 0)
        return -1;
    snprintf(t, sizeof t, "%s/nowrite", d);
    if (chmod(t, 0555) < 0)                   // searchable, not writable
        return -1;
    snprintf(t, sizeof t, "%s/nosearch", d);
    return chmod(t, 0644);                    // readable, not searchable
}

static void drop_fixtures(const char *d) {
    char t[512];
    static const char *names[] = {"symdir", "symfile", "symgone", "file", "ghost",
                                  "target", NULL};
    chmod(d, 0755);
    for (unsigned i = 0; names[i] != NULL; i++) {
        snprintf(t, sizeof t, "%s/%s", d, names[i]);
        unlink(t);
    }
    snprintf(t, sizeof t, "%s/nowrite", d);  chmod(t, 0755); rmdir(t);
    snprintf(t, sizeof t, "%s/nosearch", d); chmod(t, 0755); rmdir(t);
    snprintf(t, sizeof t, "%s/adir", d);     rmdir(t);
    rmdir(d);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    char base[192];
    snprintf(base, sizeof base, "/tmp/final_dot_order.%d", (int) getpid());
    snprintf(src, sizeof src, "%s.src", base);
    snprintf(newname, sizeof newname, "%s.new", base);
    // 0666 so an unprivileged caller may still hard-link it: Linux's
    // protected_hardlinks refuses a link to a file the caller can neither read
    // nor write, and that EPERM would answer before the errors this test is
    // about.
    int fd = open(src, O_CREAT | O_WRONLY, 0666);
    if (fd < 0) {
        printf("fs_final_dot_order: SKIP (cannot create %s: %s)\n", src, strerror(errno));
        return 0;
    }
    close(fd);
    chmod(src, 0666);

    snprintf(dir, sizeof dir, "%s.w", base);
    if (make_fixtures(dir, 0) < 0) {
        printf("fs_final_dot_order: SKIP (cannot set up %s: %s)\n", dir, strerror(errno));
        unlink(src);
        return 0;
    }

    char p[400];
    // (a) a parent that is there and writable: the dot rule answers.
    snprintf(p, sizeof p, "%s/adir", dir);
    run_parent("adir", p, 0, 4);
    // ...including when the parent is reached through a symlink.
    snprintf(p, sizeof p, "%s/symdir", dir);
    run_parent("symdir", p, 0, 2);
    // (c) a parent that is not there, spelled three ways.
    snprintf(p, sizeof p, "%s/gone", dir);
    run_parent("gone", p, ENOENT, 4);
    snprintf(p, sizeof p, "%s/symgone", dir);
    run_parent("symgone", p, ENOENT, 2);
    snprintf(p, sizeof p, "%s/adir/deeper/still", dir);
    run_parent("deep-missing", p, ENOENT, 2);
    // (d) a parent that is a regular file, directly and through a link.
    snprintf(p, sizeof p, "%s/file", dir);
    run_parent("file", p, ENOTDIR, 4);
    snprintf(p, sizeof p, "%s/symfile", dir);
    run_parent("symfile", p, ENOTDIR, 2);

    ordinary_names();
    drop_fixtures(dir);

    // (b) an unwritable parent, plus an unsearchable one. Both need a caller
    // the mode bits actually bind, so drop to an unprivileged uid in a child;
    // an already unprivileged caller just runs it, which is how this runs on
    // the Linux oracle.
    char udir[256];
    snprintf(udir, sizeof udir, "%s.u", base);
    if (make_fixtures(udir, 1) < 0) {
        printf("fs_final_dot_order: SKIP unprivileged leg (cannot set up %s: %s)\n",
               udir, strerror(errno));
    } else if (geteuid() == 0) {
        fflush(NULL);
        pid_t c = fork();
        if (c == 0) {
            if (setgid(1000) < 0 || setuid(1000) < 0)
                _exit(70);
            int crc = run_unprivileged(udir);
            fflush(NULL);        // _exit skips the flush; the diagnosis lives here
            _exit(crc);
        }
        int st = 0;
        while (waitpid(c, &st, 0) < 0 && errno == EINTR)
            continue;
        int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (code == 70)
            printf("fs_final_dot_order: SKIP unprivileged leg (cannot drop privileges)\n");
        else if (code != 0)
            failf("unprivileged final-dot ordering", (uint64_t) code, (uint64_t) st, 0, 0, 0, 0);
        drop_fixtures(udir);
    } else {
        run_unprivileged(udir);
        drop_fixtures(udir);
    }

    unlink(src);
    unlink(newname);
    return finish_suite("fs_final_dot_order");
}
