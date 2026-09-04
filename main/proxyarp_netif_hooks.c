/*
 * proxyarp_netif_hooks.c
 *
 * Skeleton for the packet capture/injection layer needed to port
 * parprouted's arp.c onto ESP-IDF. This is the one piece with no
 * direct 1:1 replacement in the original codebase - everything else
 * (the arptab linked list, request queue, replace_entry/findentry/
 * remove_other_routes, refresharp) ports across largely unchanged.
 *
 * Two different capture strategies are used because WiFi STA and
 * Ethernet behave differently in ESP-IDF:
 *
 *   - WiFi STA: use promiscuous mode as a READ-ONLY tap. The normal
 *     esp-netif -> lwIP data path keeps running untouched in parallel;
 *     we just get a copy of every over-the-air frame to inspect for
 *     ARP. We never need to "forward" anything here.
 *
 *   - Ethernet (W5500): there is no separate promiscuous tap exposed
 *     the same way, so we install ourselves via
 *     esp_eth_update_input_path() as a wrapper around the normal
 *     input function. We inspect for ARP, then ALWAYS forward every
 *     frame on to esp_netif_receive() so normal IP/DHCP-relay traffic
 *     keeps flowing exactly as before. This is a tap-and-forward,
 *     not a replacement.
 *
 * IMPORTANT: esp_wifi_internal_tx(), esp_wifi_set_promiscuous_rx_cb(),
 * and esp_eth_update_input_path() are lower-level/quasi-internal APIs.
 * Exact signatures have shifted slightly across ESP-IDF releases -
 * check against your installed IDF version's headers
 * (esp_wifi_internal.h, esp_eth_com.h) before relying on this as-is.
 * Treat this file as an architecture sketch, not drop-in production code.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"   /* esp_wifi_internal_tx, promiscuous rx cb */
#include "esp_eth.h"
#include "esp_eth_com.h"        /* esp_eth_update_input_path */
#include "esp_netif.h"
#include "esp_log.h"

static const char *TAG = "proxyarp_hooks";

/* ---- Shared ARP frame definitions -------------------------------- */
/* lwIP's own etharp headers are laid out slightly differently and are
 * mostly private; easiest to just define our own, matching the BSD
 * layout parprouted's arp.c already assumes. Keeps arp.c's parsing
 * code unmodified. */

#include "proxyarp_netif_hooks.h"


/* Frames land here; your ported arp() task (adapted from arp.c's main
 * loop) reads from this instead of calling recv() on a raw socket. */
static QueueHandle_t s_arp_rx_queue = NULL;
#define ARP_RX_QUEUE_LEN 16

/* MAC addresses, filled in at init from esp_netif_get_mac() for each
 * interface - needed by arp_reply()/arp_req() when building frames. */
static uint8_t s_iface_mac[IFACE_COUNT][6];

/* esp_eth handle, needed for esp_eth_transmit() on the Ethernet side */
static esp_eth_handle_t s_eth_handle = NULL;

/* Netif handles, needed to call esp_netif_receive() when forwarding
 * non-intercepted-but-still-needed traffic on the Ethernet tap path */
static esp_netif_t *s_eth_netif = NULL;

/* -------------------------------------------------------------------
 * WiFi STA side: promiscuous read-only tap
 * ---------------------------------------------------------------- */

static void wifi_promisc_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    /* NO LONGER USED - kept only as a documented cautionary tale.
     * Promiscuous mode delivers RAW, UNDECRYPTED 802.11 frames for
     * WPA2/WPA3 traffic - confirmed empirically via hex dump, which
     * showed the Protected bit set and CCMP header bytes immediately
     * following the 802.11 header. No amount of correct 802.11
     * header-offset parsing would ever find a valid ARP structure
     * here; we were reading ciphertext. See wifi_rx_tap() below for
     * the actual working approach - hooking the internal rxcb at the
     * point where frames are already decrypted. */
    (void)buf;
    (void)type;
}

/* The real capture mechanism: register directly at the raw
 * esp_wifi_internal_reg_rxcb() slot for WIFI_IF_STA. This runs BELOW
 * esp-netif's own registration (which we are replacing) but ABOVE
 * the crypto engine - confirmed via wifi_sta_receive()'s own source,
 * which does no translation of its own, just a direct pass-through
 * to whatever's registered. Frames here are already decrypted and
 * already in clean 802.3 Ethernet format, matching what
 * esp_wifi_internal_tx() expects on the transmit side.
 *
 * Because this replaces esp-netif's own registration rather than
 * chaining to it (there is no documented way to retrieve what was
 * there before - a single-slot design, not stackable), we must
 * explicitly forward every frame to esp_netif_receive() ourselves -
 * same tap-and-forward obligation as the Ethernet side, just at a
 * different layer. wifiSTA is the same esp_netif_t* global already
 * used throughout this fork (esp32_nat_router.c, proxyarp_startup.c). */
extern esp_netif_t *wifiSTA;

static esp_err_t wifi_rx_tap(void *buffer, uint16_t len, void *eb)
{
    if (len >= sizeof(eth_arp_frame_t)) {
        eth_hdr_t *eth = (eth_hdr_t *)buffer;
        if (eth->ethertype == ETHERTYPE_ARP_NET) {
            queued_arp_frame_t qf;
            qf.iface = IFACE_WIFI_STA;
            memcpy(&qf.frame, buffer, sizeof(eth_arp_frame_t));
            xQueueSend(s_arp_rx_queue, &qf, 0);
        }
    }

    /* Always forward, exactly as wifi_sta_receive() would have -
     * normal STA networking (DHCP client, IP stack, everything else)
     * depends on this still happening. */
    return esp_netif_receive(wifiSTA, buffer, len, eb);
}

/* Inject a raw frame on the WiFi STA link. Used by your ported
 * arp_reply() / arp_req() in place of sendto() on an AF_PACKET socket. */
esp_err_t wifi_send_raw_arp_frame(const eth_arp_frame_t *frame)
{
    return esp_wifi_internal_tx(WIFI_IF_STA, (void *)frame, sizeof(*frame));
}

/* -------------------------------------------------------------------
 * Ethernet (W5500) side: tap-and-forward
 * ---------------------------------------------------------------- */

/* Original input function saved so we can still forward everything
 * to the normal esp-netif path after inspecting it. */
static esp_err_t (*s_orig_eth_input)(esp_eth_handle_t handle,
                                      uint8_t *buffer,
                                      uint32_t length,
                                      void *priv) = NULL;

static esp_err_t eth_tap_input(esp_eth_handle_t handle, uint8_t *buffer,
                                uint32_t length, void *priv)
{
    if (length >= sizeof(eth_arp_frame_t)) {
        eth_hdr_t *eth = (eth_hdr_t *)buffer;
        if (eth->ethertype == ETHERTYPE_ARP_NET) {
            queued_arp_frame_t qf;
            qf.iface = IFACE_ETH_W5500;
            memcpy(&qf.frame, buffer, sizeof(eth_arp_frame_t));
            xQueueSend(s_arp_rx_queue, &qf, 0);
            /* deliberately NOT returning here - still forward below,
             * so lwIP's own bookkeeping stays consistent too */
        }
    }

    /* Always forward to the normal path - non-ARP traffic (IoT devices'
     * actual IP traffic, DHCP relay packets you add later, etc.) must
     * keep flowing exactly as it did before this hook was installed. */
    if (s_orig_eth_input) {
        return s_orig_eth_input(handle, buffer, length, priv);
    }
    return esp_netif_receive(s_eth_netif, buffer, length, NULL);
}

/* Inject a raw frame on the Ethernet link. */
esp_err_t eth_send_raw_arp_frame(const eth_arp_frame_t *frame)
{
    return esp_eth_transmit(s_eth_handle, (void *)frame, sizeof(*frame));
}

/* -------------------------------------------------------------------
 * Init
 * ---------------------------------------------------------------- */

esp_err_t proxyarp_hooks_init(esp_eth_handle_t eth_handle,
                               esp_netif_t *eth_netif)
{
    s_eth_handle = eth_handle;
    s_eth_netif = eth_netif;

    s_arp_rx_queue = xQueueCreate(ARP_RX_QUEUE_LEN, sizeof(queued_arp_frame_t));
    if (!s_arp_rx_queue) {
        return ESP_ERR_NO_MEM;
    }

    /* Cache MAC addresses for use by the ported arp_reply()/arp_req() */
    esp_wifi_get_mac(WIFI_IF_STA, s_iface_mac[IFACE_WIFI_STA]);
    esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, s_iface_mac[IFACE_ETH_W5500]);

    /* WiFi: hook the raw internal rxcb, replacing esp-netif's own
     * registration (wifi_sta_receive -> esp_netif_receive) with our
     * own tap-and-forward wrapper. See wifi_rx_tap() above for why -
     * promiscuous mode only ever delivered undecrypted ciphertext. */
    esp_err_t wifi_rxcb_err = esp_wifi_internal_reg_rxcb(WIFI_IF_STA, wifi_rx_tap);
    if (wifi_rxcb_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_internal_reg_rxcb failed: %s", esp_err_to_name(wifi_rxcb_err));
        return wifi_rxcb_err;
    }

    /* Ethernet: tap-and-forward wrapper.
     * NOTE: esp_eth_update_input_path()'s exact signature (whether it
     * returns/exposes the previous handler for you to save, or expects
     * you to have captured it another way) varies by IDF version -
     * verify against esp_eth_com.h for your release. */
    esp_eth_update_input_path(eth_handle, eth_tap_input, NULL);

    ESP_LOGI(TAG, "proxy-ARP capture/injection hooks installed");
    return ESP_OK;
}

QueueHandle_t proxyarp_get_rx_queue(void)
{
    return s_arp_rx_queue;
}

const uint8_t *proxyarp_get_iface_mac(bridge_iface_t iface)
{
    return s_iface_mac[iface];
}
