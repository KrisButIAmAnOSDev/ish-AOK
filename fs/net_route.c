#include "fs/net_route.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#if defined(__APPLE__)
#include <net/if_var.h>
#include <sys/sysctl.h>
#if __has_include(<net/route.h>)
#include <net/route.h>
#else
// The iOS SDK ships no <net/route.h>. The routing messages a PF_ROUTE sysctl
// returns are XNU's, the same on iOS as on macOS, so this is that header's
// layout.
struct rt_metrics {
    u_int32_t rmx_locks;
    u_int32_t rmx_mtu;
    u_int32_t rmx_hopcount;
    int32_t rmx_expire;
    u_int32_t rmx_recvpipe;
    u_int32_t rmx_sendpipe;
    u_int32_t rmx_ssthresh;
    u_int32_t rmx_rtt;
    u_int32_t rmx_rttvar;
    u_int32_t rmx_pksent;
    u_int32_t rmx_filler[4];
};
struct rt_msghdr {
    u_short rtm_msglen;
    u_char rtm_version;
    u_char rtm_type;
    u_short rtm_index;
    int rtm_flags;
    int rtm_addrs;
    pid_t rtm_pid;
    int rtm_seq;
    int rtm_errno;
    int rtm_use;
    u_int32_t rtm_inits;
    struct rt_metrics rtm_rmx;
};
#define RTM_VERSION 5
#define RTF_UP 0x1
#define RTF_GATEWAY 0x2
#define RTF_HOST 0x4
#define RTF_IFSCOPE 0x1000000
#define RTAX_DST 0
#define RTAX_GATEWAY 1
#define RTAX_NETMASK 2
#define RTAX_MAX 8
#endif
_Static_assert(sizeof(struct rt_msghdr) == 92, "XNU rt_msghdr is 92 bytes");
#endif

struct route_iface_info {
    char ifname[IFNAMSIZ];
    unsigned flags;
    uint32_t mtu;
    uint32_t ifindex;
};

static bool route_name_has_prefix(const char *name, const char *prefix) {
    return strncmp(name, prefix, strlen(prefix)) == 0;
}

static bool route_name_hidden(const char *name) {
    return route_name_has_prefix(name, "awdl") ||
        route_name_has_prefix(name, "llw") ||
        route_name_has_prefix(name, "anpi");
}

static bool route_addr_is_link_local(uint32_t addr_be) {
    uint32_t addr = ntohl(addr_be);
    return (addr & 0xffff0000u) == 0xa9fe0000u;
}

static uint8_t route_prefixlen(uint32_t mask_be) {
    uint32_t mask = ntohl(mask_be);
    uint8_t prefix = 0;
    while ((mask & 0x80000000u) != 0) {
        prefix++;
        mask <<= 1;
    }
    return prefix;
}

static struct route_iface_info *route_iface_find(struct route_iface_info *infos, size_t count,
        const char *ifname) {
    for (size_t i = 0; i < count; i++) {
        if (strncmp(infos[i].ifname, ifname, sizeof(infos[i].ifname)) == 0)
            return &infos[i];
    }
    return NULL;
}

static int route_iface_upsert(struct route_iface_info **infos, size_t *count, const char *ifname,
        unsigned flags, uint32_t mtu, uint32_t ifindex) {
    struct route_iface_info *info = route_iface_find(*infos, *count, ifname);
    if (info != NULL) {
        info->flags = flags;
        if (mtu != 0)
            info->mtu = mtu;
        if (ifindex != 0)
            info->ifindex = ifindex;
        return 0;
    }
    struct route_iface_info *new_infos = realloc(*infos, sizeof(**infos) * (*count + 1));
    if (new_infos == NULL)
        return -1;
    *infos = new_infos;
    info = &new_infos[*count];
    memset(info, 0, sizeof(*info));
    strncpy(info->ifname, ifname, sizeof(info->ifname) - 1);
    info->flags = flags;
    info->mtu = mtu;
    info->ifindex = ifindex;
    (*count)++;
    return 0;
}

static int host_route_table_append(struct host_route_table *table, const struct host_route_entry *entry) {
    for (size_t i = 0; i < table->count; i++) {
        struct host_route_entry *existing = &table->entries[i];
        if (strncmp(existing->ifname, entry->ifname, sizeof(existing->ifname)) == 0 &&
                existing->destination_be == entry->destination_be &&
                existing->mask_be == entry->mask_be &&
                existing->gateway_be == entry->gateway_be &&
                existing->is_default == entry->is_default) {
            return 0;
        }
    }
    struct host_route_entry *new_entries = realloc(table->entries, sizeof(*new_entries) * (table->count + 1));
    if (new_entries == NULL)
        return -1;
    table->entries = new_entries;
    table->entries[table->count++] = *entry;
    return 0;
}

static int route_default_score(const char *ifname, unsigned flags, uint32_t addr_be) {
    if ((flags & IFF_UP) == 0 || (flags & IFF_RUNNING) == 0)
        return -1;
    if ((flags & IFF_LOOPBACK) != 0)
        return -1;
    if (route_name_hidden(ifname))
        return -1;
    if (route_addr_is_link_local(addr_be))
        return -1;

    int score = 200;
    if (route_name_has_prefix(ifname, "en") ||
            route_name_has_prefix(ifname, "bridge") ||
            route_name_has_prefix(ifname, "pdp_ip"))
        score = 400;
    else if (route_name_has_prefix(ifname, "ap"))
        score = 350;
    else if (route_name_has_prefix(ifname, "utun"))
        score = 300;
    if (flags & IFF_POINTOPOINT)
        score -= 25;
    return score;
}

// One of the host's IPv4 default routes through a router: what a Linux default
// route names with "via". getifaddrs() knows addresses, not routes, so these
// come from the host's routing table. `primary` marks the one the host actually
// sends through: Darwin's unscoped default (the others carry RTF_IFSCOPE), or
// the lowest metric on Linux.
struct host_default_gateway {
    uint32_t ifindex;
    uint32_t gateway_be;
    bool primary;
};

#define HOST_DEFAULT_GATEWAYS_MAX 16

#if defined(__APPLE__)
// A routing message trims each sockaddr to its significant bytes, so a netmask
// can be shorter than a sockaddr_in and a default route's mask can be empty.
// The bytes it leaves out are zero.
static uint32_t route_sockaddr_ipv4(const struct sockaddr *sa) {
    uint8_t bytes[4] = {0};
    size_t base = offsetof(struct sockaddr_in, sin_addr);
    for (size_t i = 0; i < sizeof(bytes); i++) {
        if (base + i < sa->sa_len)
            bytes[i] = ((const uint8_t *) sa)[base + i];
    }
    uint32_t addr_be;
    memcpy(&addr_be, bytes, sizeof(addr_be));
    return addr_be;
}

static size_t host_default_gateways_collect(struct host_default_gateway *out, size_t cap) {
    int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_GATEWAY};
    char *buf = NULL;
    size_t len = 0;
    for (int attempt = 0;; attempt++) {
        size_t want = 0;
        if (sysctl(mib, 6, NULL, &want, NULL, 0) != 0 || want == 0) {
            free(buf);
            return 0;
        }
        // Routes can appear between sizing the table and reading it.
        want += want / 4 + 512;
        char *grown = realloc(buf, want);
        if (grown == NULL) {
            free(buf);
            return 0;
        }
        buf = grown;
        len = want;
        if (sysctl(mib, 6, buf, &len, NULL, 0) == 0)
            break;
        if (errno != ENOMEM || attempt == 2) {
            free(buf);
            return 0;
        }
    }

    size_t count = 0;
    size_t off = 0;
    while (off + sizeof(struct rt_msghdr) <= len && count < cap) {
        const struct rt_msghdr *rtm = (const struct rt_msghdr *) (buf + off);
        if (rtm->rtm_msglen < sizeof(*rtm) || off + rtm->rtm_msglen > len)
            break;
        size_t end = off + rtm->rtm_msglen;
        if (rtm->rtm_version == RTM_VERSION &&
                (rtm->rtm_flags & (RTF_UP | RTF_GATEWAY)) == (RTF_UP | RTF_GATEWAY)) {
            const struct sockaddr *sas[RTAX_MAX] = {0};
            size_t sa_off = off + sizeof(*rtm);
            for (int i = 0; i < RTAX_MAX; i++) {
                if (!(rtm->rtm_addrs & (1 << i)))
                    continue;
                if (sa_off >= end)
                    break;
                const struct sockaddr *sa = (const struct sockaddr *) (buf + sa_off);
                // Each sockaddr is padded to a 4-byte boundary, and an empty one
                // still takes 4 bytes.
                size_t step = sa->sa_len > 0
                    ? 1 + (((size_t) sa->sa_len - 1) | (sizeof(uint32_t) - 1))
                    : sizeof(uint32_t);
                if (sa_off + step > end)
                    break;
                sas[i] = sa;
                sa_off += step;
            }
            const struct sockaddr *dst = sas[RTAX_DST];
            const struct sockaddr *gateway = sas[RTAX_GATEWAY];
            const struct sockaddr *mask = sas[RTAX_NETMASK];
            bool is_default = dst != NULL && dst->sa_family == AF_INET &&
                route_sockaddr_ipv4(dst) == 0 &&
                (mask != NULL ? route_sockaddr_ipv4(mask) == 0 : !(rtm->rtm_flags & RTF_HOST));
            // A gateway that is not an IPv4 address (link#N, as a utun default
            // has) names no router.
            if (is_default && gateway != NULL && gateway->sa_family == AF_INET &&
                    gateway->sa_len >= sizeof(struct sockaddr_in) &&
                    route_sockaddr_ipv4(gateway) != 0) {
                out[count].ifindex = rtm->rtm_index;
                out[count].gateway_be = route_sockaddr_ipv4(gateway);
                out[count].primary = !(rtm->rtm_flags & RTF_IFSCOPE);
                count++;
            }
        }
        off = end;
    }
    free(buf);
    return count;
}
#elif defined(__linux__)
static size_t host_default_gateways_collect(struct host_default_gateway *out, size_t cap) {
    FILE *routes = fopen("/proc/net/route", "r");
    if (routes == NULL)
        return 0;
    char line[256];
    size_t count = 0;
    long best_metric = -1;
    size_t best = 0;
    // Destination, Gateway and Mask are the raw network-order words, printed
    // with %08X, so reading them back as integers gives network order again.
    while (fgets(line, sizeof(line), routes) != NULL && count < cap) {
        char ifname[IFNAMSIZ];
        unsigned destination, gateway, flags, mask;
        int refcnt, use;
        long metric;
        if (sscanf(line, "%15s %x %x %x %d %d %ld %x", ifname, &destination, &gateway, &flags,
                    &refcnt, &use, &metric, &mask) != 8)
            continue;
        if (destination != 0 || mask != 0 || gateway == 0 ||
                (flags & (HOST_ROUTE_PROC_FLAG_UP | HOST_ROUTE_PROC_FLAG_GATEWAY)) !=
                (HOST_ROUTE_PROC_FLAG_UP | HOST_ROUTE_PROC_FLAG_GATEWAY))
            continue;
        uint32_t ifindex = if_nametoindex(ifname);
        if (ifindex == 0)
            continue;
        out[count].ifindex = ifindex;
        out[count].gateway_be = gateway;
        out[count].primary = false;
        if (best_metric < 0 || metric < best_metric) {
            best_metric = metric;
            best = count;
        }
        count++;
    }
    fclose(routes);
    if (count != 0)
        out[best].primary = true;
    return count;
}
#else
static size_t host_default_gateways_collect(struct host_default_gateway *out, size_t cap) {
    (void) out;
    (void) cap;
    return 0;
}
#endif

// The router for a default route leaving by `ifindex`: the host's primary
// default when it leaves there, otherwise any per-interface default that does.
static uint32_t host_default_gateway_for(const struct host_default_gateway *gateways, size_t count,
        uint32_t ifindex) {
    uint32_t found = 0;
    for (size_t i = 0; i < count; i++) {
        if (gateways[i].ifindex != ifindex)
            continue;
        if (gateways[i].primary)
            return gateways[i].gateway_be;
        if (found == 0)
            found = gateways[i].gateway_be;
    }
    return found;
}

int host_route_table_collect(struct host_route_table *table) {
    memset(table, 0, sizeof(*table));

    struct ifaddrs *addrs = NULL;
    if (getifaddrs(&addrs) != 0)
        return -1;

    struct host_default_gateway gateways[HOST_DEFAULT_GATEWAYS_MAX];
    size_t gateway_count = host_default_gateways_collect(gateways, HOST_DEFAULT_GATEWAYS_MAX);
    uint32_t primary_ifindex = 0;
    for (size_t i = 0; i < gateway_count; i++) {
        if (gateways[i].primary) {
            primary_ifindex = gateways[i].ifindex;
            break;
        }
    }

    struct route_iface_info *ifinfos = NULL;
    size_t ifinfos_count = 0;
    for (const struct ifaddrs *cursor = addrs; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_name == NULL || cursor->ifa_addr == NULL)
            continue;
#if defined(__APPLE__)
        if (cursor->ifa_addr->sa_family == AF_LINK) {
            uint32_t mtu = 0;
            if (cursor->ifa_data != NULL) {
                const struct if_data *stats = (const struct if_data *) cursor->ifa_data;
                mtu = stats->ifi_mtu;
            }
            if (route_iface_upsert(&ifinfos, &ifinfos_count, cursor->ifa_name,
                    cursor->ifa_flags, mtu, if_nametoindex(cursor->ifa_name)) < 0) {
                free(ifinfos);
                freeifaddrs(addrs);
                host_route_table_free(table);
                return -1;
            }
        }
#endif
    }

    struct host_route_entry default_route = {};
    int default_score = -1;
    bool default_is_primary = false;
    for (const struct ifaddrs *cursor = addrs; cursor != NULL; cursor = cursor->ifa_next) {
        if (cursor->ifa_name == NULL || cursor->ifa_addr == NULL || cursor->ifa_netmask == NULL)
            continue;
        if (cursor->ifa_addr->sa_family != AF_INET)
            continue;

        struct route_iface_info *ifinfo = route_iface_find(ifinfos, ifinfos_count, cursor->ifa_name);
        unsigned flags = ifinfo != NULL ? ifinfo->flags : cursor->ifa_flags;
        uint32_t mtu = ifinfo != NULL ? ifinfo->mtu : 0;
        uint32_t ifindex = ifinfo != NULL && ifinfo->ifindex != 0 ? ifinfo->ifindex : if_nametoindex(cursor->ifa_name);

        struct sockaddr_in *addr_in = (struct sockaddr_in *) cursor->ifa_addr;
        struct sockaddr_in *mask_in = (struct sockaddr_in *) cursor->ifa_netmask;
        uint32_t addr_be = addr_in->sin_addr.s_addr;
        uint32_t mask_be = mask_in->sin_addr.s_addr;
        if (mask_be == 0 && !(flags & IFF_LOOPBACK))
            continue;

        if (!route_name_hidden(cursor->ifa_name) && !route_addr_is_link_local(addr_be)) {
            struct host_route_entry route = {};
            strncpy(route.ifname, cursor->ifa_name, sizeof(route.ifname) - 1);
            route.ifindex = ifindex;
            route.destination_be = addr_be & mask_be;
            route.gateway_be = 0;
            route.mask_be = mask_be;
            route.prefsrc_be = addr_be;
            route.mtu = mtu != 0 ? mtu : 1500;
            route.prefix_len = route_prefixlen(mask_be);
            route.scope = (flags & IFF_LOOPBACK) ? HOST_ROUTE_SCOPE_HOST : HOST_ROUTE_SCOPE_LINK;
            route.protocol = HOST_ROUTE_PROTOCOL_KERNEL;
            route.proc_flags = HOST_ROUTE_PROC_FLAG_UP;
            if (mask_be == htonl(0xffffffffu))
                route.proc_flags |= HOST_ROUTE_PROC_FLAG_HOST;
            if (host_route_table_append(table, &route) < 0) {
                free(ifinfos);
                freeifaddrs(addrs);
                host_route_table_free(table);
                return -1;
            }
        }

        // The interface the host's own primary default route leaves by wins
        // outright when it has a usable address; the name heuristic only
        // chooses when the host's routing table could not be read or does not
        // name one of these interfaces.
        int score = route_default_score(cursor->ifa_name, flags, addr_be);
        bool is_primary = score >= 0 && primary_ifindex != 0 && ifindex == primary_ifindex;
        if (default_is_primary)
            continue;
        if (is_primary || score > default_score ||
                (score == default_score && default_route.ifindex != 0 && ifindex < default_route.ifindex)) {
            default_score = score;
            default_is_primary = is_primary;
            memset(&default_route, 0, sizeof(default_route));
            strncpy(default_route.ifname, cursor->ifa_name, sizeof(default_route.ifname) - 1);
            default_route.ifindex = ifindex;
            default_route.destination_be = 0;
            default_route.gateway_be = 0;
            default_route.mask_be = 0;
            default_route.prefsrc_be = addr_be;
            default_route.mtu = mtu != 0 ? mtu : 1500;
            default_route.prefix_len = 0;
            default_route.protocol = HOST_ROUTE_PROTOCOL_BOOT;
            default_route.is_default = true;
        }
    }

    if (default_score >= 0) {
        // A default route through a router is "via" it, scope global, and
        // flagged G, as on Linux. Without one it is a link-scope route straight
        // out of the interface, and must not claim RTF_GATEWAY: /proc/net/route
        // used to set G beside a zero gateway, and rtnetlink never carried
        // RTA_GATEWAY at all, so waybar, which takes the default route to be
        // the one with a gateway, found none and showed "Disconnected".
        default_route.gateway_be = host_default_gateway_for(gateways, gateway_count,
                default_route.ifindex);
        default_route.proc_flags = HOST_ROUTE_PROC_FLAG_UP;
        if (default_route.gateway_be != 0) {
            default_route.scope = HOST_ROUTE_SCOPE_UNIVERSE;
            default_route.proc_flags |= HOST_ROUTE_PROC_FLAG_GATEWAY;
        } else {
            default_route.scope = HOST_ROUTE_SCOPE_LINK;
        }
        if (host_route_table_append(table, &default_route) < 0) {
            free(ifinfos);
            freeifaddrs(addrs);
            host_route_table_free(table);
            return -1;
        }
    }

    free(ifinfos);
    freeifaddrs(addrs);
    return 0;
}

void host_route_table_free(struct host_route_table *table) {
    free(table->entries);
    table->entries = NULL;
    table->count = 0;
}
