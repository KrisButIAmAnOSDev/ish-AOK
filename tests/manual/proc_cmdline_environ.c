// /proc/<pid>/cmdline and /proc/<pid>/environ: each string followed by exactly
// one NUL, and nothing after the last one.
//
// AOK's exec copied argv and envp onto the new stack as its internal packed
// blocks -- every string NUL-terminated, then one MORE NUL ending the list --
// and recorded arg_end and env_end past that list terminator. Both files
// therefore ended in an extra NUL: `xargs -0 < /proc/PID/cmdline` ran its
// command with a trailing empty argument, and an empty environment read back
// as a single NUL instead of nothing.
//
// Linux lays the strings out back to back -- argv[0] .. argv[argc-1] then
// envp[0] .. envp[envc-1], no terminators between the lists -- so arg_end ==
// env_start, and fs/proc/base.c's get_mm_cmdline() depends on that:
//
//   - [arg_start, arg_end) is returned exactly, UNLESS the byte at arg_end-1
//     is not NUL. That means a setproctitle() overwrote the terminator, and
//     the first NUL-terminated string starting at arg_start is returned
//     instead (its NUL included, one page at most), running on into the
//     environment area -- but only when env_start == arg_end, and never past
//     env_end.
//   - arg_start >= arg_end, or a task with no mm (a zombie, a kernel thread),
//     reads as empty.
//
// AOK's reader had its own heuristics in place of that (a title containing
// ':' followed by any non-NUL byte was cut at its first NUL), which Linux does
// not have, and it answered ESRCH rather than nothing for a zombie. This test
// checks the exact bytes of every shape above, an exec with an empty argv
// (Linux >= 5.18 runs it with argc 1 and argv[0] == ""), and the PR_SET_MM
// ranges that let a process point cmdline and environ anywhere -- including
// ranges above 4 GiB, which AOK's reader truncated to 32 bits.
//
// Everything here was run on a native Linux 6.12 x86_64 host, 64-bit and -m32,
// as an unprivileged user. That user has no CAP_SYS_RESOURCE, so the oracle
// reaches the PR_SET_MM range cases through PR_SET_MM_MAP, which needs none;
// AOK does not implement PR_SET_MM_MAP and runs them through the single-field
// operations as root. Both routes validate with the same kernel function
// (validate_prctl_map_addr), so the refusals below are checked on both.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "test_common.h"

#ifndef PR_SET_MM
#define PR_SET_MM 35
#endif
#ifndef PR_SET_MM_ARG_START
#define PR_SET_MM_ARG_START 8
#define PR_SET_MM_ARG_END 9
#define PR_SET_MM_ENV_START 10
#define PR_SET_MM_ENV_END 11
#endif
#ifndef PR_SET_MM_MAP
#define PR_SET_MM_MAP 14
#endif

#define CHILD_ARGV0 "pce-child"
#define PAGE 4096

extern char **environ;

static const char *self_exe;

// ---------------------------------------------------------------- helpers

static void escape_print(const char *label, const char *buf, size_t len) {
    printf("  %s (%zu bytes): \"", label, len);
    size_t shown = len > 160 ? 160 : len;
    for (size_t i = 0; i < shown; i++) {
        unsigned char c = (unsigned char) buf[i];
        if (c == 0)
            printf("\\0");
        else if (c < 32 || c > 126)
            printf("\\x%02x", c);
        else
            putchar(c);
    }
    printf("%s\"\n", shown < len ? "..." : "");
}

// Read a whole file with read(2) chunks of `chunk` bytes. Returns 0, or the
// errno of the open or of the first failing read.
static int read_file(const char *path, size_t chunk, char *buf, size_t cap, size_t *len_out) {
    *len_out = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return errno;
    size_t len = 0;
    for (;;) {
        size_t want = chunk;
        if (want > cap - len)
            want = cap - len;
        if (want == 0)
            break;
        ssize_t n = read(fd, buf + len, want);
        if (n < 0) {
            int e = errno;
            close(fd);
            *len_out = len;
            return e;
        }
        if (n == 0)
            break;
        len += (size_t) n;
    }
    close(fd);
    *len_out = len;
    return 0;
}

// Compare a /proc file's contents, read in one go and a few bytes at a time.
static void expect_file(const char *label, const char *path, const char *want, size_t want_len) {
    static char got[3 * PAGE];
    static char got_small[3 * PAGE];
    size_t got_len = 0, small_len = 0;
    int err = read_file(path, sizeof(got), got, sizeof(got), &got_len);
    if (err != 0) {
        printf("FAIL %s: reading %s: %s\n", label, path, strerror(err));
        failures_total++;
        return;
    }
    if (got_len != want_len || memcmp(got, want, want_len) != 0) {
        printf("FAIL %s: %s differs\n", label, path);
        escape_print("got ", got, got_len);
        escape_print("want", want, want_len);
        failures_total++;
        return;
    }
    err = read_file(path, 7, got_small, sizeof(got_small), &small_len);
    if (err != 0 || small_len != want_len || memcmp(got_small, want, want_len) != 0) {
        printf("FAIL %s: %s read 7 bytes at a time differs (err=%d)\n", label, path, err);
        escape_print("got ", got_small, small_len);
        escape_print("want", want, want_len);
        failures_total++;
        return;
    }
    test_logf("%s: %s ok (%zu bytes)\n", label, path, want_len);
}

static void expect_open_errno(const char *label, const char *path, int want_errno) {
    int fd = open(path, O_RDONLY);
    int e = errno;
    if (fd >= 0) {
        close(fd);
        printf("FAIL %s: open %s succeeded, want %s\n", label, path, strerror(want_errno));
        failures_total++;
    } else if (e != want_errno) {
        printf("FAIL %s: open %s: %s, want %s\n", label, path, strerror(e), strerror(want_errno));
        failures_total++;
    } else {
        test_logf("%s: open %s -> %s ok\n", label, path, strerror(e));
    }
}

// Join strings the way the kernel lays them out: each one NUL-terminated.
static size_t join(char *out, const char *const *strs) {
    size_t n = 0;
    for (size_t i = 0; strs[i] != NULL; i++) {
        size_t one = strlen(strs[i]) + 1;
        memcpy(out + n, strs[i], one);
        n += one;
    }
    return n;
}

// ------------------------------------------------------------ child side
//
// A re-executed copy of this binary, recognised by argv[0]. It optionally
// rewrites its own argv area, writes one byte to fd 3 to say it is ready,
// and then waits for fd 4 to close. The parent reads its /proc files in
// between. It must not touch its environment: setenv would move environ.

static unsigned long stat_field(const char *stat, int field) {
    // Field 2 (comm) may contain spaces; count from the closing paren.
    const char *p = strrchr(stat, ')');
    if (p == NULL)
        return 0;
    p += 2; // now at field 3
    for (int f = 3; f < field; f++) {
        p = strchr(p, ' ');
        if (p == NULL)
            return 0;
        p++;
    }
    return strtoul(p, NULL, 10);
}

static int child_main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    const char *env_mode = getenv("PCE_CHILD");
    // Started with an empty argv: Linux supplies one empty argument.
    bool bad_argc = env_mode != NULL && strcmp(env_mode, "empty-argv") == 0 &&
        (argc != 1 || argv[0] == NULL || argv[0][0] != '\0');
    char *area = argc > 0 ? argv[0] : NULL;
    size_t arg_len = 0;
    for (int i = 0; i < argc; i++)
        arg_len += strlen(argv[i]) + 1;
    size_t env_len = 0;
    for (char **e = environ; *e != NULL; e++)
        env_len += strlen(*e) + 1;

    // Layout: the strings are back to back, and /proc/self/stat's arg/env
    // fields agree with the pointers the program was handed. Reported rather
    // than asserted here -- the parent owns the verdict.
    char layout_fail = 0;
    {
        char stat[1024];
        size_t n = 0;
        int err = read_file("/proc/self/stat", sizeof(stat) - 1, stat, sizeof(stat) - 1, &n);
        stat[n] = '\0';
        if (err == 0) {
            unsigned long arg_start = stat_field(stat, 48);
            unsigned long arg_end = stat_field(stat, 49);
            unsigned long env_start = stat_field(stat, 50);
            unsigned long env_end = stat_field(stat, 51);
            if ((area != NULL && arg_start != (unsigned long) area) ||
                    arg_end != arg_start + arg_len ||
                    env_start != arg_end || env_end != env_start + env_len)
                layout_fail = 1;
            if (environ[0] != NULL && (unsigned long) environ[0] != env_start)
                layout_fail = 1;
        } else {
            layout_fail = 1;
        }
    }

    if (strcmp(mode, "overwrite-args") == 0) {
        // Every byte of the argv area, its final NUL included.
        memset(area, 'X', arg_len);
    } else if (strcmp(mode, "overwrite-all") == 0) {
        // The argv AND environment areas, with no NUL anywhere.
        memset(area, 'Z', arg_len + env_len);
    } else if (strcmp(mode, "dot-title") == 0) {
        // libbsd's setproctitle: a short title, and a non-NUL byte planted on
        // the old terminator so the kernel shows the title alone.
        memcpy(area, "ttl", 4);
        area[arg_len - 1] = '.';
    } else if (strcmp(mode, "pad-title") == 0) {
        // OpenSSH's SPT_REUSEARGV: the title, then NUL padding to the end.
        memset(area, 0, arg_len);
        memcpy(area, "sshd: probe", 11);
    } else if (strcmp(mode, "colon-title") == 0) {
        // A title with a colon over argv[0] alone, the other args left intact.
        memcpy(area, "a: b", 5);
    } else if (strcmp(mode, "big-args") == 0) {
        memset(area, 'Q', arg_len);
    }

    char ready = bad_argc ? 'A' : layout_fail ? 'L' : 'R';
    if (write(3, &ready, 1) != 1)
        return 1;
    char c;
    while (read(4, &c, 1) > 0)
        ;
    return 0;
}

// ------------------------------------------------------------ parent side

struct child {
    pid_t pid;
    int done_fd;
};

// Start a child with this argv/envp and wait for it to be ready. argv[0] is
// CHILD_ARGV0, or argv is empty and envp carries PCE_CHILD=empty-argv.
static bool child_start(const char *label, struct child *ch, const char *const *argv,
                        const char *const *envp) {
    int ready[2], done[2];
    if (pipe(ready) != 0 || pipe(done) != 0) {
        printf("FAIL %s: pipe: %s\n", label, strerror(errno));
        failures_total++;
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL %s: fork: %s\n", label, strerror(errno));
        failures_total++;
        return false;
    }
    if (pid == 0) {
        close(ready[0]);
        close(done[1]);
        if (dup2(ready[1], 3) < 0 || dup2(done[0], 4) < 0)
            _exit(126);
        execve(self_exe, (char *const *) argv, (char *const *) envp);
        _exit(127);
    }
    close(ready[1]);
    close(done[0]);
    char c = 0;
    ssize_t n = read(ready[0], &c, 1);
    close(ready[0]);
    ch->pid = pid;
    ch->done_fd = done[1];
    if (n != 1) {
        printf("FAIL %s: child never became ready\n", label);
        failures_total++;
        close(done[1]);
        waitpid(pid, NULL, 0);
        return false;
    }
    if (c == 'A') {
        printf("FAIL %s: an empty argv did not arrive as argc 1, argv[0] \"\"\n", label);
        failures_total++;
    }
    if (c == 'L') {
        printf("FAIL %s: /proc/self/stat arg_start/arg_end/env_start/env_end do not "
               "describe the strings back to back\n", label);
        failures_total++;
    }
    return true;
}

static void child_finish(struct child *ch) {
    close(ch->done_fd);
    waitpid(ch->pid, NULL, 0);
}

static void check_exec_case(const char *label, const char *const *argv, const char *const *envp,
                            const char *want_cmdline, size_t want_cmdline_len) {
    struct child ch;
    if (!child_start(label, &ch, argv, envp))
        return;
    char path[64];
    static char want_env[2 * PAGE];
    size_t want_env_len = join(want_env, envp);
    static char want_cmd[3 * PAGE];
    if (want_cmdline == NULL) {
        want_cmdline_len = join(want_cmd, argv);
        want_cmdline = want_cmd;
    }
    snprintf(path, sizeof(path), "/proc/%d/cmdline", ch.pid);
    expect_file(label, path, want_cmdline, want_cmdline_len);
    snprintf(path, sizeof(path), "/proc/%d/environ", ch.pid);
    expect_file(label, path, want_env, want_env_len);
    child_finish(&ch);
}

static void test_exec_layouts(void) {
    static char want[3 * PAGE];

    // Plain, with an empty argument in the middle: it is one NUL on its own.
    {
        const char *const argv[] = {CHILD_ARGV0, "plain", "alpha", "", "beta", NULL};
        const char *const envp[] = {"PCE_A=1", "PCE_B=22", NULL};
        check_exec_case("plain", argv, envp, NULL, 0);
    }

    // An empty argv: one empty argument, so cmdline is a single NUL.
    {
        const char *const argv[] = {NULL};
        const char *const envp[] = {"PCE_CHILD=empty-argv", NULL};
        check_exec_case("empty argv", argv, envp, "", 1);
    }

    // An empty environment reads as nothing at all.
    {
        const char *const argv[] = {CHILD_ARGV0, "empty-env", NULL};
        const char *const envp[] = {NULL};
        check_exec_case("empty environment", argv, envp, NULL, 0);
    }

    // The whole argv area overwritten, terminator included: the first string
    // runs on into the environment and stops at its first NUL, which is shown.
    {
        const char *const argv[] = {CHILD_ARGV0, "overwrite-args", "alpha", NULL};
        const char *const envp[] = {"PCE_A=1", "PCE_B=22", NULL};
        size_t arg_len = join(want, argv);
        memset(want, 'X', arg_len);
        memcpy(want + arg_len, "PCE_A=1", 8);
        check_exec_case("setproctitle into the environment", argv, envp, want, arg_len + 8);
    }

    // The same with no environment to run into: arg_end is the limit, and
    // there is no NUL to show.
    {
        const char *const argv[] = {CHILD_ARGV0, "overwrite-args", "alpha", NULL};
        const char *const envp[] = {NULL};
        size_t arg_len = join(want, argv);
        memset(want, 'X', arg_len);
        check_exec_case("setproctitle with an empty environment", argv, envp, want, arg_len);
    }

    // Argv and environment both overwritten with no NUL left: env_end is the
    // limit. (The parent's own view of environ is the check here: the child
    // cannot read its environment back, it has none left.)
    {
        const char *const argv[] = {CHILD_ARGV0, "overwrite-all", NULL};
        const char *const envp[] = {"PCE_A=1", "PCE_B=22", NULL};
        struct child ch;
        if (child_start("overwrite everything", &ch, argv, envp)) {
            size_t arg_len = join(want, argv);
            size_t env_len = join(want, envp);
            memset(want, 'Z', arg_len + env_len);
            char path[64];
            snprintf(path, sizeof(path), "/proc/%d/cmdline", ch.pid);
            expect_file("overwrite everything", path, want, arg_len + env_len);
            snprintf(path, sizeof(path), "/proc/%d/environ", ch.pid);
            expect_file("overwrite everything", path, want, env_len);
            child_finish(&ch);
        }
    }

    // libbsd: a title plus a planted '.' shows the title and its NUL only.
    {
        const char *const argv[] = {CHILD_ARGV0, "dot-title", "alpha", NULL};
        const char *const envp[] = {"PCE_A=1", NULL};
        check_exec_case("setproctitle, libbsd style", argv, envp, "ttl", 4);
    }

    // OpenSSH: the title and its NUL padding, all of it. The last byte is a
    // NUL, so this is the exact-range case, padding and all.
    {
        const char *const argv[] = {CHILD_ARGV0, "pad-title", "alpha", NULL};
        const char *const envp[] = {"PCE_A=1", NULL};
        size_t arg_len = join(want, argv);
        memset(want, 0, arg_len);
        memcpy(want, "sshd: probe", 11);
        check_exec_case("setproctitle, OpenSSH style", argv, envp, want, arg_len);
    }

    // A colon title over argv[0] leaves the remaining arguments visible: Linux
    // has no rule about colons.
    {
        const char *const argv[] = {CHILD_ARGV0, "colon-title", "alpha", NULL};
        const char *const envp[] = {"PCE_A=1", NULL};
        size_t arg_len = join(want, argv);
        memcpy(want, "a: b", 5);
        check_exec_case("title with a colon", argv, envp, want, arg_len);
    }

    // An overwritten argv area bigger than a page: the setproctitle read is
    // one page, and a page with no NUL in it is shown without one.
    {
        static char big[PAGE + 1000];
        memset(big, 'b', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        const char *const argv[] = {CHILD_ARGV0, "big-args", big, NULL};
        const char *const envp[] = {"PCE_A=1", NULL};
        memset(want, 'Q', PAGE);
        check_exec_case("setproctitle longer than a page", argv, envp, want, PAGE);
    }

    // The same arguments untouched come back in full, past the page.
    {
        static char big[PAGE + 1000];
        memset(big, 'b', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        const char *const argv[] = {CHILD_ARGV0, "plain", big, NULL};
        const char *const envp[] = {"PCE_A=1", NULL};
        check_exec_case("arguments longer than a page", argv, envp, NULL, 0);
    }
}

// A zombie has no mm: its cmdline is empty. Its environ file is owned by root
// once the mm is gone, so an unprivileged opener is refused before reading.
static void test_zombie(void) {
    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL zombie: fork: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    if (pid == 0)
        _exit(0);
    char path[64];
    bool zombie = false;
    for (int i = 0; i < 2000 && !zombie; i++) {
        char stat[512];
        size_t n = 0;
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        if (read_file(path, sizeof(stat) - 1, stat, sizeof(stat) - 1, &n) == 0) {
            stat[n] = '\0';
            const char *p = strrchr(stat, ')');
            if (p != NULL && p[1] == ' ' && p[2] == 'Z')
                zombie = true;
        }
        if (!zombie) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
    }
    if (!zombie) {
        printf("FAIL zombie: child %d never showed state Z\n", pid);
        failures_total++;
    } else {
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        expect_file("zombie", path, "", 0);
        snprintf(path, sizeof(path), "/proc/%d/environ", pid);
        if (geteuid() == 0)
            expect_file("zombie (root)", path, "", 0);
        else
            expect_open_errno("zombie (unprivileged)", path, EACCES);
    }
    waitpid(pid, NULL, 0);
}

// kthreadd is pid 2 on Linux and on AOK. Skipped where pid 2 is something else.
static void test_kthread(void) {
    char stat[512];
    size_t n = 0;
    if (read_file("/proc/2/stat", sizeof(stat) - 1, stat, sizeof(stat) - 1, &n) != 0) {
        test_logf("kthread: no /proc/2, skipped\n");
        return;
    }
    stat[n] = '\0';
    if (strstr(stat, "(kthreadd)") == NULL) {
        test_logf("kthread: pid 2 is not kthreadd, skipped\n");
        return;
    }
    expect_file("kthreadd", "/proc/2/cmdline", "", 0);
    if (geteuid() == 0)
        expect_file("kthreadd (root)", "/proc/2/environ", "", 0);
    else
        expect_open_errno("kthreadd (unprivileged)", "/proc/2/environ", EACCES);
}

// ---------------------------------------------------------- PR_SET_MM

struct prctl_mm_map_ {
    unsigned long long start_code, end_code, start_data, end_data, start_brk, brk,
            start_stack, arg_start, arg_end, env_start, env_end;
    unsigned long long auxv;   // a pointer in the uapi, but 8 bytes on every ABI
    unsigned int auxv_size;
    unsigned int exe_fd;
};

enum set_mm_route { ROUTE_NONE, ROUTE_SINGLE, ROUTE_MAP };

static enum set_mm_route route;
static struct prctl_mm_map_ base_map;

// Set arg and env ranges. Returns 0 or an errno.
static int set_ranges(unsigned long arg_start, unsigned long arg_end, unsigned long env_start,
                      unsigned long env_end) {
    if (route == ROUTE_MAP) {
        struct prctl_mm_map_ map = base_map;
        map.arg_start = arg_start;
        map.arg_end = arg_end;
        map.env_start = env_start;
        map.env_end = env_end;
        if (prctl(PR_SET_MM, PR_SET_MM_MAP, (unsigned long) &map, sizeof(map), 0) != 0)
            return errno;
        return 0;
    }
    // One field at a time, each step keeping start <= end: widen first, then
    // narrow onto the target.
    unsigned long lo_a = arg_start, hi_a = arg_end, lo_e = env_start, hi_e = env_end;
    if (prctl(PR_SET_MM, PR_SET_MM_ARG_START, 0x10000UL, 0, 0) != 0)
        return errno;
    if (prctl(PR_SET_MM, PR_SET_MM_ARG_END, hi_a, 0, 0) != 0)
        return errno;
    if (prctl(PR_SET_MM, PR_SET_MM_ARG_START, lo_a, 0, 0) != 0)
        return errno;
    if (prctl(PR_SET_MM, PR_SET_MM_ENV_START, 0x10000UL, 0, 0) != 0)
        return errno;
    if (prctl(PR_SET_MM, PR_SET_MM_ENV_END, hi_e, 0, 0) != 0)
        return errno;
    if (prctl(PR_SET_MM, PR_SET_MM_ENV_START, lo_e, 0, 0) != 0)
        return errno;
    return 0;
}

static bool load_base_map(void) {
    char stat[1024];
    size_t n = 0;
    if (read_file("/proc/self/stat", sizeof(stat) - 1, stat, sizeof(stat) - 1, &n) != 0)
        return false;
    stat[n] = '\0';
    memset(&base_map, 0, sizeof(base_map));
    base_map.start_code = stat_field(stat, 26);
    base_map.end_code = stat_field(stat, 27);
    base_map.start_stack = stat_field(stat, 28);
    base_map.start_data = stat_field(stat, 45);
    base_map.end_data = stat_field(stat, 46);
    base_map.start_brk = stat_field(stat, 47);
    base_map.brk = (unsigned long) sbrk(0);
    base_map.arg_start = stat_field(stat, 48);
    base_map.arg_end = stat_field(stat, 49);
    base_map.env_start = stat_field(stat, 50);
    base_map.env_end = stat_field(stat, 51);
    base_map.exe_fd = (unsigned int) -1;
    return true;
}

static unsigned long base_of(const char *region) {
    return (unsigned long) region;
}

static void expect_errno(const char *label, int ret, int want_errno) {
    int e = errno;
    if (ret != -1 || e != want_errno) {
        printf("FAIL %s: ret=%d errno=%s, want %s\n", label, ret, strerror(e),
               strerror(want_errno));
        failures_total++;
    } else {
        test_logf("%s: %s ok\n", label, strerror(e));
    }
}

static void set_mm_case(const char *label, const char *region, size_t arg_off, size_t arg_len,
                        size_t env_off, size_t env_len, const char *want_cmd, size_t want_cmd_len) {
    unsigned long base = (unsigned long) region;
    int err = set_ranges(base + arg_off, base + arg_off + arg_len, base + env_off,
                         base + env_off + env_len);
    if (err != 0) {
        printf("FAIL %s: setting the ranges: %s\n", label, strerror(err));
        failures_total++;
        return;
    }
    expect_file(label, "/proc/self/cmdline", want_cmd, want_cmd_len);
    expect_file(label, "/proc/self/environ", region + env_off, env_len);
}

// In a forked child, so this process's own ranges are left alone.
static int set_mm_child(void) {
    // Page-aligned heap memory, so every address is mapped and well above
    // mmap_min_addr on every host.
    char *region = aligned_alloc(PAGE, PAGE);
    if (region == NULL)
        return 1;
    memset(region, 0, PAGE);
    //                 0123456789012345678901234567
    memcpy(region,    "prctl\0range\0", 12);
    memcpy(region + 32, "one\0twoX", 8);
    memcpy(region + 48, "abcdefgh", 8);          // region[56] stays NUL
    memcpy(region + 64, "abcd" "ef\0g", 8);
    memcpy(region + 80, "E=1\0F=2\0", 8);

    set_mm_case("PR_SET_MM exact range", region, 0, 12, 80, 8, "prctl\0range\0", 12);
    set_mm_case("PR_SET_MM last byte set", region, 32, 8, 80, 8, "one", 4);
    // The first string runs past arg_end to a NUL, but the environment is not
    // adjacent, so arg_end is the limit.
    set_mm_case("PR_SET_MM no NUL, env elsewhere", region, 48, 8, 80, 8, "abcdefgh", 8);
    // Adjacent environment: the title runs into it.
    set_mm_case("PR_SET_MM no NUL, env adjacent", region, 64, 4, 68, 4, "abcdef", 7);
    // Empty ranges.
    set_mm_case("PR_SET_MM empty ranges", region, 0, 0, 80, 0, "", 0);

    // Refusals: an inverted pair, and an address under mmap_min_addr. Each is
    // EINVAL and leaves the ranges as they were.
    if (set_ranges(base_of(region), base_of(region) + 12, base_of(region) + 80,
                   base_of(region) + 88) != 0) {
        printf("FAIL PR_SET_MM: could not reset the ranges\n");
        failures_total++;
        return 1;
    }
    unsigned long b = base_of(region);
    if (route == ROUTE_SINGLE) {
        expect_errno("PR_SET_MM_ARG_START past arg_end",
                     prctl(PR_SET_MM, PR_SET_MM_ARG_START, b + 13, 0, 0), EINVAL);
        expect_errno("PR_SET_MM_ENV_END below env_start",
                     prctl(PR_SET_MM, PR_SET_MM_ENV_END, b + 79, 0, 0), EINVAL);
        expect_errno("PR_SET_MM_ARG_START under mmap_min_addr",
                     prctl(PR_SET_MM, PR_SET_MM_ARG_START, 4096UL, 0, 0), EINVAL);
        expect_errno("PR_SET_MM_ARG_END with arg4 set",
                     prctl(PR_SET_MM, PR_SET_MM_ARG_END, b + 12, 1UL, 0), EINVAL);
    } else {
        struct prctl_mm_map_ map = base_map;
        map.arg_start = b + 13;
        map.arg_end = b + 12;
        map.env_start = b + 80;
        map.env_end = b + 88;
        expect_errno("PR_SET_MM_MAP inverted arguments",
                     prctl(PR_SET_MM, PR_SET_MM_MAP, (unsigned long) &map, sizeof(map), 0), EINVAL);
        map.arg_start = 4096;
        expect_errno("PR_SET_MM_MAP arguments under mmap_min_addr",
                     prctl(PR_SET_MM, PR_SET_MM_MAP, (unsigned long) &map, sizeof(map), 0), EINVAL);
    }
    expect_file("PR_SET_MM after refusals", "/proc/self/cmdline", "prctl\0range\0", 12);
    expect_file("PR_SET_MM after refusals", "/proc/self/environ", region + 80, 8);
    return failures_total != 0;
}

static void test_set_mm(void) {
    // Unprivileged: the single-field operations need CAP_SYS_RESOURCE -- but a
    // stray arg4 or arg5 is refused before that is even asked.
    if (geteuid() != 0) {
        unsigned long addr = (unsigned long) &route;
        errno = 0;
        expect_errno("PR_SET_MM_ARG_START unprivileged",
                     prctl(PR_SET_MM, PR_SET_MM_ARG_START, addr, 0, 0), EPERM);
        expect_errno("PR_SET_MM_ARG_START unprivileged, arg4 set",
                     prctl(PR_SET_MM, PR_SET_MM_ARG_START, addr, 1UL, 0), EINVAL);
        expect_errno("PR_SET_MM_ARG_START unprivileged, arg5 set",
                     prctl(PR_SET_MM, PR_SET_MM_ARG_START, addr, 0, 1UL), EINVAL);
    }
    if (!load_base_map()) {
        printf("FAIL PR_SET_MM: cannot read /proc/self/stat\n");
        failures_total++;
        return;
    }
    if (geteuid() == 0) {
        route = ROUTE_SINGLE;
    } else {
        // PR_SET_MM_MAP needs no capability (CONFIG_CHECKPOINT_RESTORE), so it
        // is how an unprivileged run reaches the reader. AOK does not implement
        // it; there the range cases need root.
        struct prctl_mm_map_ map = base_map;
        if (prctl(PR_SET_MM, PR_SET_MM_MAP, (unsigned long) &map, sizeof(map), 0) == 0) {
            route = ROUTE_MAP;
        } else {
            test_logf("PR_SET_MM range cases skipped: unprivileged and PR_SET_MM_MAP: %s\n",
                      strerror(errno));
            return;
        }
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        int rc = set_mm_child();
        fflush(stdout);
        _exit(rc);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
        printf("FAIL PR_SET_MM range cases (status %#x)\n", status);
        failures_total++;
    }
}

int main(int argc, char **argv) {
    if ((argc > 0 && argv[0] != NULL && strcmp(argv[0], CHILD_ARGV0) == 0) ||
            getenv("PCE_CHILD") != NULL)
        return child_main(argc, argv);
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    static char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        printf("proc_cmdline_environ: SKIP (no /proc/self/exe)\n");
        return 0;
    }
    exe[n] = '\0';
    self_exe = exe;

    // This process's own cmdline, as the runner started it.
    {
        static char want[PAGE];
        size_t len = 0;
        for (int i = 0; i < argc; i++) {
            size_t one = strlen(argv[i]) + 1;
            memcpy(want + len, argv[i], one);
            len += one;
        }
        expect_file("self", "/proc/self/cmdline", want, len);
    }

    test_exec_layouts();
    test_zombie();
    test_kthread();
    test_set_mm();
    return finish_suite("proc_cmdline_environ");
}
