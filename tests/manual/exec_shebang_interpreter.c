// What a #! line names, and what actually runs.
//
//   An interpreter named on a #! line is executed, so it faces every question
//   any other executable faces -- including, in iSH-AOK, whether it is one of
//   the programs compiled into the binary (/AOK/native/*, kernel/native.h).
//   That question was only asked in __do_execve, so it was asked when such a
//   program was exec'd directly and NOT when it was reached as an interpreter.
//
//   The failure was silent, which is what makes it worth a test. The file
//   served at /AOK/native/<name> is a `#!/bin/sh` placeholder, so a script
//   saying `#!/AOK/native/bash` got a shell script as its interpreter, found no
//   loader that would take one, and came back ENOEXEC -- and ENOEXEC is exactly
//   the errno every shell answers by re-running the file under /bin/sh. So
//   `#!/AOK/native/bash` ran under dash: not the program asked for, not the
//   placeholder's diagnostic, and no error anywhere.
//
// The ordinary #! rules are here too, because they are what the fix must not
// break: an interpreter with no argument, one with an argument, and one reached
// through a symlink. Those run everywhere including on a real Linux oracle; the
// native cases are skipped where there is no /AOK.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define OUT_MAX 4096

// Named on a #! line as this binary's argument, it makes the binary report the
// argv the kernel handed it and stop. See the symlink case below.
#define INTERP_MARKER "--print-argv"

static char base[128];

static void ok(const char *label, const char *got, const char *want) {
    if (strcmp(got, want) == 0) {
        test_logf("  %-52s %s\n", label, got);
        return;
    }
    printf("FAIL %s\n       got: %s\n  expected: %s\n", label, got, want);
    failures_total++;
}

static void failf_msg(const char *label, const char *why) {
    printf("FAIL %s: %s\n", label, why);
    failures_total++;
}

// Write `text` to `path` and make it executable.
static int write_script(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f == NULL)
        return -1;
    fputs(text, f);
    if (fclose(f) != 0)
        return -1;
    return chmod(path, 0755);
}

// execv `path` with `args` and collect the merged stdout+stderr.
//
// Merged deliberately: when this regresses, the symptom is a diagnostic from
// the WRONG interpreter -- "Syntax error: Bad for loop variable" is dash -- and
// a failure that shows it names its own cause.
//
// execv rather than system(): a shell hides the bug. It catches ENOEXEC and
// re-runs the file under /bin/sh, which IS the silence being tested for. execv
// reports the errno instead, and the child prints it into the same pipe.
static int run_exec(const char *path, char *const args[], char *out) {
    out[0] = '\0';
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;

    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        if (pipefd[1] != STDOUT_FILENO && pipefd[1] != STDERR_FILENO)
            close(pipefd[1]);
        execv(path, args);
        fprintf(stderr, "EXECV-FAILED errno=%d (%s)", errno, strerror(errno));
        fflush(NULL);
        _exit(127);
    }
    close(pipefd[1]);

    size_t n = 0;
    while (n < OUT_MAX - 1) {
        ssize_t r = read(pipefd[0], out + n, OUT_MAX - 1 - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        n += (size_t) r;
    }
    out[n] = '\0';
    close(pipefd[0]);
    // Trailing newlines only; interior ones are part of what is compared.
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = '\0';

    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        continue;
    return 0;
}

// Run a script through its own #! line, with "alpha beta" appended, and compare
// what came back.
static void case_script(const char *label, const char *name, const char *text,
                        const char *want, int with_args) {
    char script[sizeof base + 32];
    snprintf(script, sizeof script, "%s/%s", base, name);
    if (write_script(script, text) != 0) {
        failf_msg(label, strerror(errno));
        return;
    }
    char *const args_with[] = { script, (char *) "alpha", (char *) "beta", NULL };
    char *const args_bare[] = { script, NULL };
    char out[OUT_MAX];
    if (run_exec(script, with_args ? args_with : args_bare, out) != 0) {
        failf_msg(label, strerror(errno));
        return;
    }
    ok(label, out, want);
}

// Is /AOK/native/<name> a program this build actually carries? The path exists
// on every iSH-AOK root whether or not the program is compiled in -- what is
// served there when it is not is a placeholder that says so and exits 127
// (fs/aok.c). Ask by running it: an absent program cannot answer.
static int native_available(const char *prog) {
    char path[64];
    snprintf(path, sizeof path, "/AOK/native/%s", prog);
    if (access(path, X_OK) != 0)
        return 0;
    char *const args[] = { path, (char *) "-c", (char *) "echo NATIVE-READY", NULL };
    char out[OUT_MAX];
    if (run_exec(path, args, out) != 0)
        return 0;
    return strcmp(out, "NATIVE-READY") == 0;
}

static void cleanup(void) {
    static const char *names[] = {
        "no-arg.sh", "with-arg.sh", "linked.sh", "plain.sh",
        "native-bash.sh", "linked-bash.sh", "native-zsh.sh",
        "link-interp", "mybash",
    };
    char path[sizeof base + 32];
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", base, names[i]);
        unlink(path);
    }
    rmdir(base);
}

int main(int argc, char **argv) {
    // Interpreter mode, for the symlink case: print the argv the kernel built,
    // one field per '|'. Before test_init, which rejects unknown options.
    if (argc >= 2 && strcmp(argv[1], INTERP_MARKER) == 0) {
        for (int i = 0; i < argc; i++)
            printf("%s%s", i == 0 ? "" : "|", argv[i]);
        fflush(NULL);
        return 0;
    }
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    snprintf(base, sizeof base, "/tmp/shebang_interp_%d", (int) getpid());
    if (mkdir(base, 0755) != 0) {
        printf("FAIL could not create %s: %s\n", base, strerror(errno));
        return finish_suite("exec_shebang_interpreter");
    }

    // --- the ordinary #! rules, which the native fix must leave alone ------
    //
    // /bin/echo is the interpreter for these: it prints the argv the kernel
    // built for it, which is the whole of what a #! line is specified to do.
    if (access("/bin/echo", X_OK) == 0) {
        char want[sizeof base + 64];

        // No argument: interpreter, script, then the caller's args.
        snprintf(want, sizeof want, "%s/no-arg.sh alpha beta", base);
        case_script("shebang: interpreter, no argument", "no-arg.sh",
                    "#!/bin/echo\n", want, 1);

        // One argument, which sits between the interpreter and the script.
        // Leading spaces after #! and trailing whitespace are both tolerated by
        // Linux and must stay tolerated here.
        snprintf(want, sizeof want, "MARK %s/with-arg.sh alpha beta", base);
        case_script("shebang: interpreter with an argument", "with-arg.sh",
                    "#!  /bin/echo MARK   \n", want, 1);

    } else {
        test_logf("  (no /bin/echo here -- ordinary #! cases skipped)\n");
    }

    // Through a symlink. This binary is the interpreter rather than /bin/echo,
    // because on a BusyBox root every /bin tool is itself a symlink and busybox
    // dispatches on argv[0] -- a link named anything else is "applet not
    // found", which says the symlink resolved but tells us nothing about argv.
    // Reached this way the test prints the whole argv the kernel built, so
    // argv[0] (the interpreter exactly as written on the #! line) is checked
    // too.
    {
        const char *label = "shebang: interpreter through a symlink";
        char self[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", self, sizeof self - 1);
        if (len <= 0) {
            test_logf("  (no /proc/self/exe -- symlinked-interpreter case skipped)\n");
        } else {
            self[len] = '\0';
            char link[sizeof base + 32];
            snprintf(link, sizeof link, "%s/link-interp", base);
            unlink(link);
            if (symlink(self, link) != 0) {
                failf_msg(label, strerror(errno));
            } else {
                char text[sizeof base + 64];
                snprintf(text, sizeof text, "#!%s %s\n", link, INTERP_MARKER);
                char want[4 * sizeof base];
                snprintf(want, sizeof want, "%s|%s|%s/linked.sh|alpha|beta",
                         link, INTERP_MARKER, base);
                case_script(label, "linked.sh", text, want, 1);
            }
        }
    }

    // The everyday case: /bin/sh, which must still be /bin/sh.
    {
        char want[sizeof base + 64];
        snprintf(want, sizeof want, "plain %s/plain.sh alpha beta", base);
        case_script("shebang: #!/bin/sh still runs under /bin/sh", "plain.sh",
                    "#!/bin/sh\necho \"plain $0 $1 $2\"\n", want, 1);
    }

    // --- native interpreters, the class this test exists for ---------------

    if (access("/AOK/native", F_OK) != 0) {
        test_logf("  (no /AOK/native here -- native interpreter cases skipped)\n");
        cleanup();
        return finish_suite("exec_shebang_interpreter");
    }

    // bash. A C-style for loop is a bash-ism dash rejects outright, so the
    // ENOEXEC-to-dash fallback this test guards against cannot pass it.
    if (native_available("bash")) {
        case_script("shebang: #!/AOK/native/bash dispatches natively",
                    "native-bash.sh",
                    "#!/AOK/native/bash\n"
                    "for ((i=0;i<2;i++)); do echo \"i=$i\"; done\n"
                    "[ -n \"$BASH_VERSION\" ] && echo bash-ok\n",
                    "i=0\ni=1\nbash-ok", 0);

        // ...and through a symlink, which is how a native program is meant to
        // be given an ordinary name (kernel/native.h): dispatch is keyed off
        // the resolved fd, and that has to hold on the #! path too.
        char link[sizeof base + 32];
        snprintf(link, sizeof link, "%s/mybash", base);
        unlink(link);
        if (symlink("/AOK/native/bash", link) != 0) {
            failf_msg("shebang: native bash through a symlink", strerror(errno));
        } else {
            char text[sizeof base + 96];
            snprintf(text, sizeof text,
                     "#!%s\n[ -n \"$BASH_VERSION\" ] && echo linked-bash-ok\n",
                     link);
            case_script("shebang: native bash through a symlink",
                        "linked-bash.sh", text, "linked-bash-ok", 0);
        }
    } else {
        test_logf("  (native bash not in this build -- skipped)\n");
    }

    // zsh. `print -r --` is a zsh builtin dash does not have, and ZSH_VERSION
    // is set only by zsh itself.
    if (native_available("zsh")) {
        case_script("shebang: #!/AOK/native/zsh dispatches natively",
                    "native-zsh.sh",
                    "#!/AOK/native/zsh\n"
                    "[[ -n $ZSH_VERSION ]] && print -r -- zsh-ok\n",
                    "zsh-ok", 0);
    } else {
        test_logf("  (native zsh not in this build -- skipped)\n");
    }

    cleanup();
    return finish_suite("exec_shebang_interpreter");
}
