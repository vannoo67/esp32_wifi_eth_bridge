#include "proxyarp_startup.h"
#include "proxyarp_netif_hooks.h"
#include "proxyarp_task.h"
#include "proxyarp_dhcp_relay.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "proxyarp_startup";

extern esp_netif_t *wifiSTA;
extern esp_netif_t *ethNetif;
extern esp_eth_handle_t eth_handle;
extern struct netif *esp_netif_get_netif_impl(esp_netif_t *esp_netif);

static void proxyarp_startup_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "waiting for wifiSTA, ethNetif, and eth_handle...");

    while (wifiSTA == NULL || ethNetif == NULL || eth_handle == NULL) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* Small extra margin past "non-NULL" to let the interfaces
     * actually settle - same caution as the earlier test seed task. */
    vTaskDelay(pdMS_TO_TICKS(2000));

    esp_err_t err = proxyarp_hooks_init(eth_handle, ethNetif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "proxyarp_hooks_init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    struct netif *wifi_lwip = esp_netif_get_netif_impl(wifiSTA);
    struct netif *eth_lwip = esp_netif_get_netif_impl(ethNetif);

    if (wifi_lwip == NULL || eth_lwip == NULL) {
        ESP_LOGE(TAG, "esp_netif_get_netif_impl returned NULL - aborting startup");
        vTaskDelete(NULL);
        return;
    }

    proxyarp_task_set_netifs(wifi_lwip, eth_lwip);
    proxyarp_task_start();

    /* Deliberately started here, inside this gated task, NOT as an
     * independent call in app_main(). relay_task calls socket() on
     * its first line with no waiting of its own - if started before
     * lwIP's own TCP/IP thread has finished initializing (a real risk
     * this early in boot), it hits "Invalid mbox" and crashes. This
     * task's own wait loop above already guarantees the network stack
     * is up by this point. */
    proxyarp_dhcp_relay_start();

    ESP_LOGI(TAG, "proxy-ARP fully initialized and running");

    vTaskDelete(NULL);
}

void proxyarp_startup_begin(void)
{
    xTaskCreate(proxyarp_startup_task, "proxyarp_startup", 4096, NULL, 5, NULL);
}
