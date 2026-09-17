// syslog(2) SYSLOG_ACTION_READ_CLEAR returns the byte count, not 0.
//
//   AOK's do_syslog computed the read, checked it for an error, and then fell
//   THROUGH into SYSLOG_ACTION_CLEAR's `log_max_since_clear = 0; return 0;`.
//   The bytes reached the guest buffer correctly; only the count was thrown
//   away. Measured in an arm64 guest before the fix, against Linux 6.12:
//
//       call              AOK    Linux
//       SIZE_UNREAD(9)    106    -
//       READ_ALL(3)       106    8154
//       READ_CLEAR(4)       0    8154
//
//   The consequence is `dmesg -c`, which prints only as many bytes as the
//   call reported and so printed NOTHING in a guest. Plain `dmesg` uses
//   READ_ALL and was never affected, which is what made this quiet.
//
// Measured against x86_64 glibc on Linux 6.12 (camd) as root, with rsyslogd
// paused for the run: it holds /proc/kmsg open, the same destructive global
// reader position, and would otherwise steal records. Nothing in an AOK guest
// consumes the log that way, so only the oracle needs that.
//
// One deliberate difference from the oracle is checked loosely rather than
// exactly. Linux's syslog_print_all is RECORD-oriented: asked for 115 bytes
// of a 125-byte log it returned 84, the two whole records that fit, and asked
// for less than one record it returned 0. AOK's log is a byte stream with no
// record boundaries to round to, so it returns the last min(len, available)
// bytes. Both agree on what matters here -- a short read is non-empty when a
// record fits, never longer than the buffer, comes off the TAIL, and still
// clears the whole log -- so that is what is asserted.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static int klogctl_(int type, char *buf, int len) {
    return (int) syscall(SYS_syslog, type, buf, len);
}

#define SYSLOG_READ_ALL    3
#define SYSLOG_READ_CLEAR  4
#define SYSLOG_CLEAR       5
#define SYSLOG_SIZE_UNREAD 9
#define SYSLOG_SIZE_BUFFER 10

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%-10ld want=%ld\n", label, got, want);
}

static void ck_ge(const char *label, long got, long floor) {
    if (got < floor)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) floor, 0, 0);
    test_logf("  %-58s got=%-10ld want>=%ld\n", label, got, floor);
}

static void ck_le(const char *label, long got, long ceil) {
    if (got > ceil)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) ceil, 0, 0);
    test_logf("  %-58s got=%-10ld want<=%ld\n", label, got, ceil);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Put one line in the log. Root-only (the node is 0644), checked once up front.
static int kmsg_say(const char *text) {
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd < 0)
        return -1;
    char line[256];
    int n = snprintf(line, sizeof line, "<6>%s\n", text);
    ssize_t w = write(fd, line, (size_t) n);
    close(fd);
    return w == n ? 0 : -1;
}

static char buf[1 << 16];

// READ_ALL into buf, NUL-terminated. Returns the count.
static int read_all(void) {
    int n = klogctl_(SYSLOG_READ_ALL, buf, (int) sizeof buf - 1);
    buf[n > 0 ? n : 0] = '\0';
    return n;
}

static int has(const char *needle) {
    return strstr(buf, needle) != NULL ? 1 : 0;
}

// A watchdog PROCESS rather than alarm(), matching syslog_read_blocks: a
// READ_CLEAR that wrongly waited would otherwise hang the whole suite instead
// of failing this test.
static pid_t watchdog_start(unsigned secs) {
    pid_t victim = getpid();
    fflush(stdout);
    pid_t dog = fork();
    if (dog != 0)
        return dog;
    for (unsigned i = 0; i < secs; i++) {
        sleep(1);
        if (getppid() != victim)
            _exit(0);
    }
    printf("syslog_read_clear: FAIL watchdog fired after %us -- a call never "
           "returned\n", secs);
    fflush(stdout);
    kill(victim, SIGKILL);
    _exit(1);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    // /dev/kmsg is 0644 root-owned, as on Linux, so an unprivileged run cannot
    // write the lines every check below reads back. The suite is not always
    // root (the CLI harness is, an ssh session into the app is not).
    if (kmsg_say("syslog_read_clear: start") != 0) {
        if (geteuid() != 0) {
            printf("syslog_read_clear: SKIP (unprivileged: cannot write "
                   "/dev/kmsg, %s)\n", strerror(errno));
            return 0;
        }
        ck("/dev/kmsg accepts a write", 0, 1);
        return finish_suite("syslog_read_clear");
    }
    // Linux gates every syslog(2) type on CAP_SYSLOG when dmesg_restrict is
    // set. AOK has no such check, so this only trips on an unprivileged oracle.
    if (klogctl_(SYSLOG_SIZE_BUFFER, NULL, 0) < 0 && errno == EPERM) {
        printf("syslog_read_clear: SKIP (unprivileged: syslog(2) is EPERM)\n");
        return 0;
    }

    pid_t watchdog = watchdog_start(test_watchdog_secs(60));

    // ---- the bug: READ_CLEAR reports what it copied ----------------------
    {
        ck("CLEAR to start from a known state", klogctl_(SYSLOG_CLEAR, NULL, 0), 0);
        for (int i = 0; i < 5; i++)
            ck("wrote a line", kmsg_say("syslog_read_clear: MARKER-COUNTED"), 0);

        int all = read_all();
        ck_ge("READ_ALL sees the lines", all, 1);
        ck("  and they are the lines written", has("MARKER-COUNTED"), 1);

        long t0 = now_ms();
        memset(buf, 0, sizeof buf);
        int n = klogctl_(SYSLOG_READ_CLEAR, buf, (int) sizeof buf - 1);
        long waited = now_ms() - t0;
        buf[n > 0 ? n : 0] = '\0';

        // The failure this file exists for: the bytes arrived, the count was 0.
        ck_ge("READ_CLEAR returns a byte count, not 0", n, 1);
        ck("  exactly what READ_ALL just returned", n, all);
        ck("  and the buffer holds those bytes", has("MARKER-COUNTED"), 1);
        ck_le("  without waiting (ms)", waited, 200);
    }

    // ---- the clear half still happens ------------------------------------
    {
        int n = klogctl_(SYSLOG_READ_CLEAR, buf, (int) sizeof buf - 1);
        ck("a second READ_CLEAR returns 0", n, 0);
        ck("  and READ_ALL is empty too", read_all(), 0);

        // Not a general amnesia: new lines land normally afterwards.
        ck("wrote a line after the clear",
           kmsg_say("syslog_read_clear: MARKER-AFTER-CLEAR"), 0);
        ck_ge("  READ_ALL sees it", read_all(), 1);
        ck("  and only it", has("MARKER-COUNTED"), 0);
        ck("  which is the new line", has("MARKER-AFTER-CLEAR"), 1);
    }

    // ---- it never waits, however empty the log ---------------------------
    // Only type 2 blocks. A guest running `dmesg -c` on a cleared log must
    // come straight back, not park.
    {
        ck("CLEAR", klogctl_(SYSLOG_CLEAR, NULL, 0), 0);
        long t0 = now_ms();
        for (int i = 0; i < 5; i++)
            ck("READ_CLEAR on an empty log returns 0",
               klogctl_(SYSLOG_READ_CLEAR, buf, (int) sizeof buf - 1), 0);
        ck_le("  five of them, total (ms)", now_ms() - t0, 500);
    }

    // ---- a len shorter than the log --------------------------------------
    // Byte-exact on AOK, record-rounded on Linux -- see the header. What both
    // guarantee: at most len, non-empty, off the tail, and a full clear.
    {
        ck("CLEAR", klogctl_(SYSLOG_CLEAR, NULL, 0), 0);
        ck("wrote the first line", kmsg_say("syslog_read_clear: HEAD-LINE"), 0);
        for (int i = 0; i < 6; i++)
            kmsg_say("syslog_read_clear: filler 0123456789abcdefghijklmnopqrstuvwxyz");
        ck("wrote the last line", kmsg_say("syslog_read_clear: TAIL-LINE"), 0);

        int all = read_all();
        ck_ge("READ_ALL sees the whole thing", all, 200);
        ck("  head is in it", has("HEAD-LINE"), 1);
        ck("  tail is in it", has("TAIL-LINE"), 1);

        int want = all - 100;  // room for most of it, but not the first line
        memset(buf, 0, sizeof buf);
        int n = klogctl_(SYSLOG_READ_CLEAR, buf, want);
        buf[n > 0 ? n : 0] = '\0';
        ck_ge("READ_CLEAR with a short buffer returns bytes", n, 1);
        ck_le("  no more than asked for", n, want);
        ck("  it is the TAIL of the log", has("TAIL-LINE"), 1);
        ck("  the head fell off the front", has("HEAD-LINE"), 0);

        // The short read still cleared EVERYTHING, not just what it returned.
        ck("  and it cleared the whole log", read_all(), 0);
        ck("wrote a line afterwards",
           kmsg_say("syslog_read_clear: AFTER-SHORT-CLEAR"), 0);
        ck_ge("  which is all that is left", read_all(), 1);
        ck("  no leftover tail", has("TAIL-LINE"), 0);
    }

    // ---- len 0 answers 0 and does NOT clear ------------------------------
    // Linux returns before syslog_print_all for `!len`, so the clear never
    // runs. AOK used to clear anyway, through the same fallthrough.
    {
        ck("CLEAR", klogctl_(SYSLOG_CLEAR, NULL, 0), 0);
        ck("wrote a line", kmsg_say("syslog_read_clear: SURVIVES-LEN-ZERO"), 0);
        ck("READ_CLEAR len=0 returns 0", klogctl_(SYSLOG_READ_CLEAR, buf, 0), 0);
        ck_ge("  the log is still there", read_all(), 1);
        ck("  with the line in it", has("SURVIVES-LEN-ZERO"), 1);
        ck("READ_ALL len=0 returns 0", klogctl_(SYSLOG_READ_ALL, buf, 0), 0);
        ck_ge("  and that did not clear it either", read_all(), 1);
    }

    // ---- argument checks come first --------------------------------------
    // Measured on 6.12: a null buffer is EINVAL for both read types even with
    // a zero length, and a negative length is EINVAL. AOK used to reach
    // user_write with the null and answer EFAULT.
    {
        ck("CLEAR", klogctl_(SYSLOG_CLEAR, NULL, 0), 0);
        ck("wrote a line", kmsg_say("syslog_read_clear: SURVIVES-EINVAL"), 0);
        struct { const char *label; int type; char *b; int len; } bad[] = {
            { "READ_CLEAR buf=NULL is EINVAL",          SYSLOG_READ_CLEAR, NULL, 16 },
            { "READ_CLEAR buf=NULL len=0 is EINVAL",    SYSLOG_READ_CLEAR, NULL, 0 },
            { "READ_CLEAR len<0 is EINVAL",             SYSLOG_READ_CLEAR, buf, -1 },
            { "READ_ALL buf=NULL is EINVAL",            SYSLOG_READ_ALL,   NULL, 16 },
            { "READ_ALL buf=NULL len=0 is EINVAL",      SYSLOG_READ_ALL,   NULL, 0 },
            { "READ_ALL len<0 is EINVAL",               SYSLOG_READ_ALL,   buf, -1 },
        };
        for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            errno = 0;
            int r = klogctl_(bad[i].type, bad[i].b, bad[i].len);
            ck(bad[i].label, r < 0 ? -errno : r, -EINVAL);
        }
        ck_ge("  and none of them cleared the log", read_all(), 1);
        ck("  the line is still there", has("SURVIVES-EINVAL"), 1);
    }

    // ---- plain CLEAR (type 5) still answers 0 ----------------------------
    // The other half of the fix: separating the two cases must not give CLEAR
    // a count. Measured on 6.12, it validates nothing -- a negative length and
    // a garbage pointer both return 0.
    {
        for (int i = 0; i < 3; i++)
            kmsg_say("syslog_read_clear: for the CLEAR checks");
        ck_ge("the log is non-empty", read_all(), 1);
        ck("CLEAR returns 0", klogctl_(SYSLOG_CLEAR, NULL, 0), 0);
        ck("  and it cleared", read_all(), 0);
        ck("CLEAR with a buffer still returns 0",
           klogctl_(SYSLOG_CLEAR, buf, (int) sizeof buf), 0);
        ck("CLEAR with len<0 still returns 0", klogctl_(SYSLOG_CLEAR, NULL, -1), 0);
        ck("CLEAR with a garbage pointer still returns 0",
           klogctl_(SYSLOG_CLEAR, (char *) 1, -999), 0);
    }

    // ---- what the user sees: `dmesg -c` prints ---------------------------
    // The whole point. Two things have to be got right for this to mean
    // anything.
    //
    // It must be a dmesg that actually ISSUES SYSLOG_ACTION_READ_CLEAR.
    // busybox's only method is klogctl, so plain `dmesg -c` is right there.
    // util-linux's default method is /dev/kmsg, and `dmesg -c` then never
    // calls syslog(2) at all -- so on a Devuan guest this check passed or
    // failed on something else entirely. Measured on util-linux 2.41 under
    // AOK: plain `dmesg` prints BLANK LINES, because /dev/kmsg here emits
    // "[ctime] text" rather than Linux's "<prio>,<seq>,<usec>,<flag>;text"
    // record format and util-linux parses every line as an empty record.
    // (That is a real and separate gap in the /dev/kmsg node, not something
    // this file can assert on; `dmesg --syslog` is unaffected by it.)
    //
    // So: prefer `--syslog`, which forces the klogctl method, and fall back
    // to plain `-c` for a dmesg that does not know the option -- which is
    // busybox, whose only method is klogctl anyway. The probe runs BEFORE the
    // marker is written, because a successful one clears the log.
    //
    // What this block is worth, honestly, differs by root. Against the
    // pre-fix kernel busybox's `dmesg -c` printed NOTHING and these checks
    // failed, which is the bug as a user meets it. util-linux's
    // `dmesg --syslog -c` printed the line anyway -- measured, on the same
    // broken kernel -- so it does not depend on what READ_CLEAR returns and
    // this block cannot fail there. The klogctl assertions above are the real
    // coverage on such a root: 14 of them fail against the pre-fix kernel.
    {
        int have_syslog_opt = system("dmesg --syslog -c >/dev/null 2>&1") == 0;
        const char *dmesg_cmd = have_syslog_opt
            ? "dmesg --syslog -c 2>/dev/null"
            : "dmesg -c 2>/dev/null";
        test_logf("  %-58s %s\n", "dmesg command", dmesg_cmd);

        ck("CLEAR", klogctl_(SYSLOG_CLEAR, NULL, 0), 0);
        ck("wrote the line dmesg should print",
           kmsg_say("syslog_read_clear: DMESG-SHOULD-PRINT-THIS"), 0);

        FILE *p = popen(dmesg_cmd, "r");
        if (p == NULL) {
            test_logf("  %-58s (no popen)\n", "dmesg -c");
        } else {
            static char out[1 << 16];
            size_t got = fread(out, 1, sizeof out - 1, p);
            out[got] = '\0';
            int status = pclose(p);
            if (status != 0 && got == 0) {
                printf("syslog_read_clear: note: no usable dmesg (status %d), "
                       "skipping the end-to-end check\n", status);
            } else {
                ck("`dmesg -c` printed something", got > 0 ? 1 : 0, 1);
                ck("  and it is the line written",
                   strstr(out, "DMESG-SHOULD-PRINT-THIS") != NULL ? 1 : 0, 1);

                // And the clear took: a second one must not repeat the line.
                p = popen(dmesg_cmd, "r");
                if (p != NULL) {
                    got = fread(out, 1, sizeof out - 1, p);
                    out[got] = '\0';
                    pclose(p);
                    ck("  a second `dmesg -c` does not repeat it",
                       strstr(out, "DMESG-SHOULD-PRINT-THIS") != NULL ? 1 : 0, 0);
                }
            }
        }
    }

    // ---- leave the log non-empty -----------------------------------------
    // Every clear above hides the buffer from READ_ALL. /dev/kmsg positions
    // itself absolutely and is unaffected, but the suite shares one guest and
    // a neighbour asserting on a non-empty log should not have to care which
    // reader this one used.
    {
        for (int i = 0; i < 8; i++)
            kmsg_say("syslog_read_clear: refilling the log for the next test");
        ck_ge("left lines in the log for the next test", read_all(), 1);
    }

    if (watchdog > 0) {
        kill(watchdog, SIGKILL);
        waitpid(watchdog, NULL, 0);
    }
    return finish_suite("syslog_read_clear");
}
