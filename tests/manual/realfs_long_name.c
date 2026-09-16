// A host directory entry whose name is longer than 255 bytes must be listed
// whole, exactly once, and be usable by the name it was listed under.
//
// The host limit is not NAME_MAX. APFS allows 255 UTF-16 units per name, so
// a non-ASCII name legitimately runs to 765 bytes; exFAT and SMB volumes are
// the same. realfs_readdir strcpy'd the host's d_name into a 256-byte
// dir_entry.name on sys_getdents_common's stack, so `ls` of a real mount
// holding such a file failed __stack_chk_fail and took the whole emulator
// down, every guest process with it.
//
// Linux lists names like that: fs/readdir.c refuses only a name of PATH_MAX
// or more (or one holding a '/'), so exFAT, ntfs3, CIFS and FUSE mounts show
// them. Leaving them out instead would make files silently vanish from ls,
// find, tar and cp -a, and rm -rf of their directory fail with ENOTEMPTY; the
// last checks below are that case. Cutting a name short is no better: 255
// bytes of the 765-byte name below is exactly the 255-byte name next to it.
//
// The names are created from inside the guest, through the mount: realfs
// hands a create straight to the host, which accepts them. Where the host
// refuses them there is nothing to test and the test skips. Needs root to
// mount; skips otherwise, and on real Linux, which has no "real" filesystem.
//
// The host directory is ISH_TEST_REALFS_HOST_DIR if set, else the app's
// Documents directory from /proc/ish/documents, else the host's /tmp (the
// command-line build). Everything is created in a per-pid subdirectory and
// removed again.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "test_common.h"

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

// U+4E2D, three bytes in UTF-8 and one UTF-16 unit; U+00E9, two bytes.
static const char zh[] = "\xe4\xb8\xad";
static const char eacute[] = "\xc3\xa9";

static void repeat(char *out, const char *prefix, const char *unit, int count) {
    strcpy(out, prefix);
    for (int i = 0; i < count; i++)
        strcat(out, unit);
}

enum { N_SHORT, N_A255, N_ZH255, N_B256, N_E400, N_ZH765, N_DIR, N_COUNT };
static char names[N_COUNT][1024];
static int created[N_COUNT];
static const char *labels[N_COUNT] = {"short", "a*255", "zh*85 (255 bytes)",
    "b+zh*85 (256 bytes)", "e-acute*200 (400 bytes)", "zh*255 (765 bytes)",
    "directory d+zh*200 (601 bytes)"};

// Inside the N_DIR directory: a 361-byte name, so a path through two long
// components is walked, listed and removed too. It is kept short enough that
// the whole path below the mount stays under Darwin's 1024-byte path limit,
// which is a separate limit from the one tested here.
static char inner[1024];

// The name a long file is renamed to and back: 'r' in place of its first
// character, so it has as many UTF-16 units as the original.
static void renamed(char *out, size_t size, const char *work, int i) {
    const char *rest = names[i] + (names[i][0] == '\xe4' ? 3 : names[i][0] == '\xc3' ? 2 : 1);
    snprintf(out, size, "%s/r%s", work, rest);
}

// Tally one directory entry: which expected name it is, or an unexpected one.
// The name is passed as a plain pointer: a record's name runs past the 256
// bytes struct dirent declares, into the rest of the libc's buffer.
static void tally(const char *name, unsigned char type, int seen[N_COUNT], int *dots, const char *how) {
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        (*dots)++;
        return;
    }
    for (int i = 0; i < N_COUNT; i++) {
        if (strcmp(name, names[i]) == 0) {
            seen[i]++;
            unsigned char want = i == N_DIR ? DT_DIR : DT_REG;
            check(type == want || type == DT_UNKNOWN, "%s: %s has d_type %d, want %d",
                    how, labels[i], type, want);
            return;
        }
    }
    check(0, "%s: unexpected entry of %zu bytes (a truncated long name?)", how, strlen(name));
}

static void verify(const int seen[N_COUNT], int dots, const char *how) {
    check(dots == 2, "%s: . and .. listed once each (got %d)", how, dots);
    for (int i = 0; i < N_COUNT; i++) {
        int want = created[i] ? 1 : 0;
        check(seen[i] == want, "%s: %s listed %d time(s), want %d", how, labels[i], seen[i], want);
    }
}

static void list_readdir(const char *work) {
    int seen[N_COUNT] = {0}, dots = 0;
    DIR *d = opendir(work);
    if (d == NULL) {
        check(0, "opendir (%s)", strerror(errno));
        return;
    }
    struct dirent *ent;
    errno = 0;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        tally(name, ent->d_type, seen, &dots, "readdir");
    }
    int err = errno;
    check(err == 0, "readdir ended without an error (%s)", strerror(err));
    closedir(d);
    verify(seen, dots, "readdir");
}

static unsigned short record_reclen(const char *rec) {
    unsigned short reclen;
    memcpy(&reclen, rec + 16, sizeof(reclen)); // ino, off, then d_reclen
    return reclen;
}

// getdents64 with a buffer too small for most of these records. Two edges of
// the syscall run:
// - A record that does not fit after others were stored ends the call, and
//   the next call must start with that record, not the one after it.
// - A record that does not fit an empty buffer is EINVAL, as on Linux, and
//   must not move the position: the call is retried with room for it.
static void list_getdents(const char *work) {
    int seen[N_COUNT] = {0}, dots = 0;
    int dfd = open(work, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) {
        check(0, "open directory (%s)", strerror(errno));
        return;
    }
    enum { SMALL = 288, LARGE = 1024 };
    char buf[LARGE];
    int calls = 0, einval = 0, stopped_short = 0;
    size_t size = SMALL;
    long last_n = 0;
    size_t last_size = 0;
    for (;;) {
        long n = syscall(SYS_getdents64, dfd, buf, size);
        if (n < 0 && errno == EINVAL && size == SMALL) {
            einval++;
            size = LARGE;
            continue;
        }
        if (n <= 0) {
            if (n < 0)
                check(0, "getdents64 with a %zu-byte buffer (%s)", size, strerror(errno));
            break;
        }
        // The previous call stored something and stopped before this record,
        // because it did not fit the room left: the stop-and-return-it-next
        // case really ran.
        if (last_n > 0 && record_reclen(buf) > last_size - (size_t) last_n)
            stopped_short++;
        calls++;
        for (long off = 0; off < n;) {
            unsigned short reclen = record_reclen(buf + off);
            unsigned char type = (unsigned char) buf[off + 18];
            check(reclen >= 20 && off + reclen <= n, "getdents64: record at %ld has a sane d_reclen %u", off, reclen);
            if (reclen < 20 || off + reclen > n)
                break;
            tally(buf + off + 19, type, seen, &dots, "getdents64");
            off += reclen;
        }
        last_n = n;
        last_size = size;
        size = SMALL;
        if (calls > 64) {
            check(0, "getdents64 did not reach the end in 64 calls");
            break;
        }
    }
    close(dfd);
    test_logf("getdents64 took %d calls, %d EINVAL retries, %d short stops\n", calls, einval, stopped_short);
    verify(seen, dots, "getdents64");
    if (created[N_ZH765])
        check(einval > 0, "getdents64: a record larger than the buffer was refused with EINVAL (%d)", einval);
    check(stopped_short > 0, "getdents64: a call stopped at a record that did not fit (%d)", stopped_short);
}

// Remove a directory tree using only names a readdir walk returns, the way
// rm -rf does. Names are collected first so no entry is removed while the
// directory is being read.
static int remove_tree(const char *path, int depth) {
    DIR *d = opendir(path);
    if (d == NULL) {
        check(0, "remove walk: opendir at depth %d (%s)", depth, strerror(errno));
        return -1;
    }
    char *found[64];
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < 64) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        found[count++] = strdup(name);
    }
    closedir(d);
    int err = 0;
    for (int i = 0; i < count; i++) {
        size_t len = strlen(path) + 1 + strlen(found[i]) + 1;
        char *child = malloc(len);
        snprintf(child, len, "%s/%s", path, found[i]);
        struct stat st;
        if (lstat(child, &st) != 0) {
            check(0, "remove walk: lstat of a listed %zu-byte name (%s)", strlen(found[i]), strerror(errno));
            err = -1;
        } else if (S_ISDIR(st.st_mode)) {
            if (depth < 4 && remove_tree(child, depth + 1) != 0)
                err = -1;
        } else if (unlink(child) != 0) {
            check(0, "remove walk: unlink of a listed %zu-byte name (%s)", strlen(found[i]), strerror(errno));
            err = -1;
        }
        free(child);
        free(found[i]);
    }
    if (rmdir(path) != 0) {
        check(0, "remove walk: rmdir at depth %d after unlinking what readdir listed (%s)", depth, strerror(errno));
        err = -1;
    }
    return err;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));

    if (geteuid() != 0) {
        printf("realfs_long_name: SKIP (needs root to mount)\n");
        return 0;
    }

    char host[PATH_MAX] = "/tmp";
    const char *env = getenv("ISH_TEST_REALFS_HOST_DIR");
    if (env != NULL && env[0] != '\0') {
        snprintf(host, sizeof(host), "%s", env);
    } else {
        FILE *f = fopen("/proc/ish/documents", "r");
        char line[PATH_MAX];
        if (f != NULL && fgets(line, sizeof(line), f) != NULL && line[0] == '/') {
            line[strcspn(line, "\n")] = '\0';
            snprintf(host, sizeof(host), "%s", line);
        }
        if (f != NULL)
            fclose(f);
    }

    strcpy(names[N_SHORT], "short");
    memset(names[N_A255], 'a', 255);
    repeat(names[N_ZH255], "", zh, 85);
    repeat(names[N_B256], "b", zh, 85);
    repeat(names[N_E400], "", eacute, 200);
    repeat(names[N_ZH765], "", zh, 255);
    repeat(names[N_DIR], "d", zh, 200);
    repeat(inner, "i", zh, 120);

    // A run that took the emulator down left its mount point and host files
    // behind, and guest pids repeat from boot to boot; clear them rather than
    // skipping on EEXIST.
    char mnt[64], work[128];
    static char path[16384], path2[16384];
    snprintf(mnt, sizeof(mnt), "/tmp/realfs_long_name.%d", (int) getpid());
    rmdir(mnt);
    if (mkdir(mnt, 0755) != 0) {
        printf("realfs_long_name: SKIP (cannot create %s: %s)\n", mnt, strerror(errno));
        return 0;
    }
    if (mount(host, mnt, "real", 0, NULL) != 0) {
        printf("realfs_long_name: SKIP (cannot mount host %s as real: %s)\n", host, strerror(errno));
        rmdir(mnt);
        return 0;
    }
    test_logf("mounted host %s at %s\n", host, mnt);

    snprintf(work, sizeof(work), "%s/aok-realfs-long-name.%d", mnt, (int) getpid());
    snprintf(path, sizeof(path), "%s/%s/%s", work, names[N_DIR], inner);
    unlink(path);
    for (int i = 0; i < N_COUNT; i++) {
        snprintf(path, sizeof(path), "%s/%s", work, names[i]);
        if (i == N_DIR) {
            rmdir(path);
        } else {
            unlink(path);
            renamed(path, sizeof(path), work, i);
            unlink(path);
        }
    }
    rmdir(work);
    if (mkdir(work, 0755) != 0) {
        int err = errno;
        umount(mnt);
        rmdir(mnt);
        if (err == EEXIST) {
            printf("realfs_long_name: FAIL (%s is left over from an earlier run and could not be removed)\n", work);
            return 1;
        }
        printf("realfs_long_name: SKIP (cannot create %s: %s)\n", work, strerror(err));
        return 0;
    }

    int longs_created = 0;
    for (int i = 0; i < N_COUNT; i++) {
        snprintf(path, sizeof(path), "%s/%s", work, names[i]);
        int ok;
        if (i == N_DIR) {
            ok = mkdir(path, 0755) == 0;
        } else {
            int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
            ok = fd >= 0;
            if (ok) {
                ssize_t n = write(fd, names[i], strlen(names[i]));
                check(n == (ssize_t) strlen(names[i]), "write %s (%zd, %s)", labels[i], n, strerror(errno));
                close(fd);
            }
        }
        if (ok) {
            created[i] = 1;
            // The host really holds it: a lookup by the full name finds it.
            struct stat st;
            int r = stat(path, &st);
            check(r == 0, "stat %s by its full name (%s)", labels[i], strerror(errno));
            if (strlen(names[i]) > 255)
                longs_created++;
        } else {
            int err = errno;
            check(strlen(names[i]) > 255, "create %s (%s)", labels[i], strerror(err));
            test_logf("host refused %s: %s\n", labels[i], strerror(err));
        }
    }
    if (created[N_DIR]) {
        snprintf(path, sizeof(path), "%s/%s/%s", work, names[N_DIR], inner);
        int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd >= 0)
            close(fd);
        else
            check(0, "create a 361-byte name inside the long-named directory (%s)", strerror(errno));
    }

    if (longs_created == 0) {
        printf("realfs_long_name: SKIP (host %s refuses names over 255 bytes)\n", host);
    } else {
        // 1. readdir(3): the listing a guest `ls` does.
        list_readdir(work);
        // 2. getdents64 directly, with a small buffer.
        list_getdents(work);

        // 3. The listed name is usable: open and read it back, rename it to
        // another long name and back.
        for (int i = 0; i < N_COUNT; i++) {
            if (!created[i] || i == N_DIR || strlen(names[i]) <= 255)
                continue;
            snprintf(path, sizeof(path), "%s/%s", work, names[i]);
            int fd = open(path, O_RDONLY);
            if (fd < 0)
                check(0, "open %s (%s)", labels[i], strerror(errno));
            if (fd >= 0) {
                char buf[1024];
                ssize_t n = read(fd, buf, sizeof(buf));
                check(n == (ssize_t) strlen(names[i]) && memcmp(buf, names[i], (size_t) n) == 0,
                        "read back %s (%zd bytes)", labels[i], n);
                close(fd);
            }
            renamed(path2, sizeof(path2), work, i);
            int r = rename(path, path2);
            check(r == 0, "rename %s to another long name (%s)", labels[i], strerror(errno));
            struct stat st;
            r = stat(path, &st);
            check(r != 0 && errno == ENOENT, "old name of %s is gone after rename", labels[i]);
            r = rename(path2, path);
            check(r == 0, "rename %s back (%s)", labels[i], strerror(errno));
            r = stat(path, &st);
            check(r == 0, "stat %s after renaming it back (%s)", labels[i], strerror(errno));
        }
        list_readdir(work);
    }

    // 4. Remove everything by the names readdir returns, as rm -rf does. A
    // name that was not listed, or listed wrong, leaves rmdir with ENOTEMPTY.
    if (remove_tree(work, 0) == 0) {
        struct stat st;
        int r = stat(work, &st);
        check(r != 0 && errno == ENOENT, "work directory is gone");
    }
    int r = umount(mnt);
    check(r == 0, "umount (%s)", strerror(errno));
    rmdir(mnt);
    if (longs_created == 0 && failures_total == 0)
        return 0;
    return finish_suite("realfs_long_name");
}
