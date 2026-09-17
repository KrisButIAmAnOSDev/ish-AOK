// SysV shm: a segment marked IPC_RMID stays attachable for as long as it exists.
//
// shmat refused a segment already marked IPC_RMID with EINVAL even while it
// was still attached, and so still existed. Linux lets anyone with access
// attach it until the last detach destroys it -- shmctl(2) documents that as
// Linux-specific -- and MIT-SHM depends on it: a client creates a segment,
// attaches it, marks it IPC_RMID so a crash cannot leak it, and only then asks
// the X server to attach it by id. The server's shmat failed, and the client
// got BadAccess on X_ShmAttach.
//
// IPC_STAT on such a segment differed too: Linux reports the key as
// IPC_PRIVATE and sets SHM_DEST in the mode; AOK kept the old key and mode.
//
// Case [8] races an attach against the last detach. It accepts every outcome
// Linux can produce, and checks that the segment is gone afterwards, which is
// what catches an attach that loses count of itself.
//
// Checked against Linux 6.12 (x86_64 glibc, 64-bit and -m32) as uid 1000. The
// root-only half of [5] drops to uid 1000 in a child, so it tests the same
// rule when the suite runs as root.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>

#include "test_common.h"

#define SHM_DEST_BIT 01000
#define UNPRIV_UID 1000
#define UNPRIV_GID 1000

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-60s got=%-8ld want=%ld\n", label, got, want);
}

// 0 on success, else errno.
static long err_of(int r) {
    return r == 0 ? 0 : errno;
}

static void *attach(int id, const void *addr, int flags, long *err) {
    errno = 0;
    void *p = shmat(id, addr, flags);
    *err = p == (void *) -1 ? errno : 0;
    return p == (void *) -1 ? NULL : p;
}

struct stat_view {
    long err;
    long key;
    long mode;
    long nattch;
};

static struct stat_view stat_of(int id) {
    struct shmid_ds ds;
    memset(&ds, 0, sizeof ds);
    struct stat_view v = { 0 };
    errno = 0;
    if (shmctl(id, IPC_STAT, &ds) != 0) {
        v.err = errno;
        return v;
    }
#ifdef __GLIBC__
    v.key = (long) (unsigned) ds.shm_perm.__key;
#else
    v.key = (long) (unsigned) ds.shm_perm.__ipc_perm_key;
#endif
    v.mode = (long) ds.shm_perm.mode;
    v.nattch = (long) ds.shm_nattch;
    return v;
}

static int child_status_failures(pid_t pid) {
    int st;
    if (waitpid(pid, &st, 0) != pid) {
        printf("FAIL waitpid: %s\n", strerror(errno));
        return 1;
    }
    if (WIFSIGNALED(st)) {
        printf("FAIL child died on signal %d\n", WTERMSIG(st));
        return 1;
    }
    return WEXITSTATUS(st);
}

struct race {
    int id;
    int yields;
    pthread_barrier_t *barrier;
    long err;
    int bytes_ok;
};

static void *race_attach(void *arg) {
    struct race *r = arg;
    pthread_barrier_wait(r->barrier);
    // Stagger the attach against the detach, so both orders get exercised.
    for (int i = 0; i < r->yields; i++)
        sched_yield();
    void *p = attach(r->id, NULL, 0, &r->err);
    if (p != NULL) {
        r->bytes_ok = ((unsigned char *) p)[0] == 0x5a && ((unsigned char *) p)[4095] == 0x5a;
        shmdt(p);
    }
    return NULL;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(180));
    setvbuf(stdout, NULL, _IONBF, 0);

    key_t key = 0;
    int id = -1;
    for (int i = 0; i < 64 && id < 0; i++) {
        key = (key_t) (0x41000000 | (((unsigned) getpid() + (unsigned) i) & 0xffffff));
        id = shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0600);
        if (id < 0 && errno != EEXIST)
            break;
    }
    if (id < 0) {
        printf("FAIL shmget: %s\n", strerror(errno));
        return 1;
    }

    // The second process: forked before any attach, so it holds none of its
    // own -- the X server's position. It waits for [2].
    int go[2];
    if (pipe(go) != 0) {
        printf("FAIL pipe: %s\n", strerror(errno));
        return 1;
    }
    fflush(NULL);
    pid_t peer = fork();
    if (peer == 0) {
        close(go[1]);
        char c;
        if (read(go[0], &c, 1) != 1)
            _exit(1);
        failures_total = 0;
        long e;
        unsigned char *q = attach(id, NULL, 0, &e);
        ck("[2] another process attaches it by id", e, 0);
        if (q != NULL) {
            ck("[2]   and sees the creator's bytes", q[0], 'P');
            q[2] = 'C';
            struct stat_view v = stat_of(id);
            ck("[2]   IPC_STAT counts three attaches", v.nattch, 3);
            ck("[2]   shmdt", err_of(shmdt(q)), 0);
        }
        _exit(failures_total > 250 ? 250 : (int) failures_total);
    }
    close(go[0]);

    test_logf("[1] marked IPC_RMID while attached, it can still be attached\n");
    long e;
    unsigned char *p = attach(id, NULL, 0, &e);
    ck("[1] first attach", e, 0);
    if (p == NULL)
        return finish_suite("shm_rmid_attach");
    p[0] = 'P';
    ck("[1] IPC_RMID", err_of(shmctl(id, IPC_RMID, NULL)), 0);
    struct stat_view v = stat_of(id);
    ck("[1] IPC_STAT still works", v.err, 0);
    ck("[1]   nattch", v.nattch, 1);
    ck("[1]   the key reads back as IPC_PRIVATE", v.key, IPC_PRIVATE);
    ck("[1]   the mode carries SHM_DEST", v.mode & SHM_DEST_BIT, SHM_DEST_BIT);
    ck("[1]   and keeps its permission bits", v.mode & 0777, 0600);
    errno = 0;
    ck("[1] shmget by the old key finds nothing",
       shmget(key, 4096, 0600) < 0 ? errno : 0, ENOENT);

    unsigned char *p2 = attach(id, NULL, 0, &e);
    ck("[1] a second attach by the creator", e, 0);
    if (p2 != NULL) {
        ck("[1]   maps the same memory", p2[0], 'P');
        p2[1] = 'Q';
        ck("[1]   in both directions", p[1], 'Q');
    }
    unsigned char *p3 = attach(id, NULL, SHM_RDONLY, &e);
    ck("[1] a SHM_RDONLY attach", e, 0);
    ck("[1]   IPC_STAT counts three attaches", stat_of(id).nattch, 3);
    if (p3 != NULL)
        ck("[1]   shmdt the read-only one", err_of(shmdt(p3)), 0);
    ck("[1] a misaligned attach still fails with EINVAL",
       (attach(id, (void *) 0x10001, 0, &e), e), EINVAL);
    ck("[1]   and is not counted", stat_of(id).nattch, 2);

    test_logf("[2] another process attaches it\n");
    if (write(go[1], "g", 1) != 1)
        failures_total++;
    close(go[1]);
    failures_total += (unsigned) child_status_failures(peer);
    ck("[2] the creator sees the other process's write", p[2], 'C');
    ck("[2] and its detach is counted", stat_of(id).nattch, 2);

    test_logf("[3] a forked child inherits both attaches and its exit drops them\n");
    fflush(NULL);
    pid_t kid = fork();
    if (kid == 0) {
        failures_total = 0;
        ck("[3] the child counts four attaches", stat_of(id).nattch, 4);
        _exit(failures_total > 250 ? 250 : (int) failures_total);   // no shmdt
    }
    failures_total += (unsigned) child_status_failures(kid);
    ck("[3] after it exits the count is two again", stat_of(id).nattch, 2);

    test_logf("[4] the key is free for a new segment\n");
    int id2 = shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0600);
    ck("[4] shmget(key, IPC_CREAT | IPC_EXCL)", id2 >= 0 ? 0 : errno, 0);
    if (id2 >= 0) {
        ck("[4]   is a different segment", id2 != id, 1);
        struct stat_view v2 = stat_of(id2);
        ck("[4]   which has the key", v2.key, (long) (unsigned) key);
        ck("[4]   and no SHM_DEST", v2.mode & SHM_DEST_BIT, 0);
        ck("[4]   removing it unattached", err_of(shmctl(id2, IPC_RMID, NULL)), 0);
        ck("[4]   destroys it at once", (attach(id2, NULL, 0, &e), e), EINVAL);
    }

    test_logf("[5] attaching after IPC_RMID still needs permission\n");
    if (geteuid() == 0) {
        fflush(NULL);
        pid_t u = fork();
        if (u == 0) {
            failures_total = 0;
            if (setgid(UNPRIV_GID) != 0 || setuid(UNPRIV_UID) != 0) {
                printf("FAIL could not drop to uid %d: %s\n", UNPRIV_UID, strerror(errno));
                _exit(1);
            }
            ck("[5] uid 1000 attaching root's 0600 segment",
               (attach(id, NULL, 0, &e), e), EACCES);
            ck("[5]   read-only too", (attach(id, NULL, SHM_RDONLY, &e), e), EACCES);
            _exit(failures_total > 250 ? 250 : (int) failures_total);
        }
        failures_total += (unsigned) child_status_failures(u);
    } else {
        struct shmid_ds ds;
        memset(&ds, 0, sizeof ds);
        ck("[5] IPC_STAT", err_of(shmctl(id, IPC_STAT, &ds)), 0);
        ds.shm_perm.mode = 0400;
        ck("[5] IPC_SET mode 0400 after IPC_RMID", err_of(shmctl(id, IPC_SET, &ds)), 0);
        ck("[5]   SHM_DEST survives it", stat_of(id).mode, SHM_DEST_BIT | 0400);
        ck("[5]   a read-write attach is refused", (attach(id, NULL, 0, &e), e), EACCES);
        unsigned char *ro = attach(id, NULL, SHM_RDONLY, &e);
        ck("[5]   a read-only attach is allowed", e, 0);
        if (ro != NULL)
            shmdt(ro);
        ds.shm_perm.mode = 0600;
        ck("[5]   IPC_SET mode back to 0600", err_of(shmctl(id, IPC_SET, &ds)), 0);
    }
    ck("[5] refusals are not counted", stat_of(id).nattch, 2);

    test_logf("[6] the last detach destroys it\n");
    if (p2 != NULL)
        ck("[6] shmdt the second attach", err_of(shmdt(p2)), 0);
    ck("[6]   one attach left", stat_of(id).nattch, 1);
    ck("[6] shmdt the last attach", err_of(shmdt(p)), 0);
    ck("[6]   IPC_STAT now fails", stat_of(id).err, EINVAL);
    ck("[6]   and so does shmat", (attach(id, NULL, 0, &e), e), EINVAL);

    test_logf("[7] a segment nobody has attached is destroyed by IPC_RMID at once\n");
    int id3 = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    ck("[7] shmget(IPC_PRIVATE)", id3 >= 0 ? 0 : errno, 0);
    ck("[7] IPC_RMID", err_of(shmctl(id3, IPC_RMID, NULL)), 0);
    ck("[7]   shmat", (attach(id3, NULL, 0, &e), e), EINVAL);
    ck("[7]   IPC_STAT", stat_of(id3).err, EINVAL);

    test_logf("[8] an attach racing the last detach\n");
    {
        int attached = 0, refused = 0, bad = 0, leaked = 0;
        pthread_barrier_t barrier;
        pthread_barrier_init(&barrier, NULL, 2);
        for (int i = 0; i < 200; i++) {
            int rid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
            if (rid < 0) {
                bad++;
                break;
            }
            unsigned char *a = attach(rid, NULL, 0, &e);
            if (a == NULL) {
                bad++;
                shmctl(rid, IPC_RMID, NULL);
                break;
            }
            memset(a, 0x5a, 4096);
            shmctl(rid, IPC_RMID, NULL);
            struct race r = { .id = rid, .yields = i % 8, .barrier = &barrier, .err = -1 };
            pthread_t t;
            if (pthread_create(&t, NULL, race_attach, &r) != 0) {
                shmdt(a);
                bad++;
                break;
            }
            pthread_barrier_wait(&barrier);
            shmdt(a);
            pthread_join(t, NULL);
            if (r.err == 0 && r.bytes_ok)
                attached++;
            else if (r.err == EINVAL || r.err == EIDRM)
                refused++;
            else {
                if (bad == 0)
                    printf("FAIL [8] iteration %d: attach err=%ld bytes_ok=%d\n", i, r.err, r.bytes_ok);
                bad++;
            }
            // Both attaches are gone, so the segment must be too.
            if (stat_of(rid).err != EINVAL) {
                if (leaked == 0)
                    printf("FAIL [8] iteration %d: segment %d outlived its last detach (nattch %ld)\n",
                           i, rid, stat_of(rid).nattch);
                leaked++;
            }
        }
        pthread_barrier_destroy(&barrier);
        test_logf("    attached %d, refused %d\n", attached, refused);
        ck("[8] every attach either mapped the segment or was refused cleanly", bad, 0);
        ck("[8] no segment outlived its last detach", leaked, 0);
    }

    return finish_suite("shm_rmid_attach");
}
