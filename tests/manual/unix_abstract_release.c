// An abstract AF_UNIX name belongs to the socket bound to it, and is free
// again the moment that socket is closed.
//
// AOK's abstract namespace is a refcounted table (fs/sock.c). bind() took a
// reference -- and so did every LOOKUP: each connect(), sendto() or sendmsg()
// to the name counted one more holder, and nothing ever gave it back. So once
// anything had connected, the name outlived its listener for good, and binding
// it again failed. A session bus, an X server or a Wayland compositor that
// restarts on the same abstract name could never come back, and the refusal
// was EEXIST, where Linux says EADDRINUSE even for a name really in use.
//
// Linux (net/unix/af_unix.c): the name lives in the bound socket's hash entry.
// An accepted socket shares the address for getsockname() but is not in the
// table, so it holds nothing; a lookup holds nothing; the entry goes when the
// bound socket is released -- the last close of its file, so a dup keeps it.
// Every expectation below was measured on Linux 6.12, 64-bit and -m32.
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

static struct sockaddr_un name_addr;
static socklen_t name_len;

static void set_name(const char *name) {
    memset(&name_addr, 0, sizeof(name_addr));
    name_addr.sun_family = AF_UNIX;
    size_t n = strlen(name);
    memcpy(name_addr.sun_path + 1, name, n);   // sun_path[0] stays NUL
    name_len = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

static void expect_ok(const char *label, int ret) {
    if (ret < 0) {
        printf("FAIL %s: %s\n", label, strerror(errno));
        failures_total++;
    } else {
        test_logf("%s: ok\n", label);
    }
}

static void expect_err(const char *label, int ret, int want) {
    int e = errno;
    if (ret >= 0) {
        printf("FAIL %s: succeeded, want %s\n", label, strerror(want));
        failures_total++;
    } else if (e != want) {
        printf("FAIL %s: %s, want %s\n", label, strerror(e), strerror(want));
        failures_total++;
    } else {
        test_logf("%s: %s ok\n", label, strerror(e));
    }
}

static int sock_stream(void) {
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) {
        printf("FAIL socket: %s\n", strerror(errno));
        failures_total++;
    }
    return s;
}

static int do_bind(int s) {
    return bind(s, (struct sockaddr *) &name_addr, name_len);
}

static int do_connect(int s) {
    return connect(s, (struct sockaddr *) &name_addr, name_len);
}

// accept() that gives up after five seconds instead of hanging the suite when
// a connection went somewhere other than this listener.
static int accept_timed(const char *label, int l) {
    struct pollfd p = {.fd = l, .events = POLLIN};
    if (poll(&p, 1, 5000) != 1) {
        printf("FAIL %s: no connection arrived at the listener\n", label);
        failures_total++;
        return -1;
    }
    int s = accept(l, NULL, NULL);
    if (s < 0) {
        printf("FAIL %s: accept: %s\n", label, strerror(errno));
        failures_total++;
    }
    return s;
}

// Connect to the name, accept on `l`, and pass a byte through.
static void expect_reaches(const char *label, int l) {
    int c = sock_stream();
    if (do_connect(c) < 0) {
        printf("FAIL %s: connect: %s\n", label, strerror(errno));
        failures_total++;
        close(c);
        return;
    }
    int s = accept_timed(label, l);
    if (s >= 0) {
        char b = 0;
        if (write(c, "k", 1) != 1 || read(s, &b, 1) != 1 || b != 'k') {
            printf("FAIL %s: byte did not pass through\n", label);
            failures_total++;
        }
        close(s);
    }
    close(c);
}

static void test_accepted_sockets_hold_nothing(void) {
    int l = sock_stream();
    expect_ok("bind", do_bind(l));
    expect_ok("listen", listen(l, 8));

    int other = sock_stream();
    expect_err("bind while the name is held", do_bind(other), EADDRINUSE);
    close(other);

    int c = sock_stream();
    expect_ok("connect", do_connect(c));
    int s = accept_timed("accept", l);

    // Only the listener goes. The accepted socket and the client stay open.
    close(l);

    struct sockaddr_un got;
    socklen_t got_len = sizeof(got);
    if (s >= 0 && getsockname(s, (struct sockaddr *) &got, &got_len) == 0) {
        if (got_len != name_len || memcmp(&got, &name_addr, name_len) != 0) {
            printf("FAIL getsockname on the accepted socket: len %u, want %u\n",
                   (unsigned) got_len, (unsigned) name_len);
            failures_total++;
        }
    } else if (s >= 0) {
        printf("FAIL getsockname on the accepted socket: %s\n", strerror(errno));
        failures_total++;
    }

    int c2 = sock_stream();
    expect_err("connect with no listener", do_connect(c2), ECONNREFUSED);
    close(c2);

    int l2 = sock_stream();
    expect_ok("bind again, accepted socket still open", do_bind(l2));
    expect_ok("listen again", listen(l2, 8));
    expect_reaches("connect to the new listener", l2);

    close(l2);
    if (s >= 0)
        close(s);
    close(c);
}

static void test_lookups_hold_nothing(void) {
    // Connections that were never accepted.
    int l = sock_stream();
    expect_ok("bind for unaccepted connects", do_bind(l));
    expect_ok("listen for unaccepted connects", listen(l, 64));
    for (int i = 0; i < 20; i++) {
        int c = sock_stream();
        if (do_connect(c) < 0) {
            printf("FAIL unaccepted connect %d: %s\n", i, strerror(errno));
            failures_total++;
        }
        close(c);
    }
    close(l);
    int l2 = sock_stream();
    expect_ok("bind after unaccepted connects", do_bind(l2));
    close(l2);

    // A great many accepted ones.
    int l3 = sock_stream();
    expect_ok("bind for repeated connects", do_bind(l3));
    expect_ok("listen for repeated connects", listen(l3, 8));
    for (int i = 0; i < 200; i++) {
        int c = sock_stream();
        if (do_connect(c) < 0) {
            printf("FAIL repeated connect %d: %s\n", i, strerror(errno));
            failures_total++;
            close(c);
            break;
        }
        int s = accept_timed("repeated accept", l3);
        close(c);
        if (s < 0)
            break;
        close(s);
    }
    close(l3);
    int l4 = sock_stream();
    expect_ok("bind after 200 connections", do_bind(l4));
    close(l4);

    // A lookup from another process.
    int l5 = sock_stream();
    expect_ok("bind for a child's connect", do_bind(l5));
    expect_ok("listen for a child's connect", listen(l5, 8));
    pid_t pid = fork();
    if (pid == 0) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        _exit(do_connect(c) == 0 ? 0 : 1);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
        printf("FAIL child connect (status %#x)\n", status);
        failures_total++;
    }
    close(l5);
    int l6 = sock_stream();
    expect_ok("bind after a child's connect", do_bind(l6));
    close(l6);
}

static void test_last_close_releases(void) {
    int l = sock_stream();
    expect_ok("bind before dup", do_bind(l));
    int d = dup(l);
    close(l);
    int other = sock_stream();
    expect_err("bind while a dup holds the name", do_bind(other), EADDRINUSE);
    close(d);
    expect_ok("bind after the dup closed", do_bind(other));
    close(other);
}

static void test_datagrams(void) {
    int b = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    expect_ok("datagram bind", do_bind(b));
    int tx = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    for (int i = 0; i < 5; i++) {
        if (sendto(tx, "d", 1, 0, (struct sockaddr *) &name_addr, name_len) != 1) {
            printf("FAIL datagram sendto %d: %s\n", i, strerror(errno));
            failures_total++;
        }
    }
    struct msghdr msg = {0};
    struct iovec iov = {.iov_base = "m", .iov_len = 1};
    msg.msg_name = &name_addr;
    msg.msg_namelen = name_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    expect_ok("datagram sendmsg", (int) sendmsg(tx, &msg, 0));
    close(b);
    expect_err("datagram sendto with no binder",
               (int) sendto(tx, "d", 1, 0, (struct sockaddr *) &name_addr, name_len),
               ECONNREFUSED);
    int b2 = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    expect_ok("datagram bind again", do_bind(b2));
    char got = 0;
    if (sendto(tx, "e", 1, 0, (struct sockaddr *) &name_addr, name_len) != 1 ||
            recv(b2, &got, 1, MSG_DONTWAIT) != 1 || got != 'e') {
        printf("FAIL datagram to the new binder: %s\n", strerror(errno));
        failures_total++;
    }
    close(b2);
    close(tx);
}

static void test_never_bound(void) {
    int c = sock_stream();
    expect_err("connect to a name never bound", do_connect(c), ECONNREFUSED);
    close(c);
    int tx = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    expect_err("sendto a name never bound",
               (int) sendto(tx, "d", 1, 0, (struct sockaddr *) &name_addr, name_len),
               ECONNREFUSED);
    close(tx);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    char name[64];

    snprintf(name, sizeof(name), "aok-abstract-release-%d-a", getpid());
    set_name(name);
    test_accepted_sockets_hold_nothing();

    snprintf(name, sizeof(name), "aok-abstract-release-%d-b", getpid());
    set_name(name);
    test_lookups_hold_nothing();

    snprintf(name, sizeof(name), "aok-abstract-release-%d-c", getpid());
    set_name(name);
    test_last_close_releases();

    snprintf(name, sizeof(name), "aok-abstract-release-%d-d", getpid());
    set_name(name);
    test_datagrams();

    snprintf(name, sizeof(name), "aok-abstract-release-%d-never", getpid());
    set_name(name);
    test_never_bound();

    return finish_suite("unix_abstract_release");
}
