// Linux lets an AF_UNIX SOCK_STREAM client pass file descriptors with
// SCM_RIGHTS as soon as connect(2) returns. The transport connection exists
// at that point -- accept(2) only hands the server a descriptor for a child
// socket that is already connected -- so the server's schedule is irrelevant
// to the client, and a client that connects and immediately sendmsg()s an fd
// is doing something completely ordinary. Wayland clients, D-Bus and systemd's
// socket activation all rely on it.
//
// AOK gave that client EPIPE. AF_UNIX peers there are linked by an 8-byte
// cookie the connect side writes as the first bytes on the wire, which only
// the ACCEPT side consumes (unix_socket_finish_peer); sys_connect_common
// deliberately does not wait for that acknowledgement, because waiting wedges
// clients of daemons that accept asynchronously. So between connect() and the
// server's accept() the client socket has no unix_peer -- and the SCM_RIGHTS
// path needs one, because a stream parcel is queued on the RECEIVING fd,
// which does not exist yet. sendmsg took the EPIPE branch.
//
// Observed as tests/manual/wayland_scm_shm's test_named_socket failing 3 runs
// in 15 with "sendmsg ... Broken pipe" from the child, on Devuan arm64 as an
// unprivileged user, with the rest of the subtest's failures a cascade of it.
//
// Also passes on real Linux (camd oracle, 64-bit and -m32, verified).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define PAYLOAD "scm-before-accept\n"

static char sock_path[96];
static char file_path[96];
// Every bounded wait in this test, parent and child alike, scaled by
// ISH_TEST_WATCHDOG_SCALE with the rest of the suite. Set in main().
static int wait_ms = 15000;

static void check(int cond, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!cond) {
        printf("FAIL ");
        vprintf(fmt, ap);
        printf("\n");
        failures_total++;
    } else if (test_verbose) {
        printf("ok ");
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
}

static int mklistener(const char *path) {
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0 || listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int connect_to(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Send one fd plus one byte of ordinary data. Returns 0, or -errno.
static int send_one_fd(int s, int fd, char payload) {
    char buf[1] = { payload };
    struct iovec iov = { .iov_base = buf, .iov_len = 1 };
    union {
        struct cmsghdr align;
        char b[CMSG_SPACE(sizeof(int))];
    } cbuf;
    memset(&cbuf, 0, sizeof cbuf);
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf.b;
    msg.msg_controllen = sizeof cbuf.b;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    errno = 0;
    ssize_t n = sendmsg(s, &msg, 0);
    if (n != 1)
        return errno != 0 ? -errno : -EIO;
    return 0;
}

// Receive one fd plus one data byte, waiting at most `ms`. Returns 0, or
// -errno; on success *out_fd is the received descriptor (-1 if none came).
static int recv_one_fd(int s, int *out_fd, char *payload, int ms) {
    *out_fd = -1;
    struct pollfd pfd = { .fd = s, .events = POLLIN };
    int r = poll(&pfd, 1, ms);
    if (r == 0)
        return -ETIMEDOUT;
    if (r < 0)
        return -errno;
    char buf[1] = { 0 };
    struct iovec iov = { .iov_base = buf, .iov_len = 1 };
    union {
        struct cmsghdr align;
        char b[CMSG_SPACE(sizeof(int) * 4)];
    } cbuf;
    memset(&cbuf, 0, sizeof cbuf);
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf.b;
    msg.msg_controllen = sizeof cbuf.b;
    errno = 0;
    ssize_t n = recvmsg(s, &msg, 0);
    if (n != 1)
        return errno != 0 ? -errno : -EIO;
    *payload = buf[0];
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            memcpy(out_fd, CMSG_DATA(c), sizeof(int));
            break;
        }
    }
    return 0;
}

// Read the whole of a received descriptor and compare it with PAYLOAD.
static int fd_carries_payload(int fd) {
    char buf[sizeof(PAYLOAD)];
    memset(buf, 0, sizeof buf);
    if (lseek(fd, 0, SEEK_SET) == (off_t) -1)
        return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    if (n < 0)
        return 0;
    buf[n] = '\0';
    return strcmp(buf, PAYLOAD) == 0;
}

// One file per client, named after the byte that client sends: two clients
// sharing one path would truncate it under each other.
static int open_payload_file(char tag) {
    char path[160];
    snprintf(path, sizeof path, "%s.%c", file_path, tag);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    if (write(fd, PAYLOAD, strlen(PAYLOAD)) != (ssize_t) strlen(PAYLOAD)) {
        close(fd);
        return -1;
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

// The client half of a subtest: connect, optionally wait to be released, send
// one fd, and report the result through `report`. Never returns.
static void client_child(int release_rd, int report_wr, char payload) {
    int c = connect_to(sock_path);
    if (c < 0) {
        int e = -errno;
        (void) !write(report_wr, &e, sizeof e);
        _exit(0);
    }
    if (release_rd >= 0) {
        // Ordered the other way round (server accepts first): wait for it.
        char go;
        struct pollfd pfd = { .fd = release_rd, .events = POLLIN };
        if (poll(&pfd, 1, wait_ms) != 1 || read(release_rd, &go, 1) != 1) {
            int e = -ETIMEDOUT;
            (void) !write(report_wr, &e, sizeof e);
            _exit(0);
        }
    }
    int fd = open_payload_file(payload);
    if (fd < 0) {
        int e = -errno;
        (void) !write(report_wr, &e, sizeof e);
        _exit(0);
    }
    int err = send_one_fd(c, fd, payload);
    (void) !write(report_wr, &err, sizeof err);
    close(fd);
    // Hold the connection open until the parent has accepted and read: an
    // immediate close would race the accept on a stream whose data is still
    // in flight.
    char done;
    struct pollfd pfd = { .fd = c, .events = POLLIN };
    (void) poll(&pfd, 1, wait_ms);
    (void) read(c, &done, 1);
    close(c);
    _exit(0);
}

// Wait for the child's report with a bound. Returns 0 and fills *err, or -1.
static int wait_report(int report_rd, int *err, int ms) {
    struct pollfd pfd = { .fd = report_rd, .events = POLLIN };
    if (poll(&pfd, 1, ms) != 1)
        return -1;
    return read(report_rd, err, sizeof *err) == (ssize_t) sizeof *err ? 0 : -1;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    wait_ms = (int) test_watchdog_secs(1) * 15000;

    snprintf(sock_path, sizeof sock_path, "/tmp/scm_ba.%d.sock", (int) getpid());
    snprintf(file_path, sizeof file_path, "/tmp/scm_ba.%d.dat", (int) getpid());

    // 1. The bug: sendmsg(SCM_RIGHTS) issued after connect() but BEFORE the
    // server's accept() must succeed, and the fd must arrive intact once the
    // server does accept.
    int lfd = mklistener(sock_path);
    check(lfd >= 0, "listener created (%s)", strerror(errno));
    if (lfd >= 0) {
        int report[2];
        check(pipe(report) == 0, "report pipe");
        fflush(NULL);
        pid_t kid = fork();
        if (kid == 0) {
            close(lfd);
            close(report[0]);
            client_child(-1, report[1], 'A');
        }
        close(report[1]);
        int err = -EIO;
        int got = wait_report(report[0], &err, wait_ms);
        check(got == 0, "client reported before accept (timed out otherwise)");
        check(err == 0, "sendmsg(SCM_RIGHTS) before accept succeeded (err=%d %s)",
              -err, err == 0 ? "ok" : strerror(-err));

        // The server accepts only now, long after the client's send.
        struct pollfd lp = { .fd = lfd, .events = POLLIN };
        int ready = poll(&lp, 1, wait_ms);
        check(ready == 1, "connection pending at accept (poll=%d)", ready);
        int conn = ready == 1 ? accept(lfd, NULL, NULL) : -1;
        check(conn >= 0, "accept after the client's send (%s)", strerror(errno));
        if (conn >= 0) {
            int rfd = -1;
            char payload = 0;
            int rerr = recv_one_fd(conn, &rfd, &payload, wait_ms);
            check(rerr == 0, "recvmsg on the accepted fd (err=%d)", -rerr);
            check(payload == 'A', "data byte arrived (%d)", (int) payload);
            check(rfd >= 0, "an SCM_RIGHTS descriptor arrived");
            if (rfd >= 0) {
                check(fd_carries_payload(rfd), "received fd reads back the file");
                close(rfd);
            }
            (void) !write(conn, "d", 1);
            close(conn);
        }
        close(report[0]);
        waitpid(kid, NULL, 0);
        close(lfd);
    }
    unlink(sock_path);

    // 2. Control: the same exchange with the server accepting FIRST. This
    // path always worked; it is here so a failure in 1 can be read as
    // "before accept" rather than "fd passing is broken in this root".
    lfd = mklistener(sock_path);
    check(lfd >= 0, "control listener created (%s)", strerror(errno));
    if (lfd >= 0) {
        int report[2], release[2];
        check(pipe(report) == 0 && pipe(release) == 0, "control pipes");
        fflush(NULL);
        pid_t kid = fork();
        if (kid == 0) {
            close(lfd);
            close(report[0]);
            close(release[1]);
            client_child(release[0], report[1], 'B');
        }
        close(report[1]);
        close(release[0]);
        struct pollfd lp = { .fd = lfd, .events = POLLIN };
        int ready = poll(&lp, 1, wait_ms);
        int conn = ready == 1 ? accept(lfd, NULL, NULL) : -1;
        check(conn >= 0, "control accept (%s)", strerror(errno));
        (void) !write(release[1], "g", 1);
        int err = -EIO;
        int got = wait_report(report[0], &err, wait_ms);
        check(got == 0 && err == 0, "control sendmsg after accept (got=%d err=%d)", got, -err);
        if (conn >= 0) {
            int rfd = -1;
            char payload = 0;
            int rerr = recv_one_fd(conn, &rfd, &payload, wait_ms);
            check(rerr == 0 && payload == 'B' && rfd >= 0,
                  "control fd arrived (err=%d payload=%d fd=%d)", -rerr, (int) payload, rfd);
            if (rfd >= 0) {
                check(fd_carries_payload(rfd), "control fd reads back the file");
                close(rfd);
            }
            (void) !write(conn, "d", 1);
            close(conn);
        }
        close(report[0]);
        close(release[1]);
        waitpid(kid, NULL, 0);
        close(lfd);
    }
    unlink(sock_path);

    // 3. Two clients connect and send before either is accepted: each
    // accepted socket must get its own sender's descriptor, not the other's
    // and not both.
    lfd = mklistener(sock_path);
    check(lfd >= 0, "two-client listener created (%s)", strerror(errno));
    if (lfd >= 0) {
        int report[2][2];
        pid_t kids[2];
        int ok_pipes = 1;
        for (int i = 0; i < 2; i++)
            ok_pipes = ok_pipes && pipe(report[i]) == 0;
        check(ok_pipes, "two-client pipes");
        fflush(NULL);
        for (int i = 0; i < 2; i++) {
            kids[i] = fork();
            if (kids[i] == 0) {
                close(lfd);
                close(report[i][0]);
                if (i == 1)
                    close(report[0][1]);
                client_child(-1, report[i][1], (char) ('X' + i));
            }
            close(report[i][1]);
        }
        for (int i = 0; i < 2; i++) {
            int err = -EIO;
            int got = wait_report(report[i][0], &err, wait_ms);
            check(got == 0 && err == 0,
                  "client %d sent before accept (got=%d err=%d %s)", i, got, -err,
                  err == 0 ? "ok" : strerror(-err));
        }
        int seen_x = 0, seen_y = 0;
        for (int i = 0; i < 2; i++) {
            struct pollfd lp = { .fd = lfd, .events = POLLIN };
            int ready = poll(&lp, 1, wait_ms);
            int conn = ready == 1 ? accept(lfd, NULL, NULL) : -1;
            check(conn >= 0, "two-client accept %d (%s)", i, strerror(errno));
            if (conn < 0)
                continue;
            int rfd = -1;
            char payload = 0;
            int rerr = recv_one_fd(conn, &rfd, &payload, wait_ms);
            check(rerr == 0 && rfd >= 0, "two-client recv %d (err=%d fd=%d)", i, -rerr, rfd);
            if (payload == 'X') seen_x++;
            if (payload == 'Y') seen_y++;
            if (rfd >= 0) {
                check(fd_carries_payload(rfd), "two-client fd %d reads back the file", i);
                close(rfd);
            }
            (void) !write(conn, "d", 1);
            close(conn);
        }
        check(seen_x == 1 && seen_y == 1,
              "each accepted socket got its own sender (X=%d Y=%d)", seen_x, seen_y);
        for (int i = 0; i < 2; i++) {
            close(report[i][0]);
            waitpid(kids[i], NULL, 0);
        }
        close(lfd);
    }
    unlink(sock_path);

    // 4. The client sends before the accept and then CLOSES, so by the time
    // the server accepts there is no sender left. Linux still delivers: the
    // fd is already queued on the connection, and closing the sending socket
    // does not discard what it sent. AOK keeps the connection's cookie entry
    // alive as a tombstone for exactly this.
    lfd = mklistener(sock_path);
    check(lfd >= 0, "closed-sender listener created (%s)", strerror(errno));
    if (lfd >= 0) {
        int report[2];
        check(pipe(report) == 0, "closed-sender pipe");
        fflush(NULL);
        pid_t kid = fork();
        if (kid == 0) {
            close(lfd);
            close(report[0]);
            int c = connect_to(sock_path);
            int err = c < 0 ? -errno : 0;
            if (c >= 0) {
                int fd = open_payload_file('C');
                err = fd < 0 ? -errno : send_one_fd(c, fd, 'C');
                if (fd >= 0)
                    close(fd);
                close(c);          // gone before the server ever accepts
            }
            (void) !write(report[1], &err, sizeof err);
            _exit(0);
        }
        close(report[1]);
        int err = -EIO;
        int got = wait_report(report[0], &err, wait_ms);
        check(got == 0 && err == 0,
              "closed-sender sendmsg before accept (got=%d err=%d %s)", got, -err,
              err == 0 ? "ok" : strerror(-err));
        int st;
        waitpid(kid, &st, 0);      // the client is really gone now
        struct pollfd lp = { .fd = lfd, .events = POLLIN };
        int ready = poll(&lp, 1, wait_ms);
        int conn = ready == 1 ? accept(lfd, NULL, NULL) : -1;
        check(conn >= 0, "closed-sender accept (%s)", strerror(errno));
        if (conn >= 0) {
            int rfd = -1;
            char payload = 0;
            int rerr = recv_one_fd(conn, &rfd, &payload, wait_ms);
            check(rerr == 0 && payload == 'C' && rfd >= 0,
                  "fd from a closed sender arrived (err=%d payload=%d fd=%d)",
                  -rerr, (int) payload, rfd);
            if (rfd >= 0) {
                check(fd_carries_payload(rfd), "closed-sender fd reads back the file");
                close(rfd);
            }
            close(conn);
        }
        close(report[0]);
        close(lfd);
    }
    unlink(sock_path);

    for (const char *t = "ABCXY"; *t != '\0'; t++) {
        char path[160];
        snprintf(path, sizeof path, "%s.%c", file_path, *t);
        unlink(path);
    }

    return finish_suite("unix_scm_before_accept");
}
