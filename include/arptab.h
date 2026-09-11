#pragma once
/*
 * Derived from parprouted with Vladimir Ivashchenko's consent.
 *
 * The ARP table structure and lookup logic here (arptab_replace_entry,
 * arptab_find, arptab_remove_other_routes) are a port of parprouted's
 * arptab linked list (https://github.com/Adellica/parprouted,
 * parprouted.c). Vladimir Ivashchenko, parprouted's original author,
 * has kindly agreed to license the relevant parprouted code under MIT
 * specifically to allow its use here, alongside esp32_ethernet_router
 * (https://github.com/martin-ger/esp32_ethernet_router), which this
 * project is built on.
 */
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

/*
 * Ported from parprouted.c's ARPTAB_ENTRY linked list.
 *
 * Differences from the original:
 *   - ifname (char*) -> netif (struct netif*). We only ever have two
 *     interfaces, so a direct pointer is simpler and avoids string
 *     comparisons parprouted needed for its N-interface generality.
 *   - route_added / route_add() / route_remove() are GONE. The
 *     original shelled out to `ip route add/del ... dev X`. On
 *     ESP32 there's no separate kernel routing table to update -
 *     want_route here is consulted directly by ip4_route_src_hook()
 *     in lwip_hooks.c at packet-send time. No side-effecting "install
 *     the route" step is needed; the table itself IS the route.
 */

typedef struct arptab_entry {
    ip4_addr_t ipaddr;
    struct netif *netif;
    uint8_t hwaddr[6];
    time_t tstamp;
    int incomplete;
    int want_route;
    struct arptab_entry *next;
} arptab_entry_t;

#define ARP_TABLE_ENTRY_TIMEOUT_DEFAULT 300  /* seconds, matches parprouted default-ish */

void arptab_init(void);

/* Runtime-configurable entry timeout, in seconds. Defaults to
 * ARP_TABLE_ENTRY_TIMEOUT_DEFAULT until explicitly set (typically
 * from persisted config at boot, or live via CLI/WebUI). */
void arptab_set_timeout(int seconds);
int arptab_get_timeout(void);

/* Find-or-create an entry for (ip, netif). Mirrors replace_entry().
 * If out_is_new is non-NULL, set to true only when a brand-new entry
 * was created (not when refreshing an existing one) - lets callers
 * distinguish "just learned this host for the first time" from
 * "saw traffic from an already-known host again". */
arptab_entry_t *arptab_replace_entry(const ip4_addr_t *ip, struct netif *netif, bool *out_is_new);

/* Mirrors findentry(): does this IP exist anywhere in the table? */
int arptab_find(const ip4_addr_t *ip);

/* Mirrors remove_other_routes(): mark entries for this IP on OTHER
 * interfaces as no-longer-wanted, since we just saw it on `netif`. */
int arptab_remove_other_routes(const ip4_addr_t *ip, struct netif *netif);

/* Mirrors processarp()'s cleanup pass, minus the route_add/route_remove
 * shell-outs - just expires stale entries and drops want_route=0 ones. */
void arptab_age_out(void);

/* Used by ip4_route_src_hook(). Returns the netif to route this
 * destination through, or NULL if unknown / not wanted. */
struct netif *arptab_lookup_netif(const ip4_addr_t *ip);

/* Number of currently-active (want_route) entries - for status/UI display. */
int arptab_count(void);

/* Correctly formats a netif's name for display/logging. netif->name is
 * NOT a null-terminated string - it's a fixed 2-byte array, with the
 * numeric suffix in a separate 'num' field. Never printf("%s", ...)
 * a netif->name directly. buf should be at least 8 bytes. */
void netif_name_str(struct netif *netif, char *buf, size_t buflen);

/* Prints a formatted table of all entries to stdout - for a CLI
 * `show arptab` command. */
void arptab_print(void);
