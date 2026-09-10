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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kernel/calls.h"
#include "kernel/checkpoint.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/signal.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/tty.h"
#include "fs/devices.h"
#include "emu/memory.h"
#include "util/sync.h"

#define CKPT_MAGIC "AOKCKPT"
#define CKPT_VERSION 1

// Kinds of descriptor this version knows how to bring back. Anything else is a
// refusal naming the fd number and the filesystem it came from, because "the
// checkpoint failed" is useless and "fd 7 is a pipe" is actionable.
enum ckpt_fd_kind {
    CKPT_FD_FILE = 1,     // regular file: re-open by path, seek to offset
    CKPT_FD_DIR,          // directory: re-open by path
    CKPT_FD_TTY,          // the console: re-attach to this run's tty
    CKPT_FD_STDIO,        // 0/1/2 as the app handed them over: re-attach too
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
    uint32_t n_maps;
    uint32_t n_fds;
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
    uint32_t pid, pgid, sid;
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
    uint32_t reserved;
};

// ------------------------------------------------------------------ status

static lock_t ckpt_lock = LOCK_INITIALIZER;
static struct checkpoint_status ckpt_status;
static char ckpt_pending_path[PATH_MAX];
static bool ckpt_pending;

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
    for (unsigned i = 0; i < snap.count; i++) {
        struct task *t = snap.tasks[i];
        if (t == current)
            continue;
        ckpt_refuse("pid %d (%s) is also running -- v1 checkpoints a "
                    "single-task guest", t->pid, t->comm);
        err = _EBUSY;
        break;
    }
    task_snapshot_release(&snap);
    if (err != 0)
        return err;

    if (current->native_exec != NULL || current->native_cmdline != NULL) {
        ckpt_refuse("pid %d is a native program (%s); a native program is a C "
                    "function on a host thread and its stack cannot be "
                    "serialised", current->pid,
                    current->comm[0] ? current->comm : "?");
        return _EOPNOTSUPP;
    }
    return 0;
}

// One descriptor, gathered under files->lock and described afterwards.
struct ckpt_saved_fd {
    struct fd *fd;
    unsigned num;
    unsigned cloexec;
    int kind;
    uint64_t offset;
    char path[MAX_PATH + 1];
};

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
    if (S_ISCHR(fd->type) || fd->tty != NULL)
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
    if (!S_ISREG(fd->type) && !S_ISDIR(fd->type)) {
        ckpt_refuse("fd %d is a %s on %s with no restore rule",
                    num, S_ISFIFO(fd->type) ? "pipe" :
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

int checkpoint_save(const char *host_path) {
    int err = ckpt_check_scope();
    if (err < 0)
        return err;

    struct mem *mem = current->mem;
    struct mm *mm = current->mm;

    // Classify every descriptor BEFORE opening the output file, so a guest
    // holding something unrestorable gets a refusal and no half-written image.
    // Every descriptor is gathered ONCE, with a reference held, and the table
    // lock is dropped before anything is asked of them. Two reasons, and the
    // second was found the hard way:
    //
    //  - Classifying twice (a pre-flight pass and then the write) meant
    //    describing a table that could have changed in between.
    //  - Asking a descriptor where it is positioned runs the filesystem's
    //    lseek, and on /proc/ish/checkpoint that regenerates the file --
    //    which walks every task's fd table, including this one. Holding
    //    files->lock across it deadlocked the guest against itself.
    struct fdtable *files = current->files;
    struct ckpt_saved_fd *saved;
    unsigned nfds = 0, cap;
    uint32_t stdio_is_tty = 0;

    lock(&files->lock, 0);
    cap = files->size;
    saved = calloc(cap != 0 ? cap : 1, sizeof(*saved));
    if (saved == NULL) {
        unlock(&files->lock);
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
        s->kind = ckpt_classify_fd((int) s->num, s->fd, s->path, sizeof(s->path));
        if (s->kind == 0) {
            for (unsigned j = 0; j < nfds; j++)
                fd_close(saved[j].fd);
            free(saved);
            return _EOPNOTSUPP;
        }
        s->offset = s->kind == CKPT_FD_STDIO ? (uint64_t) s->fd->real_fd
                                             : ckpt_fd_offset(s->fd);
        if (s->num <= 2 && s->kind == CKPT_FD_TTY)
            stdio_is_tty = 1;
    }

    FILE *f = fopen(host_path, "wb");
    if (f == NULL)
        return errno_map();
    struct ckpt_writer w = { .f = f };

    read_lock(&mem->lock);
    struct ckpt_count_ctx counts = {0};
    ckpt_for_each_map(mem, ckpt_count_map, &counts);

    struct ckpt_header h = {
        .version = CKPT_VERSION,
        .abi = (uint32_t) current->abi,
        .cpu_state_size = (uint32_t) sizeof(struct cpu_state),
        .page_size = PAGE_SIZE,
        .build_fingerprint = ckpt_fingerprint(),
        .n_maps = counts.maps,
        .n_fds = nfds,
        .total_pages = counts.pages,
        .stdio_is_tty = stdio_is_tty,
    };
    memcpy(h.magic, CKPT_MAGIC, sizeof(h.magic));
    wr(&w, &h, sizeof(h));

    char cwd[MAX_PATH + 1] = "/", root[MAX_PATH + 1] = "/";
    lock(&current->fs->lock, 0);
    if (current->fs->pwd != NULL)
        generic_getpath(current->fs->pwd, cwd);
    if (current->fs->root != NULL)
        generic_getpath(current->fs->root, root);
    mode_t_ umask = current->fs->umask;
    unlock(&current->fs->lock);

    struct ckpt_task t = {
        .pid = current->pid,
        .pgid = current->group->pgid, .sid = current->group->sid,
        .uid = current->uid, .gid = current->gid,
        .euid = current->euid, .egid = current->egid,
        .suid = current->suid, .sgid = current->sgid,
        .fsuid = current->fsuid, .fsgid = current->fsgid,
        .umask = umask,
        .blocked = current->blocked, .pending = current->pending,
        .altstack = current->altstack, .altstack_size = current->altstack_size,
        .clear_tid = current->clear_tid,
        .brk = mm->brk, .start_brk = mm->start_brk,
        .vdso = mm->vdso, .stack_start = mm->stack_start,
        .argv_start = mm->argv_start, .argv_end = mm->argv_end,
        .env_start = mm->env_start, .env_end = mm->env_end,
        .auxv_start = mm->auxv_start, .auxv_end = mm->auxv_end,
        .n_sigactions = NUM_SIGS,
        .cwd_len = (uint32_t) strlen(cwd),
        .root_len = (uint32_t) strlen(root),
        .page_limit = mem->page_limit,
        .mmap_floor = mem->mmap_floor,
        .mmap_ceiling = mem->mmap_ceiling,
        .stack_top = mem->stack_top,
        .stack_limit_pages = mem->stack_limit_pages,
    };
    memcpy(t.comm, current->comm, sizeof(t.comm));
    wr(&w, &t, sizeof(t));
    wr(&w, cwd, t.cwd_len);
    wr(&w, root, t.root_len);

    // The register file, as bytes. See ckpt_fingerprint for why that is safe
    // and what stops it from being unsafe.
    wr(&w, &current->cpu, sizeof(struct cpu_state));

    lock(&current->sighand->lock, 0);
    wr(&w, current->sighand->action, sizeof(struct sigaction_) * NUM_SIGS);
    unlock(&current->sighand->lock);

    lock(&current->group->lock, 0);
    wr(&w, current->group->limits, sizeof(current->group->limits));
    unlock(&current->group->lock);

    struct ckpt_emit_ctx emit = { .w = &w, .mem = mem };
    ckpt_for_each_map(mem, ckpt_emit_map, &emit);
    read_unlock(&mem->lock);

    for (unsigned i = 0; i < nfds && w.err == 0; i++) {
        struct ckpt_saved_fd *s = &saved[i];
        struct ckpt_fd cf = {
            .fd = s->num,
            .cloexec = s->cloexec,
            .flags = s->fd->flags,
            .kind = (uint32_t) s->kind,
            // For a standard stream the offset is meaningless and the host
            // descriptor it mirrors is what matters, so the field carries
            // that instead. A pipe has no offset to lose.
            .offset = s->offset,
            .path_len = (uint32_t) strlen(s->path),
        };
        CKPT_TRACE("save fd %u %-5s real_fd %d type %o flags %#x off %llu %s\n",
                   cf.fd, ckpt_kind_name(cf.kind), s->fd->real_fd,
                   (unsigned) (s->fd->type >> 12), cf.flags,
                   (unsigned long long) cf.offset, s->path);
        wr(&w, &cf, sizeof(cf));
        wr(&w, s->path, cf.path_len);
    }
    for (unsigned i = 0; i < nfds; i++)
        fd_close(saved[i].fd);
    free(saved);

    err = w.err;
    if (fclose(f) != 0 && err == 0)
        err = errno_map();
    if (err != 0) {
        unlink(host_path);
        return err;
    }

    lock(&ckpt_lock, 0);
    ckpt_status.saves++;
    ckpt_status.last_err = 0;
    ckpt_status.last_refusal[0] = '\0';
    snprintf(ckpt_status.last_path, sizeof(ckpt_status.last_path), "%s", host_path);
    ckpt_status.pages = (unsigned long) counts.pages;
    ckpt_status.fds = nfds;
    ckpt_status.bytes = (unsigned long long) counts.pages * PAGE_SIZE;
    unlock(&ckpt_lock);
    return 0;
}

// ------------------------------------------------------------------ reading

int checkpoint_restore(const char *host_path) {
    FILE *f = fopen(host_path, "rb");
    if (f == NULL)
        return errno_map();

    int err;
    struct ckpt_header h;
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

    struct ckpt_task t;
    if ((err = rd(f, &t, sizeof(t))) < 0)
        goto out;
    err = _EINVAL;
    if (t.cwd_len > MAX_PATH || t.root_len > MAX_PATH || t.n_sigactions != NUM_SIGS)
        goto out;
    char cwd[MAX_PATH + 1] = {0}, root[MAX_PATH + 1] = {0};
    if ((err = rd(f, cwd, t.cwd_len)) < 0) goto out;
    if ((err = rd(f, root, t.root_len)) < 0) goto out;

    struct cpu_state cpu;
    if ((err = rd(f, &cpu, sizeof(cpu))) < 0)
        goto out;

    struct sigaction_ actions[NUM_SIGS];
    if ((err = rd(f, actions, sizeof(actions))) < 0)
        goto out;
    rlim_t_ limits[sizeof(current->group->limits) / sizeof(current->group->limits[0])][2];
    if ((err = rd(f, limits, sizeof(limits))) < 0)
        goto out;

    // The address space. current already has a fresh one from
    // become_first_process; every mapping in the image is added to it, and the
    // bytes go in through the ordinary write path so the pager, the JIT's
    // invalidation and the accounting all see them the way they see a guest's
    // own writes.
    // The guest architecture, and with it the address space's shape. A fresh
    // init's mm is built for the entry point's default; the image says what
    // the checkpointed guest actually was, and every mapping below depends on
    // it. Set before a single page is mapped.
    current->abi = (enum guest_abi) h.abi;
    struct mem *mem = current->mem;
    struct mm *mm = current->mm;
    mem_set_page_limit(mem, (page_t) t.page_limit);
    mem_set_mmap_window(mem, (page_t) t.mmap_floor, (page_t) t.mmap_ceiling);
    mem_set_stack_bounds(mem, (page_t) t.stack_top,
                         (uint64_t) t.stack_limit_pages << PAGE_BITS);

    for (uint32_t i = 0; i < h.n_maps; i++) {
        struct ckpt_map m;
        if ((err = rd(f, &m, sizeof(m))) < 0)
            goto out;
        page_t start = (page_t) (m.start >> PAGE_BITS);
        write_lock(&mem->lock);
        // A fresh init's mm is not empty -- mm_new maps the vdso -- and the
        // image is the complete truth about this address space, so anything
        // already sitting where a mapping goes is replaced rather than
        // collided with.
        pt_unmap_always(mem, start, (pages_t) m.pages);
        // Mapped WRITABLE regardless of the saved protection, then set to the
        // saved flags once the bytes are in: a PROT_NONE guard page or a
        // read-only text segment cannot be filled through mem_ptr otherwise.
        err = pt_map_nothing(mem, start, (pages_t) m.pages,
                             P_READ | P_WRITE | P_ANONYMOUS);
        write_unlock(&mem->lock);
        if (err < 0)
            goto out;
        for (uint64_t p = 0; p < m.pages; p++) {
            guest_addr_t addr = ((guest_addr_t) (start + p)) << PAGE_BITS;
            write_lock(&mem->lock);
            char *dst = mem_ptr(mem, addr, MEM_WRITE);
            write_unlock(&mem->lock);
            if (dst == NULL) {
                err = _EFAULT;
                goto out;
            }
            if ((err = rd(f, dst, PAGE_SIZE)) < 0)
                goto out;
        }
        write_lock(&mem->lock);
        err = pt_set_flags(mem, start, (pages_t) m.pages, (int) m.flags);
        write_unlock(&mem->lock);
        if (err < 0)
            goto out;
    }

    mm->brk = t.brk;
    mm->start_brk = t.start_brk;
    mm->vdso = t.vdso;
    mm->stack_start = t.stack_start;
    mm->argv_start = t.argv_start; mm->argv_end = t.argv_end;
    mm->env_start = t.env_start; mm->env_end = t.env_end;
    mm->auxv_start = t.auxv_start; mm->auxv_end = t.auxv_end;

    // The descriptors. Everything the fresh init opened for itself goes first:
    // the image is the complete truth about what this guest had open.
    struct fdtable *files = current->files;
    lock(&files->lock, 0);
    for (unsigned i = 0; i < files->size; i++) {
        if (files->files[i] != NULL) {
            fd_close(files->files[i]);
            files->files[i] = NULL;
        }
    }
    unlock(&files->lock);

    // The standard streams, set up the way the entry point sets them up at
    // boot rather than reconstructed. Done BEFORE the descriptor loop so a
    // record for 0, 1 or 2 finds them already there and leaves them alone.
    if (h.stdio_is_tty)
        create_stdio("/dev/tty1", TTY_CONSOLE_MAJOR, 1);
    else
        create_piped_stdio();
    // Held aside, because the loop below can REPLACE what is at 0, 1 or 2. A
    // shell in the middle of `> file` has the redirection live on fd 1 and its
    // real stdout parked on fd 10, so the image says exactly that -- and
    // mirroring fd 10 from "whatever is at slot 1 now" gave it the redirection
    // instead of the terminal. `echo` in the restored shell answered EIO.
    // RETAINED, not just pointed at. Installing the image's fd 1 detaches
    // and CLOSES whatever was in that slot, and with a refcount of one that
    // frees it -- so mirroring fd 10 from the saved pointer afterwards was a
    // use-after-free, which surfaced as `echo: I/O error` in the restored
    // shell rather than as a crash. Released at the end of the loop.
    struct fd *stdio[3];
    lock(&files->lock, 0);
    for (unsigned i = 0; i < 3; i++) {
        stdio[i] = i < files->size ? files->files[i] : NULL;
        if (stdio[i] != NULL)
            fd_retain(stdio[i]);
    }
    unlock(&files->lock);

    for (uint32_t i = 0; i < h.n_fds; i++) {
        struct ckpt_fd cf;
        if ((err = rd(f, &cf, sizeof(cf))) < 0)
            goto out;
        char path[MAX_PATH + 1] = {0};
        if (cf.path_len > MAX_PATH) { err = _EINVAL; goto out; }
        if ((err = rd(f, path, cf.path_len)) < 0)
            goto out;

        // Re-attached, not restored: the terminal or the host pipe this guest
        // was talking to went with the process that owned it. sockrestart is
        // the precedent -- record enough to REBUILD, because the original is
        // destroyed either way. 0, 1 and 2 were set up above; anywhere else
        // the same stream is another reference to one of those three.
        CKPT_TRACE("load fd %u %-5s flags %#x off %llu %s\n",
                   cf.fd, ckpt_kind_name(cf.kind), cf.flags,
                   (unsigned long long) cf.offset, path);
        if (cf.kind == CKPT_FD_STDIO || cf.kind == CKPT_FD_TTY) {
            if (cf.fd <= 2)
                continue;
            unsigned mirror = cf.kind == CKPT_FD_STDIO ?
                    (unsigned) cf.offset : 0;
            if (mirror > 2)
                mirror = 0;
            struct fd *src = stdio[mirror];
            if (src != NULL)
                fd_retain(src);
            if (src != NULL &&
                    (err = fdtable_install_at(files, (fd_t) cf.fd, src,
                                              cf.cloexec != 0)) < 0)
                goto out;
            continue;
        }

        struct fd *fd;
        if (cf.kind == CKPT_FD_TTY) {
            fd = generic_open("/dev/tty1", O_RDWR_, 0);
            if (IS_ERR(fd))
                fd = generic_open("/dev/null", O_RDWR_, 0);
        } else {
            fd = generic_open(path, (int) cf.flags, 0);
        }
        if (IS_ERR(fd)) {
            err = (int) PTR_ERR(fd);
            goto out;
        }
        if (cf.kind == CKPT_FD_FILE && fd->ops->lseek != NULL)
            fd->ops->lseek(fd, (off_t_) cf.offset, LSEEK_SET);
        fd->offset = cf.offset;
        // A fresh init's table holds three descriptors; the image may name
        // fd 10, because a shell parks its saved stdin up there. Grow to fit
        // rather than refuse -- the number is part of what is restored.
        err = fdtable_install_at(files, (fd_t) cf.fd, fd, cf.cloexec != 0);
        if (err < 0)
            goto out;
    }

    for (unsigned i = 0; i < 3; i++)
        if (stdio[i] != NULL)
            fd_close(stdio[i]);

    // Credentials, identity and the rest of the task.
    current->uid = t.uid; current->gid = t.gid;
    current->euid = t.euid; current->egid = t.egid;
    current->suid = t.suid; current->sgid = t.sgid;
    current->fsuid = t.fsuid; current->fsgid = t.fsgid;
    memcpy(current->comm, t.comm, sizeof(current->comm));
    current->blocked = t.blocked;
    current->pending = t.pending;
    current->altstack = t.altstack;
    current->altstack_size = t.altstack_size;
    current->clear_tid = t.clear_tid;

    lock(&current->sighand->lock, 0);
    memcpy(current->sighand->action, actions, sizeof(actions));
    unlock(&current->sighand->lock);
    lock(&current->group->lock, 0);
    memcpy(current->group->limits, limits, sizeof(limits));
    unlock(&current->group->lock);

    lock(&current->fs->lock, 0);
    current->fs->umask = t.umask;
    unlock(&current->fs->lock);
    if (cwd[0] == '/') {
        struct fd *pwd = generic_open(cwd, O_RDONLY_, 0);
        if (!IS_ERR(pwd))
            fs_chdir(current->fs, pwd);
    }

    // The register file last, so nothing above can have run guest code with a
    // half-restored one.
    struct mmu *mmu = current->cpu.mmu;
    bool *poked = current->cpu.poked_ptr;
    current->cpu = cpu;
    // The two pointers in struct cpu_state name host objects that belong to
    // THIS run: the address space's mmu, and the flag the wake path sets to
    // break out of guest execution. Everything else in there is guest value
    // state; these two are re-attached rather than restored.
    current->cpu.mmu = mmu;
    current->cpu.poked_ptr = poked;

    lock(&ckpt_lock, 0);
    ckpt_status.restored = true;
    ckpt_status.generation++;
    snprintf(ckpt_status.last_path, sizeof(ckpt_status.last_path), "%s", host_path);
    ckpt_status.pages = (unsigned long) h.total_pages;
    ckpt_status.fds = h.n_fds;
    unlock(&ckpt_lock);
    err = 0;

out:
    fclose(f);
    return err;
}

// ------------------------------------------------------------ the trigger

int checkpoint_request(const char *host_path) {
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
    ckpt_status.last_err = 0;
    ckpt_status.last_refusal[0] = '\0';
    unlock(&ckpt_lock);
    return 0;
}

void checkpoint_run_pending(void) {
    char path[PATH_MAX];
    lock(&ckpt_lock, 0);
    bool want = ckpt_pending;
    if (want) {
        memcpy(path, ckpt_pending_path, sizeof(path));
        ckpt_pending = false;
    }
    unlock(&ckpt_lock);
    if (!want)
        return;

    int err = checkpoint_save(path);
    if (err < 0) {
        lock(&ckpt_lock, 0);
        ckpt_status.last_err = err;
        unlock(&ckpt_lock);
    }
}
