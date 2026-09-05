#pragma once

/* Starts the DHCP relay task. Requires the built-in DHCP server to
 * be OFF on the Ethernet side first (`set_eth_dhcps off`) or this
 * will fail to bind port 67. Relays client requests from the
 * Ethernet segment to the WiFi uplink's own gateway (assumed to be
 * the real DHCP server - true for the overwhelming majority of home
 * networks), and broadcasts server replies back onto the Ethernet
 * segment so real devices get addresses in the main subnet. */
void proxyarp_dhcp_relay_start(void);
