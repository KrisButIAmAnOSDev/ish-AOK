#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <sys/uio.h>
#if LOG_HANDLER_NSLOG
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "kernel/calls.h"
#include "util/sync.h"
#include "util/fifo.h"
#include "kernel/task.h"
#include "fs/mem.h"
#include "misc.h"

#define LOG_BUF_SHIFT 20
static char log_buffer[1 << LOG_BUF_SHIFT];
static struct fifo log_buf = FIFO_INIT(log_buffer);
static size_t log_max_since_clear = 0;
static lock_t log_lock = LOCK_INITIALIZER;
// Total bytes ever appended. The fifo overwrites when it fills, so its own
// size stops growing and an offset INTO it stops meaning anything -- a reader
// parked at the end would never see another byte once the buffer had wrapped
// once. Stream readers position themselves against this instead, which keeps
// counting, and the window still in the buffer is [total - fifo_size, total).
static uint64_t log_total_written = 0;
// Total lines ever appended. Every stored line is one output_line() call and
// ends in a newline, so this is also the number of newlines ever written --
// which is what makes a line's index recoverable from a byte position, and is
// what /dev/kmsg reports as a record's sequence number.
static uint64_t log_total_lines = 0;
// Signalled whenever a line lands, so a blocking reader wakes instead of
// spinning on a zero-length read.
static cond_t log_cond = COND_INITIALIZER;

#define SYSLOG_ACTION_CLOSE_ 0
#define SYSLOG_ACTION_OPEN_ 1
#define SYSLOG_ACTION_READ_ 2
#define SYSLOG_ACTION_READ_ALL_ 3
#define SYSLOG_ACTION_READ_CLEAR_ 4
#define SYSLOG_ACTION_CLEAR_ 5
#define SYSLOG_ACTION_CONSOLE_OFF_ 6
#define SYSLOG_ACTION_CONSOLE_ON_ 7
#define SYSLOG_ACTION_CONSOLE_LEVEL_ 8
#define SYSLOG_ACTION_SIZE_UNREAD_ 9
#define SYSLOG_ACTION_SIZE_BUFFER_ 10

// What a read case has taken out of the log and still owes the guest. The copy
// into guest memory happens after log_lock has been dropped: user_write reaches
// into the guest address space, which can fault and take the memory lock, and a
// fault path logs -- and printk takes this same lock.
struct syslog_copy {
    char *buf;  // malloc()ed, and NULL when there is nothing to hand over
    size_t len;
};

// Take up to `len` bytes out of the log. Caller holds log_lock. Returns the
// byte count, or a negative errno; the bytes themselves go into *out, for the
// caller to copy out once the lock is clear.
static size_t syslog_take(size_t len, int flags, struct syslog_copy *out) {
    size_t available = fifo_size(&log_buf);
    if (flags & FIFO_LAST && available > log_max_since_clear)
        available = log_max_since_clear;
    if (len > available)
        len = available;
    if (len == 0)
        return 0;

    char *buf = malloc(len);
    if (buf == NULL)
        return _ENOMEM;
    if (fifo_read(&log_buf, buf, len, flags)) {
        free(buf);
        return _EIO;
    }
    out->buf = buf;
    out->len = len;
    return len;
}

// Wait until SYSLOG_ACTION_READ has something to give.
//
// Linux parks that read in wait_event_interruptible until there are records
// past the reader's position (printk.c, syslog_print); returning 0 for "the log
// is empty" instead turns the main loop of every syslog daemon into a spin.
// busybox klogd is a klogctl(2, ...) loop that treats 0 as "read nothing, go
// round again": measured before this, `klogd -n` sat in state R and burned 100
// ticks a second, a whole core, for as long as it was left running.
//
// The condition is a non-empty buffer rather than a position, because this read
// CONSUMES what it returns -- there is nothing for ish_log_wait_past's absolute
// position, which the /dev/kmsg stream reader uses, to be compared against.
//
// Caller holds log_lock, which wait_for_blocked releases while it sleeps.
// wait_for_blocked and not wait_for: the task is parked in a syscall, so it
// must read as sleeping, and a bare address-space poke is a spurious wakeup
// rather than an interruption -- wait_for would hand that back as _EINTR and
// fail a read that nothing had interrupted.
static int syslog_wait_for_data(void) {
    int err = 0;
    while (fifo_size(&log_buf) == 0) {
        err = wait_for_blocked(&log_cond, &log_lock, NULL);
        if (err < 0)
            break;
    }
    return err;
}

size_t ish_log_size(void) {
    lock(&log_lock, 0);
    size_t size = fifo_size(&log_buf);
    unlock(&log_lock);
    return size;
}

ssize_t ish_log_read_bytes(size_t offset, void *buf, size_t len) {
    lock(&log_lock, 0);
    size_t available = fifo_size(&log_buf);
    if (offset >= available) {
        unlock(&log_lock);
        return 0;
    }
    if (len > available - offset)
        len = available - offset;
    if (len == 0) {
        unlock(&log_lock);
        return 0;
    }

    size_t start = (log_buf.start + offset) % log_buf.capacity;
    size_t first_copy_size = log_buf.capacity - start;
    if (first_copy_size > len)
        first_copy_size = len;
    memcpy(buf, &log_buf.buf[start], first_copy_size);
    memcpy((char *) buf + first_copy_size, &log_buf.buf[0], len - first_copy_size);
    unlock(&log_lock);
    return (ssize_t) len;
}

// Oldest absolute position still held in the ring buffer. Caller holds
// log_lock.
static uint64_t log_oldest_locked(void) {
    return log_total_written - fifo_size(&log_buf);
}

uint64_t ish_log_total_written(void) {
    lock(&log_lock, 0);
    uint64_t total = log_total_written;
    unlock(&log_lock);
    return total;
}

// Copy out from an ABSOLUTE position, advancing *pos past what was copied.
// Returns 0 when *pos is already at the end. A position that has fallen off
// the back of the buffer is moved forward to the oldest byte still there
// rather than failing: Linux's /dev/kmsg reports that overrun with EPIPE, but
// a stream this coarse (bytes, not records) cannot say where the loss began,
// and silently resuming beats handing a log daemon an error it will treat as
// fatal.
ssize_t ish_log_read_at(uint64_t *pos, void *buf, size_t len) {
    lock(&log_lock, 0);
    uint64_t oldest = log_oldest_locked();
    if (*pos < oldest)
        *pos = oldest;
    if (*pos >= log_total_written) {
        unlock(&log_lock);
        return 0;
    }
    if (len > log_total_written - *pos)
        len = (size_t) (log_total_written - *pos);

    size_t start = (log_buf.start + (size_t) (*pos - oldest)) % log_buf.capacity;
    size_t first_copy_size = log_buf.capacity - start;
    if (first_copy_size > len)
        first_copy_size = len;
    memcpy(buf, &log_buf.buf[start], first_copy_size);
    memcpy((char *) buf + first_copy_size, &log_buf.buf[0], len - first_copy_size);
    *pos += len;
    unlock(&log_lock);
    return (ssize_t) len;
}

// ---- the log as LINES, for /dev/kmsg ----------------------------------
//
// Everything above treats the log as bytes, which is what syslog(2) and
// /proc/kmsg want. /dev/kmsg wants records: one whole message per read, each
// carrying a sequence number and a timestamp (fs/mem.c has the wire format).
// Rather than store records -- which would put a second meaning on every
// absolute position the byte readers already use -- the line structure is
// recovered from the bytes. Every stored line is one output_line() call ending
// in exactly one newline, so newlines and lines are the same thing, and a
// line's sequence number is just how many lines precede it.

// Absolute position of the first '\n' in [from, to), or `to` if there is none.
// Caller holds log_lock; [from, to) must lie inside the buffered window.
static uint64_t log_find_newline(uint64_t from, uint64_t to) {
    if (from >= to)
        return to;
    size_t off = (size_t) (from - log_oldest_locked());
    size_t len = (size_t) (to - from);
    size_t start = (log_buf.start + off) % log_buf.capacity;
    size_t first = log_buf.capacity - start;
    if (first > len)
        first = len;
    const char *hit = memchr(&log_buf.buf[start], '\n', first);
    if (hit != NULL)
        return from + (uint64_t) (hit - &log_buf.buf[start]);
    if (len > first) {
        hit = memchr(&log_buf.buf[0], '\n', len - first);
        if (hit != NULL)
            return from + first + (uint64_t) (hit - &log_buf.buf[0]);
    }
    return to;
}

// Lines wholly inside [from, to). Each memchr resumes where the last one
// stopped, so this scans the span once however many lines are in it.
// Caller holds log_lock.
static uint64_t log_count_lines(uint64_t from, uint64_t to) {
    uint64_t lines = 0;
    while (from < to) {
        uint64_t at = log_find_newline(from, to);
        if (at == to)
            break;
        lines++;
        from = at + 1;
    }
    return lines;
}

// Caller holds log_lock; [from, from+len) must lie inside the buffered window.
static void log_copy_out(uint64_t from, void *buf, size_t len) {
    size_t start = (log_buf.start + (size_t) (from - log_oldest_locked())) % log_buf.capacity;
    size_t first = log_buf.capacity - start;
    if (first > len)
        first = len;
    memcpy(buf, &log_buf.buf[start], first);
    memcpy((char *) buf + first, &log_buf.buf[0], len - first);
}

uint64_t ish_log_oldest(void) {
    lock(&log_lock, 0);
    uint64_t oldest = log_oldest_locked();
    unlock(&log_lock);
    return oldest;
}

// Where the log stood at the last syslog(2) clear -- what /dev/kmsg's
// SEEK_DATA names, and what `dmesg` seeks to before its first read. The
// cap log_buf_append puts on log_max_since_clear can only pull this back
// to the start of the buffer, which ish_log_line_seek clamps.
uint64_t ish_log_clear_pos(void) {
    lock(&log_lock, 0);
    uint64_t pos = log_total_written - log_max_since_clear;
    unlock(&log_lock);
    return pos;
}

// Move *pos to the start of the first whole line at or after it, and return
// that line's sequence number.
//
// A position older than the buffer is not simply clamped to the oldest byte:
// unless nothing has ever been evicted, those first bytes are the tail of a
// line whose beginning went with them, and handing that tail back as a record
// would give the reader half a message with a whole message's sequence number.
// Skipping to the next newline costs at most one line, and only after a wrap,
// which is exactly when Linux reports the overrun as lost records anyway.
uint64_t ish_log_line_seek(uint64_t *pos) {
    lock(&log_lock, 0);
    uint64_t oldest = log_oldest_locked();
    uint64_t at = *pos;
    // <= and not <: landing exactly ON the oldest byte is the same problem,
    // and SEEK_DATA can land there when a clear has fallen out of the buffer.
    if (at <= oldest) {
        at = oldest;
        // oldest == 0 means the buffer has never overwritten anything, so its
        // first byte really is the start of the first line ever logged.
        if (oldest != 0) {
            uint64_t nl = log_find_newline(at, log_total_written);
            at = (nl == log_total_written) ? log_total_written : nl + 1;
        }
    }
    if (at > log_total_written)
        at = log_total_written;
    uint64_t seq = log_total_lines - log_count_lines(at, log_total_written);
    *pos = at;
    unlock(&log_lock);
    return seq;
}

// Copy the whole line that starts at `pos`, its newline excluded, and report
// in *next where the line after it starts. Nothing here owns a position: the
// caller keeps it and moves it on, which is what lets it decide when a line
// counts as delivered.
//
//   > 0       the line's length; *next set
//   0         no whole line at `pos` yet -- the reader is caught up
//   _EPIPE    `pos` has fallen off the back of the buffer
//   _E2BIG    `bufsize` is too small; *needed is the length required
//
// *next is written only on success, so a caller that seeds it with `pos` can
// tell "a line was there" from "nothing yet" even for an empty line.
ssize_t ish_log_peek_line(uint64_t pos, void *buf, size_t bufsize,
                          uint64_t *next, size_t *needed) {
    lock(&log_lock, 0);
    ssize_t res;
    if (pos < log_oldest_locked()) {
        res = _EPIPE;
        goto out;
    }
    if (pos >= log_total_written) {
        res = 0;
        goto out;
    }
    uint64_t nl = log_find_newline(pos, log_total_written);
    if (nl == log_total_written) {
        // A line always reaches the buffer with its newline (output_line
        // appends both under this lock), so this is unreachable in practice --
        // and "wait" is the safe answer if it ever is not.
        res = 0;
        goto out;
    }
    size_t len = (size_t) (nl - pos);
    if (needed != NULL)
        *needed = len;
    if (len > bufsize) {
        res = _E2BIG;
        goto out;
    }
    log_copy_out(pos, buf, len);
    *next = nl + 1;
    res = (ssize_t) len;
out:
    unlock(&log_lock);
    return res;
}

// Block until something lands past pos. Returns 0, or _EINTR if a guest
// signal arrived first.
int ish_log_wait_past(uint64_t pos) {
    lock(&log_lock, 0);
    int err = 0;
    while (log_total_written <= pos) {
        err = wait_for_blocked(&log_cond, &log_lock, NULL);
        if (err < 0)
            break;
    }
    unlock(&log_lock);
    return err;
}

static size_t do_syslog(int type, guest_addr_t buf_addr, int_t len, struct syslog_copy *out) {
    int res;
    switch (type) {
        case SYSLOG_ACTION_READ_: {
            if (len < 0)
                return _EINVAL;
            // Both checked before anything waits, because Linux checks them
            // before it waits (measured on 6.12: a NULL buffer is EINVAL and a
            // zero length returns 0 at once, on a log with nothing unread).
            // They used to fall out of a read that could not block at all; now
            // that it waits, a guest passing either would wait forever.
            if (buf_addr == 0)
                return _EINVAL;
            if (len == 0)
                return 0;
            int err = 0;
            // Nothing may return out of TASK_MAY_BLOCK. It is a for loop, so
            // leaving its body any way but the bottom skips task_may_block_end
            // and leaves the task marked blocked while it goes on running.
            TASK_MAY_BLOCK {
                err = syslog_wait_for_data();
            }
            if (err < 0)
                return err;
            return syslog_take(len, 0, out);
        }
        case SYSLOG_ACTION_READ_ALL_:
        case SYSLOG_ACTION_READ_CLEAR_:
            // Both arguments are checked before anything is read, and a null
            // buffer is EINVAL even with a zero length -- Linux tests
            // `!buf || len < 0` first and `!len` second (printk.c, do_syslog;
            // measured on 6.12). A zero length therefore answers 0 without
            // ever reaching syslog_print_all, so it does not clear either.
            if (buf_addr == 0 || len < 0)
                return _EINVAL;
            if (len == 0)
                return 0;
            res = (int)syslog_take(len, FIFO_LAST | FIFO_PEEK, out);
            if (res < 0)
                return res;
            // The clear is unconditional once the read has been attempted --
            // even at zero bytes returned, and even if the copy to the guest
            // faults afterwards. Linux advances clear_seq after the copy loop
            // whether or not that loop broke on -EFAULT.
            if (type == SYSLOG_ACTION_READ_CLEAR_)
                log_max_since_clear = 0;
            // And the count is the answer. This used to fall into the CLEAR_
            // case below and return its 0, which is not a harmless
            // discrepancy: both busybox's and util-linux's dmesg print only
            // as many bytes as the call reported, so `dmesg -c` printed
            // NOTHING in a guest while the bytes sat correctly in its buffer.
            // Measured on 6.12: READ_CLEAR returns exactly what the READ_ALL
            // before it returned.
            return (size_t) res;

        case SYSLOG_ACTION_CLEAR_:
            // Type 5 takes no buffer and validates nothing -- measured on
            // 6.12, a negative length and a garbage pointer both return 0.
            log_max_since_clear = 0;
            return 0;

        case SYSLOG_ACTION_SIZE_UNREAD_:
            return (int)fifo_size(&log_buf);
        case SYSLOG_ACTION_SIZE_BUFFER_:
            return (int)fifo_capacity(&log_buf);

        case SYSLOG_ACTION_CLOSE_:
        case SYSLOG_ACTION_OPEN_:
        case SYSLOG_ACTION_CONSOLE_OFF_:
        case SYSLOG_ACTION_CONSOLE_ON_:
        case SYSLOG_ACTION_CONSOLE_LEVEL_:
            return 0;
        default:
            return _EINVAL;
    }
}
size_t sys_syslog(int_t type, addr_t buf_addr, int_t len) {
    return sys_syslog_guest(type, buf_addr, len);
}

size_t sys_syslog_guest(int_t type, guest_addr_t buf_addr, int_t len) {
    struct syslog_copy copy = { .buf = NULL, .len = 0 };
    lock(&log_lock, 0);
    size_t retval = do_syslog(type, buf_addr, len, &copy);
    unlock(&log_lock);

    // Outside log_lock -- see struct syslog_copy. A destructive READ has
    // already consumed these bytes by the time the copy fails, so an EFAULT
    // loses them; Linux loses them the same way, having already advanced
    // syslog_seq past the records it copied.
    if (copy.buf != NULL) {
        if (user_write(buf_addr, copy.buf, copy.len))
            retval = _EFAULT;
        free(copy.buf);
    }

    // Linux ends a waiting SYSLOG_ACTION_READ with -ERESTARTSYS, so SA_RESTART
    // decides whether the guest ever sees the interruption. Measured on 6.12:
    // interrupted by a handler without SA_RESTART the call fails with EINTR,
    // and with SA_RESTART it resumes and returns the message that landed
    // afterwards. A no-op for every other type, none of which can wait.
    return (size_t) signal_restart_or_eintr((int_t) retval);
}

static void log_buf_append(const char *msg) {
    size_t len = strlen(msg);
    fifo_write(&log_buf, msg, len, FIFO_OVERWRITE);
    log_total_written += len;
    log_max_since_clear += len;
    if (log_max_since_clear > fifo_capacity(&log_buf))
        log_max_since_clear = fifo_capacity(&log_buf);
    // Called with log_lock held (ish_vprintk), which is what wait_for below
    // releases while it sleeps.
    notify(&log_cond);
}

static void log_line(const char *line);

static void output_line(const char *line) {
     if (strncmp(line, "INFO:", 5) == 0)
         return;
     // UTC, not the host's local time.
     //
     // This stamp is guest-visible text: /proc/kmsg and syslog(2) hand the
     // line back byte for byte, so `dmesg --syslog`, busybox's dmesg and
     // `cat /proc/kmsg` all print it exactly as it stands. It was rendered
     // with ctime(3), which is the HOST's local time -- a zone the guest has
     // no way to learn and no reason to share. On a Mac on BST every line in
     // a UTC guest read an hour into the FUTURE, and `dmesg -S -T` printed
     // both times an hour apart on the same line: its own correct rendering
     // and this one. Measured 2026-09-18, exactly -3600 s.
     //
     // UTC is the one reference the two ends can agree on without asking
     // userspace where it thinks it is. It also removes a real ambiguity:
     // kmsg_line_time() in fs/mem.c has to read this stamp back, and a local
     // rendering is genuinely ambiguous for one hour a year across a DST
     // change, where timegm() is an exact inverse of asctime_r(gmtime_r()).
     // Local time still reaches the user where it belongs -- `dmesg -T`
     // renders the READER's zone from the record timestamp recovered here.
     time_t t = time(NULL);
     struct tm utc;
     char stamp[32];
     // asctime_r wants 26 bytes and ends the string with a newline, which is
     // ctime's exact layout minus the timezone question. It only fails on a
     // time_t no clock will produce; a line still gets logged if it does,
     // just without a stamp (kmsg_line_time reports 0 for one it cannot
     // parse, which is what Linux reports for a record with no timestamp).
     if (gmtime_r(&t, &utc) == NULL || asctime_r(&utc, stamp) == NULL)
         stamp[0] = '\0';
     else
         stamp[strcspn(stamp, "\n")] = '\0';

     char tmpbuff[16384];
     int stamped = stamp[0] != '\0'
         ? snprintf(tmpbuff, sizeof(tmpbuff), "[%s] %s", stamp, line)
         : snprintf(tmpbuff, sizeof(tmpbuff), "%s", line);
     if (stamped >= (int) sizeof(tmpbuff)) { // Insufficient room, need to terminate at buffer size
         tmpbuff[sizeof(tmpbuff) - 1] = '\0';
     }
    // send it to stdout or wherever
    if(tmpbuff[0] != '\0') { // Don't log empty string
        log_line(tmpbuff);
        // add it to the circular buffer
        log_buf_append(tmpbuff);
        log_buf_append("\n");
        // Both appends happen under the one log_lock hold ish_vprintk takes,
        // so a reader can never see the text without its newline, nor the
        // count without the bytes.
        log_total_lines++;
    }
}

void ish_vprintk(const char *msg, va_list args) {
    // format the message
    static __thread char buf[16384] = "";
    static __thread size_t buf_size = 0;

    size_t available = sizeof(buf) - buf_size;
    if (available > 0) {
        int ret = vsnprintf(buf + buf_size, available, msg, args);
        if (ret > 0) {
            if ((size_t)ret >= available)
                buf_size = sizeof(buf) - 1;
            else
                buf_size += ret;
        }
    }

    // output up to the last newline, leave the rest in the buffer
    bool logged = false;
    complex_lockt(&log_lock, 1);
    char *b = buf;
    char *p;
    while ((p = strchr(b, '\n')) != NULL) {
        *p = '\0';
        output_line(b);
        *p = '\n';
        buf_size -= p + 1 - b;
        b = p + 1;
        logged = true;
    }

    if (buf_size >= sizeof(buf) - 1) {
        output_line(b);
        buf_size = 0;
        b = buf + sizeof(buf) - 1;
        buf[0] = '\0';
        logged = true;
    }

    unlock(&log_lock);
    // Only once log_lock is clear: waking a poller reaches into the poll
    // machinery, which logs on its own error paths, and doing that under the
    // log lock would deadlock the first time it did.
    if (logged)
        kmsg_notify_readers();
    memmove(buf, b, strlen(b) + 1);
}

void ish_printk(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    ish_vprintk(msg, args);
    va_end(args);
}

#if LOG_HANDLER_DPRINTF
#define NEWLINE "\r\n"
static void log_line(const char *line) {
    struct iovec output[2] = {{(void *) line, strlen(line)}, {"\n", 1}};
    writev(555, output, 2);
}
#elif LOG_HANDLER_NSLOG
static void log_line(const char *line) {
    extern void NSLog(CFStringRef msg, ...);
    if(line[0] != '\0') // Don't log empty string
        NSLog(CFSTR("%s"), line);
}
#endif

static void default_die_handler(const char *msg) {
    printk("%s\n", msg);
}
void (*die_handler)(const char *msg) = default_die_handler;
__attribute__((__noreturn__)) void die(const char *msg, ...);
void die(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), msg, args);
    die_handler(buf);
    abort();  
    va_end(args);
}
