/* rest_api.c – REST-Schnittstelle der Weboberfläche
 *
 * Rollen: 0=Admin  1=Service  2=Benutzer  3=Gast
 *
 *  POST /api/login {user,pass}            → Cookie hcsid
 *  POST /api/logout
 *  POST /api/password {pass}              (eigenes Passwort)
 *  GET  /api/state                        Livedaten (wie WebSocket)
 *  GET  /api/config                       komplette Konfiguration (≤ Service)
 *  POST /api/config                       Teil-Update (JSON-Merge, ≤ Service)
 *  GET/POST /api/hc/<n>                   Heizkreis lesen/schreiben (≤ Benutzer: Sollwerte)
 *  POST /api/boiler/reset                 Störung quittieren (≤ Service)
 *  GET  /api/sensors                      Fühlerliste inkl. gefundener DS18B20
 *  POST /api/sensors                      Zuordnung speichern (≤ Service)
 *  GET  /api/alarms                       aktive Alarme
 *  GET  /api/alarms/history               Historie (JSON-Zeilen)
 *  GET  /api/log?series=…&range=…         Datenlogger
 *  GET  /api/log/export?fmt=csv|json      Export
 *  GET/POST /api/schema                   Hydraulikschema des Editors
 *  GET  /api/backup                       Konfigurations-Export (Download)
 *  POST /api/backup                       Import (≤ Admin)
 *  POST /api/system/reboot|factory        (≤ Admin)
 *  POST /api/ota/url {url}                OTA von URL (≤ Admin)
 */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "websrv.h"
#include "auth.h"
#include "datamodel.h"
#include "config_store.h"
#include "alarms.h"
#include "netsvc.h"
#include "datalog.h"
#include "board.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <unistd.h>

static const char *TAG = "rest";

static char *read_body(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 32 * 1024) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, buf + got, len - got);
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[len] = 0;
    return buf;
}

static void send_json(httpd_req_t *req, cJSON *o)
{
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt ? txt : "{}");
    free(txt);
}

static void send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ── Login/Logout ────────────────────────────────────────────────── */
static esp_err_t h_login(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) { httpd_resp_send_err(req, 400, "Body fehlt"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) { httpd_resp_send_err(req, 400, "JSON ungültig"); return ESP_OK; }
    const char *user = cJSON_GetStringValue(cJSON_GetObjectItem(j, "user"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(j, "pass"));
    char token[33]; int role;
    esp_err_t err = (user && pass) ? auth_login(user, pass, token, &role) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j);
    if (err != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(800));      /* Brute-Force bremsen */
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Anmeldung fehlgeschlagen\"}");
        return ESP_OK;
    }
    char cookie[96];
    snprintf(cookie, sizeof(cookie), "hcsid=%s; Path=/; HttpOnly; SameSite=Lax", token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "role", role);
    cJSON_AddBoolToObject(r, "mustChange", auth_must_change_password());
    send_json(req, r);
    return ESP_OK;
}

static esp_err_t h_logout(httpd_req_t *req)
{
    char cookie[128] = { 0 };
    httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie));
    char *p = strstr(cookie, "hcsid=");
    if (p) {
        char token[33] = { 0 };
        strncpy(token, p + 6, 32);
        char *e = strchr(token, ';'); if (e) *e = 0;
        auth_logout(token);
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "hcsid=; Path=/; Max-Age=0");
    send_ok(req);
    return ESP_OK;
}

static esp_err_t h_password(httpd_req_t *req)
{
    if (websrv_require_role(req, 2) != ESP_OK) return ESP_OK;
    char *body = read_body(req);
    if (!body) { httpd_resp_send_err(req, 400, "Body fehlt"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body);
    free(body);
    const char *pw = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "pass")) : NULL;
    /* vereinfachend: nur admin (Index 0) ändert hier – Mehrbenutzer über /api/config */
    esp_err_t err = pw ? auth_set_password(0, pw) : ESP_ERR_INVALID_ARG;
    if (j) cJSON_Delete(j);
    if (err != ESP_OK) httpd_resp_send_err(req, 400, "Passwort ungültig (min. 4 Zeichen)");
    else send_ok(req);
    return ESP_OK;
}

/* ── Zustand / Konfiguration ─────────────────────────────────────── */
static esp_err_t h_state(httpd_req_t *req)
{
    char *json = websrv_build_state_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

static esp_err_t h_config_get(httpd_req_t *req)
{
    if (websrv_require_role(req, 1) != ESP_OK) return ESP_OK;
    char *json = cfg_export_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

static esp_err_t h_config_post(httpd_req_t *req)
{
    if (websrv_require_role(req, 1) != ESP_OK) return ESP_OK;
    char *body = read_body(req);
    if (!body) { httpd_resp_send_err(req, 400, "Body fehlt"); return ESP_OK; }
    esp_err_t err = cfg_import_json(body);
    free(body);
    if (err != ESP_OK) { httpd_resp_send_err(req, 400, "Konfiguration ungültig"); return ESP_OK; }
    send_ok(req);
    return ESP_OK;
}

/* ── Heizkreis-Schnellzugriff (Benutzerrolle: Sollwerte + Modus) ─── */
static esp_err_t h_hc_post(httpd_req_t *req)
{
    if (websrv_require_role(req, 2) != ESP_OK) return ESP_OK;
    int n = atoi(req->uri + strlen("/api/hc/"));
    if (n < 1 || n > DM_MAX_HC) { httpd_resp_send_err(req, 404, "Heizkreis?"); return ESP_OK; }
    char *body = read_body(req);
    if (!body) { httpd_resp_send_err(req, 400, "Body fehlt"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) { httpd_resp_send_err(req, 400, "JSON ungültig"); return ESP_OK; }

    dm_t *d = dm_lock();
    dm_hc_t *h = &d->hc[n - 1];
    cJSON *v;
    if ((v = cJSON_GetObjectItem(j, "mode")) && cJSON_IsNumber(v) &&
        v->valueint >= 0 && v->valueint <= HCM_OFF) h->mode = v->valueint;
    if ((v = cJSON_GetObjectItem(j, "rday")) && cJSON_IsNumber(v) &&
        v->valuedouble >= 5 && v->valuedouble <= 30) h->room_set_day = v->valuedouble;
    if ((v = cJSON_GetObjectItem(j, "rnight")) && cJSON_IsNumber(v) &&
        v->valuedouble >= 5 && v->valuedouble <= 30) h->room_set_night = v->valuedouble;
    if ((v = cJSON_GetObjectItem(j, "holidayUntil")) && cJSON_IsNumber(v)) {
        h->holiday_until = (time_t)v->valuedouble;
        h->mode = HCM_HOLIDAY;
    }
    if ((v = cJSON_GetObjectItem(j, "partyUntil")) && cJSON_IsNumber(v)) {
        h->party_until = (time_t)v->valuedouble;
        h->mode = HCM_PARTY;
    }
    dm_unlock();
    cJSON_Delete(j);
    cfg_save();
    send_ok(req);
    return ESP_OK;
}

static esp_err_t h_boiler_reset(httpd_req_t *req)
{
    if (websrv_require_role(req, 1) != ESP_OK) return ESP_OK;
    dm_t *d = dm_lock();
    if (d->boiler.state == BST_FAULT) d->boiler.state = BST_OFF;
    dm_unlock();
    alarm_clear(ALM_BOILER_OVERTEMP);
    alarm_clear(ALM_BOILER_NO_RISE);
    send_ok(req);
    return ESP_OK;
}

/* ── Fühler ──────────────────────────────────────────────────────── */
static esp_err_t h_sensors_get(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();

    cJSON *found = cJSON_AddArrayToObject(r, "ds18b20");
    for (int i = 0; i < board_ds18b20_count(); i++) {
        const ds18b20_t *ds = board_ds18b20_get(i);
        cJSON *o = cJSON_CreateObject();
        char rom[20];
        snprintf(rom, sizeof(rom), "%016llx", (unsigned long long)ds->rom);
        cJSON_AddStringToObject(o, "rom", rom);
        cJSON_AddNumberToObject(o, "bus", ds->bus);
        if (ds->valid) cJSON_AddNumberToObject(o, "t", ds->temp_c);
        cJSON_AddItemToArray(found, o);
    }

    cJSON *ntc = cJSON_AddArrayToObject(r, "ntc");
    for (int i = 0; i < BOARD_MAX_AIN; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ain", i);
        float t = board_ain_ntc10k(i);
        if (!isnan(t)) cJSON_AddNumberToObject(o, "t", t);
        cJSON_AddItemToArray(ntc, o);
    }

    cJSON *cfgd = cJSON_AddArrayToObject(r, "sensors");
    dm_t *d = dm_lock();
    for (int i = 0; i < DM_MAX_SENSORS; i++) {
        dm_sensor_t *s = &d->sensor[i];
        if (!s->name[0] && s->src == SRC_NONE) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "i", i);
        cJSON_AddStringToObject(o, "name", s->name);
        cJSON_AddNumberToObject(o, "src", s->src);
        cJSON_AddNumberToObject(o, "role", s->role);
        cJSON_AddNumberToObject(o, "ridx", s->role_idx);
        char rom[20]; snprintf(rom, sizeof(rom), "%016llx", (unsigned long long)s->ref);
        cJSON_AddStringToObject(o, "ref", rom);
        cJSON_AddNumberToObject(o, "off", s->offset);
        cJSON_AddBoolToObject(o, "en", s->enabled);
        if (s->valid) cJSON_AddNumberToObject(o, "t", s->value);
        cJSON_AddItemToArray(cfgd, o);
    }
    dm_unlock();
    send_json(req, r);
    return ESP_OK;
}

static esp_err_t h_sensors_rescan(httpd_req_t *req)
{
    if (websrv_require_role(req, 1) != ESP_OK) return ESP_OK;
    board_ds18b20_rescan();
    return h_sensors_get(req);
}

/* ── Alarme ──────────────────────────────────────────────────────── */
static esp_err_t h_alarms(httpd_req_t *req)
{
    alarm_t list[ALARM_MAX_ACTIVE];
    int n = alarms_get_active(list, ALARM_MAX_ACTIVE);
    cJSON *r = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "lvl", list[i].level);
        cJSON_AddNumberToObject(o, "code", list[i].code);
        cJSON_AddNumberToObject(o, "t", (double)list[i].timestamp);
        cJSON_AddStringToObject(o, "msg", list[i].message);
        cJSON_AddItemToArray(r, o);
    }
    send_json(req, r);
    return ESP_OK;
}

static esp_err_t send_file(httpd_req_t *req, const char *path, const char *ctype)
{
    FILE *f = fopen(path, "r");
    if (!f) { httpd_resp_set_type(req, ctype); httpd_resp_sendstr(req, ""); return ESP_OK; }
    httpd_resp_set_type(req, ctype);
    char buf[1024]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        httpd_resp_send_chunk(req, buf, n);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t h_alarm_history(httpd_req_t *req)
{
    return send_file(req, "/fs/alarms.log", "application/x-ndjson");
}

/* ── Datenlogger ─────────────────────────────────────────────────── */
static esp_err_t h_log(httpd_req_t *req)
{
    char q[64] = "", range[16] = "24h";
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "range", range, sizeof(range));
    char *json = datalog_query_json(range);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "[]");
    free(json);
    return ESP_OK;
}

static esp_err_t h_log_export(httpd_req_t *req)
{
    char q[64] = "", fmt[8] = "csv";
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "fmt", fmt, sizeof(fmt));
    if (!strcmp(fmt, "json")) {
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=heizcore-log.json");
        char *json = datalog_query_json("365d");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json ? json : "[]");
        free(json);
    } else {
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=heizcore-log.csv");
        httpd_resp_set_type(req, "text/csv");
        datalog_export_csv(req);   /* streamt chunked */
    }
    return ESP_OK;
}

/* ── Hydraulikschema (Editor-Blob) ───────────────────────────────── */
static esp_err_t h_schema_get(httpd_req_t *req)
{
    return send_file(req, "/fs/schema.json", "application/json");
}

static esp_err_t h_schema_post(httpd_req_t *req)
{
    if (websrv_require_role(req, 1) != ESP_OK) return ESP_OK;
    char *body = read_body(req);
    if (!body) { httpd_resp_send_err(req, 400, "Body fehlt"); return ESP_OK; }
    cJSON *test = cJSON_Parse(body);           /* validieren */
    if (!test) { free(body); httpd_resp_send_err(req, 400, "JSON ungültig"); return ESP_OK; }
    cJSON_Delete(test);
    FILE *f = fopen("/fs/schema.json", "w");
    if (f) { fwrite(body, 1, strlen(body), f); fclose(f); }
    free(body);
    send_ok(req);
    return ESP_OK;
}

/* ── Backup ──────────────────────────────────────────────────────── */
static esp_err_t h_backup_get(httpd_req_t *req)
{
    if (websrv_require_role(req, 1) != ESP_OK) return ESP_OK;
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=heizcore-backup.json");
    char *json = cfg_export_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

static esp_err_t h_backup_post(httpd_req_t *req)
{
    if (websrv_require_role(req, 0) != ESP_OK) return ESP_OK;
    char *body = read_body(req);
    if (!body) { httpd_resp_send_err(req, 400, "Body fehlt"); return ESP_OK; }
    esp_err_t err = cfg_import_json(body);
    free(body);
    if (err != ESP_OK) { httpd_resp_send_err(req, 400, "Backup ungültig"); return ESP_OK; }
    send_ok(req);
    return ESP_OK;
}

/* ── System ──────────────────────────────────────────────────────── */
static esp_err_t h_reboot(httpd_req_t *req)
{
    if (websrv_require_role(req, 0) != ESP_OK) return ESP_OK;
    send_ok(req);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_factory(httpd_req_t *req)
{
    if (websrv_require_role(req, 0) != ESP_OK) return ESP_OK;
    unlink("/fs/config.json");
    unlink("/fs/config.bak");
    unlink("/fs/schema.json");
    send_ok(req);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_ota_url(httpd_req_t *req)
{
    if (websrv_require_role(req, 0) != ESP_OK) return ESP_OK;
    char *body = read_body(req);
    if (!body) { httpd_resp_send_err(req, 400, "Body fehlt"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body);
    free(body);
    const char *url = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "url")) : NULL;
    if (!url) { if (j) cJSON_Delete(j); httpd_resp_send_err(req, 400, "url fehlt"); return ESP_OK; }
    send_ok(req);                     /* Antwort vor dem Download senden */
    char *u = strdup(url);
    cJSON_Delete(j);
    ota_svc_from_url(u);              /* blockiert; startet bei Erfolg neu */
    free(u);
    return ESP_OK;
}

static esp_err_t h_sysinfo(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "fw", FIRMWARE_VERSION);
    cJSON_AddStringToObject(r, "ip", netsvc_ip_str());
    cJSON_AddNumberToObject(r, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(r, "uptime", (double)(esp_log_timestamp() / 1000));
    const dm_t *d = cfg_get();
    cJSON_AddNumberToObject(r, "board", d->board_model);
    cJSON_AddStringToObject(r, "plant", d->plant_name);
    cJSON_AddStringToObject(r, "lang", d->language);
    send_json(req, r);
    return ESP_OK;
}

/* ── Registrierung ───────────────────────────────────────────────── */
void rest_api_register(httpd_handle_t s)
{
#define R(u, m, h) do { httpd_uri_t x = { .uri = u, .method = m, .handler = h }; \
                        httpd_register_uri_handler(s, &x); } while (0)
    R("/api/login",          HTTP_POST, h_login);
    R("/api/logout",         HTTP_POST, h_logout);
    R("/api/password",       HTTP_POST, h_password);
    R("/api/state",          HTTP_GET,  h_state);
    R("/api/config",         HTTP_GET,  h_config_get);
    R("/api/config",         HTTP_POST, h_config_post);
    R("/api/hc/*",           HTTP_POST, h_hc_post);
    R("/api/boiler/reset",   HTTP_POST, h_boiler_reset);
    R("/api/sensors",        HTTP_GET,  h_sensors_get);
    R("/api/sensors/rescan", HTTP_POST, h_sensors_rescan);
    R("/api/alarms",         HTTP_GET,  h_alarms);
    R("/api/alarms/history", HTTP_GET,  h_alarm_history);
    R("/api/log",            HTTP_GET,  h_log);
    R("/api/log/export",     HTTP_GET,  h_log_export);
    R("/api/schema",         HTTP_GET,  h_schema_get);
    R("/api/schema",         HTTP_POST, h_schema_post);
    R("/api/backup",         HTTP_GET,  h_backup_get);
    R("/api/backup",         HTTP_POST, h_backup_post);
    R("/api/system/reboot",  HTTP_POST, h_reboot);
    R("/api/system/factory", HTTP_POST, h_factory);
    R("/api/system/info",    HTTP_GET,  h_sysinfo);
    R("/api/ota/url",        HTTP_POST, h_ota_url);
#undef R
    ESP_LOGI(TAG, "REST-API registriert");
}
