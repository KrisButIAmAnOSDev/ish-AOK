#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel/errno.h"
#include "kernel/log.h"
#include "kernel/random.h"
#include "fs/poll.h"
#include "fs/mem.h"
#include "fs/dev.h"
#include "fs/devices.h"

extern struct dev_ops
    null_dev,
    zero_dev,
    full_dev,
    random_dev,
    kmsg_dev;

// this file handles major device number MEM_MAJOR, minor device numbers are mapped in table below
struct dev_ops *mem_devs[256] = {
    // [1] = &prog_mem_dev,
    // [2] = &kmem_dev, // (not really applicable)
    [DEV_NULL_MINOR] = &null_dev,
    // [4] = &port_dev,
    [DEV_ZERO_MINOR] = &zero_dev,
    [DEV_FULL_MINOR] = &full_dev,
    [DEV_RANDOM_MINOR] = &random_dev,
    [DEV_URANDOM_MINOR] = &random_dev,
    // [10] = &aio_dev,
    [DEV_KMSG_MINOR] = &kmsg_dev,
    // [12] = &oldmem_dev, // replaced by /proc/vmcore
};

// dispatch device for major device 1
static int mem_open(int major, int minor, struct fd *fd) {
    struct dev_ops *dev = mem_devs[minor];
    if (dev == NULL) {
        return _ENXIO;
    }
    fd->ops = &dev->fd;
    if (!dev->open)
        return 0;
    return dev->open(major, minor, fd);
}

struct dev_ops mem_dev = {
    .open = mem_open,
};

static int ready_poll(struct fd *UNUSED(fd)) {
    return POLL_READ | POLL_WRITE;
}

// begin inline devices
static int null_open(int UNUSED(major), int UNUSED(minor), struct fd *UNUSED(fd)) {
    return 0;
}
static ssize_t null_read(struct fd *UNUSED(fd), void *UNUSED(buf), size_t UNUSED(bufsize)) {
    return 0;
}
static ssize_t null_write(struct fd *UNUSED(fd), const void *UNUSED(buf), size_t bufsize) {
    return bufsize;
}
static off_t_ null_lseek(struct fd *UNUSED(fd), off_t_ UNUSED(off), int UNUSED(whence)) {
    return 0;
}
struct dev_ops null_dev = {
    .open = null_open,
    .fd.read = null_read,
    .fd.write = null_write,
    .fd.lseek = null_lseek,
    .fd.poll = ready_poll,
};

static ssize_t zero_read(struct fd *UNUSED(fd), void *buf, size_t bufsize) {
    memset(buf, 0, bufsize);
    return bufsize;
}
static ssize_t zero_write(struct fd *UNUSED(fd), const void *UNUSED(buf), size_t bufsize) {
    return bufsize;
}
// mmap of /dev/zero is a plain zero-filled mapping -- exactly MAP_ANONYMOUS,
// which is what Linux does with it (mmap_zero in drivers/char/mem.c). Without
// an .mmap op the generic path returned ENODEV, so the oldest portable idiom
// for getting anonymous memory failed outright.
//
// Only /dev/zero. /dev/null and /dev/full have no mmap in Linux either and
// keep returning ENODEV; that was measured, not assumed.
static int zero_mmap(struct fd *UNUSED(fd), struct mem *mem, page_t start,
                     pages_t pages, off_t UNUSED(offset), int prot, int UNUSED(flags)) {
    // prot already carries P_SHARED when the caller asked for MAP_SHARED, so a
    // shared mapping of /dev/zero is shared anonymous memory, as on Linux.
    return pt_map_nothing(mem, start, pages, prot);
}

struct dev_ops zero_dev = {
    .open = null_open,
    .fd.read = zero_read,
    .fd.write = zero_write,
    .fd.lseek = null_lseek,
    .fd.poll = ready_poll,
    .fd.mmap = zero_mmap,
};

static ssize_t full_write(struct fd *UNUSED(fd), const void *UNUSED(buf), size_t UNUSED(bufsize)) {
    return _ENOSPC;
}
struct dev_ops full_dev = {
    .open = null_open,
    .fd.read = zero_read,
    .fd.write = full_write,
    .fd.lseek = null_lseek,
    .fd.poll = ready_poll,
};

static ssize_t random_read(struct fd *UNUSED(fd), void *buf, size_t bufsize) {
    get_random(buf, bufsize);
    return bufsize;
}

static ssize_t random_ioctl_size(int cmd) {
    switch (cmd) {
        case RNDGETENTCNT_: case RNDADDTOENTCNT_:
            return sizeof(dword_t);
        case RNDADDENTROPY_:
            // struct rand_pool_info header { int entropy_count; int buf_size; };
            // the variable-length entropy payload that follows is ignored.
            return 2 * sizeof(dword_t);
        case RNDZAPENTCNT_: case RNDCLEARPOOL_: case RNDRESEEDCRNG_:
            return 0;
    }
    return -1;
}

// iSH has no real entropy pool — randomness comes from the host CSPRNG, so the
// pool is always full and crediting/reseeding are no-ops. We only answer the
// "how much entropy is available" query so callers (seedrng's RNDADDENTROPY,
// rng-tools) succeed instead of getting ENOTTY and reporting a seeding error.
static int random_ioctl(struct fd *UNUSED(fd), int cmd, void *arg) {
    switch (cmd) {
        case RNDGETENTCNT_:
            *(dword_t *) arg = RANDOM_POOL_BITS;
            return 0;
        case RNDADDTOENTCNT_:
        case RNDADDENTROPY_:
        case RNDZAPENTCNT_:
        case RNDCLEARPOOL_:
        case RNDRESEEDCRNG_:
            return 0;
    }
    return _ENOTTY;
}

struct dev_ops random_dev = {
    .open = null_open,
    .fd.read = random_read,
    .fd.write = null_write,
    .fd.lseek = null_lseek,
    .fd.poll = ready_poll,
    .fd.ioctl_size = random_ioctl_size,
    .fd.ioctl = random_ioctl,
};

// Open /dev/kmsg fds. A log line arrives from anywhere in the emulator and
// has no idea who is watching, so the watchers are kept here -- the same shape
// as fs/proc.c's mountinfo watch list, for the same reason.
static struct list kmsg_fds = LIST_INITIALIZER(kmsg_fds);
static lock_t kmsg_fds_lock = LOCK_INITIALIZER;

static int kmsg_open(int UNUSED(major), int UNUSED(minor), struct fd *fd) {
    // Start at the oldest line still buffered rather than at "now": a fresh
    // open of /dev/kmsg reads the buffer from its start on Linux too
    // (devkmsg_open seeks to the first valid record, the same place SEEK_SET
    // names), so a daemon started after boot still gets the boot messages.
    // ish_log_line_seek clamps 0 forward to the oldest whole line and hands
    // back its sequence number.
    uint64_t at = 0;
    fd->kmsg.seq = ish_log_line_seek(&at);
    fd->offset = (unsigned long) at;
    lock(&kmsg_fds_lock, 0);
    list_add(&kmsg_fds, &fd->kmsg.link);
    unlock(&kmsg_fds_lock);
    return 0;
}

static int kmsg_close(struct fd *fd) {
    lock(&kmsg_fds_lock, 0);
    if (!list_null(&fd->kmsg.link))
        list_remove(&fd->kmsg.link);
    unlock(&kmsg_fds_lock);
    return 0;
}

void kmsg_notify_readers(void) {
    // ish_vprintk calls this with log_lock released, so nothing here can be
    // waiting on it. poll_wakeup's FIXME path logs, though, which would come
    // straight back in and try to take a poll_lock this thread already holds.
    static __thread bool notifying = false;
    if (notifying)
        return;
    notifying = true;
    lock(&kmsg_fds_lock, 0);
    struct fd *fd;
    list_for_each_entry(&kmsg_fds, fd, kmsg.link)
        poll_wakeup(fd, POLL_READ);
    unlock(&kmsg_fds_lock);
    notifying = false;
}

// The kernel log as a byte stream, which is what /proc/kmsg serves
// (fs/proc/root.c). /dev/kmsg no longer comes through here -- it serves
// records, see kmsg_read below. Positions are absolute -- see ish_log_read_at.
ssize_t kmsg_stream_read(unsigned long *pos, void *buf, size_t bufsize, bool nonblock) {
    // A zero-length read returns 0 at once. POSIX says so, every Linux driver
    // implements it, and here it is load-bearing rather than pedantic: the
    // loop below cannot terminate without it. ish_log_read_at can only ever
    // copy zero bytes for a zero-length buffer, which this loop reads as
    // "nothing new", so it waits -- and wakes immediately, because there IS
    // something new -- and asks again, forever.
    //
    // rsyslogd probes /proc/kmsg with exactly this call at startup, and the
    // spin sits inside kernel code with no syscall boundary in it, so no
    // guest signal can land and the task cannot even be killed. Boot stopped
    // there: rsyslogd never answered, and init gave up on it sixty seconds
    // later having pinned a CPU the whole time.
    if (bufsize == 0)
        return 0;

    for (;;) {
        uint64_t at = *pos;
        uint64_t started_at = at;
        ssize_t res = ish_log_read_at(&at, buf, bufsize);
        if (res != 0) {
            if (res > 0) {
                // Stop at the first newline so a line is never split across
                // two reads. A reader whose buffer did not land on a line
                // boundary got a partial line and logged it as a whole one --
                // busybox's klogd logged the tail of a timestamp as its own
                // syslog entry.
                const char *nl = memchr(buf, '\n', (size_t) res);
                if (nl != NULL) {
                    size_t upto = (size_t) (nl - (const char *) buf) + 1;
                    if (upto < (size_t) res) {
                        res = (ssize_t) upto;
                        at = started_at + upto;
                    }
                }
                *pos = (unsigned long) at;
            }
            return res;
        }
        // Nothing new. Linux blocks here, and a log daemon's entire main loop
        // is this read: answering 0 turned that loop into a spin, which is
        // why the device node was never created in the first place.
        if (nonblock)
            return _EAGAIN;
        int err = ish_log_wait_past(*pos);
        if (err < 0)
            return err;
    }
}

int kmsg_stream_poll(unsigned long pos) {
    return ish_log_total_written() > pos ? POLL_READ : 0;
}

// ---- /dev/kmsg's record format ----------------------------------------
//
// Linux hands the guest one whole RECORD per read, not a slice of a byte
// stream:
//
//     prio,seq,timestamp_usec,flag;text\n
//
// util-linux's dmesg parses that field by field -- facility/level, then the
// sequence number, then the microsecond timestamp, then an optional flag --
// and takes everything after the ';' as the message. Handed AOK's bare
// "[Thu Sep 17 21:01:27 2026] text" it found no ';' at all and read every line
// as an empty message: `/bin/dmesg` in a Devuan guest printed nothing but
// blank lines, while `dmesg --syslog`, which goes to syslog(2) instead, was
// fine. Measured against util-linux 2.41, and against Linux 6.12 for
// everything below.
//
// The cost is that `cat /dev/kmsg` is now a machine format rather than the
// log as a human reads it -- exactly as it is on Linux, and for the same
// unavoidable reason: the header is what makes the line parseable, and it has
// to come before the text. Nothing is lost by it. /proc/kmsg and
// `dmesg --syslog` still serve the same bytes AOK has always printed, so the
// human view is one command away, and `dmesg` renders these records the way
// it renders a real kernel's.

// facility 0 (kern), level 6 (info). AOK's printk carries no level -- there is
// nowhere in a byte-stream log to keep one, and kmsg_write has always dropped
// the "<N>" a guest writes -- so every record gets the one that describes
// them: informational kernel messages. `dmesg --decode` reads it as kern.info,
// and a level filter behaves, which it would not if everything claimed to be a
// warning.
#define KMSG_PRIORITY 6

// output_line() stamps every stored line with ctime(3)'s fixed 24-character
// form in brackets: "[Www Mmm dd hh:mm:ss yyyy] ", 27 characters in all.
#define KMSG_CTIME_LEN 27

// Most log lines are well under this; the rest take a malloc.
#define KMSG_LINE_FAST 1024

static int kmsg_month(const char *s) {
    static const char names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    for (int i = 0; i < 12; i++)
        if (memcmp(s, &names[i * 3], 3) == 0)
            return i;
    return -1;
}

// ctime space-pads the day of the month, so " 7" is a number here.
static bool kmsg_two_digits(const char *s, int *out) {
    if (s[0] == ' ')
        return s[1] >= '0' && s[1] <= '9' ? (*out = s[1] - '0', true) : false;
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9')
        return false;
    *out = (s[0] - '0') * 10 + (s[1] - '0');
    return true;
}

// Recover a record's timestamp from the stamp output_line() already wrote, and
// report where the message text begins after it.
//
// That stamp is the only per-line time the log keeps, so it is the only honest
// source for this field, and once it has been read out of the line it is
// dropped from the record text -- otherwise dmesg would print its own rendering
// of the timestamp and then the same time again in words, from the same clock.
// Nothing is lost by that: /proc/kmsg still carries the line verbatim.
//
// The resolution is whole seconds, because ctime's is. The origin is boot_time,
// which is where kernel/task.c puts the guest's uptime zero, so these agree
// with /proc/uptime and with /proc/stat's btime.
//
// They do NOT currently agree with what `dmesg -T` prints, and that is a
// separate gap: util-linux derives the boot instant as "now minus
// CLOCK_BOOTTIME", and AOK's CLOCK_BOOTTIME and CLOCK_MONOTONIC report the
// HOST's uptime rather than the guest's (measured: /proc/uptime 0.77 against
// CLOCK_BOOTTIME 1264336). The relative times `dmesg` prints are right either
// way; only the absolute ones dmesg reconstructs are off, by however long the
// host had been up when the guest booted.
static uint64_t kmsg_line_time(const char *line, size_t len, size_t *text_off) {
    extern time_t boot_time;
    *text_off = 0;
    if (len < KMSG_CTIME_LEN)
        return 0;
    if (line[0] != '[' || line[4] != ' ' || line[8] != ' ' || line[11] != ' ' ||
        line[14] != ':' || line[17] != ':' || line[20] != ' ' ||
        line[25] != ']' || line[26] != ' ')
        return 0;
    int mon = kmsg_month(&line[5]);
    int day, hour, min, sec;
    if (mon < 0 || !kmsg_two_digits(&line[9], &day) ||
        !kmsg_two_digits(&line[12], &hour) || !kmsg_two_digits(&line[15], &min) ||
        !kmsg_two_digits(&line[18], &sec))
        return 0;
    int year = 0;
    for (int i = 21; i < 25; i++) {
        if (line[i] < '0' || line[i] > '9')
            return 0;
        year = year * 10 + (line[i] - '0');
    }

    struct tm tm = {
        .tm_year = year - 1900, .tm_mon = mon, .tm_mday = day,
        .tm_hour = hour, .tm_min = min, .tm_sec = sec,
        // ctime() rendered local time, so mktime() is its exact inverse. -1
        // lets it work out DST; the one ambiguous hour a year can land on
        // either side of the change, which costs an hour on those records and
        // nothing on any other.
        .tm_isdst = -1,
    };
    time_t when = mktime(&tm);
    if (when == (time_t) -1)
        return 0;
    // Only now is the stamp known to be one of ours, so only now is it right
    // to take it off the text.
    *text_off = KMSG_CTIME_LEN;
    if (when <= boot_time)
        return 0;
    return (uint64_t) (when - boot_time) * 1000000;
}

// Write the record for one line into the guest's buffer. Returns its length,
// or _EINVAL if the buffer cannot hold the whole thing -- which is what Linux
// answers a reader whose buffer is too small for a record.
static ssize_t kmsg_format_record(char *out, size_t outsize, uint64_t seq,
                                  const char *line, size_t len) {
    size_t text_off = 0;
    uint64_t usec = kmsg_line_time(line, len, &text_off);

    int header = snprintf(out, outsize, "%u,%llu,%llu,-;", KMSG_PRIORITY,
                          (unsigned long long) seq, (unsigned long long) usec);
    if (header < 0 || (size_t) header >= outsize)
        return _EINVAL;
    size_t at = (size_t) header;

    for (size_t i = text_off; i < len; i++) {
        unsigned char c = (unsigned char) line[i];
        // Exactly what Linux escapes (msg_print_ext_body): control bytes, the
        // high half, and the backslash itself. It is not decoration -- an
        // unescaped byte in the text could be read as the '\n' that ends the
        // record or as the space that starts a continuation line, and a guest
        // can put any byte here with a write to /dev/kmsg.
        if (c < ' ' || c >= 0x7f || c == '\\') {
            static const char hex[] = "0123456789abcdef";
            if (at + 4 > outsize)
                return _EINVAL;
            out[at++] = '\\';
            out[at++] = 'x';
            out[at++] = hex[c >> 4];
            out[at++] = hex[c & 0xf];
        } else {
            if (at + 1 > outsize)
                return _EINVAL;
            out[at++] = (char) c;
        }
    }
    if (at + 1 > outsize)
        return _EINVAL;
    out[at++] = '\n';
    return (ssize_t) at;
}

// /dev/kmsg hands back one whole record per read, so a buffer too small to
// hold one is a bad argument rather than an empty answer or a partial record:
// there would be no way for the reader to tell a truncated record from a whole
// one. Linux answers EINVAL, and a zero-length buffer can never hold a record,
// so that is EINVAL too -- measured on 6.12 at 8, 1 and 0 bytes. Measured on
// Devuan, where the same read of /proc/kmsg returns 0 instead: /proc/kmsg is a
// byte stream and has nothing to object to. The two used to share an
// implementation, so the distinction lived at this end of it; they no longer
// do, and it lives in the formatting.
//
// The record is CONSUMED either way. That is Linux's behaviour and not an
// obvious one -- devkmsg_read advances the reader past the record before it
// checks the size, so the message the caller could not receive is gone.
// Measured, because it looked like a wart worth not copying: with two lines
// waiting, a four-byte read returns EINVAL and the next full read gives the
// SECOND line. Keeping the record instead would be friendlier right up until a
// reader retried, which would then hand it the same EINVAL forever.
static ssize_t kmsg_read(struct fd *fd, void *buf, size_t bufsize) {
    char fast[KMSG_LINE_FAST];
    char *line = fast;
    size_t line_cap = sizeof fast;
    ssize_t res;

    for (;;) {
        uint64_t pos = fd->offset;
        uint64_t next = pos;
        size_t needed = 0;
        ssize_t len = ish_log_peek_line(pos, line, line_cap, &next, &needed);

        if (len == _E2BIG) {
            // A line longer than the fast path's buffer. printk lines are tens
            // of bytes, so pay for this only when it actually happens -- and
            // go round again rather than trusting `needed`, since the log can
            // move on between the two calls.
            if (line != fast)
                free(line);
            line = malloc(needed);
            if (line == NULL) {
                line = fast;
                line_cap = sizeof fast;
                res = _ENOMEM;
                break;
            }
            line_cap = needed;
            continue;
        }

        if (len == _EPIPE) {
            // The reader's position has fallen off the back of the buffer.
            // Linux says so exactly once, resets the reader to the oldest
            // record still there, and answers the next read normally --
            // util-linux's dmesg retries on EPIPE and on nothing else.
            uint64_t at = pos;
            fd->kmsg.seq = ish_log_line_seek(&at);
            fd->offset = (unsigned long) at;
            res = _EPIPE;
            break;
        }

        if (len < 0) {
            res = len;
            break;
        }

        // A line was found -- `next` moved -- even if it was empty. Testing
        // the length alone would read an empty line as "caught up" and wait
        // for something that had already arrived.
        if (len > 0 || next != pos) {
            res = kmsg_format_record(buf, bufsize, fd->kmsg.seq, line, (size_t) len);
            // Before the result is looked at, so a record the guest's buffer
            // could not hold is consumed rather than handed back forever.
            fd->offset = (unsigned long) next;
            fd->kmsg.seq++;
            break;
        }

        // Caught up. Linux blocks here, and a log daemon's entire main loop is
        // this read: answering 0 would turn that loop into a spin.
        if (fd->flags & O_NONBLOCK_) {
            res = _EAGAIN;
            break;
        }
        int err = ish_log_wait_past(fd->offset);
        if (err < 0) {
            res = err;
            break;
        }
    }

    if (line != fast)
        free(line);
    return res;
}

// Linux ignores the offset on a pread of /dev/kmsg outright -- devkmsg_read
// never looks at ppos -- so pread reads the NEXT record and advances the reader
// exactly as a plain read does. Measured on 6.12: a read, a pread at 0, a pread
// at 12345 and a read returned four consecutive sequence numbers.
//
// Not optional. Without it the generic fallback emulates pread with a pair of
// lseeks, and this driver's lseek does not take byte offsets: it would answer
// EINVAL to the LSEEK_CUR that saves the position, move the reader to the
// oldest record, and then trip the assert that the restoring seek cannot fail.
static ssize_t kmsg_pread(struct fd *fd, void *buf, size_t bufsize, off_t UNUSED(off)) {
    return kmsg_read(fd, buf, bufsize);
}

static int kmsg_poll(struct fd *fd) {
    return kmsg_stream_poll(fd->offset);
}

// Linux injects a write into the ring buffer, which is how `echo x >
// /dev/kmsg` and `logger --kernel` put a line in dmesg -- boot scripts and
// initramfs hooks use it to say where they got to. The node's 0644 keeps it
// to root, as there.
//
// A leading "<N>" is the syslog priority/facility, which Linux strips from
// the stored text. There is nowhere to route the level here, so honour the
// syntax -- a line beginning "<6>" must not appear with the marker still on
// it -- and drop the value.
#define KMSG_WRITE_MAX 1024
static ssize_t kmsg_write(struct fd *UNUSED(fd), const void *buf, size_t bufsize) {
    if (bufsize == 0)
        return 0;
    // One write is one record, and Linux caps a record at PRINTKRB_RECORD_MAX
    // -- 1024 bytes. Anything longer is rejected outright rather than stored
    // truncated (measured on 6.12: 1024 is accepted and returns 1024, 1025 is
    // EINVAL). This used to truncate silently and report the whole write
    // consumed, which loses the tail of a message without telling anyone.
    if (bufsize > KMSG_WRITE_MAX)
        return _EINVAL;
    const char *msg = buf;
    size_t len = bufsize;
    size_t skip = 0;
    if (len > 2 && msg[0] == '<') {
        size_t i = 1;
        while (i < len && msg[i] >= '0' && msg[i] <= '9')
            i++;
        if (i > 1 && i < len && msg[i] == '>')
            skip = i + 1;
    }
    size_t n = len - skip;
    // printk stores one line per call; a trailing newline of our own would
    // leave a blank line between every injected message.
    while (n > 0 && msg[skip + n - 1] == '\n')
        n--;
    if (n > 0)
        // Never as the format string itself: the text is the guest's.
        ish_printk("%.*s\n", (int) n, msg + skip);
    return (ssize_t) bufsize;
}

static off_t_ kmsg_lseek(struct fd *fd, off_t_ off, int whence) {
    // /dev/kmsg seeks by RECORD, not by byte. The only offset Linux accepts is
    // zero, each whence names a fixed point in the log, and a successful seek
    // returns 0 rather than a position (measured on 6.12: a non-zero offset is
    // ESPIPE for every whence, including SEEK_CUR and SEEK_DATA, and SEEK_CUR
    // is EINVAL even at zero).
    //
    // This is not pedantry about a rarely used call: util-linux's dmesg opens
    // /dev/kmsg and immediately seeks SEEK_DATA, so that is the seek every
    // plain `dmesg` in a guest performs before its first read.
    if (off != 0)
        return _ESPIPE;
    uint64_t at;
    switch (whence) {
        case LSEEK_SET:
            // The first record still buffered. 0 is clamped forward to it.
            at = 0;
            break;
        case LSEEK_DATA:
            // The first record logged after the last syslog(2) clear, which is
            // what `dmesg` after a `dmesg -c` is asking for.
            at = ish_log_clear_pos();
            break;
        case LSEEK_END:
            at = ish_log_total_written();
            break;
        default:
            return _EINVAL;
    }
    fd->kmsg.seq = ish_log_line_seek(&at);
    fd->offset = (unsigned long) at;
    return 0;
}

struct dev_ops kmsg_dev = {
    .open = kmsg_open,
    .fd.read = kmsg_read,
    .fd.pread = kmsg_pread,
    .fd.write = kmsg_write,
    .fd.lseek = kmsg_lseek,
    .fd.poll = kmsg_poll,
    .fd.close = kmsg_close,
};
