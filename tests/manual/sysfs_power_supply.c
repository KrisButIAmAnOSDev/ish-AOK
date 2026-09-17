// sysfs_power_supply.c — /sys/class/power_supply, the directory waybar's
// battery module has to be able to watch.
//
// waybar's battery module starts with inotify_add_watch("/sys/class/
// power_supply/", IN_CREATE | IN_DELETE) and throws when that fails, so on AOK,
// which had no such directory, the module disabled itself with "Could not
// watch for battery plug/unplug" instead of either showing the battery or
// hiding cleanly. It then watches each battery's uevent with IN_ACCESS.
//
// What is checked, and why it is shaped this way:
//
//  - The directory exists, can be listed, and can be watched -- as root and
//    as an unprivileged user, since the desktop session is not root.
//  - Every supply that IS listed looks like Linux's power_supply class
//    (Documentation/ABI/testing/sysfs-class-power): an integer capacity, a
//    status Linux can report, and a uevent that agrees with the files.
//  - A name that is not listed cannot be opened either. The table-driven sysfs
//    resolves names without consulting what readdir reports, so an absent
//    battery that still answered to its name would be a directory that lies.
//  - On AOK, no energy/charge/voltage/current/power/time figures: iOS reports
//    none, and a number made up to fill the file is worse than no file.
//  - On AOK, the directory agrees with /proc/ish/BAT0_status, which reads the
//    same cached host state, and in the command-line build -- no battery
//    source at all -- it is EMPTY, as on a Linux machine with no battery.
//
// Passes on a real Linux kernel, with or without a battery; the AOK-only
// checks are skipped there.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "test_common.h"

#define PS_DIR "/sys/class/power_supply"
#define UNPRIV_UID 1000
#define UNPRIV_GID 1000

static int check(const char *label, int cond) {
    if (cond) {
        test_logf("ok   %s\n", label);
        return 1;
    }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

// Read a small file whole. Returns the length, or -1 with errno set.
static ssize_t slurp(const char *path, char *buf, size_t bufsize) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t total = 0;
    while (total < bufsize - 1) {
        ssize_t n = read(fd, buf + total, bufsize - 1 - total);
        if (n < 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        if (n == 0)
            break;
        total += (size_t) n;
    }
    close(fd);
    buf[total] = '\0';
    return (ssize_t) total;
}

// A one-line attribute without its newline. 0 on success.
static int read_attr(const char *supply, const char *attr, char *buf, size_t bufsize) {
    char path[512];
    snprintf(path, sizeof(path), PS_DIR "/%s/%s", supply, attr);
    ssize_t n = slurp(path, buf, bufsize);
    if (n <= 0)
        return -1;
    if (buf[n - 1] != '\n')
        return -2;          // sysfs attributes end in exactly one newline
    buf[n - 1] = '\0';
    return strchr(buf, '\n') == NULL ? 0 : -2;
}

static int is_aok(void) {
    return access("/proc/ish", F_OK) == 0;
}

// The command-line build says so in /proc/ish/UIDevice.
static int is_aok_cli(void) {
    char buf[64];
    ssize_t n = slurp("/proc/ish/UIDevice", buf, sizeof(buf));
    return n > 0 && strncmp(buf, "standalone", 10) == 0;
}

// The same watch waybar's constructor takes, trailing slash and all.
static void check_watchable(const char *who) {
    char label[160];
    int ifd = inotify_init1(IN_CLOEXEC);
    snprintf(label, sizeof(label), "%s: inotify_init1", who);
    if (!check(label, ifd >= 0))
        return;
    int wd = inotify_add_watch(ifd, PS_DIR "/", IN_CREATE | IN_DELETE);
    snprintf(label, sizeof(label), "%s: inotify_add_watch(" PS_DIR "/, IN_CREATE|IN_DELETE) (errno %d)",
             who, wd < 0 ? errno : 0);
    check(label, wd >= 0);
    int wd2 = inotify_add_watch(ifd, PS_DIR, IN_CREATE | IN_DELETE);
    snprintf(label, sizeof(label), "%s: the same directory without the slash is the same watch", who);
    check(label, wd2 >= 0 && wd2 == wd);
    close(ifd);
}

static int count_entries(void) {
    DIR *d = opendir(PS_DIR);
    if (d == NULL)
        return -1;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
            n++;
    }
    closedir(d);
    return n;
}

// The uevent line for KEY, without the key. 0 when present.
static int uevent_value(const char *uevent, const char *key, char *out, size_t outsize) {
    size_t keylen = strlen(key);
    const char *line = uevent;
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t) (end - line) : strlen(line);
        if (len > keylen && strncmp(line, key, keylen) == 0 && line[keylen] == '=') {
            size_t vlen = len - keylen - 1;
            if (vlen >= outsize)
                return -1;
            memcpy(out, line + keylen + 1, vlen);
            out[vlen] = '\0';
            return 0;
        }
        if (end == NULL)
            break;
        line = end + 1;
    }
    return -1;
}

static int valid_status(const char *s) {
    return strcmp(s, "Unknown") == 0 || strcmp(s, "Charging") == 0 ||
           strcmp(s, "Discharging") == 0 || strcmp(s, "Not charging") == 0 ||
           strcmp(s, "Full") == 0;
}

// A battery's files, read twice if needed: the host can change state between
// two reads, and the question is whether they agree, not whether they raced.
static void check_battery(const char *name) {
    char label[400];
    char capacity[32], status[32], uevent[1024], value[64];
    int agreed = 0;
    for (int attempt = 0; attempt < 3 && !agreed; attempt++) {
        char path[512];
        snprintf(path, sizeof(path), PS_DIR "/%s/uevent", name);
        if (slurp(path, uevent, sizeof(uevent)) <= 0 ||
                read_attr(name, "capacity", capacity, sizeof(capacity)) != 0 ||
                read_attr(name, "status", status, sizeof(status)) != 0)
            break;
        char ue_cap[32], ue_status[32];
        agreed = uevent_value(uevent, "POWER_SUPPLY_CAPACITY", ue_cap, sizeof(ue_cap)) == 0 &&
                 uevent_value(uevent, "POWER_SUPPLY_STATUS", ue_status, sizeof(ue_status)) == 0 &&
                 strcmp(ue_cap, capacity) == 0 && strcmp(ue_status, status) == 0;
    }

    snprintf(label, sizeof(label), "%s: capacity, status and uevent read as one-line attributes", name);
    if (!check(label, read_attr(name, "capacity", capacity, sizeof(capacity)) == 0 &&
                      read_attr(name, "status", status, sizeof(status)) == 0))
        return;
    test_logf("     %s: capacity=%s status=%s\n", name, capacity, status);

    // An integer percentage. The /proc/ish/BAT0_capacity format ("83.00")
    // would read as 83 to some parsers and fail others outright.
    char *end;
    long cap = strtol(capacity, &end, 10);
    snprintf(label, sizeof(label), "%s: capacity is an integer 0-100 (got \"%s\")", name, capacity);
    check(label, capacity[0] != '\0' && *end == '\0' && cap >= 0 && cap <= 100);
    snprintf(label, sizeof(label), "%s: status is one Linux reports (got \"%s\")", name, status);
    check(label, valid_status(status));

    char present[8];
    if (read_attr(name, "present", present, sizeof(present)) == 0) {
        snprintf(label, sizeof(label), "%s: present is 0 or 1 (got \"%s\")", name, present);
        check(label, strcmp(present, "0") == 0 || strcmp(present, "1") == 0);
    }

    snprintf(label, sizeof(label), "%s: uevent names the supply", name);
    check(label, uevent_value(uevent, "POWER_SUPPLY_NAME", value, sizeof(value)) == 0 &&
                 strcmp(value, name) == 0);
    snprintf(label, sizeof(label), "%s: uevent says Battery", name);
    check(label, uevent_value(uevent, "POWER_SUPPLY_TYPE", value, sizeof(value)) == 0 &&
                 strcmp(value, "Battery") == 0);
    snprintf(label, sizeof(label), "%s: uevent's CAPACITY and STATUS agree with the files", name);
    check(label, agreed);
    snprintf(label, sizeof(label), "%s: uevent ends in a newline", name);
    check(label, uevent[0] != '\0' && uevent[strlen(uevent) - 1] == '\n');

    // waybar watches each battery's uevent for IN_ACCESS.
    char path[512];
    snprintf(path, sizeof(path), PS_DIR "/%s/uevent", name);
    int ifd = inotify_init1(IN_CLOEXEC);
    int wd = ifd >= 0 ? inotify_add_watch(ifd, path, IN_ACCESS) : -1;
    snprintf(label, sizeof(label), "%s: inotify_add_watch(uevent, IN_ACCESS)", name);
    check(label, wd >= 0);
    if (ifd >= 0)
        close(ifd);

    if (is_aok()) {
        // AOK knows a state and a percentage, and nothing else.
        static const char *const invented[] = {
            "energy_now", "energy_full", "energy_full_design", "charge_now",
            "charge_full", "charge_full_design", "voltage_now", "current_now",
            "power_now", "time_to_empty_now", "time_to_full_now",
        };
        for (size_t i = 0; i < sizeof(invented) / sizeof(invented[0]); i++) {
            snprintf(path, sizeof(path), PS_DIR "/%s/%s", name, invented[i]);
            snprintf(label, sizeof(label), "%s: no %s -- the host reports no such figure", name, invented[i]);
            check(label, access(path, F_OK) != 0 && errno == ENOENT);
        }
    }
}

static void check_mains(const char *name) {
    char label[400];
    char online[8], uevent[512], value[32];
    char path[512];
    snprintf(path, sizeof(path), PS_DIR "/%s/uevent", name);
    snprintf(label, sizeof(label), "%s: online reads as a one-line attribute", name);
    if (!check(label, read_attr(name, "online", online, sizeof(online)) == 0))
        return;
    snprintf(label, sizeof(label), "%s: online is 0, 1 or 2 (got \"%s\")", name, online);
    check(label, strcmp(online, "0") == 0 || strcmp(online, "1") == 0 || strcmp(online, "2") == 0);
    snprintf(label, sizeof(label), "%s: uevent says Mains", name);
    check(label, slurp(path, uevent, sizeof(uevent)) > 0 &&
                 uevent_value(uevent, "POWER_SUPPLY_TYPE", value, sizeof(value)) == 0 &&
                 strcmp(value, "Mains") == 0);
    snprintf(label, sizeof(label), "%s: uevent carries ONLINE", name);
    check(label, uevent_value(uevent, "POWER_SUPPLY_ONLINE", value, sizeof(value)) == 0);
}

static void check_supplies(void) {
    DIR *d = opendir(PS_DIR);
    if (!check("opendir(" PS_DIR ")", d != NULL))
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        char type[32], label[400];
        snprintf(label, sizeof(label), "%s: type reads as a one-line attribute", de->d_name);
        if (!check(label, read_attr(de->d_name, "type", type, sizeof(type)) == 0))
            continue;
        test_logf("     %s: type=%s\n", de->d_name, type);
        if (strcmp(type, "Battery") == 0)
            check_battery(de->d_name);
        else if (strcmp(type, "Mains") == 0)
            check_mains(de->d_name);
    }
    closedir(d);
}

// On AOK the listing and /proc/ish/BAT0_status come from the same cached host
// reading. An unknown state is the no-battery case, and it must mean an empty
// directory; a battery in the directory must mean a known state.
static void check_agrees_with_proc_ish(int entries) {
    char status[64];
    ssize_t n = slurp("/proc/ish/BAT0_status", status, sizeof(status));
    if (!check("/proc/ish/BAT0_status is readable", n > 0))
        return;
    int unknown = strncmp(status, "Unknown\n", 8) == 0;
    if (unknown)
        check("an Unknown battery state lists no supplies", entries == 0);
    else
        test_logf("     /proc/ish/BAT0_status: %s", status);

    struct stat st;
    int bat0 = stat(PS_DIR "/BAT0", &st) == 0;
    if (entries > 0)
        check("a listed battery means a known state", bat0 && !unknown);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    struct stat st;
    if (!check(PS_DIR " exists and is a directory", stat(PS_DIR, &st) == 0 && S_ISDIR(st.st_mode)))
        return finish_suite("sysfs_power_supply");

    int entries = count_entries();
    check("readdir(" PS_DIR ") works", entries >= 0);
    test_logf("     %d supplies listed\n", entries);

    check_watchable(geteuid() == 0 ? "root" : "unprivileged");
    // As root, also as the desktop user: the session that runs waybar is not
    // root, and a root-only pass would say nothing about it.
    if (geteuid() == 0) {
        fflush(NULL);
        pid_t child = fork();
        if (child == 0) {
            failures_total = 0;
            if (setgid(UNPRIV_GID) != 0 || setuid(UNPRIV_UID) != 0) {
                printf("FAIL could not drop to uid %d: %s\n", UNPRIV_UID, strerror(errno));
                fflush(NULL);
                _exit(1);
            }
            check_watchable("uid 1000");
            check("uid 1000: readdir(" PS_DIR ") agrees with root's", count_entries() == entries);
            fflush(NULL);
            _exit(failures_total > 250 ? 250 : (int) failures_total);
        }
        int status;
        if (check("fork for the unprivileged pass", child > 0 && waitpid(child, &status, 0) == child)) {
            if (WIFSIGNALED(status)) {
                printf("FAIL unprivileged pass died on signal %d\n", WTERMSIG(status));
                failures_total++;
            } else {
                failures_total += (unsigned) WEXITSTATUS(status);
            }
        }
    }

    check_supplies();

    // A name the listing does not include must not open either.
    if (entries == 0) {
        errno = 0;
        check("an empty directory's BAT0 is ENOENT", stat(PS_DIR "/BAT0", &st) != 0 && errno == ENOENT);
        errno = 0;
        check("an empty directory's AC is ENOENT", stat(PS_DIR "/AC", &st) != 0 && errno == ENOENT);
        errno = 0;
        check("an empty directory's BAT0/capacity is ENOENT",
              open(PS_DIR "/BAT0/capacity", O_RDONLY) < 0 && errno == ENOENT);
    }

    if (is_aok()) {
        check_agrees_with_proc_ish(entries);
        if (is_aok_cli())
            check("the command-line build has no battery source, so lists no supplies", entries == 0);
    }

    return finish_suite("sysfs_power_supply");
}
