/**
 * HeizCore – Heizungsregler-Firmware für KinCony KC868-A6 / KC868-A8
 * ===================================================================
 * Vollständige witterungsgeführte Heizungsregelung nach dem Vorbild
 * des OEG KMS-D. Konfiguration ausschließlich über die integrierte
 * Weboberfläche – nach dem Flashen sofort einsatzbereit.
 *
 * Startreihenfolge:
 *   1. NVS + LittleFS
 *   2. Board-HAL (I²C-Portexpander, Sensorik, Relais)
 *   3. Konfiguration laden (LittleFS-JSON, sonst Werkszustand)
 *   4. Netzwerk (Ethernet/WLAN, Fallback: Access Point "HeizCore-Setup")
 *   5. Regelungs-Task (1-s-Zyklus)
 *   6. Dienste: HTTP/WS, MQTT, Modbus, NTP, OTA, Datenlogger
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "esp_vfs.h"
#include "esp_littlefs.h"   /* joltwallet__littlefs – API identisch zu esp-idf builtin */

#include "board.h"
#include "datamodel.h"
#include "config_store.h"
#include "alarms.h"
#include "control.h"
#include "netsvc.h"
#include "websrv.h"
#include "datalog.h"

static const char *TAG = "main";

static void mount_littlefs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = "/fs",
        .partition_label        = "storage",
        .partition               = NULL,     /* Partition-Descriptor, NULL = Label nutzen  */
        .format_if_mount_failed  = true,
        .read_only               = false,
        .dont_mount              = false,
        .grow_on_mount           = false,    /* Partition nicht automatisch ausdehnen       */
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&conf));

    size_t total = 0, used = 0;
    esp_littlefs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "LittleFS: %u/%u kB belegt", (unsigned)(used / 1024), (unsigned)(total / 1024));
}

void app_main(void)
{
    ESP_LOGI(TAG, "HeizCore %s startet …", FIRMWARE_VERSION);

    /* --- Persistenz ------------------------------------------------ */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    mount_littlefs();

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* --- Datenmodell + Konfiguration ------------------------------- */
    dm_init();                 /* zentrales, mutex-geschütztes Datenmodell   */
    alarms_init();
    cfg_load();                /* /fs/config.json  → Datenmodell             */

    /* --- Hardware --------------------------------------------------- */
    ESP_ERROR_CHECK(board_init(cfg_get()->board_model));

    /* --- Regelung ---------------------------------------------------- */
    control_start();           /* eigener Task, Priorität 5, 1-s-Zyklus      */

    /* --- Netzwerk & Dienste ------------------------------------------ */
    netsvc_start();            /* ETH/WLAN/AP, NTP, MQTT, Modbus, OTA        */
    websrv_start();            /* HTTP + REST + WebSocket + Auth             */
    datalog_start();           /* Ringpuffer-Logger auf LittleFS             */

    /* OTA: neues Image nach erfolgreichem Start als gültig markieren */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA-Image als gültig bestätigt");
    }

    ESP_LOGI(TAG, "System bereit – Weboberfläche unter http://%s/", netsvc_ip_str());
}
