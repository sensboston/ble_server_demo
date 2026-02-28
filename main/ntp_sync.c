#include "ntp_sync.h"
#include "config.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"

#define TAG "NTP_SYNC"

void ntp_sync_start(void)
{
    // Set local timezone before starting sync
    setenv("TZ", TIMEZONE, 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    esp_netif_sntp_init(&config);

    ESP_LOGI(TAG, "NTP sync started, server: %s, timezone: %s (zip: %s)",
             NTP_SERVER, TIMEZONE, ZIP_CODE);
}
