/* http_server.c – HTTP-Server, statische Dateien, WebSocket-Livedaten
 *
 * Statisch:  /            → /fs/index.html (Weboberfläche, LittleFS)
 * WebSocket: /ws          → 1×/s Live-Zustand als JSON an alle Clients
 * REST:      /api/ ...    → rest_api.c (alle Routen unter /api)
 * Upload:    /api/ota     → Firmware-Binärupload (Admin)
 *            /api/backup  → Export/Import Konfiguration
 */
#include <string.h>
#include <fcntl.h>
#include <math.h>
#include "websrv.h"
#include "auth.h"
#include "datamodel.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "http";
static httpd_handle_t s_server;

/* ── WebSocket-Clients ───────────────────────────────────────────── */
#define WS_MAX_CLIENTS 6
static int s_ws_fd[WS_MAX_CLIENTS] = { -1, -1, -1, -1, -1, -1 };

static void ws_register(int fd)
{
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        if (s_ws_fd[i] == fd) return;
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        if (s_ws_fd[i] < 0) { s_ws_fd[i] = fd; return; }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {           /* Handshake */
        ws_register(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    /* eingehende Frames werden ignoriert (nur Push) */
    httpd_ws_frame_t f = { 0 };
    f.type = HTTPD_WS_TYPE_TEXT;
    httpd_ws_recv_frame(req, &f, 0);
    return ESP_OK;
}

/* Live-Zustand als kompaktes JSON (siehe Web-UI state handler) */
char *websrv_build_state_json(void)
{
    dm_t *d = dm_lock();
    cJSON *r = cJSON_CreateObject();
#define ADDNUM(o,k,v) do { if (!isnan((float)(v))) cJSON_AddNumberToObject(o,k,(double)(v)); } while (0)
    ADDNUM(r, "out", d->outdoor_temp);
    cJSON_AddNumberToObject(r, "rev", d->cfg_revision);

    cJSON *b = cJSON_AddObjectToObject(r, "boiler");
    cJSON_AddBoolToObject(b, "en", d->boiler.enabled);
    ADDNUM(b, "t", d->boiler.temp); ADDNUM(b, "rt", d->boiler.return_temp);
    ADDNUM(b, "fg", d->boiler.flue_temp);
    cJSON_AddNumberToObject(b, "st", d->boiler.state);
    cJSON_AddNumberToObject(b, "pw", (int)d->boiler.power_pct);
    cJSON_AddNumberToObject(b, "set", d->boiler.setpoint);
    cJSON_AddNumberToObject(b, "rh", d->boiler.runtime_s / 3600);
    cJSON_AddNumberToObject(b, "sn", d->boiler.starts);
    cJSON_AddNumberToObject(b, "kg", (int)d->boiler.consumption_kg);
    cJSON_AddNumberToObject(b, "type", d->boiler.type);

    cJSON *p = cJSON_AddObjectToObject(r, "buf");
    cJSON_AddBoolToObject(p, "en", d->buffer.enabled);
    cJSON *ta = cJSON_AddArrayToObject(p, "t");
    for (int i = 0; i < 5; i++)
        cJSON_AddItemToArray(ta, cJSON_CreateNumber(isnan(d->buffer.t[i]) ? -999 : d->buffer.t[i]));
    cJSON_AddNumberToObject(p, "chg", (int)d->buffer.charge_pct);
    ADDNUM(p, "kwh", d->buffer.energy_kwh);

    cJSON *w = cJSON_AddObjectToObject(r, "dhw");
    cJSON_AddBoolToObject(w, "en", d->dhw.enabled);
    ADDNUM(w, "t", d->dhw.temp);
    cJSON_AddNumberToObject(w, "set", d->dhw.setpoint);
    cJSON_AddBoolToObject(w, "chg", d->dhw.charging);
    cJSON_AddBoolToObject(w, "legio", d->dhw.legio_active);

    cJSON *s = cJSON_AddObjectToObject(r, "sol");
    cJSON_AddBoolToObject(s, "en", d->solar.enabled);
    ADDNUM(s, "tc", d->solar.t_collector); ADDNUM(s, "ts", d->solar.t_store);
    cJSON_AddBoolToObject(s, "pump", d->solar.pump_on);
    ADDNUM(s, "yd", d->solar.yield_day_kwh);
    ADDNUM(s, "ym", d->solar.yield_month_kwh);
    ADDNUM(s, "yy", d->solar.yield_year_kwh);

    cJSON *ha = cJSON_AddArrayToObject(r, "hc");
    for (int i = 0; i < DM_MAX_HC; i++) {
        if (!d->hc[i].enabled) continue;
        cJSON *h = cJSON_CreateObject();
        cJSON_AddNumberToObject(h, "i", i);
        cJSON_AddStringToObject(h, "name", d->hc[i].name);
        cJSON_AddNumberToObject(h, "type", d->hc[i].type);
        cJSON_AddNumberToObject(h, "mode", d->hc[i].mode);
        ADDNUM(h, "fs", d->hc[i].flow_set); ADDNUM(h, "fa", d->hc[i].flow_act);
        ADDNUM(h, "ra", d->hc[i].room_act);
        cJSON_AddNumberToObject(h, "rday", d->hc[i].room_set_day);
        cJSON_AddNumberToObject(h, "mix", (int)d->hc[i].mixer_pos_pct);
        cJSON_AddBoolToObject(h, "pump", d->hc[i].pump_on);
        cJSON_AddItemToArray(ha, h);
    }

    cJSON *ra = cJSON_AddArrayToObject(r, "relay");
    for (int i = 0; i < 8; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "n", d->relay[i].name);
        cJSON_AddBoolToObject(o, "s", d->relay[i].state);
        cJSON_AddItemToArray(ra, o);
    }
    dm_unlock();

    extern int alarms_get_active(void *, int);   /* forward über alarms.h */
    /* Alarme separat, siehe rest_api /api/alarms */

    char *txt = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    return txt;
}

static void ws_broadcast_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_server) continue;
        bool any = false;
        for (int i = 0; i < WS_MAX_CLIENTS; i++) if (s_ws_fd[i] >= 0) any = true;
        if (!any) continue;

        char *json = websrv_build_state_json();
        if (!json) continue;
        httpd_ws_frame_t f = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json,
            .len = strlen(json),
        };
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            if (s_ws_fd[i] < 0) continue;
            if (httpd_ws_send_frame_async(s_server, s_ws_fd[i], &f) != ESP_OK)
                s_ws_fd[i] = -1;    /* Client weg */
        }
        free(json);
    }
}

/* ── statische Dateien von LittleFS ──────────────────────────────── */
static const char *content_type(const char *path)
{
    const char *e = strrchr(path, '.');
    if (!e) return "text/plain";
    if (!strcmp(e, ".html")) return "text/html";
    if (!strcmp(e, ".js"))   return "application/javascript";
    if (!strcmp(e, ".css"))  return "text/css";
    if (!strcmp(e, ".json")) return "application/json";
    if (!strcmp(e, ".svg"))  return "image/svg+xml";
    if (!strcmp(e, ".png"))  return "image/png";
    if (!strcmp(e, ".ico"))  return "image/x-icon";
    return "application/octet-stream";
}

static esp_err_t static_handler(httpd_req_t *req)
{
    char path[128] = "/fs";
    if (!strcmp(req->uri, "/")) strcat(path, "/index.html");
    else {
        strncat(path, req->uri, sizeof(path) - 5);
        char *q = strchr(path, '?');
        if (q) *q = 0;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        /* SPA-Fallback auf index.html */
        f = fopen("/fs/index.html", "r");
        if (!f) { httpd_resp_send_404(req); return ESP_OK; }
        strcpy(path, "/fs/index.html");
    }
    httpd_resp_set_type(req, content_type(path));
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── OTA-Upload (POST /api/ota, multipart-frei: raw body) ────────── */
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    if (websrv_require_role(req, 0 /* ROLE_ADMIN */) != ESP_OK) return ESP_OK;

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { httpd_resp_send_err(req, 500, "Keine OTA-Partition"); return ESP_OK; }

    esp_ota_handle_t h;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
        httpd_resp_send_err(req, 500, "OTA-Start fehlgeschlagen");
        return ESP_OK;
    }
    char buf[2048];
    int remaining = req->content_len;
    ESP_LOGI(TAG, "OTA-Upload: %d Bytes", remaining);
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (n <= 0) { esp_ota_abort(h); httpd_resp_send_err(req, 500, "Übertragung abgebrochen"); return ESP_OK; }
        if (esp_ota_write(h, buf, n) != ESP_OK) {
            esp_ota_abort(h); httpd_resp_send_err(req, 500, "Schreibfehler"); return ESP_OK;
        }
        remaining -= n;
    }
    if (esp_ota_end(h) != ESP_OK ||
        esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_send_err(req, 500, "Image ungültig");
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Update installiert – Neustart\"}");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

/* ── Hilfsfunktionen für rest_api.c ──────────────────────────────── */
int websrv_get_role(httpd_req_t *req)
{
    char cookie[128] = { 0 };
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK)
        return -1;
    char *p = strstr(cookie, "hcsid=");
    if (!p) return -1;
    p += 6;
    char token[33] = { 0 };
    strncpy(token, p, 32);
    char *e = strchr(token, ';');
    if (e) *e = 0;
    return auth_check(token);
}

esp_err_t websrv_require_role(httpd_req_t *req, int max_role)
{
    int role = websrv_get_role(req);
    if (role < 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Anmeldung erforderlich\"}");
        return ESP_FAIL;
    }
    if (role > max_role) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Keine Berechtigung\"}");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void websrv_start(void)
{
    auth_init();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 40;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));

    httpd_uri_t ws = { .uri = "/ws", .method = HTTP_GET,
                       .handler = ws_handler, .is_websocket = true };
    httpd_register_uri_handler(s_server, &ws);

    httpd_uri_t ota = { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_upload_handler };
    httpd_register_uri_handler(s_server, &ota);

    rest_api_register(s_server);     /* alle Routen unter /api */

    httpd_uri_t st = { .uri = "/*", .method = HTTP_GET, .handler = static_handler };
    httpd_register_uri_handler(s_server, &st);

    xTaskCreate(ws_broadcast_task, "ws_push", 6144, NULL, 3, NULL);
    ESP_LOGI(TAG, "Webserver gestartet (Port 80)");
}
