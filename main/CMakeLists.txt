idf_component_register(SRCS "esp32_nat_router.c"
                            "portmap.c"
                            "dhcp_manager.c"
                            "vpn_manager.c"
                            "netif_hooks.c"
                            "led_strip_status.c"
                            "lwip_hooks.c"
                            "arptab.c"
                            "proxyarp_netif_hooks.c"
                            "proxyarp_task.c"
                            "proxyarp_startup.c"
                            "proxyarp_dhcp_relay.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES pcap_capture esp_wifi esp_netif console nvs_flash driver esp_http_server fatfs cmd_router dhcpserver wpa_supplicant cmd_system acl remote_console syslog json esp_wireguard esp_eth http_server app_update led_strip eth_w5500 esp_system mqtt_ha espressif__mdns netflow esp32_link_eth)

target_link_libraries(${COMPONENT_LIB} INTERFACE "-u proxyarp_ip4_route_src_hook")

# Performance optimization for the hot-path packet processing code
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
