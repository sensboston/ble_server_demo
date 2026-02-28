idf_component_register(SRCS "main.c" "ble_server.c" "wifi_manager.c" "web_server.c" "ntp_sync.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES bt nvs_flash led_strip esp_wifi wifi_provisioning esp_event esp_netif esp_netif_sntp esp_http_server)