// /dev/aokgfx speaks its wire format, and refuses the things it should.
//
// The framing case that matters is fragmentation. A guest batches a whole
// frame into one write(2) -- that is the entire point of the protocol, since a
// syscall per vertex would be slower than the VNC path it replaces -- but
// nothing guarantees the batch lands in one piece, and a device that assumed
// it did would corrupt geometry only under load. /proc/ish/roots learned the
// same lesson ("a write is a FRAGMENT, not a command"), so this writes an
// identical frame three ways: whole, split mid-command, and one byte at a
// time. All three must be accepted and parse to the same commands.
//
// The handshake is checked in both directions because it is what protects
// against the skew this protocol is guaranteed to meet: the guest half ships
// in a root filesystem, which is downloaded independently of the app, so an
// old rootfs WILL meet a new iSH-AOK. A version mismatch has to be an errno,
// not garbled output.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "test_common.h"

#define AOKGFX_MAJOR 242
#define OP_HELLO     0x0001
#define OP_PRESENT   0x0004
#define OP_CLEAR     0x0100
#define OP_DRAW_LINE 0x0103
#define EV_HELLO     0x8001
#define PROTOCOL_VERSION 1

struct cmd { uint16_t op; uint16_t len; uint32_t surface; };
struct hello { uint32_t version, flags; };
struct color { float r, g, b, a; };
struct rect { int32_t x0, y0, x1, y1; };

static size_t put(uint8_t *b, uint16_t op, const void *pl, size_t n) {
    struct cmd c = { op, (uint16_t) (sizeof(c) + n), 0 };
    memcpy(b, &c, sizeof(c));
    if (n != 0)
        memcpy(b + sizeof(c), pl, n);
    return sizeof(c) + n;
}

static void check(int ok, const char *what) {
    if (ok)
        return;
    failf(what, 0, 0, 0, 1, 0, 0);
}

// devtmpfs creates this node, but a root that never mounted devtmpfs has no
// /dev/aokgfx -- which is most fakefs roots. Make our own rather than skip:
// the device is what is under test, not the distro's boot scripts.
static const char *device_path(void) {
    static char path[] = "/tmp/aokgfx-test-node";
    if (access("/dev/aokgfx", F_OK) == 0)
        return "/dev/aokgfx";
    unlink(path);
    if (mknod(path, S_IFCHR | 0666, makedev(AOKGFX_MAJOR, 0)) != 0)
        return NULL;
    return path;
}

// Writes `n` bytes in `chunk`-sized pieces; 0 means all at once. Returns 1 if
// every piece was accepted in full.
static int write_fragmented(int fd, const uint8_t *b, size_t n, size_t chunk) {
    if (chunk == 0)
        return write(fd, b, n) == (ssize_t) n;
    for (size_t off = 0; off < n; off += chunk) {
        size_t this = n - off < chunk ? n - off : chunk;
        if (write(fd, b + off, this) != (ssize_t) this)
            return 0;
    }
    return 1;
}

static size_t build_frame(uint8_t *b) {
    struct color red = { 1, 0, 0, 1 };
    struct rect line = { 0, 0, 100, 100 };
    size_t n = put(b, OP_CLEAR, &red, sizeof(red));
    for (int i = 0; i < 8; i++)
        n += put(b + n, OP_DRAW_LINE, &line, sizeof(line));
    n += put(b + n, OP_PRESENT, NULL, 0);
    return n;
}

static int open_and_greet(const char *path) {
    int fd = open(path, O_RDWR);
    if (fd < 0)
        return -1;
    uint8_t b[64];
    struct hello hello = { PROTOCOL_VERSION, 0 };
    size_t n = put(b, OP_HELLO, &hello, sizeof(hello));
    if (write(fd, b, n) != (ssize_t) n) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    const char *path = device_path();
    check(path != NULL, "aokgfx device node available");
    if (path == NULL)
        return finish_suite("aokgfx_protocol");

    uint8_t b[1024];

    // A version this build cannot speak must be refused, not tolerated.
    int fd = open(path, O_RDWR);
    check(fd >= 0, "open");
    struct hello wrong = { PROTOCOL_VERSION + 1000, 0 };
    size_t n = put(b, OP_HELLO, &wrong, sizeof(wrong));
    check(write(fd, b, n) < 0, "unknown protocol version refused");
    close(fd);

    // Nothing may be drawn before the handshake.
    fd = open(path, O_RDWR);
    struct color red = { 1, 0, 0, 1 };
    n = put(b, OP_CLEAR, &red, sizeof(red));
    check(write(fd, b, n) < 0, "command before HELLO refused");
    close(fd);

    // The real handshake, and the ack the guest reads back.
    fd = open_and_greet(path);
    check(fd >= 0, "HELLO accepted");
    if (fd < 0)
        return finish_suite("aokgfx_protocol");

    uint8_t ev[128];
    ssize_t got = read(fd, ev, sizeof(ev));
    struct cmd ec;
    memcpy(&ec, ev, sizeof(ec));
    check(got >= (ssize_t) (sizeof(ec) + sizeof(struct hello)), "ack readable");
    check(ec.op == EV_HELLO, "ack is EV_HELLO");
    struct hello ack;
    memcpy(&ack, ev + sizeof(ec), sizeof(ack));
    check(ack.version == PROTOCOL_VERSION, "ack carries the negotiated version");

    // The same frame, three ways. Any of these failing while the others pass
    // is a fragmentation bug, which is the failure mode that would otherwise
    // only appear under load.
    n = build_frame(b);
    check(write_fragmented(fd, b, n, 0), "frame in one write");
    check(write_fragmented(fd, b, n, n / 3), "frame split mid-command");
    check(write_fragmented(fd, b, n, 1), "frame one byte at a time");

    // A length that cannot be a command at all: the stream is unparseable
    // from here, so it must be rejected rather than resynchronised past.
    struct cmd bogus = { OP_CLEAR, 3, 0 };
    memcpy(b, &bogus, sizeof(bogus));
    check(write(fd, b, sizeof(bogus)) < 0, "command shorter than its header rejected");
    close(fd);

    // The channel is single-open: two programs interleaving half-commands into
    // one surface is not a thing the host could untangle.
    fd = open(path, O_RDWR);
    check(fd >= 0, "reopen after close");
    int second = open(path, O_RDWR);
    check(second < 0, "second open refused while the first is held");
    if (second >= 0)
        close(second);
    close(fd);

    return finish_suite("aokgfx_protocol");
}
