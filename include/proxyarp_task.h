#pragma once
#include "lwip/netif.h"

/* Must be called before proxyarp_task_start(), so the task knows
 * which lwIP netif corresponds to which captured interface. */
void proxyarp_task_set_netifs(struct netif *wifi_netif, struct netif *eth_netif);

/* Starts the background task that consumes captured ARP frames from
 * proxyarp_netif_hooks.c's queue, learns real host entries into
 * arptab, and answers proxy ARP requests on their behalf. Call once
 * from app_main(), after proxyarp_hooks_init() and arptab_init(). */
void proxyarp_task_start(void);
