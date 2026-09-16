// SIOCGIFCONF sizing, the way OpenJDK calls it (#572).
//
// OpenJDK's enumIPv4Interfaces() (NetworkInterface.c) first asks how big the
// interface list is: ioctl(SIOCGIFCONF) with ifc_buf = NULL and ifc_len never
// set, so ifc_len holds whatever was on the stack. Linux dev_ifconf() ignores
// ifc_len for a NULL buffer and reports the size needed; with a buffer, a
// negative ifc_len simply fits nothing. AOK's sock_ifconf() returned EINVAL
// for any negative ifc_len and reported 0 for a NULL buffer whose ifc_len was
// too small, so NetworkInterface.getNetworkInterfaces() threw (or saw no IPv4
// interfaces) depending on stack garbage, and Gradle failed with "Could not
// determine a usable wildcard IP for this machine".
//
// Uses the raw syscall so a libc wrapper cannot hide the kernel's answer.
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
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

static long ifconf(int fd, struct ifconf *ifc) {
    errno = 0;
    return syscall(SYS_ioctl, fd, SIOCGIFCONF, ifc);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    check(fd >= 0, "AF_INET dgram socket (%s)", strerror(errno));
    if (fd < 0)
        return finish_suite("sock_ifconf_sizing");

    // The reference: a buffer big enough for everything.
    struct ifreq reqs[64];
    struct ifconf ifc = {.ifc_len = sizeof(reqs), .ifc_buf = (char *) reqs};
    long r = ifconf(fd, &ifc);
    check(r == 0, "SIOCGIFCONF with a large buffer succeeds (r=%ld errno=%d)", r, errno);
    int full = ifc.ifc_len;
    check(full > 0 && full % sizeof(struct ifreq) == 0,
          "large buffer returns whole entries, at least loopback (ifc_len=%d)", full);

    // A NULL buffer is a size query whatever ifc_len holds.
    const int garbage[] = {0, 1, (int) sizeof(struct ifreq) - 1, -1, -4096, INT_MIN};
    for (size_t i = 0; i < sizeof(garbage) / sizeof(garbage[0]); i++) {
        ifc = (struct ifconf) {.ifc_len = garbage[i], .ifc_buf = NULL};
        r = ifconf(fd, &ifc);
        check(r == 0, "NULL-buffer size query with ifc_len=%d succeeds (r=%ld errno=%d)",
              garbage[i], r, errno);
        check(ifc.ifc_len == full, "NULL-buffer size query with ifc_len=%d reports %d (got %d)",
              garbage[i], full, ifc.ifc_len);
    }

    // With a buffer, a negative or too-small length fits nothing and is not an error.
    const int small[] = {-1, INT_MIN, 0, (int) sizeof(struct ifreq) - 1};
    for (size_t i = 0; i < sizeof(small) / sizeof(small[0]); i++) {
        memset(reqs, 0xa5, sizeof(reqs));
        ifc = (struct ifconf) {.ifc_len = small[i], .ifc_buf = (char *) reqs};
        r = ifconf(fd, &ifc);
        check(r == 0, "buffer with ifc_len=%d succeeds (r=%ld errno=%d)", small[i], r, errno);
        check(ifc.ifc_len == 0, "buffer with ifc_len=%d fits nothing (got %d)", small[i], ifc.ifc_len);
        check(((unsigned char *) reqs)[0] == 0xa5, "buffer with ifc_len=%d is left untouched", small[i]);
    }

    // Room for exactly one entry returns exactly one.
    ifc = (struct ifconf) {.ifc_len = sizeof(struct ifreq), .ifc_buf = (char *) reqs};
    r = ifconf(fd, &ifc);
    check(r == 0 && ifc.ifc_len == (int) sizeof(struct ifreq),
          "room for one entry returns one (r=%ld ifc_len=%d)", r, ifc.ifc_len);

    close(fd);
    return finish_suite("sock_ifconf_sizing");
}
