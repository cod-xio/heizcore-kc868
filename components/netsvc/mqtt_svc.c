/* mqtt_svc.c – MQTT-Anbindung
 *
 * Topics (Präfix konfigurierbar, Standard "heizcore"):
 *   heizcore/status                  online/offline (LWT)
 *   heizcore/boiler/#                Kessel: temp, state, power, …
 *   heizcore/buffer/#                Puffer: t1…t5, charge, energy
 *   heizcore/dhw/#                   Warmwasser
 *   heizcore/solar/#                 Solar
 *   heizcore/hc/<n>/#                Heizkreise
 *   heizcore/relay/<n>/state         Relaiszustände
 *   heizcore/sensor/<n>/temp         alle logischen Fühler
 *   heizcore/alarm                   letzter Alarm (JSON)
 *
 * Befehle (subscribe):
 *   heizcore/hc/<n>/mode/set         0..7 (hc_mode_t)
 *   heizcore/hc/<n>/day_temp/set     Raumsoll Tag
 *   heizcore/dhw/setpoint/set
 *   heizcore/boiler/setpoint/set
 *   heizcore/sensor/<n>/value/set    für SRC_MQTT-Fühler
 *
 * Home-Assistant-Auto-Discovery: retained Config-Topics unter
 * homeassistant/... für Fühler, Relais, Kessel, WW und Heizkreise.
 */
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "netsvc.h"
#include "datamodel.h"
#include "config_store.h"
#include "alarms.h"
#include "mqtt_client.h"
#include "esp_log.h"

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_client;
static bool s_connected;
static char s_prefix[33] = "heizcore";

static void pubf(const char *sub, float v, int dec)
{
    if (!s_connected || isnan(v)) return;
    char topic[96], val[16];
    snprintf(topic, sizeof(topic), "%s/%s", s_prefix, sub);
    snprintf(val, sizeof(val), "%.*f", dec, v);
    esp_mqtt_client_publish(s_client, topic, val, 0, 0, false);
}

static void pubs(const char *sub, const char *v, bool retain)
{
    if (!s_connected) return;
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/%s", s_prefix, sub);
    esp_mqtt_client_publish(s_client, topic, v, 0, retain ? 1 : 0, retain);
}

/* ── Home-Assistant-Discovery ────────────────────────────────────── */
static void ha_discovery_sensor(const char *id, const char *name,
                                const char *state_sub, const char *unit,
                                const char *dev_class)
{
    char topic[128], payload[512];
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_%s/config", s_prefix, id);
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\","
        "\"stat_t\":\"%s/%s\",\"unit_of_meas\":\"%s\",%s%s%s"
        "\"avty_t\":\"%s/status\","
        "\"dev\":{\"ids\":[\"%s\"],\"name\":\"HeizCore\",\"mf\":\"HeizCore\","
        "\"mdl\":\"KC868\",\"sw\":\"%s\"}}",
        name, s_prefix, id, s_prefix, state_sub, unit,
        dev_class ? "\"dev_cla\":\"" : "", dev_class ? dev_class : "",
        dev_class ? "\"," : "",
        s_prefix, s_prefix, FIRMWARE_VERSION);
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);
}

static void ha_discovery_all(void)
{
    const dm_t *d = cfg_get();
    char id[24], name[48], sub[48];

    ha_discovery_sensor("outdoor", "Außentemperatur", "outdoor/temp", "°C", "temperature");
    if (d->boiler.enabled) {
        ha_discovery_sensor("boiler_temp", "Kesseltemperatur", "boiler/temp", "°C", "temperature");
        ha_discovery_sensor("boiler_power", "Kesselleistung", "boiler/power", "%", NULL);
    }
    if (d->buffer.enabled) {
        ha_discovery_sensor("buffer_top", "Puffer oben", "buffer/t1", "°C", "temperature");
        ha_discovery_sensor("buffer_mid", "Puffer Mitte", "buffer/t3", "°C", "temperature");
        ha_discovery_sensor("buffer_bot", "Puffer unten", "buffer/t5", "°C", "temperature");
        ha_discovery_sensor("buffer_charge", "Puffer Ladezustand", "buffer/charge", "%", NULL);
    }
    if (d->dhw.enabled)
        ha_discovery_sensor("dhw_temp", "Warmwasser", "dhw/temp", "°C", "temperature");
    if (d->solar.enabled) {
        ha_discovery_sensor("solar_coll", "Kollektor", "solar/collector", "°C", "temperature");
        ha_discovery_sensor("solar_yield", "Solarertrag heute", "solar/yield_day", "kWh", "energy");
    }
    for (int i = 0; i < DM_MAX_HC; i++) {
        if (!d->hc[i].enabled) continue;
        snprintf(id, sizeof(id), "hc%d_flow", i + 1);
        snprintf(name, sizeof(name), "%s Vorlauf", d->hc[i].name);
        snprintf(sub, sizeof(sub), "hc/%d/flow", i + 1);
        ha_discovery_sensor(id, name, sub, "°C", "temperature");
    }
    ESP_LOGI(TAG, "Home-Assistant-Discovery veröffentlicht");
}

/* ── eingehende Befehle ──────────────────────────────────────────── */
static void handle_command(const char *topic, const char *data)
{
    /* Erwartet: <prefix>/… – Präfix abschneiden */
    size_t pl = strlen(s_prefix);
    if (strncmp(topic, s_prefix, pl) != 0 || topic[pl] != '/') return;
    topic += pl + 1;
    float v = strtof(data, NULL);

    dm_t *d = dm_lock();
    int n;
    if (sscanf(topic, "hc/%d/mode/set", &n) == 1 && n >= 1 && n <= DM_MAX_HC) {
        int m = (int)v;
        if (m >= 0 && m <= HCM_OFF) d->hc[n - 1].mode = (hc_mode_t)m;
    } else if (sscanf(topic, "hc/%d/day_temp/set", &n) == 1 && n >= 1 && n <= DM_MAX_HC) {
        if (v >= 5 && v <= 30) d->hc[n - 1].room_set_day = v;
    } else if (strcmp(topic, "dhw/setpoint/set") == 0) {
        if (v >= 30 && v <= 70) d->dhw.setpoint = v;
    } else if (strcmp(topic, "boiler/setpoint/set") == 0) {
        if (v >= 40 && v <= 90) d->boiler.setpoint = v;
    } else if (sscanf(topic, "sensor/%d/value/set", &n) == 1 &&
               n >= 0 && n < DM_MAX_SENSORS && d->sensor[n].src == SRC_MQTT) {
        d->sensor[n].value = v;
        d->sensor[n].valid = true;
    }
    dm_unlock();
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        alarm_clear(ALM_NET_MQTT_DOWN);
        pubs("status", "online", true);
        char sub[64];
        snprintf(sub, sizeof(sub), "%s/+/+/set", s_prefix);
        esp_mqtt_client_subscribe(s_client, sub, 0);
        snprintf(sub, sizeof(sub), "%s/+/+/+/set", s_prefix);
        esp_mqtt_client_subscribe(s_client, sub, 0);
        if (cfg_get()->net.mqtt_discovery) ha_discovery_all();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        alarm_raise(ALARM_INFO, ALM_NET_MQTT_DOWN, "MQTT-Verbindung getrennt");
        break;
    case MQTT_EVENT_DATA: {
        char topic[128] = { 0 }, payload[64] = { 0 };
        int tl = e->topic_len < 127 ? e->topic_len : 127;
        int dl = e->data_len < 63 ? e->data_len : 63;
        memcpy(topic, e->topic, tl);
        memcpy(payload, e->data, dl);
        handle_command(topic, payload);
        break;
    }
    default: break;
    }
}

static void alarm_to_mqtt(const alarm_t *a)
{
    if (!s_connected) return;
    char json[192];
    snprintf(json, sizeof(json),
             "{\"level\":%d,\"code\":%d,\"msg\":\"%s\",\"t\":%lld}",
             a->level, a->code, a->message, (long long)a->timestamp);
    pubs("alarm", json, false);
}

void mqtt_svc_start(void)
{
    const dm_t *d = cfg_get();
    if (!d->net.mqtt_enabled || !d->net.mqtt_uri[0]) {
        ESP_LOGI(TAG, "MQTT deaktiviert");
        return;
    }
    strncpy(s_prefix, d->net.mqtt_prefix, sizeof(s_prefix) - 1);

    char lwt_topic[64];
    snprintf(lwt_topic, sizeof(lwt_topic), "%s/status", s_prefix);
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = d->net.mqtt_uri,
        .credentials.username = d->net.mqtt_user[0] ? d->net.mqtt_user : NULL,
        .credentials.authentication.password = d->net.mqtt_pass[0] ? d->net.mqtt_pass : NULL,
        .session.last_will = {
            .topic = lwt_topic, .msg = "offline", .qos = 1, .retain = true,
        },
    };
    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_mqtt_client_start(s_client);
    alarms_register_notify(alarm_to_mqtt);
    ESP_LOGI(TAG, "MQTT-Client gestartet: %s", d->net.mqtt_uri);
}

void mqtt_svc_reconfigure(void)
{
    if (s_client) { esp_mqtt_client_stop(s_client); esp_mqtt_client_destroy(s_client); s_client = NULL; }
    s_connected = false;
    mqtt_svc_start();
}

/* wird 1×/5 s vom Datenlogger-Task mitgetriggert */
void mqtt_svc_publish_state(void)
{
    if (!s_connected) return;
    dm_t *d = dm_lock();

    pubf("outdoor/temp", d->outdoor_temp, 1);

    if (d->boiler.enabled) {
        pubf("boiler/temp", d->boiler.temp, 1);
        pubf("boiler/return", d->boiler.return_temp, 1);
        pubf("boiler/flue", d->boiler.flue_temp, 0);
        pubf("boiler/power", d->boiler.power_pct, 0);
        pubf("boiler/runtime_h", d->boiler.runtime_s / 3600.0f, 1);
        pubf("boiler/starts", d->boiler.starts, 0);
        pubf("boiler/consumption_kg", d->boiler.consumption_kg, 1);
        static const char *st[] = { "off", "ready", "ignition", "firing",
                                    "part_load", "full_load", "ember", "fault" };
        pubs("boiler/state", st[d->boiler.state], false);
    }
    if (d->buffer.enabled) {
        for (int i = 0; i < 5; i++) {
            char sub[16]; snprintf(sub, sizeof(sub), "buffer/t%d", i + 1);
            pubf(sub, d->buffer.t[i], 1);
        }
        pubf("buffer/charge", d->buffer.charge_pct, 0);
        pubf("buffer/energy_kwh", d->buffer.energy_kwh, 1);
    }
    if (d->dhw.enabled) {
        pubf("dhw/temp", d->dhw.temp, 1);
        pubf("dhw/setpoint", d->dhw.setpoint, 1);
        pubs("dhw/charging", d->dhw.charging ? "ON" : "OFF", false);
    }
    if (d->solar.enabled) {
        pubf("solar/collector", d->solar.t_collector, 1);
        pubf("solar/store", d->solar.t_store, 1);
        pubs("solar/pump", d->solar.pump_on ? "ON" : "OFF", false);
        pubf("solar/yield_day", d->solar.yield_day_kwh, 2);
        pubf("solar/yield_year", d->solar.yield_year_kwh, 1);
    }
    for (int i = 0; i < DM_MAX_HC; i++) {
        if (!d->hc[i].enabled) continue;
        char sub[32];
        snprintf(sub, sizeof(sub), "hc/%d/flow", i + 1);      pubf(sub, d->hc[i].flow_act, 1);
        snprintf(sub, sizeof(sub), "hc/%d/flow_set", i + 1);  pubf(sub, d->hc[i].flow_set, 1);
        snprintf(sub, sizeof(sub), "hc/%d/room", i + 1);      pubf(sub, d->hc[i].room_act, 1);
        snprintf(sub, sizeof(sub), "hc/%d/mixer", i + 1);     pubf(sub, d->hc[i].mixer_pos_pct, 0);
        snprintf(sub, sizeof(sub), "hc/%d/mode", i + 1);      pubf(sub, d->hc[i].mode, 0);
        snprintf(sub, sizeof(sub), "hc/%d/pump", i + 1);
        pubs(sub, d->hc[i].pump_on ? "ON" : "OFF", false);
    }
    for (int i = 0; i < 8; i++) {
        char sub[24];
        snprintf(sub, sizeof(sub), "relay/%d/state", i + 1);
        pubs(sub, d->relay[i].state ? "ON" : "OFF", false);
    }
    dm_unlock();
}
