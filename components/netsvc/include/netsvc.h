#pragma once
#include "esp_err.h"
#include <stdbool.h>

void        netsvc_start(void);           /* ETH/WLAN/AP, NTP, MQTT, Modbus */
const char *netsvc_ip_str(void);
bool        netsvc_online(void);

/* MQTT */
void mqtt_svc_start(void);
void mqtt_svc_publish_state(void);        /* wird zyklisch aufgerufen        */
void mqtt_svc_reconfigure(void);

/* Modbus */
void modbus_svc_start(void);
void modbus_svc_reconfigure(void);

/* OTA */
esp_err_t ota_svc_from_url(const char *url);
esp_err_t ota_svc_rollback(void);
