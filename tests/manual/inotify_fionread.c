// FIONREAD on an inotify descriptor.
//
// inotify's fd_ops had no ioctl at all, so ioctl(fd, FIONREAD, &n) failed with
// ENOTTY. Linux answers it (inotify_ioctl() in fs/notify/inotify/
// inotify_user.c) with the number of bytes a read would return right now:
// every queued struct inotify_event plus its name, padded the way read pads it.
//
// GLib depends on it. Its inotify backend (gio/inotify/inotify-kernel.c) reads
// into a 4096-byte buffer, and whenever a read comes back within one maximal
// event (16 + NAME_MAX + 1 = 272 bytes) of full it asks FIONREAD how much is
// left, so it can take the rest in one go. A failed ioctl there is g_error():
// "inotify ioctl(FIONREAD): Inappropriate ioctl for device", after which the
// "gmain" thread spun at 100% CPU for good. Thunar hit it about 8 s after every
// launch, and any GLib program watching a directory hits it the first time a
// burst of changes queues more than ~3.8 KB of events. Case [4] is that shape.
//
// Every expectation here was checked against Linux 6.12 (x86_64, glibc, both
// 64-bit and -m32).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>

#include "test_common.h"

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-60s got=%-8ld want=%ld\n", label, got, want);
}

static char base[160];

// Create base/<len x c> and return the bytes its IN_CREATE record occupies:
// the 16-byte header plus strlen+1 rounded up to a multiple of 16.
static long create_named(size_t len, char c) {
    char name[300], path[500];
    memset(name, c, len);
    name[len] = '\0';
    snprintf(path, sizeof path, "%s/%s", base, name);
    int f = open(path, O_RDWR | O_CREAT, 0644);
    if (f < 0) {
        printf("FAIL could not create %s: %s\n", path, strerror(errno));
        failures_total++;
        return 0;
    }
    close(f);
    return (long) sizeof(struct inotify_event) + (long) ((len + 1 + 15) & ~(size_t) 15);
}

// ioctl(FIONREAD), or -errno.
static long fionread(int fd) {
    int n = -12345;
    if (ioctl(fd, FIONREAD, &n) != 0)
        return -errno;
    return n;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    // A blocking FIONREAD on an empty queue is one of the cases: a hang is a
    // failure, not a stuck suite.
    alarm(test_watchdog_secs(120));
    snprintf(base, sizeof base, "/tmp/inotfionread-%d", (int) getpid());
    { char c[400]; snprintf(c, sizeof c, "rm -rf '%s' && mkdir -p '%s'", base, base);
      if (system(c) < 0) { printf("FAIL could not make the scratch dir\n"); return 1; } }

    test_logf("[1] an empty queue reports 0, and does not block\n");
    {
        int fd = inotify_init1(0);            // blocking on purpose
        ck("inotify_init1", fd >= 0, 1);
        ck("FIONREAD on a fresh inotify fd", fionread(fd), 0);
        int wd = inotify_add_watch(fd, base, IN_CREATE);
        ck("inotify_add_watch", wd > 0, 1);
        ck("FIONREAD with a watch and nothing queued", fionread(fd), 0);
        close(fd);
    }

    test_logf("[2] it writes an int and nothing past it\n");
    {
        int fd = inotify_init1(IN_NONBLOCK);
        inotify_add_watch(fd, base, IN_CREATE);
        long want = create_named(3, 'i');
        int words[2] = { -1, 0x5a5a5a5a };
        errno = 0;
        int r = ioctl(fd, FIONREAD, &words[0]);
        ck("ioctl(FIONREAD) returns 0", r == 0 ? 0 : -errno, 0);
        ck("  the int holds the queued byte count", words[0], want);
        ck("  the next int is untouched", words[1], 0x5a5a5a5a);
        close(fd);
    }

    test_logf("[3] the count is exactly what a read returns, name padding included\n");
    {
        int fd = inotify_init1(IN_NONBLOCK);
        inotify_add_watch(fd, base, IN_CREATE);
        // Name lengths either side of each 16-byte boundary: 15+1 fits one
        // block, 16+1 needs two, and so on.
        static const size_t lens[] = { 1, 14, 15, 16, 17, 31, 32, 33, 200 };
        long want = 0;
        for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++)
            want += create_named(lens[i], (char) ('a' + i));
        test_logf("    predicted %ld bytes\n", want);
        ck("FIONREAD after nine creates", fionread(fd), want);
        ck("  asking twice does not consume anything", fionread(fd), want);
        char buf[8192];
        ssize_t r = read(fd, buf, sizeof buf);
        ck("  a large read returns that many bytes", (long) r, want);
        ck("  and the queue is then empty", fionread(fd), 0);
        close(fd);
    }

    test_logf("[3b] a record with no name is the bare 16-byte header\n");
    {
        int fd = inotify_init1(IN_NONBLOCK);
        int wd = inotify_add_watch(fd, base, IN_CREATE);
        ck("inotify_rm_watch", inotify_rm_watch(fd, wd), 0);   // queues IN_IGNORED
        ck("FIONREAD with only IN_IGNORED queued", fionread(fd),
           (long) sizeof(struct inotify_event));
        char buf[256];
        ssize_t r = read(fd, buf, sizeof buf);
        ck("  and a read returns the same", (long) r, (long) sizeof(struct inotify_event));
        close(fd);
    }

    test_logf("[4] GLib's shape: a 4096-byte read nearly fills, FIONREAD sizes the rest\n");
    {
        int fd = inotify_init1(IN_NONBLOCK);
        inotify_add_watch(fd, base, IN_CREATE);
        long total = 0;
        char tag = 'A';
        for (int i = 0; i < 60; i++) {
            // 64-character names: 16 + 80 = 96 bytes per record, 5760 in all.
            char name[80], path[300];
            snprintf(name, sizeof name, "%c%063d", tag, i);
            snprintf(path, sizeof path, "%s/%s", base, name);
            int f = open(path, O_RDWR | O_CREAT, 0644);
            if (f >= 0)
                close(f);
            total += 96;
        }
        ck("FIONREAD before any read", fionread(fd), total);
        char buf[4096];
        ssize_t first = read(fd, buf, sizeof buf);
        test_logf("    first read took %zd of %ld bytes\n", first, total);
        ck("  the first read stops on a record boundary", first > 0 && first % 96 == 0, 1);
        // GLib's test for "there may be more": n_read + MAX_EVENT_SIZE > len.
        ck("  and is close enough to full that GLib asks FIONREAD",
           (long) first + 16 + 256 > (long) sizeof buf, 1);
        long rest = fionread(fd);
        ck("  FIONREAD reports exactly the remainder", rest, total - (long) first);
        if (rest > 0) {
            char *more = malloc((size_t) rest);
            ssize_t second = more != NULL ? read(fd, more, (size_t) rest) : -1;
            ck("  a read of exactly that size takes all of it", (long) second, rest);
            free(more);
        }
        ck("  after which FIONREAD is 0", fionread(fd), 0);
        close(fd);
    }

    test_logf("[5] other terminal ioctls are still refused\n");
    {
        int fd = inotify_init1(IN_NONBLOCK);
        struct termios t;
        errno = 0;
        ck("TCGETS on an inotify fd", tcgetattr(fd, &t) == 0 ? 0 : errno, ENOTTY);
        close(fd);
    }

    { char c[400]; snprintf(c, sizeof c, "rm -rf '%s'", base); if (system(c) < 0) {} }
    return finish_suite("inotify_fionread");
}
