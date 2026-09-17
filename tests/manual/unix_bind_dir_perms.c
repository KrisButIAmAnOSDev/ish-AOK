// bind() of a filesystem AF_UNIX socket creates a name, and must be allowed to.
//
// Linux's unix_bind_bsd() resolves the path with kern_path_create() and makes
// the node with vfs_mknod(), exactly as mknod(2) would:
//
//   - search permission on every directory on the way (EACCES), a missing
//     component (ENOENT), a component that is not a directory (ENOTDIR);
//   - the final component is NOT followed, and a name that exists -- a file, a
//     directory, a dangling symlink -- is EEXIST, which bind reports as
//     EADDRINUSE, before any permission is asked;
//   - "name/" asks for a directory and a socket is not one: ENOENT;
//   - a read-only mount is EROFS;
//   - then write and search permission on the parent (EACCES), which
//     CAP_DAC_OVERRIDE bypasses. A sticky directory restricts removing names,
//     not creating them.
//
// AOK only ever checked the lookup. bind asked nothing of the parent directory,
// so an unprivileged guest bound sockets in root's directories -- /usr/lib, or a
// /tmp/.X11-unix a root session had left 0755, where a later non-root labwc
// then bound X1 -- and it followed a final symlink, creating the socket at its
// target, and created "name/" as "name".
//
// Everything unprivileged below was measured on Linux 6.12 x86_64 as uid 1000,
// 64-bit and -m32. Run as root, the test checks the unprivileged rules in a
// child that drops to uid 1000 (a root run otherwise tests nothing), and adds
// the root-only cases -- DAC override and EROFS -- which the oracle account
// cannot reach; those follow from generic_permission() and filename_create().
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define UNPRIV_UID 1000
#define UNPRIV_GID 1000

static socklen_t make_addr(struct sockaddr_un *a, const char *path, int abstract) {
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    size_t n = strlen(path);
    if (abstract) {
        memcpy(a->sun_path + 1, path, n);
        return (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + n);
    }
    memcpy(a->sun_path, path, n + 1);
    return (socklen_t) (offsetof(struct sockaddr_un, sun_path) + n + 1);
}

// bind a fresh socket; 0 or the errno.
static int bind_err(const char *path, int abstract) {
    struct sockaddr_un a;
    socklen_t len = make_addr(&a, path, abstract);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        return errno;
    int err = bind(s, (struct sockaddr *) &a, len) == 0 ? 0 : errno;
    close(s);
    return err;
}

static int connect_err(const char *path) {
    struct sockaddr_un a;
    socklen_t len = make_addr(&a, path, 0);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        return errno;
    int err = connect(s, (struct sockaddr *) &a, len) == 0 ? 0 : errno;
    close(s);
    return err;
}

static void expect(const char *label, int got, int want) {
    if (got != want) {
        printf("FAIL %s: %s, want %s\n", label, got ? strerror(got) : "success",
               want ? strerror(want) : "success");
        failures_total++;
    } else {
        test_logf("%s: %s ok\n", label, got ? strerror(got) : "success");
    }
}

static void expect_absent(const char *label, const char *path) {
    struct stat st;
    if (lstat(path, &st) == 0) {
        printf("FAIL %s: %s exists\n", label, path);
        failures_total++;
    } else {
        test_logf("%s: ok\n", label);
    }
}

static void unprivileged_rules(void) {
    uid_t uid = geteuid();
    char dir[96], p[160], q[160];
    snprintf(dir, sizeof(dir), "/tmp/aok-ubind-%d-%d", (int) getpid(), (int) uid);
    umask(022);
    if (mkdir(dir, 0700) != 0) {
        printf("FAIL mkdir %s: %s\n", dir, strerror(errno));
        failures_total++;
        return;
    }

    // Root's directory. A wrong success leaves a socket there that this uid
    // cannot remove; a root run's parent cleans it up.
    struct stat root_st;
    if (stat("/", &root_st) == 0 && root_st.st_uid == 0 && (root_st.st_mode & 0022) == 0) {
        snprintf(p, sizeof(p), "/aok-ubind-%d.sock", (int) getpid());
        expect("bind in root's 0755 /", bind_err(p, 0), EACCES);
        expect_absent("  and nothing was created", p);
        if (chdir("/") == 0) {
            snprintf(q, sizeof(q), "aok-ubind-rel-%d.sock", (int) getpid());
            expect("bind a relative name with cwd /", bind_err(q, 0), EACCES);
        }
    } else {
        test_logf("/ is writable here; skipped the root-directory cases\n");
    }
    if (chdir("/tmp") != 0) {
        printf("FAIL chdir /tmp: %s\n", strerror(errno));
        failures_total++;
    }

    snprintf(p, sizeof(p), "%s/s", dir);
    chmod(dir, 0500);
    expect("bind in a 0500 directory (no write)", bind_err(p, 0), EACCES);
    chmod(dir, 0600);
    expect("bind in a 0600 directory (no search)", bind_err(p, 0), EACCES);
    chmod(dir, 0300);
    expect("bind in a 0300 directory (write and search)", bind_err(p, 0), 0);
    struct stat st;
    if (lstat(p, &st) == 0) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != uid || (st.st_mode & 07777) != 0755) {
            printf("FAIL the socket is mode %o uid %d, want a socket, 0755, uid %d\n",
                   (unsigned) st.st_mode, (int) st.st_uid, (int) uid);
            failures_total++;
        }
        unlink(p);
    }
    chmod(dir, 0700);

    snprintf(q, sizeof(q), "%s/sub", dir);
    mkdir(q, 0600);
    snprintf(p, sizeof(p), "%s/sub/s", dir);
    expect("bind below a directory without search", bind_err(p, 0), EACCES);
    chmod(q, 0700);
    rmdir(q);

    snprintf(p, sizeof(p), "/aok-ubind-missing-%d/s", (int) getpid());
    expect("bind below a missing directory in root's /", bind_err(p, 0), ENOENT);
    expect("bind over /etc/passwd (exists; directory not writable)",
           bind_err("/etc/passwd", 0), EADDRINUSE);
    expect("bind over /tmp (an existing directory)", bind_err("/tmp", 0), EADDRINUSE);
    expect("bind below a regular file", bind_err("/etc/passwd/s", 0), ENOTDIR);

    // /tmp is sticky and not ours: sticky governs removal, not creation.
    snprintf(p, sizeof(p), "/tmp/aok-ubind-sticky-%d-%d.sock", (int) getpid(), (int) uid);
    expect("bind in sticky /tmp", bind_err(p, 0), 0);
    expect("  and unlink it", unlink(p) == 0 ? 0 : errno, 0);

    if (chdir("/") == 0) {
        snprintf(q, sizeof(q), "aok-ubind-abstract-%d-%d", (int) getpid(), (int) uid);
        expect("abstract bind with cwd /", bind_err(q, 1), 0);
        if (chdir("/tmp") != 0) {
            printf("FAIL chdir /tmp: %s\n", strerror(errno));
            failures_total++;
        }
    }

    snprintf(p, sizeof(p), "%s/dangling", dir);
    snprintf(q, sizeof(q), "%s/target", dir);
    if (symlink(q, p) == 0) {
        expect("bind over a dangling symlink", bind_err(p, 0), EADDRINUSE);
        expect_absent("  and its target was not created", q);
        unlink(q);
        unlink(p);
    }

    snprintf(q, sizeof(q), "%s/real", dir);
    mkdir(q, 0700);
    snprintf(p, sizeof(p), "%s/link", dir);
    if (symlink(q, p) == 0) {
        snprintf(p, sizeof(p), "%s/link/s", dir);
        expect("bind below a symlink to a directory", bind_err(p, 0), 0);
        unlink(p);
        snprintf(p, sizeof(p), "%s/link", dir);
        unlink(p);
    }
    rmdir(q);

    snprintf(p, sizeof(p), "%s/slash/", dir);
    expect("bind \"name/\"", bind_err(p, 0), ENOENT);
    snprintf(q, sizeof(q), "%s/slash", dir);
    expect_absent("  and nothing was created", q);
    unlink(q);

    // The lookup side, which was already right: connect needs search on the
    // way and write on the socket.
    snprintf(p, sizeof(p), "%s/srv", dir);
    struct sockaddr_un a;
    socklen_t len = make_addr(&a, p, 0);
    int l = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (bind(l, (struct sockaddr *) &a, len) == 0 && listen(l, 1) == 0) {
        chmod(dir, 0600);
        expect("connect below a directory without search", connect_err(p), EACCES);
        chmod(dir, 0700);
        chmod(p, 0555);
        expect("connect to a socket without write", connect_err(p), EACCES);
        chmod(p, 0755);
    } else {
        printf("FAIL listening socket for the connect cases: %s\n", strerror(errno));
        failures_total++;
    }
    close(l);
    unlink(p);
    rmdir(dir);
}

static void root_rules(void) {
    char dir[96], p[160];
    snprintf(dir, sizeof(dir), "/tmp/aok-ubind-root-%d", (int) getpid());
    umask(022);
    if (mkdir(dir, 0700) != 0) {
        printf("FAIL mkdir %s: %s\n", dir, strerror(errno));
        failures_total++;
        return;
    }
    snprintf(p, sizeof(p), "%s/s", dir);
    // DAC override: neither write nor search is needed.
    if (chown(dir, UNPRIV_UID, UNPRIV_GID) == 0 && chmod(dir, 0500) == 0) {
        expect("root: bind in another user's 0500 directory", bind_err(p, 0), 0);
        unlink(p);
    }
    chmod(dir, 0000);
    expect("root: bind in a 0000 directory", bind_err(p, 0), 0);
    unlink(p);
    chmod(dir, 0700);

    // A read-only mount refuses the create.
    char ro[128];
    snprintf(ro, sizeof(ro), "%s/ro", dir);
    mkdir(ro, 0755);
    if (mount("none", ro, "tmpfs", MS_RDONLY, NULL) == 0) {
        snprintf(p, sizeof(p), "%s/s", ro);
        expect("root: bind on a read-only mount", bind_err(p, 0), EROFS);
        umount2(ro, MNT_DETACH);
    } else {
        test_logf("no read-only tmpfs (%s); skipped EROFS\n", strerror(errno));
    }
    rmdir(ro);
    rmdir(dir);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    if (geteuid() != 0) {
        unprivileged_rules();
        return finish_suite("unix_bind_dir_perms");
    }

    root_rules();

    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        if (setgid(UNPRIV_GID) != 0 || setuid(UNPRIV_UID) != 0) {
            printf("FAIL could not drop to uid %d: %s\n", UNPRIV_UID, strerror(errno));
            fflush(NULL);
            _exit(1);
        }
        failures_total = 0;
        unprivileged_rules();
        fflush(NULL);
        _exit(failures_total > 250 ? 250 : (int) failures_total);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid) {
        failures_total++;
    } else if (WIFSIGNALED(status)) {
        printf("FAIL the unprivileged child died on signal %d\n", WTERMSIG(status));
        failures_total++;
    } else {
        failures_total += (unsigned) WEXITSTATUS(status);
    }

    // Whatever a wrong success left where the child could not remove it.
    char p[96];
    snprintf(p, sizeof(p), "/aok-ubind-%d.sock", (int) pid);
    unlink(p);
    snprintf(p, sizeof(p), "/aok-ubind-rel-%d.sock", (int) pid);
    unlink(p);
    snprintf(p, sizeof(p), "rm -rf /tmp/aok-ubind-%d-%d", (int) pid, UNPRIV_UID);
    if (system(p) < 0)
        failures_total++;

    return finish_suite("unix_bind_dir_perms");
}
