#pragma once

/* Waits for wifiSTA/ethNetif/eth_handle to be ready, then initializes
 * the ARP capture/injection hooks and starts the real proxy-ARP task.
 * Call once from app_main(), after arptab_init(). Replaces the
 * earlier proxyarp_test_seed_start() - remove that call if present. */
void proxyarp_startup_begin(void);
