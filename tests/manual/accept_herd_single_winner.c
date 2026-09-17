// One connection, several accepters: exactly one of them gets it, and the one
// it gets is OURS.
//
// accept_rcvtimeo's part 4 already counts winners, but counting alone cannot
// say which bug a second winner is. This test makes each winner write its own
// index byte into the socket it accepted and then reads from the single
// connection the parent made: two winners plus two bytes here would mean one
// host connection was handed out twice, while two winners plus one byte means
// a connection arrived that this process never made. It was the second.
//
// Reported as "tests/manual/accept_rcvtimeo.c part 4 sometimes fails with
// FAIL herd: 1 winner, 2 EAGAIN losers (w=2 l=1 o=0)" on a busy host. The
// cause was not accept at all: before 3f4017c5 every CLI guest put its bound
// AF_UNIX sockets at /tmp/ishsock.<id> with ids restarting at 1 per process,
// so a second ish process on the same Mac -- which is what "busy host" meant --
// bound its listener over this one's path and its client's connect landed in
// this listener's backlog. Two connections, two winners. Rebuilt with that
// shared layout restored, three concurrent guests reproduce it within a round:
// w=2 l=1 o=0 and w=3 l=0 o=0, always with one byte on the parent's own
// connection. tests/manual/unix_sock_host_isolation.sh is the host-side test
// for the layout itself; this is the guest-visible symptom.
//
// Both halves of the count are checked, because an emulation that lost
// connections would also pass a winners<=1 test: a second connect must produce
// a second winner.
//
// Also passes on real Linux (camd oracle, 64-bit and -m32, verified).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "test_common.h"

#define KIDS 3

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

// Reap `pid` for real: a wait that fails must not leave the caller reading an
// uninitialised status, which is its own way to invent a second winner.
static int reap_status(pid_t pid) {
    int st = -1;
    pid_t r;
    do {
        r = waitpid(pid, &st, 0);
    } while (r < 0 && errno == EINTR);
    if (r != pid)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// Read everything a connected socket has: up to `first_ms` for the first
// byte, then only a short grace for more. A second byte here would mean a
// second winner accepted this same connection, so the grace has to exist --
// but waiting the full timeout for a byte that must not come would cost the
// whole test's runtime.
#define DRAIN_GRACE_MS 500
static int drain(int fd, char *buf, int max, int first_ms) {
    int n = 0;
    int timeout = first_ms;
    while (n < max) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, timeout) != 1)
            break;
        ssize_t r = read(fd, buf + n, (size_t) (max - n));
        if (r <= 0)
            break;
        n += (int) r;
        timeout = DRAIN_GRACE_MS;
    }
    buf[n] = '\0';
    return n;
}

// One round: `conns` connections offered to KIDS accepters.
static void herd_round(int round, int conns, int rcv_s, int wait_ms) {
    char path[96];
    snprintf(path, sizeof path, "/tmp/herd1w.%d.%d", (int) getpid(), round);
    unlink(path);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0 || bind(lfd, (struct sockaddr *) &sa, sizeof sa) < 0 ||
            listen(lfd, 8) < 0) {
        check(0, "round %d listener (%s)", round, strerror(errno));
        if (lfd >= 0)
            close(lfd);
        return;
    }
    struct timeval tv = { .tv_sec = rcv_s, .tv_usec = 0 };
    check(setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0,
          "round %d SO_RCVTIMEO (%s)", round, strerror(errno));

    pid_t kids[KIDS];
    int forked = 0;
    fflush(NULL);
    for (int i = 0; i < KIDS; i++) {
        kids[i] = fork();
        if (kids[i] == 0) {
            int k = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (k >= 0) {
                char b = (char) ('0' + i);
                (void) !write(k, &b, 1);
                struct timespec ts = { 0, 300000000L };
                nanosleep(&ts, NULL);
                close(k);
                _exit(10);
            }
            _exit(errno == EAGAIN ? 11 : 12);
        }
        if (kids[i] > 0)
            forked++;
    }
    check(forked == KIDS, "round %d forked %d accepters", round, forked);

    // Let the accepters get in before the connection arrives: the point of
    // the test is a herd already waiting, not a pending backlog.
    usleep(300000);
    int cfd[KIDS];
    int made = 0;
    for (int i = 0; i < conns; i++) {
        cfd[i] = socket(AF_UNIX, SOCK_STREAM, 0);
        if (cfd[i] >= 0 && connect(cfd[i], (struct sockaddr *) &sa, sizeof sa) == 0) {
            made++;
        } else {
            check(0, "round %d connect %d (%s)", round, i, strerror(errno));
            if (cfd[i] >= 0)
                close(cfd[i]);
            cfd[i] = -1;
        }
    }

    // Every byte the winners wrote must arrive on the connections WE made:
    // one byte per connection, no more and no fewer.
    int total_bytes = 0;
    for (int i = 0; i < conns; i++) {
        if (cfd[i] < 0)
            continue;
        char buf[8];
        int n = drain(cfd[i], buf, (int) sizeof buf - 1, wait_ms);
        check(n == 1, "round %d connection %d got one winner's byte (n=%d [%s])",
              round, i, n, buf);
        total_bytes += n;
    }

    int winners = 0, losers = 0, others = 0;
    for (int i = 0; i < KIDS; i++) {
        if (kids[i] < 0)
            continue;
        int code = reap_status(kids[i]);
        if (code == 10) winners++;
        else if (code == 11) losers++;
        else others++;
    }
    check(winners == made && losers == KIDS - made && others == 0,
          "round %d: %d connection(s) -> %d winner(s), %d EAGAIN (w=%d l=%d o=%d)",
          round, made, made, KIDS - made, winners, losers, others);
    check(total_bytes == made,
          "round %d winners accepted OUR connections (bytes=%d conns=%d)",
          round, total_bytes, made);

    // Nothing may be left waiting in the backlog: a stray connection from
    // outside this process shows up here even when the counts look right.
    // O_NONBLOCK first, or this drain waits out SO_RCVTIMEO to learn that
    // the backlog is empty -- on Linux as well as here.
    int lfl = fcntl(lfd, F_GETFL);
    if (lfl >= 0)
        fcntl(lfd, F_SETFL, lfl | O_NONBLOCK);
    int extra = 0;
    for (;;) {
        int e = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (e < 0)
            break;
        extra++;
        close(e);
    }
    check(extra == 0, "round %d backlog empty afterwards (extra=%d)", round, extra);

    for (int i = 0; i < conns; i++)
        if (cfd[i] >= 0)
            close(cfd[i]);
    close(lfd);
    unlink(path);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(180));
    // Generous on purpose: the losers must not time out before a winner that
    // the emulator has been slow to schedule, and the parent must not give up
    // on the winner's byte. A correct run finishes far inside both.
    int scale = (int) test_watchdog_secs(1);
    int rcv_s = 2 * scale;
    int wait_ms = 5000 * scale;
    signal(SIGPIPE, SIG_IGN);

    // One connection, three accepters: one winner, two EAGAIN at the timeout.
    for (int r = 0; r < 4; r++)
        herd_round(r, 1, rcv_s, wait_ms);
    // Two connections: two winners, one EAGAIN. An emulation that dropped a
    // connection would pass the rounds above and fail here.
    herd_round(100, 2, rcv_s, wait_ms);

    return finish_suite("accept_herd_single_winner");
}
