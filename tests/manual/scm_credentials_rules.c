// scm_credentials_rules.c -- who may send SCM_CREDENTIALS on a unix socket,
// and which pid a unix socket credential reports.
//
// GLib's D-Bus client sends its credentials over the bus socket from its
// worker thread: one byte with SCM_CREDENTIALS carrying getpid(), geteuid()
// and getegid(). AOK refused that with EPERM, so a GLib program with a worker
// thread could not connect to a session bus at all. waybar in the Wayland
// applet printed "Error sending credentials: Error sending message: Operation
// not permitted" and exited.
//
// The rule it has to follow is Linux's scm_check_creds (net/core/scm.c):
//   - the pid must be the sender's PROCESS id (its thread group), from any
//     thread, unless the sender has CAP_SYS_ADMIN;
//   - the uid must be the real, effective or saved uid, unless CAP_SETUID;
//     the gid likewise with CAP_SETGID;
//   - a uid or gid of -1 names nobody, and is EINVAL before anything else.
// AOK compared the pid with the sending thread's own id and the uid and gid
// with the effective ids only. The credentials it reported had the same flaw:
// SO_PASSCRED and SO_PEERCRED carried the id of whichever thread sent or
// connected, where Linux reports the process id.
//
// Root and non-root expect different answers for a foreign pid, and only root
// can make a real uid differ from the effective one, so those cases are split
// on geteuid(). The unprivileged cases were checked against Linux; the root-only
// ones follow scm_check_creds.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef SCM_CREDENTIALS
#define SCM_CREDENTIALS 2
#endif

static void check(const char *label, int cond) {
    if (cond) {
        test_logf("ok   %s\n", label);
    } else {
        printf("FAIL %s\n", label);
        failures_total++;
    }
}

// Sends one byte, with explicit credentials when `with_creds`. Returns 0 or
// the errno.
static int send_byte(int fd, int with_creds, pid_t pid, uid_t uid, gid_t gid) {
    char byte = 'x';
    struct iovec iov = {.iov_base = &byte, .iov_len = 1};
    char control[CMSG_SPACE(sizeof(struct ucred))];
    memset(control, 0, sizeof control);
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    if (with_creds) {
        msg.msg_control = control;
        msg.msg_controllen = sizeof control;
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_CREDENTIALS;
        c->cmsg_len = CMSG_LEN(sizeof(struct ucred));
        struct ucred cred = {.pid = pid, .uid = uid, .gid = gid};
        memcpy(CMSG_DATA(c), &cred, sizeof cred);
    }
    return sendmsg(fd, &msg, MSG_NOSIGNAL) == 1 ? 0 : errno;
}

// Receives one byte and whatever SCM_CREDENTIALS came with it. Returns 1 when a
// credential arrived, 0 when none did, -1 when the byte did not.
static int recv_byte_cred(int fd, struct ucred *out) {
    char byte;
    struct iovec iov = {.iov_base = &byte, .iov_len = 1};
    char control[256];
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = control, .msg_controllen = sizeof control};
    if (recvmsg(fd, &msg, 0) != 1)
        return -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS) {
            memcpy(out, CMSG_DATA(c), sizeof *out);
            return 1;
        }
    }
    return 0;
}

struct thread_job {
    int fd;
    int with_creds;
    struct ucred cred;
    int err;
    pid_t tid;
};

static void *send_from_thread(void *arg) {
    struct thread_job *job = arg;
    job->tid = (pid_t) syscall(SYS_gettid);
    job->err = send_byte(job->fd, job->with_creds, job->cred.pid, job->cred.uid, job->cred.gid);
    return NULL;
}

static int run_in_thread(struct thread_job *job) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, send_from_thread, job) != 0)
        return -1;
    pthread_join(thread, NULL);
    return 0;
}

static int passcred_pair(int type, int sv[2]) {
    if (socketpair(AF_UNIX, type, 0, sv) != 0)
        return -1;
    int one = 1;
    return setsockopt(sv[1], SOL_SOCKET, SO_PASSCRED, &one, sizeof one);
}

// The GDBus shape: a worker thread sends the process's own credentials.
static void test_thread_sends_process_creds(int type, const char *name) {
    int sv[2];
    char label[160];
    if (passcred_pair(type, sv) != 0) {
        snprintf(label, sizeof label, "%s: socketpair + SO_PASSCRED (%s)", name, strerror(errno));
        check(label, 0);
        return;
    }
    struct thread_job job = {.fd = sv[0], .with_creds = 1,
                             .cred = {.pid = getpid(), .uid = geteuid(), .gid = getegid()}};
    if (run_in_thread(&job) != 0) {
        check("pthread_create", 0);
        goto out;
    }
    // Positive control: the sending thread really is not the process's first
    // thread, or this case tests nothing.
    snprintf(label, sizeof label, "%s: worker thread id %d differs from pid %d", name, job.tid, getpid());
    check(label, job.tid != getpid());

    snprintf(label, sizeof label,
             "%s: a worker thread sends pid=getpid() uid=geteuid() gid=getegid() (err=%d %s)",
             name, job.err, strerror(job.err));
    check(label, job.err == 0);
    if (job.err != 0)
        goto out;

    struct ucred got;
    int have = recv_byte_cred(sv[1], &got);
    snprintf(label, sizeof label, "%s: the receiver gets a credential (have=%d)", name, have);
    check(label, have == 1);
    if (have == 1) {
        snprintf(label, sizeof label, "%s: it carries pid %d uid %u gid %u (got %d %u %u)", name,
                 getpid(), (unsigned) geteuid(), (unsigned) getegid(), got.pid, got.uid, got.gid);
        check(label, got.pid == getpid() && got.uid == geteuid() && got.gid == getegid());
    }
out:
    close(sv[0]);
    close(sv[1]);
}

// No explicit credentials: the kernel attaches the sender's own, and the pid in
// them is the process id even when a worker thread sent the message.
static void test_attached_cred_is_process_id(int type, const char *name) {
    int sv[2];
    char label[160];
    if (passcred_pair(type, sv) != 0) {
        snprintf(label, sizeof label, "%s: socketpair + SO_PASSCRED (%s)", name, strerror(errno));
        check(label, 0);
        return;
    }
    struct thread_job job = {.fd = sv[0], .with_creds = 0};
    if (run_in_thread(&job) != 0) {
        check("pthread_create", 0);
        goto out;
    }
    snprintf(label, sizeof label, "%s: a worker thread sends a plain byte (err=%d)", name, job.err);
    check(label, job.err == 0);
    struct ucred got;
    int have = recv_byte_cred(sv[1], &got);
    snprintf(label, sizeof label, "%s: the receiver gets a credential (have=%d)", name, have);
    check(label, have == 1);
    if (have == 1) {
        snprintf(label, sizeof label, "%s: its pid is the process id %d, not thread %d (got %d)",
                 name, getpid(), job.tid, got.pid);
        check(label, got.pid == getpid());
    }
out:
    close(sv[0]);
    close(sv[1]);
}

struct connect_job {
    const char *path;
    int fd;
    int err;
};

static void *connect_from_thread(void *arg) {
    struct connect_job *job = arg;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", job->path);
    job->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    job->err = job->fd < 0 ? errno
        : (connect(job->fd, (struct sockaddr *) &addr, sizeof addr) == 0 ? 0 : errno);
    return NULL;
}

// SO_PEERCRED on the accepting side names the process that connected, even
// when a worker thread made the connection.
static void test_peercred_is_process_id(void) {
    char label[160];
    char path[64];
    snprintf(path, sizeof path, "/tmp/scmcred-%d.sock", (int) getpid());
    unlink(path);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (listener < 0 || bind(listener, (struct sockaddr *) &addr, sizeof addr) != 0 ||
            listen(listener, 1) != 0) {
        snprintf(label, sizeof label, "peercred: listen on %s (%s)", path, strerror(errno));
        check(label, 0);
        if (listener >= 0)
            close(listener);
        return;
    }
    struct connect_job job = {.path = path, .fd = -1};
    pthread_t thread;
    if (pthread_create(&thread, NULL, connect_from_thread, &job) != 0) {
        check("pthread_create", 0);
        close(listener);
        unlink(path);
        return;
    }
    int conn = accept(listener, NULL, NULL);
    pthread_join(thread, NULL);
    snprintf(label, sizeof label, "peercred: a worker thread connects (err=%d), accept=%d", job.err, conn);
    check(label, job.err == 0 && conn >= 0);
    if (conn >= 0) {
        struct ucred cred;
        socklen_t len = sizeof cred;
        int r = getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cred, &len);
        snprintf(label, sizeof label, "peercred: SO_PEERCRED names pid %d uid %u (r=%d got %d %u)",
                 getpid(), (unsigned) geteuid(), r, cred.pid, cred.uid);
        check(label, r == 0 && cred.pid == getpid() && cred.uid == geteuid());
        close(conn);
    }
    if (job.fd >= 0)
        close(job.fd);
    close(listener);
    unlink(path);
}

// Values that are not the sender's own.
static void test_foreign_and_invalid(void) {
    int sv[2];
    char label[160];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        check("socketpair", 0);
        return;
    }
    int root = geteuid() == 0;

    // A pid that belongs to another process: CAP_SYS_ADMIN may, others may not.
    int e = send_byte(sv[0], 1, getppid(), geteuid(), getegid());
    snprintf(label, sizeof label, "foreign pid %d as %s: %s (err=%d %s)", (int) getppid(),
             root ? "root" : "non-root", root ? "allowed" : "EPERM", e, strerror(e));
    check(label, root ? e == 0 : e == EPERM);

    // A uid of -1 maps to no user: EINVAL for everyone, before any permission.
    e = send_byte(sv[0], 1, getpid(), (uid_t) -1, getegid());
    snprintf(label, sizeof label, "uid -1: EINVAL (err=%d %s)", e, strerror(e));
    check(label, e == EINVAL);
    e = send_byte(sv[0], 1, getpid(), geteuid(), (gid_t) -1);
    snprintf(label, sizeof label, "gid -1: EINVAL (err=%d %s)", e, strerror(e));
    check(label, e == EINVAL);

    close(sv[0]);
    close(sv[1]);
}

// Root only: with the effective ids dropped and the real and saved ids still 0,
// the real uid and gid are accepted and a stranger's are not.
static void test_real_and_saved_ids(void) {
    if (geteuid() != 0) {
        test_logf("skip real/saved id case: needs root to make the ids differ\n");
        return;
    }
    int report[2];
    if (pipe(report) != 0) {
        check("pipe", 0);
        return;
    }
    pid_t child = fork();
    if (child == 0) {
        close(report[0]);
        int codes[3] = {-1, -1, -1};
        int sv[2];
        if (setresgid(0, 65534, 0) == 0 && setresuid(0, 65534, 0) == 0 &&
                socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            codes[0] = send_byte(sv[0], 1, getpid(), 0, 0);
            codes[1] = send_byte(sv[0], 1, getpid(), 12345, getegid());
            codes[2] = send_byte(sv[0], 1, getpid(), geteuid(), 12345);
        }
        if (write(report[1], codes, sizeof codes) != sizeof codes)
            _exit(3);
        _exit(0);
    }
    close(report[1]);
    int codes[3] = {-2, -2, -2};
    ssize_t n = read(report[0], codes, sizeof codes);
    close(report[0]);
    int status = 0;
    waitpid(child, &status, 0);
    char label[160];
    snprintf(label, sizeof label, "effective 65534, real/saved 0: child reported (n=%zd status=%d)", n, status);
    check(label, n == sizeof codes && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    snprintf(label, sizeof label, "  sends the real uid and gid 0 (err=%d %s)", codes[0], strerror(codes[0] > 0 ? codes[0] : 0));
    check(label, codes[0] == 0);
    snprintf(label, sizeof label, "  a stranger's uid is EPERM (err=%d)", codes[1]);
    check(label, codes[1] == EPERM);
    snprintf(label, sizeof label, "  a stranger's gid is EPERM (err=%d)", codes[2]);
    check(label, codes[2] == EPERM);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    test_logf("pid %d euid %u egid %u\n", getpid(), (unsigned) geteuid(), (unsigned) getegid());

    test_thread_sends_process_creds(SOCK_STREAM, "stream");
    test_thread_sends_process_creds(SOCK_DGRAM, "dgram");
    test_attached_cred_is_process_id(SOCK_STREAM, "stream");
    test_attached_cred_is_process_id(SOCK_DGRAM, "dgram");
    test_peercred_is_process_id();
    test_foreign_and_invalid();
    test_real_and_saved_ids();

    return finish_suite("scm_credentials_rules");
}
