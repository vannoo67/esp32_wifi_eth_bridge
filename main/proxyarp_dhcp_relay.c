#include <string.h>
#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "proxyarp_dhcp_relay.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dhcp_relay";

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define BOOTREQUEST 1
#define BOOTREPLY   2

typedef struct {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint8_t  options[312];
} __attribute__((packed)) dhcp_packet_t;

extern esp_netif_t *wifiSTA;
extern esp_netif_t *ethNetif;

static uint32_t get_netif_ip(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        return ip_info.ip.addr;
    }
    return 0;
}

static uint32_t get_wifi_gateway(void)
{
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(wifiSTA, &ip_info) == ESP_OK) {
        return ip_info.gw.addr;
    }
    return 0;
}

/*
 * Single receiving socket, bound to INADDR_ANY:67 - deliberately NOT
 * bound to a specific interface IP. Binding to a specific unicast
 * address is a common gotcha here: it can silently prevent the
 * socket from ever seeing broadcast-destined DHCPDISCOVER packets,
 * since their destination (255.255.255.255) won't match a specific
 * bound address on many stacks.
 *
 * No interface-of-origin ambiguity in practice despite the single
 * ANY-bound socket: client requests (op==BOOTREQUEST, giaddr==0)
 * only ever arrive via Ethernet's own broadcast domain - they can't
 * "leak" onto WiFi at the network level regardless of our bind
 * address. Server replies (op==BOOTREPLY) are unicast, addressed
 * directly to our own WiFi IP - unambiguous by construction.
 */
static int s_rx_sock = -1;

/* Second socket, bound specifically to the Ethernet-facing IP, used
 * ONLY for sending the broadcast reply back to real clients. Unlike
 * receiving, a UDP socket's OUTGOING interface for a broadcast
 * destination genuinely is determined by its bound local address in
 * lwIP - this one needs the specific bind. */
static int s_eth_tx_sock = -1;

static void relay_task(void *arg)
{
    (void)arg;

    s_rx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_rx_sock < 0) {
        ESP_LOGE(TAG, "rx socket() failed");
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(s_rx_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in rx_addr = {0};
    rx_addr.sin_family = AF_INET;
    rx_addr.sin_port = htons(DHCP_SERVER_PORT);
    rx_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_rx_sock, (struct sockaddr *)&rx_addr, sizeof(rx_addr)) < 0) {
        ESP_LOGE(TAG, "rx bind() to port 67 failed - is the built-in "
                      "DHCP server still running? Run 'set_eth_dhcps off' "
                      "first, then restart this task.");
        close(s_rx_sock);
        s_rx_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    s_eth_tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_eth_tx_sock < 0) {
        ESP_LOGE(TAG, "eth tx socket() failed");
        close(s_rx_sock);
        vTaskDelete(NULL);
        return;
    }
    int bcast = 1;
    setsockopt(s_eth_tx_sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
    int eth_reuse = 1;
    setsockopt(s_eth_tx_sock, SOL_SOCKET, SO_REUSEADDR, &eth_reuse, sizeof(eth_reuse));

    struct sockaddr_in eth_tx_addr = {0};
    eth_tx_addr.sin_family = AF_INET;
    eth_tx_addr.sin_port = htons(DHCP_SERVER_PORT);
    eth_tx_addr.sin_addr.s_addr = get_netif_ip(ethNetif);

    if (bind(s_eth_tx_sock, (struct sockaddr *)&eth_tx_addr, sizeof(eth_tx_addr)) < 0) {
        ESP_LOGE(TAG, "eth tx bind() failed");
        close(s_rx_sock);
        close(s_eth_tx_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DHCP relay listening on port 67");

    uint8_t buf[600];
    struct sockaddr_in from;
    socklen_t fromlen;

    for (;;) {
        fromlen = sizeof(from);
        int len = recvfrom(s_rx_sock, buf, sizeof(buf), 0,
                            (struct sockaddr *)&from, &fromlen);
        if (len < (int)offsetof(dhcp_packet_t, options)) {
            continue; /* too short to be real DHCP/BOOTP */
        }

        dhcp_packet_t *pkt = (dhcp_packet_t *)buf;

        if (pkt->op == BOOTREQUEST && pkt->giaddr == 0) {
            /* Fresh client request, not yet relayed by anyone -
             * assumed to be from an Ethernet-side device. */
            uint32_t our_wifi_ip = get_netif_ip(wifiSTA);
            uint32_t server_ip = get_wifi_gateway();
            if (our_wifi_ip == 0 || server_ip == 0) {
                ESP_LOGW(TAG, "WiFi side not ready yet, dropping client request");
                continue;
            }

            pkt->giaddr = our_wifi_ip;
            if (pkt->hops < 255) {
                pkt->hops += 1;
            }

            struct sockaddr_in to = {0};
            to.sin_family = AF_INET;
            to.sin_port = htons(DHCP_SERVER_PORT);
            to.sin_addr.s_addr = server_ip;

            sendto(s_rx_sock, buf, len, 0, (struct sockaddr *)&to, sizeof(to));
            ESP_LOGI(TAG, "relayed client request upstream (xid=0x%08lx)",
                     (unsigned long)ntohl(pkt->xid));

        } else if (pkt->op == BOOTREPLY && pkt->giaddr == get_netif_ip(wifiSTA)) {
            /* Server reply addressed to us as the relay agent -
             * broadcast it back onto the Ethernet segment. */
            struct sockaddr_in to = {0};
            to.sin_family = AF_INET;
            to.sin_port = htons(DHCP_CLIENT_PORT);
            to.sin_addr.s_addr = htonl(INADDR_BROADCAST);

            sendto(s_eth_tx_sock, buf, len, 0, (struct sockaddr *)&to, sizeof(to));
            ESP_LOGI(TAG, "relayed server reply to Ethernet segment (xid=0x%08lx)",
                     (unsigned long)ntohl(pkt->xid));
        }
        /* else: not something we need to relay - ignore. */
    }
}

void proxyarp_dhcp_relay_start(void)
{
    xTaskCreate(relay_task, "dhcp_relay", 4096, NULL, 5, NULL);
}
