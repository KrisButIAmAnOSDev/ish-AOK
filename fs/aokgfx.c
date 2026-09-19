// /dev/aokgfx -- the guest end of the AOK graphics channel. See fs/aokgfx.h
// for the wire format and why it looks the way it does.
//
// This lives in the kernel rather than in the app, even though the app is what
// eventually draws, so that framing, validation and the version handshake are
// enforced in exactly one place regardless of who is consuming the commands --
// the Metal renderer, a test harness, or nothing. It also means the device
// exists under ish-cli, where the wire format can be exercised with no UI.
#include <string.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "fs/aokgfx.h"
#include "fs/dev.h"
#include "fs/poll.h"
#include "util/sync.h"

#define AOKGFX_EVENT_QUEUE_MAX 256

struct aokgfx_event {
    uint16_t op;
    uint16_t len;              // payload bytes in `payload`
    uint8_t payload[sizeof(struct aokgfx_light)];
};

static struct {
    lock_t lock;
    struct aokgfx_backend backend;
    bool backend_set;

    // One writer at a time. Two guests drawing into one surface would
    // interleave half-commands, and the handshake is per-channel state, so the
    // second open is refused rather than silently corrupting the first.
    bool open;
    bool greeted;              // HELLO seen and accepted

    // A write(2) is a FRAGMENT, not a command. The guest is expected to batch
    // a whole frame into one write, but nothing makes it land in one piece --
    // a large batch will be split, and /proc/ish/roots learned this same
    // lesson the expensive way. Hold the tail until the rest arrives.
    uint8_t partial[AOKGFX_CMD_MAX];
    size_t partial_len;

    struct aokgfx_event events[AOKGFX_EVENT_QUEUE_MAX];
    unsigned events_head, events_count;
    struct fd *poll_fd;
} gfx = { .lock = LOCK_INITIALIZER };

int aokgfx_set_backend(const struct aokgfx_backend *backend) {
    lock(&gfx.lock, 0);
    if (backend != NULL && gfx.backend_set) {
        unlock(&gfx.lock);
        return _EBUSY;
    }
    if (backend == NULL) {
        memset(&gfx.backend, 0, sizeof(gfx.backend));
        gfx.backend_set = false;
    } else {
        gfx.backend = *backend;
        gfx.backend_set = true;
    }
    unlock(&gfx.lock);
    return 0;
}

// Caller must hold gfx.lock.
static void aokgfx_queue_event_locked(uint16_t op, const void *payload, size_t payload_len) {
    if (payload_len > sizeof(((struct aokgfx_event *) 0)->payload))
        return;
    // A guest that has stopped reading must not be able to grow this without
    // bound, and the newest input is the input that matters -- a stale pointer
    // position is worth less than the current one. Drop the oldest.
    if (gfx.events_count == AOKGFX_EVENT_QUEUE_MAX) {
        gfx.events_head = (gfx.events_head + 1) % AOKGFX_EVENT_QUEUE_MAX;
        gfx.events_count--;
    }
    unsigned slot = (gfx.events_head + gfx.events_count) % AOKGFX_EVENT_QUEUE_MAX;
    gfx.events[slot].op = op;
    gfx.events[slot].len = (uint16_t) payload_len;
    if (payload_len > 0)
        memcpy(gfx.events[slot].payload, payload, payload_len);
    gfx.events_count++;
}

void aokgfx_post_event(uint16_t op, const void *payload, size_t payload_len) {
    lock(&gfx.lock, 0);
    bool deliver = gfx.open;
    if (deliver)
        aokgfx_queue_event_locked(op, payload, payload_len);
    struct fd *fd = gfx.poll_fd;
    unlock(&gfx.lock);
    // Deliberately after the unlock, and deliberately not the trylock variant:
    // poll_wakeup must not be called holding a lock its poll op would take
    // (fs/poll.h), and gfx.lock is exactly that lock -- but having dropped it
    // there is no ordering problem left, and trylock would silently skip the
    // wakeup, stranding a guest blocked in poll() on a keystroke.
    if (deliver && fd != NULL)
        poll_wakeup(fd, POLL_READ);
}

// How many payload bytes an opcode requires. Returns SIZE_MAX for an opcode
// this build does not know, which is NOT an error: `len` lets us skip it, and
// skipping is what lets a newer guest run against an older host for anything
// additive. Variable-length commands validate their own tail.
static size_t aokgfx_payload_size(uint16_t op) {
    switch (op) {
        case AOKGFX_OP_HELLO:           return sizeof(struct aokgfx_hello);
        case AOKGFX_OP_SURFACE_CREATE:  return sizeof(struct aokgfx_surface_create);
        case AOKGFX_OP_SURFACE_DESTROY: return 0;
        case AOKGFX_OP_PRESENT:         return 0;
        case AOKGFX_OP_CLEAR:
        case AOKGFX_OP_SET_COLOR:       return sizeof(struct aokgfx_color);
        case AOKGFX_OP_PUT_PIXEL:       return sizeof(struct aokgfx_point);
        case AOKGFX_OP_DRAW_LINE:
        case AOKGFX_OP_DRAW_RECT:
        case AOKGFX_OP_FILL_RECT:
        case AOKGFX_OP_VIEWPORT:        return sizeof(struct aokgfx_rect);
        case AOKGFX_OP_DRAW_CIRCLE:
        case AOKGFX_OP_FILL_CIRCLE:     return sizeof(struct aokgfx_circle);
        case AOKGFX_OP_SET_LINE_WIDTH:
        case AOKGFX_OP_MATRIX_MODE:
        case AOKGFX_OP_SHADE_MODEL:     return sizeof(uint32_t);
        case AOKGFX_OP_CLEAR_DEPTH:     return sizeof(float);
        case AOKGFX_OP_DRAW_TEXT:       return sizeof(struct aokgfx_text);
        case AOKGFX_OP_PRIMITIVES:      return sizeof(struct aokgfx_primitives);
        case AOKGFX_OP_LOAD_IDENTITY:
        case AOKGFX_OP_PUSH_MATRIX:
        case AOKGFX_OP_POP_MATRIX:      return 0;
        case AOKGFX_OP_ROTATE:
        case AOKGFX_OP_TRANSLATE:
        case AOKGFX_OP_SCALE:
        case AOKGFX_OP_PERSPECTIVE:     return sizeof(struct aokgfx_vec4);
        case AOKGFX_OP_SET_STATE:       return sizeof(struct aokgfx_state);
        case AOKGFX_OP_LIGHT:           return sizeof(struct aokgfx_light);
        case AOKGFX_OP_MATERIAL:        return sizeof(struct aokgfx_material);
        case AOKGFX_OP_BLEND_FUNC:      return sizeof(struct aokgfx_blend);
        default:                        return SIZE_MAX;
    }
}

// Checks one complete command. Returns 0, or a negative errno for a command
// that is malformed rather than merely unknown -- a length that disagrees with
// the opcode is a guest bug, and failing the write is how it gets found.
static int aokgfx_validate(const struct aokgfx_cmd *cmd) {
    size_t payload = cmd->len - sizeof(*cmd);
    size_t need = aokgfx_payload_size(cmd->op);
    if (need == SIZE_MAX)
        return 0;                      // unknown: skipped, not rejected
    if (payload < need)
        return _EINVAL;

    // Variable-length tails: the fixed part declares how much follows, and a
    // count that overruns the command is the one way this protocol could be
    // talked into reading past a buffer. Check it here, once, so no backend
    // has to remember to.
    if (cmd->op == AOKGFX_OP_PRIMITIVES) {
        const struct aokgfx_primitives *p = (const void *) (cmd + 1);
        if (p->topology >= AOKGFX_PRIM__COUNT)
            return _EINVAL;
        size_t want = (size_t) p->count * sizeof(struct aokgfx_vertex);
        if (want / sizeof(struct aokgfx_vertex) != p->count)  // overflow
            return _EINVAL;
        if (payload - need != want)
            return _EINVAL;
    } else if (cmd->op == AOKGFX_OP_DRAW_TEXT) {
        const struct aokgfx_text *t = (const void *) (cmd + 1);
        if (payload - need != t->bytes)
            return _EINVAL;
    }
    return 0;
}

static ssize_t aokgfx_write(struct fd *UNUSED(fd), const void *buf, size_t size) {
    if (size == 0)
        return 0;

    lock(&gfx.lock, 0);

    // Splice this write onto whatever the last one left dangling, then consume
    // whole commands from the front. Anything left over is the next partial.
    size_t total = gfx.partial_len + size;
    if (total > sizeof(gfx.partial) + size) {
        unlock(&gfx.lock);
        return _EINVAL;
    }

    const uint8_t *in = buf;
    size_t consumed_from_in = 0;
    int err = 0;

    while (err == 0) {
        // Assemble a view of the next command: leftover bytes first.
        uint8_t header[sizeof(struct aokgfx_cmd)];
        size_t avail = gfx.partial_len + (size - consumed_from_in);
        if (avail < sizeof(struct aokgfx_cmd))
            break;
        for (size_t i = 0; i < sizeof(header); i++)
            header[i] = i < gfx.partial_len ? gfx.partial[i]
                                            : in[consumed_from_in + i - gfx.partial_len];
        struct aokgfx_cmd cmd;
        memcpy(&cmd, header, sizeof(cmd));

        if (cmd.len < sizeof(struct aokgfx_cmd) || cmd.len > AOKGFX_CMD_MAX) {
            err = _EINVAL;
            break;
        }
        if (avail < cmd.len)
            break;                      // incomplete; wait for the rest

        // Materialise the whole command contiguously so validation and the
        // backend see one buffer, not a seam.
        uint8_t whole[AOKGFX_CMD_MAX];
        for (size_t i = 0; i < cmd.len; i++)
            whole[i] = i < gfx.partial_len ? gfx.partial[i]
                                           : in[consumed_from_in + i - gfx.partial_len];

        const struct aokgfx_cmd *c = (const struct aokgfx_cmd *) whole;
        err = aokgfx_validate(c);
        if (err != 0)
            break;

        // The handshake gates everything. A guest built against a protocol
        // this build cannot speak is told so once, in an errno it can report,
        // rather than being allowed to draw garbage.
        if (!gfx.greeted) {
            if (c->op != AOKGFX_OP_HELLO) {
                err = _EPROTO;
                break;
            }
            const struct aokgfx_hello *hello = (const void *) (c + 1);
            if (hello->version != AOKGFX_PROTOCOL_VERSION || hello->flags != 0) {
                err = _EPROTONOSUPPORT;
                break;
            }
            gfx.greeted = true;
            struct aokgfx_hello ack = { .version = AOKGFX_PROTOCOL_VERSION, .flags = 0 };
            aokgfx_queue_event_locked(AOKGFX_EV_HELLO, &ack, sizeof(ack));
        } else if (gfx.backend_set) {
            // Dropped on the floor with no backend, deliberately: the wire
            // format stays exercisable with no renderer attached.
            gfx.backend.submit(gfx.backend.ctx, whole, c->len);
        }

        // Retire the bytes this command used, from the leftover first.
        size_t from_partial = gfx.partial_len < cmd.len ? gfx.partial_len : cmd.len;
        consumed_from_in += cmd.len - from_partial;
        gfx.partial_len -= from_partial;
        if (gfx.partial_len > 0)
            memmove(gfx.partial, gfx.partial + from_partial, gfx.partial_len);
    }

    if (err == 0) {
        size_t leftover = size - consumed_from_in;
        if (gfx.partial_len + leftover > sizeof(gfx.partial)) {
            err = _EINVAL;             // a command claiming more than the max
        } else {
            memcpy(gfx.partial + gfx.partial_len, in + consumed_from_in, leftover);
            gfx.partial_len += leftover;
        }
    }
    if (err != 0) {
        // A malformed stream cannot be resynchronised -- we no longer know
        // where a command starts. Drop the buffer so the error is reported
        // once instead of on every subsequent write.
        gfx.partial_len = 0;
    }
    unlock(&gfx.lock);
    return err != 0 ? err : (ssize_t) size;
}

static ssize_t aokgfx_read(struct fd *UNUSED(fd), void *buf, size_t size) {
    lock(&gfx.lock, 0);
    size_t written = 0;
    while (gfx.events_count > 0) {
        struct aokgfx_event *ev = &gfx.events[gfx.events_head];
        size_t need = sizeof(struct aokgfx_cmd) + ev->len;
        if (written + need > size)
            break;
        struct aokgfx_cmd cmd = {
            .op = ev->op,
            .len = (uint16_t) need,
            .surface = AOKGFX_SURFACE_DEFAULT,
        };
        memcpy((uint8_t *) buf + written, &cmd, sizeof(cmd));
        memcpy((uint8_t *) buf + written + sizeof(cmd), ev->payload, ev->len);
        written += need;
        gfx.events_head = (gfx.events_head + 1) % AOKGFX_EVENT_QUEUE_MAX;
        gfx.events_count--;
    }
    unlock(&gfx.lock);
    // No events is not end-of-file: a reader polls, it does not spin on EOF.
    return written > 0 ? (ssize_t) written : _EAGAIN;
}

static int aokgfx_poll(struct fd *fd) {
    lock(&gfx.lock, 0);
    gfx.poll_fd = fd;
    int events = POLL_WRITE | (gfx.events_count > 0 ? POLL_READ : 0);
    unlock(&gfx.lock);
    return events;
}

static int aokgfx_open(int UNUSED(major), int UNUSED(minor), struct fd *UNUSED(fd)) {
    lock(&gfx.lock, 0);
    if (gfx.open) {
        unlock(&gfx.lock);
        return _EBUSY;
    }
    gfx.open = true;
    gfx.greeted = false;
    gfx.partial_len = 0;
    gfx.events_head = gfx.events_count = 0;
    unlock(&gfx.lock);
    return 0;
}

static int aokgfx_close(struct fd *UNUSED(fd)) {
    lock(&gfx.lock, 0);
    gfx.open = false;
    gfx.greeted = false;
    gfx.partial_len = 0;
    gfx.events_head = gfx.events_count = 0;
    gfx.poll_fd = NULL;
    bool tell = gfx.backend_set && gfx.backend.closed != NULL;
    struct aokgfx_backend backend = gfx.backend;
    unlock(&gfx.lock);
    // Outside the lock: the backend tears down a surface here, which is UI
    // work, and holding a kernel lock across it invites the deadlock every
    // other device in this tree has already learned to avoid.
    if (tell)
        backend.closed(backend.ctx);
    return 0;
}

struct dev_ops aokgfx_dev = {
    .open = aokgfx_open,
    .fd.read = aokgfx_read,
    .fd.write = aokgfx_write,
    .fd.poll = aokgfx_poll,
    .fd.close = aokgfx_close,
};
