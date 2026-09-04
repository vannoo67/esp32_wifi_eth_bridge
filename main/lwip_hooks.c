#include "lwip_hooks.h"
#include "arptab.h"
#include "esp_log.h"

static const char *TAG = "ip4_route_hook";

/*
 * ESP-IDF's own default implementation (multi-netif source-address
 * matching) is still compiled into the build under its original
 * literal name - lwip_default_hooks.c's guard only checks whether
 * LWIP_HOOK_IP4_ROUTE_SRC is defined, not what it expands to. Rather
 * than reimplementing that matching logic ourselves, we forward-
 * declare and call it directly as our fallback path.
 */
extern struct netif *ip4_route_src_hook(const ip4_addr_t *src, const ip4_addr_t *dest);

/*
 * STAGE 1 check: this log line confirms the hook fires at all.
 * STAGE 2: arptab_lookup_netif() makes it live.
 * Fallback: anything not in our proxy-ARP table defers to ESP-IDF's
 * own default behavior, preserving whatever routing decisions it
 * was already making for you before this hook existed.
 */
struct netif *proxyarp_ip4_route_src_hook(const ip4_addr_t *src, const ip4_addr_t *dest)
{
    ESP_LOGW(TAG, "route hook called for dest %s", ip4addr_ntoa(dest));

    struct netif *target = arptab_lookup_netif(dest);
    if (target != NULL) {
        ESP_LOGW(TAG, "MATCHED - routing %s via learned netif %s",
                 ip4addr_ntoa(dest), target->name);
        return target;
    }

    ESP_LOGW(TAG, "no arptab match for %s - falling back to default", ip4addr_ntoa(dest));
    return ip4_route_src_hook(src, dest);
}

const ip4_addr_t *proxyarp_etharp_get_gw_hook(struct netif *netif, const ip4_addr_t *ipaddr)
{
    (void)netif;

    /* If we recognize this destination (regardless of which netif
     * routing picked - should already match, since ip4_route_src
     * ran first for this same packet), tell lwIP to ARP for it
     * directly rather than substituting a gateway address. */
    if (arptab_lookup_netif(ipaddr) != NULL) {
        ESP_LOGW(TAG, "etharp_get_gw: ARPing directly for %s instead of a gateway",
                 ip4addr_ntoa(ipaddr));
        return ipaddr;
    }

    /* Not one of ours - NULL defers to netif's normal gateway logic
     * (or ERR_RTE if none is configured, exactly as before). */
    return NULL;
}
