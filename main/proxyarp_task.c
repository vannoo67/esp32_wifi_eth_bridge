/*
 * Derived from parprouted with Vladimir Ivashchenko's consent.
 * See proxyarp_task.h / arptab.h for details.
 */
#include <string.h>
#include "proxyarp_task.h"
#include "proxyarp_netif_hooks.h"
#include "arptab.h"
#include "lwip/def.h"    /* htons/ntohs */
#include "lwip/netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "proxyarp_task";

#define ARPOP_REQUEST 1
#define ARPOP_REPLY   2

/* Convenience: which lwIP netif and interface MAC correspond to a
 * captured frame's origin. Assumes exactly two interfaces, matching
 * bridge_iface_t from proxyarp_netif_hooks.h. Fill in the actual
 * netif pointers from your app - see proxyarp_task_start() below. */
static struct netif *s_iface_netif[IFACE_COUNT];

void proxyarp_task_set_netifs(struct netif *wifi_netif, struct netif *eth_netif)
{
    s_iface_netif[IFACE_WIFI_STA] = wifi_netif;
    s_iface_netif[IFACE_ETH_W5500] = eth_netif;
}

static bridge_iface_t other_iface(bridge_iface_t iface)
{
    return (iface == IFACE_WIFI_STA) ? IFACE_ETH_W5500 : IFACE_WIFI_STA;
}

static esp_err_t send_on_iface(bridge_iface_t iface, const eth_arp_frame_t *frame)
{
    if (iface == IFACE_WIFI_STA) {
        return wifi_send_raw_arp_frame(frame);
    } else {
        return eth_send_raw_arp_frame(frame);
    }
}

/* Builds a raw ARP frame. htons() applied explicitly throughout since
 * these fields go straight onto the wire - this is exactly the class
 * of bug (silently-wrong byte order rather than a crash) worth being
 * careful about, given how many other subtle bugs this port has
 * already turned up. */
static void build_arp_frame(eth_arp_frame_t *out,
                             uint16_t opcode,
                             const uint8_t eth_dst[6],
                             const uint8_t sender_mac[6],
                             const ip4_addr_t *sender_ip,
                             const uint8_t target_mac[6],
                             const ip4_addr_t *target_ip)
{
    memset(out, 0, sizeof(*out));

    memcpy(out->eth.dst_mac, eth_dst, 6);
    memcpy(out->eth.src_mac, sender_mac, 6);
    out->eth.ethertype = ETHERTYPE_ARP_NET; /* already stored pre-swapped */

    out->arp.htype = htons(1);      /* Ethernet */
    out->arp.ptype = htons(0x0800); /* IPv4 */
    out->arp.hlen = 6;
    out->arp.plen = 4;
    out->arp.oper = htons(opcode);
    memcpy(out->arp.sha, sender_mac, 6);
    memcpy(out->arp.spa, sender_ip, 4);
    memcpy(out->arp.tha, target_mac, 6);
    memcpy(out->arp.tpa, target_ip, 4);
}

static const uint8_t s_broadcast_mac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t s_zero_mac[6] = { 0 };

/* Learn the sender of ANY received ARP frame (request or reply both
 * reveal sender identity - parprouted learns from both, not just
 * replies). */
static void learn_sender(bridge_iface_t iface, const arp_hdr_t *arp)
{
    struct netif *netif = s_iface_netif[iface];
    if (netif == NULL) {
        return;
    }

    ip4_addr_t sender_ip;
    memcpy(&sender_ip, arp->spa, 4);

    if (ip4_addr_isany_val(sender_ip)) {
        return; /* e.g. a gratuitous probe with no sender IP yet */
    }

    bool is_new = false;
    arptab_entry_t *entry = arptab_replace_entry(&sender_ip, netif, &is_new);
    if (entry != NULL) {
        memcpy(entry->hwaddr, arp->sha, 6);
        arptab_remove_other_routes(&sender_ip, netif);
        if (is_new) {
            char ifname[8];
            netif_name_str(netif, ifname, sizeof(ifname));
            ESP_LOGI(TAG, "learned %s on %s", ip4addr_ntoa(&sender_ip), ifname);
        }
    }
}

/* If someone asks "who has <target>?" on one interface, and we know
 * that host lives on the OTHER interface, answer on their behalf
 * using our own MAC for this interface - real proxy ARP. */
static void maybe_proxy_reply(bridge_iface_t iface, const arp_hdr_t *arp)
{
    ip4_addr_t target_ip, requester_ip;
    memcpy(&target_ip, arp->tpa, 4);
    memcpy(&requester_ip, arp->spa, 4);

    struct netif *known_netif = arptab_lookup_netif(&target_ip);
    struct netif *this_netif = s_iface_netif[iface];

    if (known_netif == NULL || known_netif == this_netif) {
        /* Not known, or known on the SAME interface that asked -
         * nothing to proxy. If unknown, actively probe the other
         * interface so we might learn it for next time. */
        if (known_netif == NULL) {
            eth_arp_frame_t probe;
            bridge_iface_t probe_iface = other_iface(iface);
            const uint8_t *our_mac = proxyarp_get_iface_mac(probe_iface);
            /* Use our REAL IP on the outgoing interface as sender,
             * not 0.0.0.0. A 0.0.0.0-sourced ARP request is RFC 5227
             * ACD-probe format (duplicate-address detection), not an
             * ordinary "who has" query - many stacks handle it
             * differently and may not generate a reply at all, which
             * would make active discovery of previously-unseen hosts
             * fail silently and consistently, not just occasionally. */
            const ip4_addr_t *probe_src_ip = netif_ip4_addr(s_iface_netif[probe_iface]);
            if (probe_src_ip == NULL || ip4_addr_isany_val(*probe_src_ip)) {
                ESP_LOGW(TAG, "probe interface has no IP yet, skipping probe for %s",
                         ip4addr_ntoa(&target_ip));
            } else {
                build_arp_frame(&probe, ARPOP_REQUEST, s_broadcast_mac,
                                 our_mac, probe_src_ip, s_zero_mac, &target_ip);
                send_on_iface(probe_iface, &probe);
                ESP_LOGD(TAG, "probing for unknown %s on other interface",
                         ip4addr_ntoa(&target_ip));
            }
        }
        return;
    }

    /* Send proxy reply back out the interface the request arrived on,
     * claiming target_ip with OUR OWN mac for that interface. */
    eth_arp_frame_t reply;
    const uint8_t *our_mac = proxyarp_get_iface_mac(iface);
    build_arp_frame(&reply, ARPOP_REPLY, arp->sha /* unicast back to requester */,
                     our_mac, &target_ip, arp->sha, &requester_ip);
    send_on_iface(iface, &reply);
    {
        char ifname[8];
        netif_name_str(this_netif, ifname, sizeof(ifname));
        ESP_LOGD(TAG, "proxy-replied for %s on %s",
                  ip4addr_ntoa(&target_ip), ifname);
    }
}

#define ARPTAB_AGE_CHECK_INTERVAL_MS 30000

static void proxyarp_task_fn(void *arg)
{
    (void)arg;
    QueueHandle_t q = proxyarp_get_rx_queue();
    queued_arp_frame_t qf;

    ESP_LOGI(TAG, "proxy-ARP task started");

    for (;;) {
        BaseType_t got_frame = xQueueReceive(q, &qf, pdMS_TO_TICKS(ARPTAB_AGE_CHECK_INTERVAL_MS));

        if (got_frame != pdTRUE) {
            /* Timed out waiting for a frame - a convenient, already-
             * running place to periodically age out stale entries,
             * rather than spawning a dedicated task just for this. */
            arptab_age_out();
            continue;
        }

        uint16_t opcode = ntohs(qf.frame.arp.oper);

        /* Every frame teaches us who the sender is, request or reply. */
        learn_sender(qf.iface, &qf.frame.arp);

        if (opcode == ARPOP_REQUEST) {
            maybe_proxy_reply(qf.iface, &qf.frame.arp);
        }
        /* ARPOP_REPLY: learn_sender() above already handled it - no
         * further action needed for a plain reply. */
    }
}

void proxyarp_task_start(void)
{
    xTaskCreate(proxyarp_task_fn, "proxyarp_task", 4096, NULL, 6, NULL);
}
