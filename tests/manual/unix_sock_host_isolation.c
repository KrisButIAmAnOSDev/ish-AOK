// Guest unix sockets, as the host backs them.
//
// Every bound guest AF_UNIX socket is a host socket at a host path, named by an
// id that restarts at 1 in each ish process. The CLI put every process's paths
// in one place, /tmp/ishsock.<id>, and bind unlinks its path first -- so two
// CLI guests running at once took each other's sockets, and a client in one
// guest reached a server in the other, or nothing. fs/sock.c now gives each
// process a private directory.
//
// A guest cannot see host paths, so the two-processes-at-once check is the
// host script beside this file, unix_sock_host_isolation.sh, which runs this
// program in --hold mode in several CLI guests together. Run plainly, as the
// suite does, it checks what a guest CAN see:
//
//   - A socket file with nothing bound to it -- here made with mknod, in life
//     left behind by a daemon that died -- refuses connect, sendto and sendmsg
//     with ECONNREFUSED, and bind over it is EADDRINUSE. Linux 6.12, measured.
//     AOK asked the host, which said ECONNREFUSED only when some other
//     process's /tmp/ishsock.<id> happened to exist at the path this one
//     computed, and ENOENT otherwise; with private directories it is always
//     the latter, so the answer is now mapped.
//   - Stream and datagram servers on abstract and filesystem names in one
//     process each answer their own clients.
//
//   unix_sock_host_isolation                  the guest-visible checks
//   unix_sock_host_isolation --hold TAG SECS  bind servers named for TAG, say
//                                             READY, then for SECS seconds
//                                             check every connection reaches
//                                             this process's own servers
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "test_common.h"

static socklen_t make_addr(struct sockaddr_un *a, const char *name, bool abstract) {
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    size_t n = strlen(name);
    if (abstract) {
        memcpy(a->sun_path + 1, name, n);
        return (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + n);
    }
    memcpy(a->sun_path, name, n + 1);
    return (socklen_t) (offsetof(struct sockaddr_un, sun_path) + n + 1);
}

static void expect_err(const char *label, long ret, int want) {
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

struct server {
    const char *label;
    int type;
    int fd;
    struct sockaddr_un addr;
    socklen_t addr_len;
    char path[108];
};

static bool server_open(struct server *s, const char *label, int type, const char *name,
                        bool abstract) {
    s->label = label;
    s->type = type;
    s->path[0] = '\0';
    s->addr_len = make_addr(&s->addr, name, abstract);
    if (!abstract) {
        snprintf(s->path, sizeof(s->path), "%s", name);
        unlink(name);
    }
    s->fd = socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    if (s->fd < 0 || bind(s->fd, (struct sockaddr *) &s->addr, s->addr_len) < 0 ||
            (type == SOCK_STREAM && listen(s->fd, 16) < 0)) {
        printf("FAIL %s: setting up the server: %s\n", label, strerror(errno));
        failures_total++;
        return false;
    }
    return true;
}

static void server_close(struct server *s) {
    if (s->fd >= 0)
        close(s->fd);
    if (s->path[0] != '\0')
        unlink(s->path);
}

// One connection or datagram, carrying a token only this process and this
// round know, which must arrive at this server and nowhere else.
static bool server_round(struct server *s, const char *token) {
    size_t len = strlen(token);
    char got[128];
    int c = socket(AF_UNIX, s->type | SOCK_CLOEXEC, 0);
    if (c < 0) {
        printf("FAIL %s: socket: %s\n", s->label, strerror(errno));
        return false;
    }
    bool ok = false;
    if (s->type == SOCK_DGRAM) {
        if (sendto(c, token, len, 0, (struct sockaddr *) &s->addr, s->addr_len) != (ssize_t) len) {
            printf("FAIL %s: sendto: %s\n", s->label, strerror(errno));
            goto out;
        }
        struct pollfd p = {.fd = s->fd, .events = POLLIN};
        if (poll(&p, 1, 5000) != 1) {
            printf("FAIL %s: the datagram never arrived here\n", s->label);
            goto out;
        }
        ssize_t n = recv(s->fd, got, sizeof(got) - 1, 0);
        if (n != (ssize_t) len || memcmp(got, token, len) != 0) {
            printf("FAIL %s: received something that was not ours (%zd bytes)\n", s->label, n);
            goto out;
        }
        ok = true;
        goto out;
    }
    if (connect(c, (struct sockaddr *) &s->addr, s->addr_len) < 0) {
        printf("FAIL %s: connect: %s\n", s->label, strerror(errno));
        goto out;
    }
    if (write(c, token, len) != (ssize_t) len) {
        printf("FAIL %s: write: %s\n", s->label, strerror(errno));
        goto out;
    }
    struct pollfd p = {.fd = s->fd, .events = POLLIN};
    if (poll(&p, 1, 5000) != 1) {
        printf("FAIL %s: the connection never arrived here\n", s->label);
        goto out;
    }
    int a = accept(s->fd, NULL, NULL);
    if (a < 0) {
        printf("FAIL %s: accept: %s\n", s->label, strerror(errno));
        goto out;
    }
    size_t have = 0;
    while (have < len) {
        struct pollfd rp = {.fd = a, .events = POLLIN};
        if (poll(&rp, 1, 5000) != 1)
            break;
        ssize_t n = read(a, got + have, len - have);
        if (n <= 0)
            break;
        have += (size_t) n;
    }
    close(a);
    if (have != len || memcmp(got, token, len) != 0) {
        printf("FAIL %s: the connection accepted here was not ours\n", s->label);
        goto out;
    }
    ok = true;
out:
    close(c);
    return ok;
}

static int open_servers(struct server *servers, const char *tag) {
    static char names[3][96];
    snprintf(names[0], sizeof(names[0]), "aok-sock-isolation-%s-%d", tag, getpid());
    snprintf(names[1], sizeof(names[1]), "/tmp/aok-sock-isolation-%s-%d.stream", tag, getpid());
    snprintf(names[2], sizeof(names[2]), "/tmp/aok-sock-isolation-%s-%d.dgram", tag, getpid());
    int n = 0;
    if (server_open(&servers[n], "abstract stream", SOCK_STREAM, names[0], true))
        n++;
    if (server_open(&servers[n], "path stream", SOCK_STREAM, names[1], false))
        n++;
    if (server_open(&servers[n], "path datagram", SOCK_DGRAM, names[2], false))
        n++;
    return n == 3 ? n : -1;
}

static void check_stale_socket_file(void) {
    char path[96];
    snprintf(path, sizeof(path), "/tmp/aok-sock-isolation-stale-%d", getpid());
    unlink(path);
    if (mknod(path, S_IFSOCK | 0777, 0) < 0) {
        printf("FAIL mknod S_IFSOCK: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    struct sockaddr_un a;
    socklen_t al = make_addr(&a, path, false);

    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    expect_err("stream connect to an unbound socket file",
               connect(s, (struct sockaddr *) &a, al), ECONNREFUSED);
    close(s);

    int d = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    expect_err("datagram sendto an unbound socket file",
               sendto(d, "x", 1, 0, (struct sockaddr *) &a, al), ECONNREFUSED);
    struct iovec iov = {.iov_base = "x", .iov_len = 1};
    struct msghdr m = {.msg_name = &a, .msg_namelen = al, .msg_iov = &iov, .msg_iovlen = 1};
    expect_err("datagram sendmsg to an unbound socket file", sendmsg(d, &m, 0), ECONNREFUSED);
    expect_err("datagram connect to an unbound socket file",
               connect(d, (struct sockaddr *) &a, al), ECONNREFUSED);
    close(d);

    int b = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    expect_err("bind over an unbound socket file", bind(b, (struct sockaddr *) &a, al),
               EADDRINUSE);
    close(b);
    unlink(path);
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static int hold(const char *tag, int seconds) {
    struct server servers[3];
    if (open_servers(servers, tag) < 0) {
        printf("unix_sock_host_isolation: FAIL servers for %s\n", tag);
        return 1;
    }
    printf("READY %s\n", tag);
    fflush(stdout);
    // Let every other guest started alongside bind too, so any sharing of host
    // paths has happened before the first connection.
    sleep(2);
    unsigned rounds = 0;
    double end = now_seconds() + seconds;
    while (now_seconds() < end && failures_total == 0) {
        for (int i = 0; i < 3; i++) {
            char token[96];
            snprintf(token, sizeof(token), "%s-%d-%u-%d", tag, getpid(), rounds, i);
            if (!server_round(&servers[i], token))
                failures_total++;
        }
        rounds++;
    }
    for (int i = 0; i < 3; i++)
        server_close(&servers[i]);
    if (failures_total != 0) {
        printf("unix_sock_host_isolation: FAIL %s after %u rounds\n", tag, rounds);
        return 1;
    }
    printf("unix_sock_host_isolation: PASS %s rounds=%u\n", tag, rounds);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "--hold") == 0)
        return hold(argv[2], atoi(argv[3]));
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    check_stale_socket_file();

    struct server servers[3];
    if (open_servers(servers, "suite") == 3) {
        for (unsigned round = 0; round < 20; round++) {
            for (int i = 0; i < 3; i++) {
                char token[96];
                snprintf(token, sizeof(token), "suite-%u-%d", round, i);
                if (!server_round(&servers[i], token))
                    failures_total++;
            }
        }
        for (int i = 0; i < 3; i++)
            server_close(&servers[i]);
    }
    return finish_suite("unix_sock_host_isolation");
}
