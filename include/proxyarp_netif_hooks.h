#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#pragma pack(push, 1)
typedef struct {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;      /* 0x0806 for ARP, stored pre-swapped: 0x0608 */
} eth_hdr_t;

typedef struct {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint8_t  spa[4];
    uint8_t  tha[6];
    uint8_t  tpa[4];
} arp_hdr_t;

typedef struct {
    eth_hdr_t eth;
    arp_hdr_t arp;
} eth_arp_frame_t;
#pragma pack(pop)

#define ETHERTYPE_ARP_NET 0x0608

typedef enum {
    IFACE_WIFI_STA = 0,
    IFACE_ETH_W5500 = 1,
    IFACE_COUNT
} bridge_iface_t;

typedef struct {
    bridge_iface_t iface;
    eth_arp_frame_t frame;
} queued_arp_frame_t;

esp_err_t proxyarp_hooks_init(esp_eth_handle_t eth_handle, esp_netif_t *eth_netif);
QueueHandle_t proxyarp_get_rx_queue(void);
const uint8_t *proxyarp_get_iface_mac(bridge_iface_t iface);
esp_err_t wifi_send_raw_arp_frame(const eth_arp_frame_t *frame);
esp_err_t eth_send_raw_arp_frame(const eth_arp_frame_t *frame);
