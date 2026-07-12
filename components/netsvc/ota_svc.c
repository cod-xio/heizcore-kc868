/* ota_svc.c – OTA-Updates: HTTPS-Download oder Upload über Weboberfläche,
 * Rollback über die zweite OTA-Partition (Bootloader-Rollback aktiv). */
#include "netsvc.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

esp_err_t ota_svc_from_url(const char *url)
{
    ESP_LOGI(TAG, "OTA von %s", url);
    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_err_t err = esp_https_ota(&cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA erfolgreich – Neustart in 3 s");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
    ESP_LOGE(TAG, "OTA fehlgeschlagen: %s", esp_err_to_name(err));
    return err;
}

esp_err_t ota_svc_rollback(void)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) return ESP_ERR_NOT_FOUND;
    esp_err_t err = esp_ota_set_boot_partition(next);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "Rollback auf %s – Neustart", next->label);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    return err;
}
