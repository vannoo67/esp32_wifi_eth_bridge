#pragma once
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

/*
 * Renamed to proxyarp_ip4_route_src_hook to avoid colliding with
 * ESP-IDF's own default hook implementation. lwip_default_hooks.c
 * compiles a function under the literal name "ip4_route_src_hook"
 * whenever LWIP_HOOK_IP4_ROUTE_SRC is defined AT ALL - regardless of
 * what it's defined as. Reusing that exact name for our own function
 * caused a duplicate-symbol collision.
 *
 * lwIP calls whatever LWIP_HOOK_IP4_ROUTE_SRC expands to for every
 * outgoing IPv4 packet BEFORE falling back to normal subnet-match
 * routing.
 */
struct netif *proxyarp_ip4_route_src_hook(const ip4_addr_t *src, const ip4_addr_t *dest);

#define LWIP_HOOK_IP4_ROUTE_SRC(src, dest) proxyarp_ip4_route_src_hook(src, dest)

/*
 * Second hook, discovered necessary after Stage 2 testing: picking
 * the right netif (above) isn't enough on its own. etharp_output()
 * separately decides WHAT ADDRESS to actually ARP for - and if the
 * destination isn't inside the outgoing netif's own subnet, it ARPs
 * for netif->gw instead of the destination, or fails outright with
 * ERR_RTE if no gateway is configured. Since our Ethernet netif has
 * no gateway (never needed one before this project), every proxied
 * packet was silently dropped right here, with zero wire activity.
 *
 * For a proxy-ARP'd host, the fix is to tell lwIP to ARP for the
 * destination address directly, not a gateway - hence just returning
 * ipaddr unchanged when we recognize it.
 */
const ip4_addr_t *proxyarp_etharp_get_gw_hook(struct netif *netif, const ip4_addr_t *ipaddr);
#define LWIP_HOOK_ETHARP_GET_GW(netif, ipaddr) proxyarp_etharp_get_gw_hook(netif, ipaddr)
