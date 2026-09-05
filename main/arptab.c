#include <string.h>
#include <stdio.h>
#include "arptab.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "arptab";
static arptab_entry_t *s_arptab = NULL;
static SemaphoreHandle_t s_arptab_mutex = NULL;

void arptab_init(void)
{
    s_arptab_mutex = xSemaphoreCreateMutex();
    s_arptab = NULL;
}

/* Ported near-verbatim from parprouted.c's replace_entry(), swapping
 * the ifname strcmp for a netif pointer comparison. */
arptab_entry_t *arptab_replace_entry(const ip4_addr_t *ip, struct netif *netif)
{
    if (s_arptab_mutex == NULL) {
        ESP_LOGW(TAG, "arptab_replace_entry called before arptab_init()");
        return NULL;
    }
    xSemaphoreTake(s_arptab_mutex, portMAX_DELAY);

    arptab_entry_t *cur = s_arptab;
    arptab_entry_t *prev = NULL;

    while (cur != NULL &&
           !(ip4_addr_cmp(&cur->ipaddr, ip) && cur->netif == netif)) {
        prev = cur;
        cur = cur->next;
    }

    if (cur == NULL) {
        cur = (arptab_entry_t *)calloc(1, sizeof(arptab_entry_t));
        if (cur == NULL) {
            ESP_LOGE(TAG, "no memory for new arptab entry");
            xSemaphoreGive(s_arptab_mutex);
            return NULL;
        }
        if (prev == NULL) {
            s_arptab = cur;
        } else {
            prev->next = cur;
        }
        cur->next = NULL;
        ESP_LOGD(TAG, "created new entry for %s", ip4addr_ntoa(ip));
    }

    /* Always (re)stamp these, whether just-created or an existing
     * match refreshed by a repeat sighting - this was the actual bug:
     * a freshly calloc'd entry never had ip/netif populated at all,
     * so ipaddr stayed 0.0.0.0 and lookups could never match it. */
    cur->ipaddr = *ip;
    cur->netif = netif;
    cur->tstamp = time(NULL);
    cur->want_route = 1;

    xSemaphoreGive(s_arptab_mutex);
    return cur;
}

int arptab_find(const ip4_addr_t *ip)
{
    if (s_arptab_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_arptab_mutex, portMAX_DELAY);
    arptab_entry_t *cur = s_arptab;
    while (cur != NULL && !ip4_addr_cmp(&cur->ipaddr, ip)) {
        cur = cur->next;
    }
    int found = (cur != NULL);
    xSemaphoreGive(s_arptab_mutex);
    return found;
}

/* Ported from remove_other_routes(): if this IP just showed up on
 * `netif`, any entries for the same IP on a DIFFERENT netif are now
 * stale (e.g. a device that moved, or a duplicate reply) - mark them
 * unwanted so arptab_age_out() cleans them up. */
int arptab_remove_other_routes(const ip4_addr_t *ip, struct netif *netif)
{
    if (s_arptab_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_arptab_mutex, portMAX_DELAY);
    int removed = 0;
    for (arptab_entry_t *cur = s_arptab; cur != NULL; cur = cur->next) {
        if (ip4_addr_cmp(&cur->ipaddr, ip) && cur->netif != netif) {
            if (cur->want_route) {
                ESP_LOGD(TAG, "marking %s on other netif for removal",
                         ip4addr_ntoa(ip));
            }
            cur->want_route = 0;
            removed++;
        }
    }
    xSemaphoreGive(s_arptab_mutex);
    return removed;
}

/* Ported from processarp()'s expiry pass. The original also called
 * route_add()/route_remove() here to shell out to `ip route`. That
 * step is gone entirely - there is nothing to "install", the table
 * itself is consulted live by arptab_lookup_netif() from the routing
 * hook. This function only prunes stale/unwanted entries. */
void arptab_age_out(void)
{
    if (s_arptab_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_arptab_mutex, portMAX_DELAY);

    arptab_entry_t *cur = s_arptab;
    arptab_entry_t *prev = NULL;
    time_t now = time(NULL);

    while (cur != NULL) {
        if (!cur->want_route || (now - cur->tstamp) > ARP_TABLE_ENTRY_TIMEOUT) {
            ESP_LOGD(TAG, "expiring entry for %s", ip4addr_ntoa(&cur->ipaddr));
            arptab_entry_t *dead = cur;
            if (prev != NULL) {
                prev->next = cur->next;
                cur = prev->next;
            } else {
                s_arptab = cur->next;
                cur = s_arptab;
            }
            free(dead);
        } else {
            prev = cur;
            cur = cur->next;
        }
    }

    xSemaphoreGive(s_arptab_mutex);
}

struct netif *arptab_lookup_netif(const ip4_addr_t *ip)
{
    if (s_arptab_mutex == NULL) {
        /* arptab_init() hasn't run yet - this is exactly what crashed
         * originally. Defer to lwIP's normal routing rather than
         * taking a null semaphore. */
        return NULL;
    }
    xSemaphoreTake(s_arptab_mutex, portMAX_DELAY);
    struct netif *result = NULL;
    for (arptab_entry_t *cur = s_arptab; cur != NULL; cur = cur->next) {
        if (ip4_addr_cmp(&cur->ipaddr, ip) && cur->want_route) {
            result = cur->netif;
            break;
        }
    }
    xSemaphoreGive(s_arptab_mutex);
    return result;
}

int arptab_count(void)
{
    if (s_arptab_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_arptab_mutex, portMAX_DELAY);
    int count = 0;
    for (arptab_entry_t *cur = s_arptab; cur != NULL; cur = cur->next) {
        if (cur->want_route) {
            count++;
        }
    }
    xSemaphoreGive(s_arptab_mutex);
    return count;
}

void netif_name_str(struct netif *netif, char *buf, size_t buflen)
{
    if (netif == NULL) {
        snprintf(buf, buflen, "?");
        return;
    }
    snprintf(buf, buflen, "%c%c%u", netif->name[0], netif->name[1], netif->num);
}

void arptab_print(void)
{
    if (s_arptab_mutex == NULL) {
        printf("(proxy-ARP table not initialized)\n");
        return;
    }
    xSemaphoreTake(s_arptab_mutex, portMAX_DELAY);
    time_t now = time(NULL);
    printf("%-16s %-9s %-17s %6s %s\n", "IP", "Interface", "MAC", "Age(s)", "Active");
    for (arptab_entry_t *cur = s_arptab; cur != NULL; cur = cur->next) {
        char ifname[8];
        netif_name_str(cur->netif, ifname, sizeof(ifname));
        printf("%-16s %-9s %02x:%02x:%02x:%02x:%02x:%02x %6ld %s\n",
               ip4addr_ntoa(&cur->ipaddr),
               ifname,
               cur->hwaddr[0], cur->hwaddr[1], cur->hwaddr[2],
               cur->hwaddr[3], cur->hwaddr[4], cur->hwaddr[5],
               (long)(now - cur->tstamp),
               cur->want_route ? "yes" : "no");
    }
    xSemaphoreGive(s_arptab_mutex);
}
