idf_component_register(
    SRCS "net_manager.c" "mqtt_svc.c" "modbus_svc.c" "ota_svc.c"
    INCLUDE_DIRS "include"
    REQUIRES core board esp_wifi esp_eth esp_netif esp_event mqtt
             esp_https_ota app_update espressif__esp-modbus driver lwip
)
