/* net_manager.c – Netzwerkaufbau
 *
 * Reihenfolge:
 *   1. Ethernet (nur KC868-A8, LAN8720 RMII) – wenn aktiviert
 *   2. WLAN-Station – wenn SSID konfiguriert
 *   3. Fallback nach 30 s ohne Verbindung: Access Point
 *      "HeizCore-Setup" (offen), Weboberfläche unter 192.168.4.1
 *
 * Nach IP-Bezug: NTP-Sync mit konfigurierter Zeitzone, danach
 * MQTT- und Modbus-Dienste starten.
 */
#include <string.h>
#include "netsvc.h"
#include "config_store.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_eth_driver.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_event.h"
#include "esp_sntp.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "net";
static char s_ip[16] = "0.0.0.0";
static EventGroupHandle_t s_ev;
#define EV_GOT_IP BIT0

const char *netsvc_ip_str(void) { return s_ip; }
bool netsvc_online(void) { return xEventGroupGetBits(s_ev) & EV_GOT_IP; }

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "IP bezogen: %s", s_ip);
    xEventGroupSetBits(s_ev, EV_GOT_IP);
}

static void wifi_sta_start(const dm_t *cfg)
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&icfg));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, cfg->net.wifi_ssid, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, cfg->net.wifi_pass, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();
    ESP_LOGI(TAG, "WLAN-Verbindung zu \"%s\" …", cfg->net.wifi_ssid);
}

static void wifi_ap_fallback(void)
{
    ESP_LOGW(TAG, "Keine Verbindung – starte Access Point \"HeizCore-Setup\"");
    esp_wifi_stop();
    esp_netif_create_default_wifi_ap();
    wifi_config_t wc = {
        .ap = {
            .ssid = "HeizCore-Setup",
            .ssid_len = 0,
            .channel = 6,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wc);
    esp_wifi_start();
    strcpy(s_ip, "192.168.4.1");
    xEventGroupSetBits(s_ev, EV_GOT_IP);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, EV_GOT_IP);
        esp_wifi_connect();     /* dauerhaft neu versuchen */
    }
}

#if CONFIG_ETH_USE_ESP32_EMAC
static void eth_start(void)
{
    /* LAN8720 an RMII, MDC=23, MDIO=18 (KC868-A8) */
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&ncfg);

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_mdc_gpio_num = 23;
    emac_cfg.smi_mdio_gpio_num = 18;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = 0;
    phy_cfg.reset_gpio_num = -1;
    /* lan87xx ist der neue Name ab ESP-IDF 5.0; älter: lan8720 */
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_cfg);

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t handle = NULL;
    if (esp_eth_driver_install(&eth_cfg, &handle) == ESP_OK) {
        esp_netif_attach(netif, esp_eth_new_netif_glue(handle));
        esp_eth_start(handle);
        ESP_LOGI(TAG, "Ethernet gestartet");
    } else {
        ESP_LOGW(TAG, "Kein Ethernet-PHY gefunden (KC868-A6?)");
    }
}
#endif

static void sntp_start(const dm_t *cfg)
{
    setenv("TZ", cfg->net.timezone, 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, cfg->net.ntp_server);
    esp_sntp_init();
}

static void net_task(void *arg)
{
    const dm_t *cfg = cfg_get();

    if (!(xEventGroupWaitBits(s_ev, EV_GOT_IP, pdFALSE, pdFALSE,
                              pdMS_TO_TICKS(30000)) & EV_GOT_IP)) {
        wifi_ap_fallback();
    }
    sntp_start(cfg);
    mqtt_svc_start();
    modbus_svc_start();
    vTaskDelete(NULL);
}

void netsvc_start(void)
{
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);

    const dm_t *cfg = cfg_get();
#if CONFIG_ETH_USE_ESP32_EMAC
    if (cfg->net.eth_enabled && cfg->board_model == 8) eth_start();
#endif
    if (cfg->net.wifi_enabled && cfg->net.wifi_ssid[0]) wifi_sta_start(cfg);
    else if (!cfg->net.eth_enabled || cfg->board_model != 8) wifi_ap_fallback();

    xTaskCreate(net_task, "net", 4096, NULL, 3, NULL);
}
