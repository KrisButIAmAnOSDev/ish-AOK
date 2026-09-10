// checkpoint.c -- save a running guest to a file and bring it back.
//
// WHAT THIS IS FOR
//
// iOS terminates this app routinely: jetsam, memory pressure, a user swiping it
// away. Today that loses the session unconditionally. The promise here is
// narrow and specific -- same device, same root, same build, back where you
// were -- and it is deliberately NOT "checkpoint any process", which is where
// this class of feature usually dies. CRIU has been at the general problem for
// a decade with a real kernel's cooperation and is still partial.
//
// WHY IT IS TRACTABLE HERE AT ALL, which is the interesting part: AOK owns the
// scheduler. There is no host kernel to negotiate with about when a guest
// thread is quiet. Every guest task passes through the top of
// task_run_current's loop, and at that point it holds no address-space lock, is
// not inside a syscall, and its struct cpu_state IS the whole of its
// execution state. Stopping the machine is a flag; the state is a struct.
//
// WHERE THE CHECKPOINT IS TAKEN, and it is not arbitrary. A guest writes to
// /proc/ish/checkpoint; that write does NOT save anything. It sets a flag, and
// the save happens at the next pass round task_run_current's loop -- after the
// syscall's return value has been stored in the guest's register file and the
// program counter has moved past it. So the image describes a task about to
// execute the instruction AFTER the write, and a restore continues rather than
// re-running. The write returns 0 in both lives, and the program tells them
// apart by reading /proc/ish/checkpoint back.
//
// WHAT v1 REFUSES, out loud, rather than approximating:
//
//   - More than one live task. The mechanism to stop several is the same flag,
//     but a task blocked INSIDE a syscall -- a read on a pipe, a wait for a
//     child -- is not at a boundary, and recording "restart this syscall" per
//     family is the next phase's work, not this one's.
//   - A native program. A native program is a C function on a host thread
//     (kernel/native.h); there is no serialising a host C stack. The rule the
//     project already has is that a native program either knows how to dump
//     its own state or the checkpoint refuses while it is running -- and it
//     refuses here, by name, so the limit is reportable rather than silent.
//   - Any descriptor with no restore rule: a pipe, a socket, an unlinked file,
//     an epoll set. Regular files, directories and the console come back;
//     everything else is named in the refusal.
//   - A different build. struct cpu_state is written as bytes, so the image
//     carries the build's own fingerprint and a mismatch is refused rather
//     than reinterpreted.
//
// WHAT IT DOES NOT REFUSE BUT DOES NOT PRESERVE: a mapping's NAME. Every
// mapping comes back as anonymous memory holding the bytes that were in it,
// which is semantically exact for a private mapping (a file-backed private
// page that has been written is already a private copy, and one that has not
// is identical to the file) and wrong only for /proc/<pid>/maps, which will
// call the text segment anonymous. A MAP_SHARED file mapping is not exact and
// is refused.
//
// THE FORMAT is a header, then one task record, then its mappings, then its
// descriptors. No compression: kernel/zswap.c already compresses guest frames
// and is the right place to do it, but wiring the pool into a file written
// once and read once is phase 1's job, and an uncompressed image is the thing
// to measure it against.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "kernel/calls.h"
#include "kernel/checkpoint.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/mm.h"
#include "kernel/personality.h"
#include "kernel/resource.h"
#include "kernel/signal.h"
#include "kernel/init.h"
#include "kernel/native.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/tty.h"
#include "fs/devices.h"
#include "emu/memory.h"
#include "util/sync.h"

#define CKPT_MAGIC "AOKCKPT"
#define CKPT_VERSION 1
// How long the freezer waits for a task to reach a syscall boundary.
//
// Generous on purpose. Every wait in the guest is broken by the poke, so a
// task normally parks in microseconds; the cases that take longer are a task
// inside a host syscall that the SIGUSR1 has to interrupt, and one running a
// long stretch of guest code between checkpoints. Five seconds is long enough
// that neither is a flake on a busy machine -- two was not, and the
// first thing it failed under was this project's own test suite running
// beside it -- and short enough that a genuinely stuck task is reported
// rather than waited for.
#define CKPT_FREEZE_TIMEOUT_MS 5000

// Kinds of descriptor this version knows how to bring back. Anything else is a
// refusal naming the fd number and the filesystem it came from, because "the
// checkpoint failed" is useless and "fd 7 is a pipe" is actionable.
enum ckpt_fd_kind {
    CKPT_FD_FILE = 1,     // regular file: re-open by path, seek to offset
    CKPT_FD_DIR,          // directory: re-open by path
    CKPT_FD_TTY,          // the console: re-attach to this run's tty
    CKPT_FD_STDIO,        // 0/1/2 as the app handed them over: re-attach too
    CKPT_FD_CHR,          // /dev/null and friends: re-open the device by path
    CKPT_FD_PIPE,         // one end of a pipe, with whatever is still in it
    CKPT_FD_REF,          // the SAME struct fd as one already described
};

// ISH_CHECKPOINT_DEBUG=1 traces every record on the way out and on the way
// back. On the HOST's stderr, not the guest's: at restore time the guest has
// no descriptors yet, and at save time the thing being diagnosed is usually
// which descriptor the guest is holding.
static bool ckpt_debug(void) {
    const char *e = getenv("ISH_CHECKPOINT_DEBUG");
    return e != NULL && e[0] != '\0' && e[0] != '0';
}
#define CKPT_TRACE(...) do { \
    if (ckpt_debug()) { fprintf(stderr, "checkpoint: " __VA_ARGS__); } \
} while (0)

static const char *ckpt_kind_name(uint32_t kind) {
    switch (kind) {
        case CKPT_FD_FILE: return "file";
        case CKPT_FD_DIR: return "dir";
        case CKPT_FD_TTY: return "tty";
        case CKPT_FD_CHR: return "chr";
        case CKPT_FD_PIPE: return "pipe";
        case CKPT_FD_REF: return "ref";
        case CKPT_FD_STDIO: return "stdio";
        default: return "?";
    }
}

struct ckpt_header {
    char magic[8];
    uint32_t version;
    uint32_t abi;
    uint32_t cpu_state_size;   // struct cpu_state is written as bytes
    uint32_t page_size;
    uint64_t build_fingerprint;
    uint32_t n_tasks;
    uint64_t total_pages;
    // Whether the checkpointed guest's standard streams were a terminal or
    // pipes. They are RE-ATTACHED rather than restored -- the terminal on the
    // other end belonged to a process that no longer exists -- so all the
    // image has to carry is which of the two setups to run, exactly as the
    // entry point chooses it at boot.
    uint32_t stdio_is_tty;
    uint32_t reserved;
};

struct ckpt_task {
    uint32_t pid, ppid, pgid, sid;
    uint32_t n_maps, n_fds;
    uint32_t abi;
    // A zombie: no address space, no descriptors, nothing but an exit status
    // its parent has not collected yet. Recorded because dropping it turns the
    // parent's wait() into a hang on the far side of a restore.
    uint32_t zombie, exit_code;
    // What a child sends its parent when it dies. Set by clone() from the
    // flags; zero on a freshly built task, which is exactly what a restored
    // child was getting -- it exited cleanly, sent nothing, and its parent's
    // wait() never returned. The rest travel with it for the same reason:
    // nothing else puts them back.
    int32_t exit_signal, pdeath_signal, nice, sched_policy;
    uint64_t robust_list;
    uint32_t did_exec;
    // A NATIVE task. There is no address space to photograph and no register
    // file that means anything -- it is a C function on a host thread -- so
    // what travels is the program's name, the argv it was given, and the state
    // it produced about itself. n_maps is 0 and no cpu_state follows; the
    // three blobs below do, in this order, each NUL-terminated.
    uint32_t native;
    uint32_t native_name_len, native_argv_len, native_state_len, native_env_len;
    uint32_t uid, gid, euid, egid, suid, sgid, fsuid, fsgid;
    uint32_t umask;
    char comm[16];
    uint64_t blocked, pending;
    uint64_t altstack, altstack_size;
    uint64_t clear_tid;
    uint64_t brk, start_brk, vdso, stack_start;
    uint64_t argv_start, argv_end, env_start, env_end, auxv_start, auxv_end;
    uint32_t n_sigactions;
    uint32_t cwd_len, root_len;
    // The address space's SHAPE, which the ELF loader sets per guest
    // architecture and which nothing else puts back. Without page_limit a
    // restored arm64 guest gets a fresh mm's default -- a 32-bit one -- and
    // every mapping above 4 GiB fails to map with ENOMEM, which is what the
    // first restore attempt did.
    uint64_t page_limit, mmap_floor, mmap_ceiling;
    uint64_t stack_top, stack_limit_pages;
};

struct ckpt_map {
    uint64_t start;    // guest address
    uint64_t pages;
    uint32_t flags;    // P_*
    uint32_t reserved;
};

struct ckpt_fd {
    uint32_t fd;
    uint32_t cloexec;
    uint32_t flags;
    uint32_t kind;
    uint64_t offset;
    uint32_t path_len;
    // WHICH struct fd this is, not just which number it sits at.
    //
    // Two descriptors that are the same object have to come back as the same
    // object: a shell and the child it forked share one struct fd for a
    // redirected file, and giving each its own on restore gives them
    // independent offsets -- the child's reads stop advancing the parent's
    // position, which is the bug `while read; done < file | ...` is made of.
    // The first record for an id describes it; every later one is a
    // CKPT_FD_REF naming it.
    uint32_t id;
    // CKPT_FD_PIPE: the inode both ends share (fs/pipe.c), so a pair held by
    // two different processes is rebuilt as ONE host pipe; and which end this
    // is. `offset` carries the number of bytes that were still in it, which
    // follow the record for a read end.
    uint64_t pipe_inode;
    uint32_t pipe_write_end;
    uint32_t reserved;
};

// ------------------------------------------------------------------ status

static lock_t ckpt_lock = LOCK_INITIALIZER;
static struct checkpoint_status ckpt_status;
static char ckpt_pending_path[PATH_MAX];
static bool ckpt_pending;
static bool ckpt_pending_halt;
static char ckpt_session_path[PATH_MAX];

void checkpoint_get_status(struct checkpoint_status *out) {
    lock(&ckpt_lock, 0);
    *out = ckpt_status;
    unlock(&ckpt_lock);
}

static void ckpt_refuse(const char *fmt, ...) {
    va_list ap;
    lock(&ckpt_lock, 0);
    va_start(ap, fmt);
    vsnprintf(ckpt_status.last_refusal, sizeof(ckpt_status.last_refusal), fmt, ap);
    va_end(ap);
    unlock(&ckpt_lock);
}

// The build's own fingerprint. struct cpu_state travels as raw bytes -- it is
// hundreds of fields across four guest architectures and transcribing it would
// be a second copy to keep in step -- so an image from another build must be
// refused rather than reinterpreted. Its size plus the compile timestamp is
// enough: the same binary always agrees, and a rebuild never does.
static uint64_t ckpt_fingerprint(void) {
    static const char stamp[] = __DATE__ __TIME__;
    uint64_t h = 1469598103934665603ULL;   // FNV-1a
    for (const char *p = stamp; *p; p++) {
        h ^= (unsigned char) *p;
        h *= 1099511628211ULL;
    }
    h ^= sizeof(struct cpu_state);
    h *= 1099511628211ULL;
    h ^= sizeof(struct ckpt_task);
    return h;
}

// ------------------------------------------------------------------ freezer

// >0 while a freeze is in progress. One global rather than a per-task read,
// because kernel/calls.c consults it on EVERY syscall return: the common
// answer is "no" and it must cost a relaxed load and a branch.
static _Atomic int ckpt_freeze_active;
// The parking lot. A leaf lock -- nothing is ever taken under it.
static pthread_mutex_t ckpt_park_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ckpt_park_cond = PTHREAD_COND_INITIALIZER;

// A task that will never park, because it is already leaving.
//
// A zombie has no thread left to reach the parking place, and a task inside
// do_exit is not going to get there either. Waiting for one is waiting for
// something that cannot happen: the freeze times out, the checkpoint refuses,
// and it looks like a bug in the freezer. It is the ordinary case -- a
// `( sleep 1; ... ) &` whose child finishes while the image is being taken.
//
// They are also not written to the image. A zombie IS real state -- its parent
// may be about to wait for it -- and losing one turns that wait into a hang,
// so it is recorded as a zombie and recreated rather than dropped. A task
// mid-exit is not recorded at all: it has already run its last instruction.
static bool ckpt_task_is_leaving(struct task *t) {
    return t->zombie || t->exiting ||
        atomic_load_explicit(&t->exit_finished, memory_order_acquire);
}

bool checkpoint_freeze_pending(void) {
    if (atomic_load_explicit(&ckpt_freeze_active, memory_order_relaxed) == 0)
        return false;
    return current != NULL &&
        atomic_load_explicit(&current->ckpt_freeze_wanted, memory_order_acquire);
}

void checkpoint_park_if_frozen(void) {
    if (atomic_load_explicit(&ckpt_freeze_active, memory_order_relaxed) == 0)
        return;
    if (current == NULL ||
            !atomic_load_explicit(&current->ckpt_freeze_wanted, memory_order_acquire))
        return;

    // No missed wakeup: the flag is re-checked under the mutex the thawer
    // signals with, the same shape as emu/memory.c's quiesce parking lot.
    pthread_mutex_lock(&ckpt_park_lock);
    atomic_store_explicit(&current->ckpt_frozen, true, memory_order_release);
    pthread_cond_broadcast(&ckpt_park_cond);
    while (atomic_load_explicit(&current->ckpt_freeze_wanted, memory_order_acquire))
        pthread_cond_wait(&ckpt_park_cond, &ckpt_park_lock);
    atomic_store_explicit(&current->ckpt_frozen, false, memory_order_release);
    pthread_mutex_unlock(&ckpt_park_lock);
}

// Set while this thread is inside a program's ckpt_dump.
//
// The dump is the program describing itself, and describing itself means
// making syscalls -- zsh's emitters write to a descriptor. Every syscall a
// native program makes goes through native_checkpoint(), which comes straight
// back here, which would call the dump again: the guard below is what stops
// that from being an infinite recursion off the end of the thread's stack,
// which took the whole app down rather than the shell.
//
// __thread rather than a task field, because it is a property of THIS call
// stack and nothing else can see it.
static __thread bool ckpt_dumping;

void checkpoint_native_park(void) {
    // Already describing itself: neither dump again nor park. Parking here
    // would stop the program halfway through producing the very state the
    // freeze is waiting for.
    if (ckpt_dumping)
        return;

    if (atomic_load_explicit(&ckpt_freeze_active, memory_order_relaxed) == 0)
        return;
    if (current == NULL ||
            !atomic_load_explicit(&current->ckpt_freeze_wanted, memory_order_acquire))
        return;

    // DESCRIBE YOURSELF FIRST, on this thread, because this is the only thread
    // the program's state exists on -- everything a native shell holds is
    // __thread (tools/dash-tls-rewrite.py and its bash/zsh predecessors). The
    // writer runs on the checkpointing task's thread and could not reach any
    // of it.
    //
    // A program with no ckpt_dump leaves this NULL, and ckpt_check_scope has
    // already refused on its behalf -- so reaching here with nothing is the
    // freeze that was allowed to start, not a silent loss.
    const struct native_program *prog = native_program_running(current);
    if (prog != NULL && prog->ckpt_dump != NULL &&
            current->ckpt_native_state == NULL) {
        ckpt_dumping = true;
        current->ckpt_native_state = prog->ckpt_dump();
        ckpt_dumping = false;
        CKPT_TRACE("native park: pid %d described itself in %zu bytes\n",
                   current->pid, current->ckpt_native_state != NULL
                   ? strlen(current->ckpt_native_state) : 0);
    }

    checkpoint_park_if_frozen();
}

// Ask every task but this one to reach a boundary and stop there.
//
// Returns 0 with every task parked, or _EBUSY with `blame` naming the one that
// would not stop. Thaws on failure, so a refusal leaves the guest exactly as
// it was.
static int ckpt_freeze_all(unsigned timeout_ms, char *blame, size_t blame_size) {
    struct task_snapshot snap = {0};
    if (task_snapshot_collect(&snap, false) < 0)
        return _EAGAIN;

    atomic_fetch_add_explicit(&ckpt_freeze_active, 1, memory_order_acq_rel);
    for (unsigned i = 0; i < snap.count; i++) {
        struct task *t = snap.tasks[i];
        if (t == current)
            continue;
        atomic_store_explicit(&t->ckpt_freeze_wanted, true, memory_order_release);
    }
    // Woken only AFTER every flag is set. A task woken while a sibling's flag
    // was still clear could run on, block again in something the freezer has
    // already passed, and never be asked a second time.
    for (unsigned i = 0; i < snap.count; i++) {
        struct task *t = snap.tasks[i];
        if (t != current)
            task_wake_for_freeze(t);
    }

    int err = 0;
    struct task *stuck = NULL;
    for (unsigned spins = 0; ; spins++) {
        stuck = NULL;
        for (unsigned i = 0; i < snap.count; i++) {
            struct task *t = snap.tasks[i];
            if (t == current)
                continue;
            if (ckpt_task_is_leaving(t))
                continue;   // see ckpt_task_is_leaving
            if (!atomic_load_explicit(&t->ckpt_frozen, memory_order_acquire)) {
                stuck = t;
                break;
            }
        }
        if (stuck == NULL)
            break;
        if (spins * 2 >= timeout_ms) {
            err = _EBUSY;
            break;
        }
        // Poked again on every pass. One wake can be lost -- a task that was
        // between waits when the first arrived takes the next one instead --
        // and a freeze that gives up because of a single dropped poke would
        // be a flake rather than a limit.
        task_wake_for_freeze(stuck);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    if (err != 0 && stuck != NULL && blame != NULL)
        snprintf(blame, blame_size, "pid %d (%s) did not reach a syscall "
                 "boundary within %ums", stuck->pid, stuck->comm, timeout_ms);
    if (err != 0) {
        for (unsigned i = 0; i < snap.count; i++)
            atomic_store_explicit(&snap.tasks[i]->ckpt_freeze_wanted, false,
                                  memory_order_release);
        pthread_mutex_lock(&ckpt_park_lock);
        pthread_cond_broadcast(&ckpt_park_cond);
        pthread_mutex_unlock(&ckpt_park_lock);
        atomic_fetch_sub_explicit(&ckpt_freeze_active, 1, memory_order_acq_rel);
    }
    task_snapshot_release(&snap);
    return err;
}

static void ckpt_thaw_all(void) {
    struct task_snapshot snap = {0};
    if (task_snapshot_collect(&snap, false) == 0) {
        for (unsigned i = 0; i < snap.count; i++)
            atomic_store_explicit(&snap.tasks[i]->ckpt_freeze_wanted, false,
                                  memory_order_release);
        task_snapshot_release(&snap);
    }
    pthread_mutex_lock(&ckpt_park_lock);
    pthread_cond_broadcast(&ckpt_park_cond);
    pthread_mutex_unlock(&ckpt_park_lock);
    atomic_fetch_sub_explicit(&ckpt_freeze_active, 1, memory_order_acq_rel);
}

// ------------------------------------------------------------------ writing

struct ckpt_writer {
    FILE *f;
    int err;
};

static void wr(struct ckpt_writer *w, const void *p, size_t n) {
    if (w->err != 0 || n == 0)
        return;
    if (fwrite(p, 1, n, w->f) != n)
        w->err = errno_map();
}

static int rd(FILE *f, void *p, size_t n) {
    if (n == 0)
        return 0;
    if (fread(p, 1, n, f) != n)
        return feof(f) ? _EINVAL : errno_map();
    return 0;
}

// Can this guest be checkpointed at all? Answered before anything is written,
// so a refusal costs nothing and names its reason.
static int ckpt_check_scope(void) {
    struct task_snapshot snap = {0};
    if (task_snapshot_collect(&snap, false) < 0) {
        ckpt_refuse("could not enumerate tasks");
        return _EAGAIN;
    }
    int err = 0;
    if (snap.count == 0) {
        task_snapshot_release(&snap);
        ckpt_refuse("there is no guest running");
        return _ESRCH;
    }
    for (unsigned i = 0; i < snap.count; i++) {
        struct task *t = snap.tasks[i];
        // A native program is a C function on a HOST thread -- there is no
        // serialising that stack, and it never returns to task_run_current's
        // loop, so it can neither be frozen nor described. The project's rule
        // is that it either dumps its own state or the checkpoint refuses;
        // this is the refusing half, and it names the program so the limit is
        // reportable rather than mysterious.
        if (t->native_exec != NULL || t->native_cmdline != NULL) {
            const struct native_program *prog = native_program_running(t);
            if (prog != NULL && prog->ckpt_dump != NULL)
                continue;   // it can describe itself; see checkpoint_native_park
            ckpt_refuse("pid %d is running the native program %s, which cannot "
                        "describe its own state -- a native program is a C "
                        "function on a host thread and its stack cannot be "
                        "serialised", t->pid,
                        prog != NULL ? prog->name :
                        (t->comm[0] ? t->comm : "?"));
            err = _EOPNOTSUPP;
            break;
        }
    }
    task_snapshot_release(&snap);
    return err;
}

// What is still in a pipe, taken out and PUT BACK.
//
// A checkpoint is a copy: the guest carries on afterwards and must not notice,
// so bytes read out here are written straight back in. Safe only because the
// machine is frozen -- nobody else is at either end -- and bounded by the pipe
// buffer, so the write can never block on a pipe we have just emptied.
//
// FIONREAD first rather than reading until EAGAIN: it says exactly how much is
// there, so there is no need to make the descriptor non-blocking and no window
// in which it is.
static int ckpt_pipe_drain(struct fd *fd, char **out, uint64_t *len) {
    *out = NULL;
    *len = 0;
    int avail = 0;
    if (ioctl(fd->real_fd, FIONREAD, &avail) < 0 || avail <= 0)
        return 0;
    char *buf = malloc((size_t) avail);
    if (buf == NULL)
        return _ENOMEM;
    ssize_t got = read(fd->real_fd, buf, (size_t) avail);
    if (got <= 0) {
        free(buf);
        return 0;
    }
    ssize_t put = write(fd->real_fd, buf, (size_t) got);
    if (put != got) {
        // Cannot happen on a pipe we have just emptied, and if it somehow does
        // the guest has lost data -- say so rather than carry on quietly.
        printk("checkpoint: pipe %llu lost %zd bytes putting them back\n",
               (unsigned long long) fd->stat.inode, got - (put < 0 ? 0 : put));
    }
    *out = buf;
    *len = (uint64_t) got;
    return 0;
}

// One descriptor, gathered under files->lock and described afterwards.
struct ckpt_saved_fd {
    struct fd *fd;
    unsigned num;
    unsigned cloexec;
    int kind;
    uint64_t offset;
    uint32_t id;
    bool first;              // this record describes the object, not a ref to it
    char *pipe_bytes;        // CKPT_FD_PIPE read end: what was still in it
    uint64_t pipe_len;
    char path[MAX_PATH + 1];
};

// Every distinct struct fd in the image, in the order first seen. The index is
// the id a record carries; a second sighting of the same pointer -- in this
// process or another -- becomes a CKPT_FD_REF to it.
struct ckpt_fd_ids {
    struct fd **fds;
    uint32_t count, cap;
};

// Returns the id, and sets *first to whether this is the first sighting.
static uint32_t ckpt_fd_id(struct ckpt_fd_ids *ids, struct fd *fd, bool *first) {
    for (uint32_t i = 0; i < ids->count; i++) {
        if (ids->fds[i] == fd) {
            *first = false;
            return i;
        }
    }
    if (ids->count == ids->cap) {
        uint32_t cap = ids->cap ? ids->cap * 2 : 32;
        struct fd **f = realloc(ids->fds, cap * sizeof(*f));
        if (f == NULL) {
            *first = true;
            return UINT32_MAX;   // caller treats it as "describe it again"
        }
        ids->fds = f;
        ids->cap = cap;
    }
    ids->fds[ids->count] = fd;
    *first = true;
    return ids->count++;
}

// Where a descriptor is positioned, asked rather than read off struct fd.
//
// fd->offset is not the answer for a realfs or fakefs file: the position lives
// in the HOST descriptor, and fd->offset is only maintained by the filesystems
// that have nowhere else to keep it. Reading it gave every restored file an
// offset of 0, so a shell that had read one line of /etc/services came back
// about to read that same line again -- which is exactly the thing this
// feature exists to get right.
static uint64_t ckpt_fd_offset(struct fd *fd) {
    if (fd->ops != NULL && fd->ops->lseek != NULL) {
        off_t_ pos = fd->ops->lseek(fd, 0, LSEEK_CUR);
        if (pos >= 0)
            return (uint64_t) pos;
    }
    return fd->offset;
}

// One descriptor, classified. Returns the kind, or 0 with a refusal recorded.
static int ckpt_classify_fd(int num, struct fd *fd, char *path, size_t path_size) {
    const char *family = fd->ops != NULL && fd->ops->name != NULL
            ? fd->ops->name : "unknown";

    path[0] = '\0';
    if (fd->ops != NULL && fd->ops->name != NULL &&
            strcmp(fd->ops->name, "devpts") == 0)
        return CKPT_FD_TTY;
    // The console the entry point wired up at boot. It is a tty by mode,
    // whatever family opened it, and it comes back by being RE-OPENED rather
    // than restored -- sockrestart's model, and the only honest one for a
    // terminal belonging to a process that no longer exists.
    if (fd->tty != NULL)
        return CKPT_FD_TTY;
    // The standard streams as the entry point handed them over. On the CLI
    // with output piped these are host descriptors wrapped in a struct fd
    // (kernel/init.c's open_fd_from_actual_fd) -- a pipe or a socket whose
    // other end is a process on the Mac. There is nothing to serialise and
    // nothing that would mean anything on the way back, so they are
    // re-attached like the tty.
    //
    // Identified by the HOST descriptor they wrap, not by the guest number
    // they sit at, and that is not pedantry: a shell moves its saved stdin to
    // fd 10 for the duration of a redirection (dash's to_upper_fd), so the
    // very first checkpoint taken from inside `while read; done < file`
    // refused on "fd 10 is a special file". It is the same stream, wherever
    // the guest is holding it.
    if (fd->real_fd >= 0 && fd->real_fd <= 2 &&
            !S_ISREG(fd->type) && !S_ISDIR(fd->type))
        return CKPT_FD_STDIO;
    // An ordinary character device -- /dev/null, /dev/zero, /dev/urandom.
    // These have a stable path and no state, so they come back by being
    // opened again, and they are NOT the terminal case above: treating
    // /dev/null as a console gave a backgrounded `sleep &` a tty for stdin,
    // which is the opposite of what putting it on /dev/null was for.
    if (S_ISCHR(fd->type) && generic_getpath(fd, path) >= 0 && path[0] == '/')
        return CKPT_FD_CHR;
    // A pipe. AOK's pipes are HOST pipes with a struct fd over each end
    // (fs/pipe.c), so what has to travel is the pairing, the direction and
    // whatever bytes are still in flight -- not the object, which cannot
    // outlive the process that owns it.
    if (S_ISFIFO(fd->type) && fd->real_fd >= 0 && fd->stat.inode != 0)
        return CKPT_FD_PIPE;
    if (!S_ISREG(fd->type) && !S_ISDIR(fd->type)) {
        ckpt_refuse("fd %d is a %s on %s with no restore rule",
                    num, S_ISFIFO(fd->type) ? "named pipe" :
                         S_ISSOCK(fd->type) ? "socket" : "special file",
                    family);
        return 0;
    }
    int err = generic_getpath(fd, path);
    if (err < 0 || path[0] != '/') {
        ckpt_refuse("fd %d on %s has no path to re-open", num, family);
        return 0;
    }
    (void) path_size;
    return S_ISDIR(fd->type) ? CKPT_FD_DIR : CKPT_FD_FILE;
}

// Walk the address space and hand each contiguous run of like-flagged mapped
// pages to `emit`. Contiguity is by FLAGS as well as by address: restoring a
// run means one pt_map_nothing, and pt_map_nothing takes one flag word.
static int ckpt_for_each_map(struct mem *mem,
        int (*emit)(void *ctx, page_t start, pages_t pages, unsigned flags),
        void *ctx) {
    // mem_next_page, not page++, and that distinction is the difference
    // between finishing and not. An arm64 guest's page_limit is the top of a
    // 64-bit address space -- billions of pages, almost all of them holes --
    // and a linear scan of it does not return. mem_next_page is the SPARSE
    // walk /proc/<pid>/maps uses for the same reason: from the last page of a
    // page-table leaf it jumps to the next leaf that exists.
    page_t page = 0;
    while (page < mem->page_limit) {
        while (page < mem->page_limit && mem_pt(mem, page) == NULL)
            mem_next_page(mem, &page);
        if (page >= mem->page_limit)
            break;
        page_t start = page;
        unsigned flags = mem_pt(mem, page)->flags;
        while (page < mem->page_limit) {
            struct pt_entry *e = mem_pt(mem, page);
            if (e == NULL || e->flags != flags)
                break;
            page++;
            // A run has to stop at the end of a leaf as well: past it the
            // next page may be billions away, and the pages between are not
            // part of this mapping.
            if (page < mem->page_limit && mem_pt(mem, page) == NULL)
                break;
        }
        int err = emit(ctx, start, page - start, flags);
        if (err < 0)
            return err;
    }
    return 0;
}

struct ckpt_count_ctx { uint32_t maps; uint64_t pages; };
static int ckpt_count_map(void *vctx, page_t UNUSED(start), pages_t pages,
        unsigned UNUSED(flags)) {
    struct ckpt_count_ctx *c = vctx;
    c->maps++;
    c->pages += pages;
    return 0;
}

struct ckpt_emit_ctx { struct ckpt_writer *w; struct mem *mem; };
static int ckpt_emit_map(void *vctx, page_t start, pages_t pages, unsigned flags) {
    struct ckpt_emit_ctx *c = vctx;
    struct ckpt_map m = {
        .start = (uint64_t) start << PAGE_BITS,
        .pages = pages,
        .flags = flags,
    };
    wr(c->w, &m, sizeof(m));
    for (pages_t i = 0; i < pages && c->w->err == 0; i++) {
        guest_addr_t addr = ((guest_addr_t) (start + i)) << PAGE_BITS;
        // MEM_READ, so a page the pager has evicted is faulted back in rather
        // than written out as a hole. That is the whole point of taking the
        // checkpoint through the ordinary read path: swap is not a second
        // place the image has to look.
        const char *p = mem_ptr(c->mem, addr, MEM_READ);
        if (p == NULL) {
            // A PROT_NONE guard page is mapped and unreadable, and that is
            // normal rather than an error -- a stack guard, or the gap
            // pthreads leaves. Write zeroes; the flags travel separately and
            // put the protection back.
            static const char zero[PAGE_SIZE];
            wr(c->w, zero, PAGE_SIZE);
        } else {
            wr(c->w, p, PAGE_SIZE);
        }
    }
    return c->w->err;
}

// One task, written in full: its record, its register file, its signal
// dispositions and limits, its address space, its descriptors.
//
// `current` is REPOINTED at the task for the duration. That is an established
// move in this tree (kernel/init.c does it in three places) and it is what
// lets every helper below -- generic_getpath, mem_ptr, the filesystem's lseek
// -- be the ordinary one rather than a second copy that takes an explicit
// task. Safe because every other task is frozen; unsafe the moment that stops
// being true.
// One task, written in full: its record, its register file, its signal
// dispositions and limits, its address space, its descriptors.
//
// `current` is REPOINTED at the task for the duration. That is an established
// move in this tree (kernel/init.c does it in three places) and it is what
// lets every helper below -- generic_getpath, mem_ptr, the filesystem's lseek
// -- be the ordinary one rather than a second copy that takes an explicit
// task. Safe because every other task is frozen; unsafe the moment that stops
// being true.
//
// Three shapes, sharing the descriptor half: an ordinary task, a NATIVE task
// (no address space to photograph, a self-description instead), and a zombie
// (a status and nothing else).
static int ckpt_save_task(struct ckpt_writer *w, struct task *task,
        uint32_t *stdio_is_tty, uint64_t *pages_out, struct ckpt_fd_ids *ids) {
    struct task *saved_current = current;
    current = task;
    int ret = 0;

    const struct native_program *prog = native_program_running(task);
    struct ckpt_saved_fd *saved = NULL;
    unsigned nfds = 0;

    if (task->zombie) {
        // Nothing but the status its parent has not collected. No address
        // space, no descriptors, no register file -- a zombie has already run
        // its last instruction, and what makes it worth recording is that
        // something is still going to wait() for it.
        struct ckpt_task z = {
            .pid = task->pid,
            .ppid = task->parent != NULL ? task->parent->pid : 0,
            .pgid = task->group != NULL ? task->group->pgid : 0,
            .sid = task->group != NULL ? task->group->sid : 0,
            .abi = (uint32_t) task->abi,
            .zombie = 1,
            .exit_code = task->exit_code,
            .n_sigactions = NUM_SIGS,
        };
        memcpy(z.comm, task->comm, sizeof(z.comm));
        CKPT_TRACE("save pid %d (ppid %d) %s: ZOMBIE, exit code %#x\n",
                   z.pid, z.ppid, z.comm, z.exit_code);
        wr(w, &z, sizeof(z));
        current = saved_current;
        return w->err;
    }

    // ---- the descriptors, gathered for either shape ----------------------
    //
    // Gathered ONCE, with a reference held, and the table lock dropped before
    // anything is asked of them. Two reasons, and the second was found the
    // hard way:
    //
    //  - Classifying twice (a pre-flight pass and then the write) meant
    //    describing a table that could have changed in between.
    //  - Asking a descriptor where it is positioned runs the filesystem's
    //    lseek, and on /proc/ish/checkpoint that regenerates the file --
    //    which walks every task's fd table, including this one. Holding
    //    files->lock across it deadlocked the guest against itself.
    struct fdtable *files = task->files;
    lock(&files->lock, 0);
    unsigned cap = files->size;
    saved = calloc(cap != 0 ? cap : 1, sizeof(*saved));
    if (saved == NULL) {
        unlock(&files->lock);
        current = saved_current;
        return _ENOMEM;
    }
    for (unsigned i = 0; i < files->size; i++) {
        struct fd *fd = files->files[i];
        if (fd == NULL)
            continue;
        saved[nfds].num = i;
        saved[nfds].fd = fd_retain(fd);
        saved[nfds].cloexec = bit_test(i, files->cloexec) ? 1 : 0;
        nfds++;
    }
    unlock(&files->lock);

    for (unsigned i = 0; i < nfds; i++) {
        struct ckpt_saved_fd *s = &saved[i];
        s->id = ckpt_fd_id(ids, s->fd, &s->first);
        if (!s->first) {
            // Already described, here or in another process. What matters is
            // that it comes back as the SAME object.
            s->kind = CKPT_FD_REF;
            continue;
        }
        s->kind = ckpt_classify_fd((int) s->num, s->fd, s->path, sizeof(s->path));
        if (s->kind == 0) {
            ret = _EOPNOTSUPP;
            goto out;
        }
        s->offset = s->kind == CKPT_FD_STDIO ? (uint64_t) s->fd->real_fd
                                             : ckpt_fd_offset(s->fd);
        if (s->kind == CKPT_FD_PIPE) {
            // Only the READ end carries the contents: the bytes are in the
            // pipe once, and taking them from both ends would double them.
            if (!(s->fd->flags & O_WRONLY_) &&
                    (ret = ckpt_pipe_drain(s->fd, &s->pipe_bytes, &s->pipe_len)) < 0)
                goto out;
            s->offset = s->pipe_len;
        }
        if (s->num <= 2 && s->kind == CKPT_FD_TTY)
            *stdio_is_tty = 1;
    }

    char cwd[MAX_PATH + 1] = "/", root[MAX_PATH + 1] = "/";
    lock(&task->fs->lock, 0);
    if (task->fs->pwd != NULL)
        generic_getpath(task->fs->pwd, cwd);
    if (task->fs->root != NULL)
        generic_getpath(task->fs->root, root);
    mode_t_ umask = task->fs->umask;
    unlock(&task->fs->lock);

    struct ckpt_task rec = {
        .pid = task->pid,
        .ppid = task->parent != NULL ? task->parent->pid : 0,
        .pgid = task->group->pgid, .sid = task->group->sid,
        .n_fds = nfds,
        .abi = (uint32_t) task->abi,
        .uid = task->uid, .gid = task->gid,
        .euid = task->euid, .egid = task->egid,
        .suid = task->suid, .sgid = task->sgid,
        .fsuid = task->fsuid, .fsgid = task->fsgid,
        .umask = umask,
        .blocked = task->blocked, .pending = task->pending,
        .altstack = task->altstack, .altstack_size = task->altstack_size,
        .clear_tid = task->clear_tid,
        .exit_signal = task->exit_signal,
        .pdeath_signal = task->pdeath_signal,
        .nice = task->nice,
        .sched_policy = task->sched_policy,
        .robust_list = task->robust_list,
        .did_exec = task->did_exec ? 1 : 0,
        .n_sigactions = NUM_SIGS,
        .cwd_len = (uint32_t) strlen(cwd),
        .root_len = (uint32_t) strlen(root),
    };
    memcpy(rec.comm, task->comm, sizeof(rec.comm));

    struct mem *mem = NULL;
    struct ckpt_count_ctx counts = {0};
    const char *native_state = NULL;
    const char *native_argv = NULL;
    char *native_env = NULL;
    size_t native_env_len = 0;

    if (prog != NULL) {
        // A NATIVE task: no address space worth photographing and no register
        // file that means anything. What travels is the program's name, the
        // argv it was given, and the state it produced about itself when the
        // freeze parked it (checkpoint_native_park runs on its own thread,
        // which is the only place its state exists).
        native_state = task->ckpt_native_state != NULL ? task->ckpt_native_state : "";
        native_argv = task->native_cmdline != NULL ? task->native_cmdline : "";
        rec.native = 1;
        rec.native_name_len = (uint32_t) strlen(prog->name);
        rec.native_argv_len = (uint32_t) (task->native_cmdline != NULL
                ? task->native_cmdline_len : 0);
        rec.native_state_len = (uint32_t) strlen(native_state);
        // The environment, as one NUL-separated block. A shell's exported
        // parameters come back with the state script, but the program reads
        // `environ` before it sources anything -- and a restored program with
        // no PATH at all would not find the first thing it was asked to run.
        for (char **e = task->native_env; e != NULL && *e != NULL; e++)
            native_env_len += strlen(*e) + 1;
        if (native_env_len != 0) {
            native_env = malloc(native_env_len);
            if (native_env == NULL) { ret = _ENOMEM; goto out; }
            size_t at = 0;
            for (char **e = task->native_env; *e != NULL; e++) {
                size_t n = strlen(*e) + 1;
                memcpy(native_env + at, *e, n);
                at += n;
            }
        }
        rec.native_env_len = (uint32_t) native_env_len;
        CKPT_TRACE("save pid %d (ppid %d) NATIVE %s: %u fds, %u bytes of state\n",
                   rec.pid, rec.ppid, prog->name, rec.n_fds,
                   rec.native_state_len);
        // The tail, because a state that does not finish is the failure this
        // has to distinguish from one that is simply wrong: the last line is
        // the program's own sentinel.
        CKPT_TRACE("  state ends: %s\n", rec.native_state_len > 90
                   ? native_state + rec.native_state_len - 90 : native_state);
    } else {
        struct mm *mm = task->mm;
        mem = task->mem;
        read_lock(&mem->lock);
        ckpt_for_each_map(mem, ckpt_count_map, &counts);
        rec.n_maps = counts.maps;
        rec.brk = mm->brk; rec.start_brk = mm->start_brk;
        rec.vdso = mm->vdso; rec.stack_start = mm->stack_start;
        rec.argv_start = mm->argv_start; rec.argv_end = mm->argv_end;
        rec.env_start = mm->env_start; rec.env_end = mm->env_end;
        rec.auxv_start = mm->auxv_start; rec.auxv_end = mm->auxv_end;
        rec.page_limit = mem->page_limit;
        rec.mmap_floor = mem->mmap_floor;
        rec.mmap_ceiling = mem->mmap_ceiling;
        rec.stack_top = mem->stack_top;
        rec.stack_limit_pages = mem->stack_limit_pages;
        CKPT_TRACE("save pid %d (ppid %d pgid %d sid %d) %s: %u maps, %u fds, "
                   "%llu pages\n", rec.pid, rec.ppid, rec.pgid, rec.sid,
                   rec.comm, rec.n_maps, rec.n_fds,
                   (unsigned long long) counts.pages);
    }

    wr(w, &rec, sizeof(rec));
    wr(w, cwd, rec.cwd_len);
    wr(w, root, rec.root_len);

    if (prog != NULL) {
        wr(w, prog->name, rec.native_name_len);
        wr(w, native_argv, rec.native_argv_len);
        wr(w, native_state, rec.native_state_len);
        wr(w, native_env, rec.native_env_len);
    } else {
        // The register file, as bytes. See ckpt_fingerprint for why that is
        // safe and what stops it from being unsafe.
        wr(w, &task->cpu, sizeof(struct cpu_state));

        lock(&task->sighand->lock, 0);
        wr(w, task->sighand->action, sizeof(struct sigaction_) * NUM_SIGS);
        unlock(&task->sighand->lock);

        lock(&task->group->lock, 0);
        wr(w, task->group->limits, sizeof(task->group->limits));
        unlock(&task->group->lock);

        struct ckpt_emit_ctx emit = { .w = w, .mem = mem };
        ckpt_for_each_map(mem, ckpt_emit_map, &emit);
        read_unlock(&mem->lock);
        mem = NULL;
        *pages_out += counts.pages;
    }

    for (unsigned i = 0; i < nfds && w->err == 0; i++) {
        struct ckpt_saved_fd *s = &saved[i];
        struct ckpt_fd cf = {
            .fd = s->num,
            .cloexec = s->cloexec,
            .flags = s->fd->flags,
            .kind = (uint32_t) s->kind,
            // For a standard stream the offset is meaningless and the host
            // descriptor it mirrors is what matters, so the field carries
            // that instead; for a pipe it is how many bytes follow.
            .offset = s->offset,
            .path_len = (uint32_t) strlen(s->path),
            .id = s->id,
            .pipe_inode = s->kind == CKPT_FD_PIPE ? s->fd->stat.inode : 0,
            .pipe_write_end = s->kind == CKPT_FD_PIPE &&
                    (s->fd->flags & O_WRONLY_) ? 1 : 0,
        };
        CKPT_TRACE("  save fd %u %-5s id %u real_fd %d flags %#x off %llu %s\n",
                   cf.fd, ckpt_kind_name(cf.kind), cf.id, s->fd->real_fd,
                   cf.flags, (unsigned long long) cf.offset, s->path);
        wr(w, &cf, sizeof(cf));
        wr(w, s->path, cf.path_len);
        if (s->pipe_len != 0)
            wr(w, s->pipe_bytes, s->pipe_len);
    }
    ret = w->err;

out:
    free(native_env);
    if (mem != NULL)
        read_unlock(&mem->lock);
    if (saved != NULL) {
        for (unsigned i = 0; i < nfds; i++) {
            fd_close(saved[i].fd);
            free(saved[i].pipe_bytes);
        }
        free(saved);
    }
    current = saved_current;
    return ret;
}

// Order the tasks so a parent is always written before its children.
//
// Not tidiness: the restore creates each task as a CHILD of one that already
// exists, because that is the only way the parent/child lists and the wait
// machinery come out right. A child written first would have nothing to be
// created under.
static void ckpt_order_tasks(struct task **tasks, unsigned count) {
    unsigned placed = 0;
    while (placed < count) {
        unsigned progress = 0;
        for (unsigned i = placed; i < count; i++) {
            struct task *t = tasks[i];
            bool parent_ready = t->parent == NULL;
            for (unsigned j = 0; j < placed && !parent_ready; j++)
                if (tasks[j] == t->parent)
                    parent_ready = true;
            if (!parent_ready)
                continue;
            struct task *swap = tasks[placed];
            tasks[placed] = t;
            tasks[i] = swap;
            placed++;
            progress++;
        }
        // A task whose parent is not in the snapshot at all -- reparented to
        // init while this was being collected. Take it anyway rather than
        // spinning; the restore reparents it to pid 1, which is where it was
        // going.
        if (progress == 0)
            break;
    }
}

// The externally-triggered form. Same body; the difference is only that there
// is no `current` to leave running, so every task is frozen.
int checkpoint_save_external(const char *host_path) {
    return checkpoint_save(host_path);
}

int checkpoint_save(const char *host_path) {
    int err = ckpt_check_scope();
    if (err < 0)
        return err;

    // STOP THE MACHINE. Everything below describes tasks that are not running,
    // which is the whole difference between a checkpoint and a photograph of a
    // moving object.
    char blame[192] = "";
    err = ckpt_freeze_all(CKPT_FREEZE_TIMEOUT_MS, blame, sizeof(blame));
    if (err < 0) {
        ckpt_refuse("%s", blame);
        return err;
    }

    struct task_snapshot snap = {0};
    if (task_snapshot_collect(&snap, false) < 0) {
        ckpt_thaw_all();
        ckpt_refuse("could not enumerate tasks");
        return _EAGAIN;
    }
    // A task inside do_exit has already run its last instruction and has no
    // state left worth carrying. Dropped here rather than in the writer, so
    // the header's task count is the number actually written.
    unsigned live = 0;
    for (unsigned i = 0; i < snap.count; i++) {
        struct task *t = snap.tasks[i];
        if (t->zombie || !ckpt_task_is_leaving(t))
            snap.tasks[live++] = t;
        else
            task_ref_cnt_mod(t, -1);
    }
    snap.count = live;
    ckpt_order_tasks(snap.tasks, snap.count);

    FILE *f = fopen(host_path, "wb");
    if (f == NULL) {
        err = errno_map();
        task_snapshot_release(&snap);
        ckpt_thaw_all();
        return err;
    }
    struct ckpt_writer w = { .f = f };

    struct ckpt_header h = {
        .version = CKPT_VERSION,
        // The first task's, not `current`'s: an external caller (the app's
        // backgrounding path) is not a guest task at all.
        .abi = (uint32_t) snap.tasks[0]->abi,
        .cpu_state_size = (uint32_t) sizeof(struct cpu_state),
        .page_size = PAGE_SIZE,
        .build_fingerprint = ckpt_fingerprint(),
        .n_tasks = snap.count,
    };
    memcpy(h.magic, CKPT_MAGIC, sizeof(h.magic));
    // Written now and rewritten at the end: the page count and the stdio kind
    // are only known once every task has been walked, and the header has to
    // come first in the file.
    long header_at = ftell(f);
    wr(&w, &h, sizeof(h));

    uint64_t pages = 0;
    // One id space for the whole image, so a descriptor two processes share is
    // described once and referenced from the other.
    struct ckpt_fd_ids ids = {0};
    for (unsigned i = 0; i < snap.count && err == 0; i++)
        err = ckpt_save_task(&w, snap.tasks[i], &h.stdio_is_tty, &pages, &ids);
    unsigned nfds_total = ids.count;
    free(ids.fds);
    task_snapshot_release(&snap);

    if (err == 0 && w.err == 0) {
        h.total_pages = pages;
        if (fseek(f, header_at, SEEK_SET) == 0)
            wr(&w, &h, sizeof(h));
        else
            w.err = errno_map();
    }
    if (err == 0)
        err = w.err;
    if (fclose(f) != 0 && err == 0)
        err = errno_map();

    ckpt_thaw_all();

    if (err != 0) {
        unlink(host_path);
        return err;
    }

    lock(&ckpt_lock, 0);
    ckpt_status.saves++;
    ckpt_status.last_err = 0;
    ckpt_status.last_refusal[0] = '\0';
    snprintf(ckpt_status.last_path, sizeof(ckpt_status.last_path), "%s", host_path);
    ckpt_status.pages = (unsigned long) pages;
    ckpt_status.tasks = h.n_tasks;
    ckpt_status.fds = nfds_total;
    ckpt_status.bytes = (unsigned long long) pages * PAGE_SIZE;
    unlock(&ckpt_lock);
    return 0;
}

// ------------------------------------------------------------------ reading

// Everything about one task, read back onto `task`, which must already exist
// and be current.
// What the restore has built so far, shared by every task in the image.
struct ckpt_restore_state {
    struct fd *stdio[3];
    // The native program the task being restored right now is, if it is one.
    // Read by the caller once ckpt_restore_task returns, so it can dispatch
    // the program rather than start a guest thread.
    char *native_name, *native_argv, *native_state, *native_env;
    uint32_t native_argv_len, native_env_len;
    // id -> the struct fd built for it. A CKPT_FD_REF installs this one again
    // rather than making a second object, which is what keeps a forked child's
    // file offset the same object as its parent's.
    struct fd **by_id;
    uint32_t id_count, id_cap;
    // pipe inode -> the two ends built for it, so a pair whose ends are held
    // by two different processes becomes ONE host pipe.
    struct { uint64_t inode; struct fd *rd, *wr; } *pipes;
    uint32_t pipe_count, pipe_cap;
};

// Record `fd` under `id`, taking a reference of the table's own.
//
// The table holding a reference is what makes the ownership rule one sentence:
// EVERY pointer this structure keeps -- stdio, by_id, both ends of each pipe --
// is one reference, released once at the end of the restore. Installing a
// descriptor in a process's table is a separate retain. Getting this wrong the
// other way round closed the standard streams an extra time each, which shut
// the app's real stdout and made a restored guest look silently hung.
static int ckpt_id_put(struct ckpt_restore_state *st, uint32_t id, struct fd *fd) {
    if (id == UINT32_MAX)
        return 0;   // the save could not allocate an id; nothing refers to it
    if (id >= st->id_cap) {
        uint32_t cap = st->id_cap ? st->id_cap * 2 : 32;
        while (id >= cap)
            cap *= 2;
        struct fd **n = realloc(st->by_id, cap * sizeof(*n));
        if (n == NULL)
            return _ENOMEM;
        memset(n + st->id_cap, 0, (cap - st->id_cap) * sizeof(*n));
        st->by_id = n;
        st->id_cap = cap;
    }
    if (id >= st->id_count)
        st->id_count = id + 1;
    st->by_id[id] = fd_retain(fd);
    return 0;
}

// The two ends of the pipe with this inode, created on first sight.
static int ckpt_pipe_for(struct ckpt_restore_state *st, uint64_t inode,
        struct fd **out_rd, struct fd **out_wr) {
    for (uint32_t i = 0; i < st->pipe_count; i++) {
        if (st->pipes[i].inode == inode) {
            *out_rd = st->pipes[i].rd;
            *out_wr = st->pipes[i].wr;
            return 0;
        }
    }
    int err = pipe_create_pair(out_rd, out_wr, inode);
    if (err < 0)
        return err;
    if (st->pipe_count == st->pipe_cap) {
        uint32_t cap = st->pipe_cap ? st->pipe_cap * 2 : 8;
        void *n = realloc(st->pipes, cap * sizeof(*st->pipes));
        if (n == NULL)
            return _ENOMEM;
        st->pipes = n;
        st->pipe_cap = cap;
    }
    st->pipes[st->pipe_count].inode = inode;
    st->pipes[st->pipe_count].rd = *out_rd;
    st->pipes[st->pipe_count].wr = *out_wr;
    st->pipe_count++;
    return 0;
}

static int ckpt_restore_task(FILE *f, const struct ckpt_header *h,
        const struct ckpt_task *rec, bool stdio_is_tty,
        struct ckpt_restore_state *st) {
    int err;
    struct fdtable *files;
    char cwd[MAX_PATH + 1] = {0}, root[MAX_PATH + 1] = {0};
    if ((err = rd(f, cwd, rec->cwd_len)) < 0) return err;
    if ((err = rd(f, root, rec->root_len)) < 0) return err;

    // A NATIVE task: the program's name, the argv it had, and the state it
    // produced about itself. No register file and no address space follow --
    // it is not being photographed, it is being told to come back and rebuild
    // itself, which is the only thing a C function on a host thread can do.
    char *native_name = NULL, *native_argv = NULL, *native_state = NULL;
    char *native_env = NULL;
    if (rec->native) {
        native_name = calloc(rec->native_name_len + 1, 1);
        native_argv = calloc(rec->native_argv_len + 1, 1);
        native_state = calloc(rec->native_state_len + 1, 1);
        native_env = calloc(rec->native_env_len + 1, 1);
        if (native_name == NULL || native_argv == NULL ||
                native_state == NULL || native_env == NULL)
            err = _ENOMEM;
        else if ((err = rd(f, native_name, rec->native_name_len)) >= 0 &&
                 (err = rd(f, native_argv, rec->native_argv_len)) >= 0 &&
                 (err = rd(f, native_state, rec->native_state_len)) >= 0)
            err = rd(f, native_env, rec->native_env_len);
        if (err < 0) {
            free(native_name); free(native_argv);
            free(native_state); free(native_env);
            return err;
        }
        st->native_name = native_name;
        st->native_argv = native_argv;
        st->native_argv_len = rec->native_argv_len;
        st->native_state = native_state;
        st->native_env = native_env;
        st->native_env_len = rec->native_env_len;
        goto descriptors;
    }

    struct cpu_state cpu;
    if ((err = rd(f, &cpu, sizeof(cpu))) < 0) return err;
    struct sigaction_ actions[NUM_SIGS];
    if ((err = rd(f, actions, sizeof(actions))) < 0) return err;
    rlim_t_ limits[sizeof(current->group->limits) / sizeof(current->group->limits[0])][2];
    if ((err = rd(f, limits, sizeof(limits))) < 0) return err;

    // The limits first, because RLIMIT_NOFILE gates how many descriptors can
    // be installed below and the image's value is the one that was in force.
    lock(&current->group->lock, 0);
    memcpy(current->group->limits, limits, sizeof(limits));
    current->group->pgid = rec->pgid;
    current->group->sid = rec->sid;
    unlock(&current->group->lock);

    // The guest architecture, and with it the address space's shape. A fresh
    // task's mm is built for the entry point's default; the image says what
    // this process actually was, and every mapping below depends on it. Set
    // before a single page is mapped.
    current->abi = (enum guest_abi) rec->abi;
    struct mem *mem = current->mem;
    struct mm *mm = current->mm;
    mem_set_page_limit(mem, (page_t) rec->page_limit);
    mem_set_mmap_window(mem, (page_t) rec->mmap_floor, (page_t) rec->mmap_ceiling);
    mem_set_stack_bounds(mem, (page_t) rec->stack_top,
                         (uint64_t) rec->stack_limit_pages << PAGE_BITS);

    for (uint32_t i = 0; i < rec->n_maps; i++) {
        struct ckpt_map m;
        if ((err = rd(f, &m, sizeof(m))) < 0)
            return err;
        page_t start_page = (page_t) (m.start >> PAGE_BITS);
        write_lock(&mem->lock);
        // A fresh mm is not empty -- mm_new maps the vdso -- and the image is
        // the complete truth about this address space, so anything already
        // sitting where a mapping goes is replaced rather than collided with.
        pt_unmap_always(mem, start_page, (pages_t) m.pages);
        // Mapped WRITABLE regardless of the saved protection, then set to the
        // saved flags once the bytes are in: a PROT_NONE guard page or a
        // read-only text segment cannot be filled through mem_ptr otherwise.
        err = pt_map_nothing(mem, start_page, (pages_t) m.pages,
                             P_READ | P_WRITE | P_ANONYMOUS);
        write_unlock(&mem->lock);
        if (err < 0)
            return err;
        for (uint64_t pg = 0; pg < m.pages; pg++) {
            guest_addr_t addr = ((guest_addr_t) (start_page + pg)) << PAGE_BITS;
            write_lock(&mem->lock);
            char *dst = mem_ptr(mem, addr, MEM_WRITE);
            write_unlock(&mem->lock);
            if (dst == NULL)
                return _EFAULT;
            if ((err = rd(f, dst, PAGE_SIZE)) < 0)
                return err;
        }
        write_lock(&mem->lock);
        err = pt_set_flags(mem, start_page, (pages_t) m.pages, (int) m.flags);
        write_unlock(&mem->lock);
        if (err < 0)
            return err;
    }

    mm->brk = rec->brk;
    mm->start_brk = rec->start_brk;
    mm->vdso = rec->vdso;
    mm->stack_start = rec->stack_start;
    mm->argv_start = rec->argv_start; mm->argv_end = rec->argv_end;
    mm->env_start = rec->env_start; mm->env_end = rec->env_end;
    mm->auxv_start = rec->auxv_start; mm->auxv_end = rec->auxv_end;

descriptors:
    // The descriptors. Everything the fresh task opened for itself goes
    // first: the image is the complete truth about what this process had open.
    files = current->files;
    lock(&files->lock, 0);
    for (unsigned i = 0; i < files->size; i++) {
        if (files->files[i] != NULL) {
            fd_close(files->files[i]);
            files->files[i] = NULL;
        }
    }
    unlock(&files->lock);

    // The standard streams. Set up ONCE for the whole restore and SHARED by
    // every process in it -- not created per task.
    //
    // Per task is what a fresh boot does, and it is wrong here for the same
    // reason it would be wrong to give a forked child its own dup of the
    // terminal: create_piped_stdio wraps the HOST's descriptors 0, 1 and 2, so
    // N restored processes meant N struct fds over the same three host
    // descriptors. The first child to exit closed them, and the app's real
    // stdout and stderr went with it -- the parent then wrote into a closed
    // descriptor and the session looked hung. A fork shares the struct fd; so
    // does this.
    if (st->stdio[0] == NULL) {
        if (stdio_is_tty)
            create_stdio("/dev/tty1", TTY_CONSOLE_MAJOR, 1);
        else
            create_piped_stdio();
        lock(&files->lock, 0);
        for (unsigned i = 0; i < 3; i++) {
            st->stdio[i] = i < files->size ? files->files[i] : NULL;
            if (st->stdio[i] != NULL)
                fd_retain(st->stdio[i]);   // the restore's own reference
        }
        unlock(&files->lock);
    }
    // RETAINED before installing, because installing the image's fd 1 detaches
    // and CLOSES whatever was in that slot -- with a refcount of one that frees
    // it, and mirroring fd 10 from a saved pointer afterwards was then a
    // use-after-free that surfaced as `echo: I/O error` in the restored shell
    // rather than as a crash.
    struct fd **stdio = st->stdio;
    for (unsigned i = 0; i < 3; i++) {
        if (stdio[i] == NULL)
            continue;
        fd_retain(stdio[i]);
        if ((err = fdtable_install_at(files, (fd_t) i, stdio[i], false)) < 0)
            return err;
    }

    for (uint32_t i = 0; i < rec->n_fds; i++) {
        struct ckpt_fd cf;
        if ((err = rd(f, &cf, sizeof(cf))) < 0)
            goto fds_done;
        char path[MAX_PATH + 1] = {0};
        if (cf.path_len > MAX_PATH) { err = _EINVAL; goto fds_done; }
        if ((err = rd(f, path, cf.path_len)) < 0)
            goto fds_done;

        CKPT_TRACE("  load fd %u %-5s id %u flags %#x off %llu %s\n",
                   cf.fd, ckpt_kind_name(cf.kind), cf.id, cf.flags,
                   (unsigned long long) cf.offset, path);

        // The same object as one already built -- a descriptor this process
        // shares with another, or with itself at a second number.
        if (cf.kind == CKPT_FD_REF) {
            struct fd *shared = cf.id < st->id_count ? st->by_id[cf.id] : NULL;
            if (shared == NULL) {
                err = _EINVAL;
                goto fds_done;
            }
            fd_retain(shared);
            if ((err = fdtable_install_at(files, (fd_t) cf.fd, shared,
                                          cf.cloexec != 0)) < 0)
                goto fds_done;
            continue;
        }

        if (cf.kind == CKPT_FD_PIPE) {
            // NOT named rd/wr: `rd` is this file's reader function, and a
            // local of that name turns rd(f, ...) below into a call through a
            // struct fd pointer.
            struct fd *pipe_rd = NULL, *pipe_wr = NULL;
            if ((err = ckpt_pipe_for(st, cf.pipe_inode, &pipe_rd, &pipe_wr)) < 0)
                goto fds_done;
            struct fd *end = cf.pipe_write_end ? pipe_wr : pipe_rd;
            // The bytes that were in flight, put back at the write end so the
            // reader sees them exactly where it left off. Only the read end's
            // record carries them, so this runs once per pipe.
            if (cf.offset != 0) {
                char *buf = malloc((size_t) cf.offset);
                if (buf == NULL) { err = _ENOMEM; goto fds_done; }
                if ((err = rd(f, buf, (size_t) cf.offset)) < 0) {
                    free(buf);
                    goto fds_done;
                }
                ssize_t put = write(pipe_wr->real_fd, buf, (size_t) cf.offset);
                free(buf);
                if (put != (ssize_t) cf.offset) {
                    ckpt_refuse("pipe %llu had %llu bytes in it, more than a "
                                "fresh pipe will hold",
                                (unsigned long long) cf.pipe_inode,
                                (unsigned long long) cf.offset);
                    err = _EAGAIN;
                    goto fds_done;
                }
            }
            if ((err = ckpt_id_put(st, cf.id, end)) < 0)
                goto fds_done;
            fd_retain(end);   // the process's own
            if ((err = fdtable_install_at(files, (fd_t) cf.fd, end,
                                          cf.cloexec != 0)) < 0)
                goto fds_done;
            continue;
        }

        // Re-attached, not restored: the terminal or the host pipe this guest
        // was talking to went with the process that owned it. sockrestart is
        // the precedent -- record enough to REBUILD, because the original is
        // destroyed either way. 0, 1 and 2 were set up above; anywhere else
        // the same stream is another reference to one of those three.
        if (cf.kind == CKPT_FD_STDIO || cf.kind == CKPT_FD_TTY) {
            if (cf.fd <= 2) {
                if (cf.fd < 3 && stdio[cf.fd] != NULL &&
                        (err = ckpt_id_put(st, cf.id, stdio[cf.fd])) < 0)
                    goto fds_done;
                continue;
            }
            unsigned mirror = cf.kind == CKPT_FD_STDIO ? (unsigned) cf.offset : 0;
            if (mirror > 2)
                mirror = 0;
            struct fd *src = stdio[mirror];
            if (src != NULL) {
                if ((err = ckpt_id_put(st, cf.id, src)) < 0)
                    goto fds_done;
                fd_retain(src);
                if ((err = fdtable_install_at(files, (fd_t) cf.fd, src,
                                              cf.cloexec != 0)) < 0)
                    goto fds_done;
            }
            continue;
        }

        struct fd *fd = generic_open(path, (int) cf.flags, 0);
        if (IS_ERR(fd)) {
            err = (int) PTR_ERR(fd);
            goto fds_done;
        }
        if (cf.kind == CKPT_FD_FILE && fd->ops->lseek != NULL)
            fd->ops->lseek(fd, (off_t_) cf.offset, LSEEK_SET);
        fd->offset = cf.offset;
        if ((err = ckpt_id_put(st, cf.id, fd)) < 0)
            goto fds_done;
        // A fresh task's table holds three descriptors; the image may name
        // fd 10, because a shell parks its saved stdin up there. Grow to fit
        // rather than refuse -- the number is part of what is restored.
        if ((err = fdtable_install_at(files, (fd_t) cf.fd, fd,
                                      cf.cloexec != 0)) < 0)
            goto fds_done;
    }
    err = 0;
fds_done:
    if (err < 0)
        return err;

    // Credentials, identity and the rest of the task.
    if (rec->native)
        goto identity;
identity:
    current->uid = rec->uid; current->gid = rec->gid;
    current->euid = rec->euid; current->egid = rec->egid;
    current->suid = rec->suid; current->sgid = rec->sgid;
    current->fsuid = rec->fsuid; current->fsgid = rec->fsgid;
    memcpy(current->comm, rec->comm, sizeof(current->comm));
    current->blocked = rec->blocked;
    current->pending = rec->pending;
    current->altstack = rec->altstack;
    current->altstack_size = rec->altstack_size;
    current->clear_tid = rec->clear_tid;
    current->exit_signal = rec->exit_signal;
    current->pdeath_signal = rec->pdeath_signal;
    current->nice = rec->nice;
    current->sched_policy = rec->sched_policy;
    current->robust_list = rec->robust_list;
    current->did_exec = rec->did_exec != 0;

    if (!rec->native) {
        lock(&current->sighand->lock, 0);
        memcpy(current->sighand->action, actions, sizeof(actions));
        unlock(&current->sighand->lock);
    }
    lock(&current->fs->lock, 0);
    current->fs->umask = rec->umask;
    unlock(&current->fs->lock);
    if (cwd[0] == '/') {
        struct fd *pwd = generic_open(cwd, O_RDONLY_, 0);
        if (!IS_ERR(pwd))
            fs_chdir(current->fs, pwd);
    }

    if (rec->native)
        return 0;   // no register file: it is a function call, not an image

    // The register file last, so nothing above can have run guest code with a
    // half-restored one. The two pointers in struct cpu_state name host
    // objects that belong to THIS run: the address space's mmu, and the flag
    // the wake path sets to break out of guest execution. Everything else in
    // there is guest value state; these two are re-attached rather than
    // restored.
    struct mmu *mmu = current->cpu.mmu;
    current->cpu = cpu;
    current->cpu.mmu = mmu;
    // poked_ptr points INTO its own cpu_state (&cpu->_poked, set by every
    // engine's entry). So it can be neither restored from the image -- that is
    // a host address from a process that has exited -- nor carried over from
    // before the assignment: a task built by task_create_with_pid got its
    // parent's whole cpu_state by struct copy, parent's _poked address and
    // all. A restored child would then have its "stop executing" flag set by
    // pokes aimed at its parent and never by its own, so it ran on past every
    // wake and its parent's wait() never returned.
    current->cpu.poked_ptr = &current->cpu._poked;
    (void) h;
    return 0;
}

// Hand a restored native program back its state and arrange for it to run.
//
// A native program is not photographed and not resumed mid-instruction: it is
// RE-LAUNCHED and told to rebuild itself, which is the only thing a C function
// on a host thread can do. The channel is the one its fork-by-relaunch child
// already reads a state from (struct native_program's ckpt_state_var), so
// nothing new has to be taught to the program.
//
// A PIPE, and the state is written before the program is dispatched. It fits:
// a shell's state is tens of kilobytes and a pipe buffer is 64, and if it did
// not, the write would block against a reader that has not started -- so an
// oversized state is refused here rather than deadlocking the restore.
static int ckpt_dispatch_native(struct task *task, struct ckpt_restore_state *st) {
    const struct native_program *prog = native_program_lookup(st->native_name);
    if (prog == NULL) {
        ckpt_refuse("this build has no native program called %s", st->native_name);
        return _ENOENT;
    }

    // argv and envp out of their NUL-separated blocks.
    unsigned argc = 0;
    for (uint32_t i = 0; i < st->native_argv_len; i++)
        if (st->native_argv[i] == '\0')
            argc++;
    unsigned envc = 0;
    for (uint32_t i = 0; i < st->native_env_len; i++)
        if (st->native_env[i] == '\0')
            envc++;

    char **argv = calloc(argc + 1, sizeof(*argv));
    char **envp = calloc(envc + 2, sizeof(*envp));
    if (argv == NULL || envp == NULL) {
        free(argv); free(envp);
        return _ENOMEM;
    }
    unsigned n = 0;
    for (uint32_t i = 0; i < st->native_argv_len && n < argc; ) {
        argv[n++] = st->native_argv + i;
        i += strlen(st->native_argv + i) + 1;
    }
    if (argc == 0)
        argv[argc = 0] = NULL;
    unsigned m = 0;
    for (uint32_t i = 0; i < st->native_env_len && m < envc; ) {
        envp[m++] = st->native_env + i;
        i += strlen(st->native_env + i) + 1;
    }

    char fdvar[64] = "";
    struct fd *state_rd = NULL, *state_wr = NULL;
    if (prog->ckpt_state_var != NULL && st->native_state[0] != '\0') {
        size_t len = strlen(st->native_state);
        // A GUEST pipe, not a host one. The program reads its state through
        // the shim, which routes every descriptor through the guest's own
        // table -- so a raw host descriptor number means nothing to it. That
        // is what made a restored zsh source an empty state and report that it
        // "did not finish": it was reading whatever the guest happened to have
        // at that number, which was nothing.
        int err = pipe_create_pair(&state_rd, &state_wr, adhoc_next_inode());
        if (err < 0) {
            free(argv); free(envp);
            return err;
        }
        ssize_t put = write(state_wr->real_fd, st->native_state, len);
        fd_close(state_wr);
        if (put != (ssize_t) len) {
            fd_close(state_rd);
            free(argv); free(envp);
            ckpt_refuse("%s's saved state is %zu bytes, more than a pipe will "
                        "hold before the program starts reading it",
                        prog->name, len);
            return _E2BIG;
        }
        // At a number nothing in the image used. The image's descriptors are
        // already installed, so the first free slot above them is free for
        // good -- and the program unsets the variable naming it at startup, so
        // nothing it runs inherits either.
        struct fdtable *files = task->files;
        fd_t at = 0;
        lock(&files->lock, 0);
        for (at = 3; (unsigned) at < files->size && files->files[at] != NULL; at++)
            ;
        unlock(&files->lock);
        if ((err = fdtable_install_at(files, at, state_rd, false)) < 0) {
            free(argv); free(envp);
            return err;
        }
        snprintf(fdvar, sizeof(fdvar), "%s=%d", prog->ckpt_state_var, (int) at);
        envp[m++] = fdvar;
        CKPT_TRACE("  native %s: %zu bytes of state on guest fd %d\n",
                   prog->name, len, (int) at);
    }
    envp[m] = NULL;

    // Recorded rather than run: task_run_current calls native_exec_run_pending
    // on the way in, which is exactly how a native program starts on a fresh
    // boot. The copies it makes are its own, so the blocks above may go.
    struct task *saved = current;
    current = task;
    int err = native_exec_set_pending(prog, (int) argc, argv, envp);
    current = saved;
    free(argv);
    free(envp);
    return err;
}

// Build a task to restore INTO: a fresh process, at the pid the image names,
// as a child of the task that image named as its parent.
//
// The same shape as kernel/init.c's construct_task, and deliberately not a
// call to it: that one allocates the next free pid and roots everything at
// init, which is exactly the two things a restore must not do.
static struct task *ckpt_new_task(struct task *parent, pid_t_ pid) {
    struct task *task = task_create_with_pid(parent, pid);
    if (task == NULL)
        return NULL;
    if (parent != NULL)
        uts_ns_retain(task->uts_ns);

    struct tgroup *group = malloc(sizeof(struct tgroup));
    if (group == NULL)
        return NULL;
    *group = (struct tgroup) {};
    list_init(&group->threads);
    lock_init(&group->lock, "ckpt_new_task\0");
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    group->leader = task;
    group->personality = ADDR_NO_RANDOMIZE_;
    // The defaults, before the image's own limits land further down. Without
    // them RLIMIT_NOFILE is zero on a freshly built tgroup, and the first
    // descriptor the restore tries to install comes back EMFILE -- a "too
    // many open files" on a table holding none.
    memcpy(group->limits, init_rlimits, sizeof(init_rlimits));
    list_add(&group->threads, &task->group_links);
    task->group = group;
    task->tgid = task->pid;
    task_setsid(task);

    task_set_mm(task, mm_new(task->abi));
    task->sighand = sighand_new();
    task->files = fdtable_new(3);
    task->fs = fs_info_new();
    task->fs->umask = 0022;

    struct task *saved = current;
    current = task;
    task->fs->root = generic_open("/", O_RDONLY_, 0);
    current = saved;
    if (IS_ERR(task->fs->root))
        return NULL;
    task->fs->pwd = fd_retain(task->fs->root);
    return task;
}

int checkpoint_restore(const char *host_path) {
    FILE *f = fopen(host_path, "rb");
    if (f == NULL)
        return errno_map();

    int err;
    struct ckpt_header h;
    struct task **built = NULL;
    unsigned nbuilt = 0;
    // What the restore builds as it goes: the shared standard streams, the
    // descriptor identity table, and the pipes. See ckpt_restore_task.
    struct ckpt_restore_state st = {0};
    if ((err = rd(f, &h, sizeof(h))) < 0)
        goto out;
    err = _EINVAL;
    if (memcmp(h.magic, CKPT_MAGIC, sizeof(h.magic)) != 0)
        goto out;
    if (h.version != CKPT_VERSION || h.page_size != PAGE_SIZE)
        goto out;
    // The refusal that keeps a byte-copied struct cpu_state honest.
    if (h.build_fingerprint != ckpt_fingerprint() ||
            h.cpu_state_size != sizeof(struct cpu_state))
        goto out;
    if (h.n_tasks == 0 || h.n_tasks > 4096)
        goto out;

    built = calloc(h.n_tasks, sizeof(*built));
    if (built == NULL) { err = _ENOMEM; goto out; }

    struct task *first = current;
    for (uint32_t i = 0; i < h.n_tasks; i++) {
        struct ckpt_task rec;
        if ((err = rd(f, &rec, sizeof(rec))) < 0)
            goto out;
        err = _EINVAL;
        if (rec.cwd_len > MAX_PATH || rec.root_len > MAX_PATH ||
                rec.n_sigactions != NUM_SIGS)
            goto out;

        struct task *task;
        if (i == 0 && rec.pid == first->pid) {
            // The image's first task IS this one: the entry point has already
            // made a pid 1 and it is the process the image calls pid 1.
            task = first;
        } else {
            struct task *parent = NULL;
            for (unsigned j = 0; j < nbuilt; j++)
                if (built[j]->pid == (pid_t_) rec.ppid)
                    parent = built[j];
            // A task whose parent is not in the image was reparented to init
            // between the freeze and the walk. init is where it was going.
            if (parent == NULL)
                parent = first;
            task = ckpt_new_task(parent, (pid_t_) rec.pid);
            if (task == NULL) {
                ckpt_refuse("could not recreate pid %u", rec.pid);
                err = _EAGAIN;
                goto out;
            }
            // Frozen from birth, so nothing runs until every task is built.
            atomic_store_explicit(&task->ckpt_freeze_wanted, true,
                                  memory_order_release);
        }
        built[nbuilt++] = task;

        if (rec.zombie) {
            CKPT_TRACE("load pid %u (ppid %u) %s: ZOMBIE, exit code %#x\n",
                       rec.pid, rec.ppid, rec.comm, rec.exit_code);
            memcpy(task->comm, rec.comm, sizeof(task->comm));
            task->exit_code = rec.exit_code;
            task->zombie = true;
            atomic_store_explicit(&task->ckpt_freeze_wanted, false,
                                  memory_order_release);
            continue;   // no register file, no maps, no descriptors follow
        }

        CKPT_TRACE("load pid %u (ppid %u pgid %u sid %u) %s: %u maps, %u fds\n",
                   rec.pid, rec.ppid, rec.pgid, rec.sid, rec.comm,
                   rec.n_maps, rec.n_fds);
        st.native_name = st.native_argv = st.native_state = st.native_env = NULL;
        struct task *saved = current;
        current = task;
        err = ckpt_restore_task(f, &h, &rec, h.stdio_is_tty != 0, &st);
        current = saved;
        if (err == 0 && rec.native)
            err = ckpt_dispatch_native(task, &st);
        free(st.native_name); free(st.native_argv);
        free(st.native_state); free(st.native_env);
        st.native_name = st.native_argv = st.native_state = st.native_env = NULL;
        if (err < 0)
            goto out;
    }

    // Every task exists and is complete; now let them go. The freezer's own
    // parking lot does the releasing, so a restored task and a checkpointed
    // one wait in exactly the same place.
    atomic_fetch_add_explicit(&ckpt_freeze_active, 1, memory_order_acq_rel);
    for (unsigned i = 1; i < nbuilt; i++) {
        if (built[i]->zombie)
            continue;   // nothing to run; it is a status waiting to be read
        CKPT_TRACE("starting restored pid %d\n", built[i]->pid);
        if (task_start(built[i]) < 0) {
            ckpt_refuse("could not start restored pid %d", built[i]->pid);
            err = _EAGAIN;
            ckpt_thaw_all();
            goto out;
        }
    }
    ckpt_thaw_all();

    lock(&ckpt_lock, 0);
    ckpt_status.restored = true;
    ckpt_status.generation++;
    snprintf(ckpt_status.last_path, sizeof(ckpt_status.last_path), "%s", host_path);
    ckpt_status.pages = (unsigned long) h.total_pages;
    ckpt_status.tasks = h.n_tasks;
    unlock(&ckpt_lock);
    err = 0;

out:
    // The restore's own references; each task holds its own.
    for (unsigned i = 0; i < 3; i++)
        if (st.stdio[i] != NULL)
            fd_close(st.stdio[i]);
    for (uint32_t i = 0; i < st.id_count; i++)
        if (st.by_id[i] != NULL)
            fd_close(st.by_id[i]);
    // Both ends of every pipe: pipe_create_pair hands each over with one
    // reference, and this is where it goes.
    for (uint32_t i = 0; i < st.pipe_count; i++) {
        fd_close(st.pipes[i].rd);
        fd_close(st.pipes[i].wr);
    }
    free(st.by_id);
    free(st.pipes);
    free(built);
    fclose(f);
    return err;
}

// ------------------------------------------------------------ the trigger

void checkpoint_set_session(const char *host_path) {
    lock(&ckpt_lock, 0);
    snprintf(ckpt_session_path, sizeof(ckpt_session_path), "%s",
             host_path != NULL ? host_path : "");
    unlock(&ckpt_lock);
}

const char *checkpoint_session(void) {
    // Read without the lock: it is written once, by the entry point, before
    // any guest task exists.
    return ckpt_session_path;
}

int checkpoint_request(const char *host_path, bool and_halt) {
    // Refuse NOW for anything that would stop the deferred save from
    // happening at all -- see the note in checkpoint.h.
    int err = ckpt_check_scope();
    if (err < 0) {
        lock(&ckpt_lock, 0);
        ckpt_status.last_err = err;
        unlock(&ckpt_lock);
        return err;
    }
    lock(&ckpt_lock, 0);
    snprintf(ckpt_pending_path, sizeof(ckpt_pending_path), "%s", host_path);
    ckpt_pending = true;
    ckpt_pending_halt = and_halt;
    ckpt_status.last_err = 0;
    ckpt_status.last_refusal[0] = '\0';
    unlock(&ckpt_lock);
    return 0;
}

void checkpoint_run_pending(void) {
    char path[PATH_MAX];
    bool halt_after;
    lock(&ckpt_lock, 0);
    bool want = ckpt_pending;
    if (want) {
        memcpy(path, ckpt_pending_path, sizeof(path));
        halt_after = ckpt_pending_halt;
        ckpt_pending = false;
    }
    unlock(&ckpt_lock);
    if (!want)
        return;

    // If the task that asked is ITSELF a native program, it describes itself
    // here. The freezer only asks the others -- this one is not frozen, it is
    // the one doing the freezing -- and its state, like theirs, exists only on
    // its own thread. Reached from native_checkpoint(), which is the only
    // place a native program comes back through.
    const struct native_program *self = native_program_running(current);
    if (self != NULL && self->ckpt_dump != NULL &&
            current->ckpt_native_state == NULL) {
        ckpt_dumping = true;
        current->ckpt_native_state = self->ckpt_dump();
        ckpt_dumping = false;
    }

    int err = checkpoint_save(path);
    if (err < 0) {
        lock(&ckpt_lock, 0);
        ckpt_status.last_err = err;
        CKPT_TRACE("save failed: %d -- %s\n", err,
                   ckpt_status.last_refusal[0] ? ckpt_status.last_refusal
                                               : "(no reason recorded)");
        unlock(&ckpt_lock);
        free(current->ckpt_native_state);
        current->ckpt_native_state = NULL;
        return;
    }
    // Consumed, whichever way it went: it describes a moment that has passed,
    // and leaving it would have the next checkpoint write a stale state.
    free(current->ckpt_native_state);
    current->ckpt_native_state = NULL;

    if (!halt_after)
        return;

    // A SUSPEND: the image is written, so this guest's job is done and the
    // next launch is the one that continues it. Stopping the machine rather
    // than exiting the task -- an exit would run the guest's own shutdown,
    // and the image already describes a process that is very much alive.
    CKPT_TRACE("suspending: image written, halting\n");
    if (halt_hook != NULL)
        halt_hook(0);
    exit(0);
}
