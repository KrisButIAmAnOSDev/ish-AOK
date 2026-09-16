// checkpoint_lazy_reservation.c -- reserved address space across a checkpoint.
//
// Driven by checkpoint_restore.sh, not by the regression suite: it needs
// ISH_GUEST_CHECKPOINT=1, and its second life is a separate ish run from the
// image (ISH_RESTORE). Deliberately does not include test_common.h, which would
// enlist it in suites that run it without either.
//
//     checkpoint_lazy_reservation <image path>
//     checkpoint_lazy_reservation --sleep <seconds>
//
// The first asks for the save itself, through /proc/ish/checkpoint. The second
// sleeps where the save would be, for one taken from outside the guest with
// ISH_CHECKPOINT_AFTER -- the app's path, checkpoint_save_external, which is a
// different function.
//
// Large anonymous mappings are lazy reservations with no page-table entries
// (emu/memory.h, struct mem_lazy_map). An image that saved only entries lost
// them all: a JVM-style PROT_NONE heap came back as a hole, so a write there was
// SEGV_MAPERR instead of SEGV_ACCERR and new mmaps could land inside it, and
// the untouched tail of a large RW mapping faulted on its first write. Saving
// the reserved pages as zeroes would be correct and cost the whole reservation
// in the image, so it must come back reserved: the image stays small and VmRSS
// stays small after the restore.
//
// Prints "life: original" or "life: restored", one line per check, and last
// "RESULT: PASS" or "RESULT: FAIL <n>". Checked in BOTH lives, before anything
// the checks do could change the answer.
#define _GNU_SOURCE
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MB (1024UL * 1024)

static sigjmp_buf jb;
static volatile sig_atomic_t fault_code;
static int failures;

static void on_fault(int sig, siginfo_t *si, void *ctx) {
    (void) sig;
    (void) ctx;
    fault_code = si->si_code;
    siglongjmp(jb, 1);
}

// 0 if the write worked, else the SIGSEGV si_code.
static int try_write(char *p, char v) {
    if (sigsetjmp(jb, 1) == 0) {
        *p = v;
        return 0;
    }
    return fault_code;
}

static int try_read(char *p, char *out) {
    if (sigsetjmp(jb, 1) == 0) {
        *out = *p;
        return 0;
    }
    return fault_code;
}

static void check(int ok, const char *what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failures++;
}

static long vm_rss_kb(void) {
    char buf[4096];
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    char *line = strstr(buf, "VmRSS:");
    return line != NULL ? strtol(line + 6, NULL, 10) : -1;
}

static int count_regions(const char *path, unsigned long lo, unsigned long hi) {
    static char buf[1 << 21];
    int fd = open(path, O_RDONLY);
    size_t len = 0;
    if (fd < 0)
        return -1;
    for (ssize_t n; len < sizeof(buf) - 1 &&
            (n = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0; )
        len += (size_t) n;
    close(fd);
    buf[len] = '\0';
    int count = 0;
    for (char *line = buf; line != NULL && *line != '\0'; ) {
        unsigned long s, e;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3 && e > lo && s < hi)
            count++;
        line = strchr(line, '\n');
        if (line != NULL)
            line++;
    }
    return count;
}

int main(int argc, char **argv) {
    int sleep_secs = argc == 3 && strcmp(argv[1], "--sleep") == 0 ? atoi(argv[2]) : -1;
    if (argc != 2 && sleep_secs < 0) {
        fprintf(stderr, "usage: %s <image path> | --sleep <seconds>\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);   // or a restore re-prints buffered lines
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);

    // A JVM-style heap: 512M PROT_NONE with one page committed in the middle,
    // which leaves both sides reserved.
    char *heap = mmap(NULL, 512 * MB, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    char *commit = heap == MAP_FAILED ? MAP_FAILED
            : mmap(heap + 256 * MB, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    // A 128M RW mapping touched only at +8M: a materialised front and an
    // untouched reserved tail.
    char *rw = mmap(NULL, 128 * MB, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (heap == MAP_FAILED || commit != heap + 256 * MB || rw == MAP_FAILED) {
        printf("setup failed\nRESULT: FAIL setup\n");
        return 1;
    }
    commit[5] = 0x5a;
    rw[8 * MB] = 0x5b;

    printf("A-BEFORE-SAVE\n");
    int fd;
    if (sleep_secs >= 0) {
        sleep((unsigned) sleep_secs);   // a restore resumes the sleep
    } else {
        fd = open("/proc/ish/checkpoint", O_WRONLY);
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "save %s\n", argv[1]);
        if (fd < 0 || write(fd, cmd, strlen(cmd)) < 0) {
            perror("checkpoint");
            printf("RESULT: FAIL no checkpoint\n");
            return 1;
        }
        close(fd);
    }

    // Which life: /proc/ish/checkpoint says.
    char status[4096] = "";
    fd = open("/proc/ish/checkpoint", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, status, sizeof(status) - 1);
        status[n > 0 ? n : 0] = '\0';
        close(fd);
    }
    int second_life = 0;
    for (char *l = status; l != NULL && *l != '\0'; ) {
        if (strncmp(l, "restored ", 9) == 0) {
            char *v = l + 9;
            while (*v == ' ')
                v++;
            second_life = strncmp(v, "yes", 3) == 0;
        }
        l = strchr(l, '\n');
        if (l != NULL)
            l++;
    }
    printf("life: %s\n", second_life ? "restored" : "original");

    // First, before any check touches a page: nothing reserved came back as
    // entries. The heap and the RW tail are 630M between them.
    long rss = vm_rss_kb();
    char line[160];
    snprintf(line, sizeof(line), "VmRSS %ld kB: the reservations were not materialised", rss);
    check(rss >= 0 && rss < 64 * 1024, line);

    int maps = count_regions("/proc/self/maps", (unsigned long) heap,
                             (unsigned long) heap + 512 * MB);
    int smaps = count_regions("/proc/self/smaps", (unsigned long) heap,
                              (unsigned long) heap + 512 * MB);
    snprintf(line, sizeof(line), "the heap is 3 regions in maps (%d) and smaps (%d)", maps, smaps);
    check(maps == 3 && smaps == 3, line);

    check((unsigned char) commit[5] == 0x5a, "the committed page kept its byte");
    int code = try_write(heap + 100 * MB, 1);
    snprintf(line, sizeof(line), "below the commit is still reserved PROT_NONE (si_code %d)", code);
    check(code == SEGV_ACCERR, line);
    code = try_write(heap + 400 * MB, 1);
    snprintf(line, sizeof(line), "above the commit is still reserved PROT_NONE (si_code %d)", code);
    check(code == SEGV_ACCERR, line);
    int inside = 0;
    for (int i = 0; i < 8; i++) {
        char *p = mmap(NULL, 32 * MB, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED && p + 32 * MB > heap && p < heap + 512 * MB)
            inside++;
    }
    snprintf(line, sizeof(line), "new mmaps stay out of the heap (%d of 8 landed inside)", inside);
    check(inside == 0, line);

    check((unsigned char) rw[8 * MB] == 0x5b, "the RW mapping kept its byte");
    char lo = 1, hi = 1;
    code = try_read(rw + 64 * MB, &lo);
    if (code == 0)
        code = try_read(rw + 127 * MB, &hi);
    snprintf(line, sizeof(line), "its untouched tail reads zero (si_code %d)", code);
    check(code == 0 && lo == 0 && hi == 0, line);
    code = try_write(rw + 120 * MB, 0x5c);
    snprintf(line, sizeof(line), "its untouched tail takes a write (si_code %d)", code);
    check(code == 0 && (unsigned char) rw[120 * MB] == 0x5c, line);

    if (failures == 0)
        printf("RESULT: PASS\n");
    else
        printf("RESULT: FAIL %d\n", failures);
    return failures != 0;
}
