/* config_store.c – Konfiguration als JSON auf LittleFS
 *
 * /fs/config.json     – aktive Konfiguration
 * /fs/config.bak      – letzte funktionierende Sicherung
 * /fs/schema.json     – Hydraulikschema des Drag&Drop-Editors (Blob)
 *
 * Beim Speichern wird atomar über eine Temporärdatei gearbeitet.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "config_store.h"
#include "datamodel.h"
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "cfg";
#define CFG_PATH  "/fs/config.json"
#define CFG_BAK   "/fs/config.bak"
#define CFG_TMP   "/fs/config.tmp"

static dm_t s_cfg_view;   /* nur für cfg_get() – Kopie unter Lock */

/* ---------- Serialisierung ------------------------------------------ */
#define J_NUM(o, k, v)  cJSON_AddNumberToObject(o, k, v)
#define J_STR(o, k, v)  cJSON_AddStringToObject(o, k, v)
#define J_BOOL(o, k, v) cJSON_AddBoolToObject(o, k, v)

static cJSON *ser_timeprog(const dm_timeprog_t *t)
{
    cJSON *o = cJSON_CreateObject();
    J_STR(o, "name", t->name); J_BOOL(o, "en", t->enabled);
    cJSON *a = cJSON_AddArrayToObject(o, "sw");
    for (int i = 0; i < t->n_switch; i++) {
        cJSON *s = cJSON_CreateObject();
        J_NUM(s, "d", t->sw[i].dow_mask);
        J_NUM(s, "on", t->sw[i].on_min);
        J_NUM(s, "off", t->sw[i].off_min);
        cJSON_AddItemToArray(a, s);
    }
    return o;
}

static cJSON *ser_hc(const dm_hc_t *h)
{
    cJSON *o = cJSON_CreateObject();
    J_BOOL(o, "en", h->enabled); J_STR(o, "name", h->name);
    J_NUM(o, "type", h->type);   J_NUM(o, "mode", h->mode);
    J_NUM(o, "slope", h->curve_slope); J_NUM(o, "offs", h->curve_offset);
    J_NUM(o, "fmin", h->flow_min); J_NUM(o, "fmax", h->flow_max);
    J_NUM(o, "rday", h->room_set_day); J_NUM(o, "rnight", h->room_set_night);
    J_NUM(o, "hlim", h->heat_limit); J_NUM(o, "frost", h->frost_limit);
    J_NUM(o, "rinf", h->room_influence);
    J_NUM(o, "tp", h->tp_index); J_NUM(o, "mrt", h->mixer_runtime_s);
    J_NUM(o, "npts", h->n_curve_pts);
    if (h->n_curve_pts) {
        cJSON *po = cJSON_AddArrayToObject(o, "pts");
        for (int i = 0; i < h->n_curve_pts; i++) {
            cJSON *p = cJSON_CreateObject();
            J_NUM(p, "o", h->curve_out[i]); J_NUM(p, "f", h->curve_flow[i]);
            cJSON_AddItemToArray(po, p);
        }
    }
    return o;
}

static cJSON *serialize(const dm_t *d)
{
    cJSON *r = cJSON_CreateObject();
    J_NUM(r, "rev", d->cfg_revision);
    J_NUM(r, "board", d->board_model);
    J_STR(r, "plant", d->plant_name);
    J_STR(r, "lang", d->language);

    cJSON *n = cJSON_AddObjectToObject(r, "net");
    J_STR(n, "host", d->net.hostname);
    J_STR(n, "ssid", d->net.wifi_ssid);   J_STR(n, "wpass", d->net.wifi_pass);
    J_BOOL(n, "eth", d->net.eth_enabled); J_BOOL(n, "wifi", d->net.wifi_enabled);
    J_BOOL(n, "dhcp", d->net.dhcp);
    J_STR(n, "ip", d->net.ip); J_STR(n, "gw", d->net.gw);
    J_STR(n, "mask", d->net.mask); J_STR(n, "dns", d->net.dns);
    J_STR(n, "ntp", d->net.ntp_server); J_STR(n, "tz", d->net.timezone);
    J_BOOL(n, "mqtt", d->net.mqtt_enabled); J_BOOL(n, "disc", d->net.mqtt_discovery);
    J_STR(n, "muri", d->net.mqtt_uri); J_STR(n, "muser", d->net.mqtt_user);
    J_STR(n, "mpass", d->net.mqtt_pass); J_STR(n, "mpre", d->net.mqtt_prefix);
    J_BOOL(n, "mbr", d->net.mb_rtu_enabled); J_NUM(n, "mbrb", d->net.mb_rtu_baud);
    J_NUM(n, "mbrm", d->net.mb_rtu_mode);
    J_BOOL(n, "mbt", d->net.mb_tcp_enabled); J_NUM(n, "mbtm", d->net.mb_tcp_mode);
    J_NUM(n, "mbtp", d->net.mb_tcp_port); J_NUM(n, "mbsid", d->net.mb_slave_id);
    J_BOOL(n, "em", d->net.email_enabled); J_STR(n, "esmtp", d->net.email_smtp);
    J_NUM(n, "eport", d->net.email_port); J_STR(n, "euser", d->net.email_user);
    J_STR(n, "epass", d->net.email_pass); J_STR(n, "eto", d->net.email_to);
    J_BOOL(n, "tg", d->net.telegram_enabled); J_STR(n, "tgtok", d->net.telegram_token);
    J_STR(n, "tgchat", d->net.telegram_chat);

    cJSON *sa = cJSON_AddArrayToObject(r, "sensors");
    for (int i = 0; i < DM_MAX_SENSORS; i++) {
        const dm_sensor_t *s = &d->sensor[i];
        if (!s->enabled && s->src == SRC_NONE && !s->name[0]) continue;
        cJSON *o = cJSON_CreateObject();
        J_NUM(o, "i", i); J_STR(o, "name", s->name);
        J_NUM(o, "src", s->src); J_NUM(o, "role", s->role);
        J_NUM(o, "ridx", s->role_idx); J_NUM(o, "off", s->offset);
        J_BOOL(o, "en", s->enabled);
        char refhex[20]; snprintf(refhex, sizeof(refhex), "%016llx", (unsigned long long)s->ref);
        J_STR(o, "ref", refhex);
        cJSON_AddItemToArray(sa, o);
    }

    cJSON *ra = cJSON_AddArrayToObject(r, "relays");
    for (int i = 0; i < 8; i++) {
        cJSON *o = cJSON_CreateObject();
        J_NUM(o, "fn", d->relay[i].fn); J_NUM(o, "fi", d->relay[i].fn_idx);
        J_STR(o, "name", d->relay[i].name);
        J_NUM(o, "rt", d->relay[i].runtime_s); J_NUM(o, "st", d->relay[i].starts);
        cJSON_AddItemToArray(ra, o);
    }

    cJSON *b = cJSON_AddObjectToObject(r, "boiler");
    J_BOOL(b, "en", d->boiler.enabled); J_NUM(b, "type", d->boiler.type);
    J_STR(b, "name", d->boiler.name);
    J_NUM(b, "set", d->boiler.setpoint); J_NUM(b, "hyst", d->boiler.hysteresis);
    J_NUM(b, "tmin", d->boiler.min_temp); J_NUM(b, "tmax", d->boiler.max_temp);
    J_NUM(b, "rmin", d->boiler.return_min); J_NUM(b, "tp", d->boiler.tp_index);
    J_NUM(b, "kgph", d->boiler.kg_per_h_full);
    J_NUM(b, "rt", d->boiler.runtime_s); J_NUM(b, "starts", d->boiler.starts);
    J_NUM(b, "cons", d->boiler.consumption_kg);

    cJSON *p = cJSON_AddObjectToObject(r, "buffer");
    J_BOOL(p, "en", d->buffer.enabled); J_STR(p, "name", d->buffer.name);
    J_NUM(p, "vol", d->buffer.volume_l);
    J_NUM(p, "tmin", d->buffer.t_min); J_NUM(p, "tmax", d->buffer.t_max);

    cJSON *w = cJSON_AddObjectToObject(r, "dhw");
    J_BOOL(w, "en", d->dhw.enabled); J_STR(w, "name", d->dhw.name);
    J_NUM(w, "set", d->dhw.setpoint); J_NUM(w, "hyst", d->dhw.hysteresis);
    J_BOOL(w, "prio", d->dhw.priority); J_NUM(w, "tp", d->dhw.tp_index);
    J_BOOL(w, "legio", d->dhw.legio_enabled);
    J_NUM(w, "lwd", d->dhw.legio_weekday); J_NUM(w, "lh", d->dhw.legio_hour);
    J_NUM(w, "lt", d->dhw.legio_temp);

    cJSON *s = cJSON_AddObjectToObject(r, "solar");
    J_BOOL(s, "en", d->solar.enabled); J_STR(s, "name", d->solar.name);
    J_NUM(s, "dton", d->solar.dt_on); J_NUM(s, "dtoff", d->solar.dt_off);
    J_NUM(s, "smax", d->solar.max_store_temp); J_NUM(s, "cmax", d->solar.collector_max);
    J_NUM(s, "lpm", d->solar.flow_lpm);
    J_NUM(s, "yd", d->solar.yield_day_kwh); J_NUM(s, "ym", d->solar.yield_month_kwh);
    J_NUM(s, "yy", d->solar.yield_year_kwh); J_NUM(s, "yt", d->solar.yield_total_kwh);

    cJSON *ha = cJSON_AddArrayToObject(r, "hc");
    for (int i = 0; i < DM_MAX_HC; i++) {
        if (!d->hc[i].enabled && i >= 2) continue;
        cJSON *o = ser_hc(&d->hc[i]);
        J_NUM(o, "i", i);
        cJSON_AddItemToArray(ha, o);
    }

    cJSON *ta = cJSON_AddArrayToObject(r, "tp");
    for (int i = 0; i < DM_MAX_TIMEPROG; i++) {
        if (!d->tp[i].enabled && !d->tp[i].name[0]) continue;
        cJSON *o = ser_timeprog(&d->tp[i]);
        J_NUM(o, "i", i);
        cJSON_AddItemToArray(ta, o);
    }

    cJSON *ua = cJSON_AddArrayToObject(r, "users");
    for (int i = 0; i < DM_MAX_USERS; i++) {
        if (!d->user[i].enabled) continue;
        cJSON *o = cJSON_CreateObject();
        J_NUM(o, "i", i); J_STR(o, "name", d->user[i].name);
        J_STR(o, "hash", d->user[i].pw_hash); J_STR(o, "salt", d->user[i].salt);
        J_NUM(o, "role", d->user[i].role);
        cJSON_AddItemToArray(ua, o);
    }
    return r;
}

/* ---------- Deserialisierung ---------------------------------------- */
static void js(const cJSON *o, const char *k, char *dst, size_t n)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsString(v)) { strncpy(dst, v->valuestring, n - 1); dst[n - 1] = 0; }
}
static double json_num(const cJSON *o, const char *k, double dflt)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}
static bool jb(const cJSON *o, const char *k, bool dflt)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}

static void deserialize(dm_t *d, const cJSON *r)
{
    d->cfg_revision = (uint32_t)json_num(r, "rev", 0);
    d->board_model  = (int)json_num(r, "board", 8);
    js(r, "plant", d->plant_name, sizeof(d->plant_name));
    js(r, "lang", d->language, sizeof(d->language));

    const cJSON *n = cJSON_GetObjectItem(r, "net");
    if (n) {
        js(n, "host", d->net.hostname, sizeof(d->net.hostname));
        js(n, "ssid", d->net.wifi_ssid, sizeof(d->net.wifi_ssid));
        js(n, "wpass", d->net.wifi_pass, sizeof(d->net.wifi_pass));
        d->net.eth_enabled = jb(n, "eth", true);
        d->net.wifi_enabled = jb(n, "wifi", true);
        d->net.dhcp = jb(n, "dhcp", true);
        js(n, "ip", d->net.ip, 16); js(n, "gw", d->net.gw, 16);
        js(n, "mask", d->net.mask, 16); js(n, "dns", d->net.dns, 16);
        js(n, "ntp", d->net.ntp_server, sizeof(d->net.ntp_server));
        js(n, "tz", d->net.timezone, sizeof(d->net.timezone));
        d->net.mqtt_enabled = jb(n, "mqtt", false);
        d->net.mqtt_discovery = jb(n, "disc", true);
        js(n, "muri", d->net.mqtt_uri, sizeof(d->net.mqtt_uri));
        js(n, "muser", d->net.mqtt_user, sizeof(d->net.mqtt_user));
        js(n, "mpass", d->net.mqtt_pass, sizeof(d->net.mqtt_pass));
        js(n, "mpre", d->net.mqtt_prefix, sizeof(d->net.mqtt_prefix));
        d->net.mb_rtu_enabled = jb(n, "mbr", false);
        d->net.mb_rtu_baud = (int)json_num(n, "mbrb", 19200);
        d->net.mb_rtu_mode = (int)json_num(n, "mbrm", 0);
        d->net.mb_tcp_enabled = jb(n, "mbt", false);
        d->net.mb_tcp_mode = (int)json_num(n, "mbtm", 1);
        d->net.mb_tcp_port = (int)json_num(n, "mbtp", 502);
        d->net.mb_slave_id = (int)json_num(n, "mbsid", 1);
        d->net.email_enabled = jb(n, "em", false);
        js(n, "esmtp", d->net.email_smtp, sizeof(d->net.email_smtp));
        d->net.email_port = (int)json_num(n, "eport", 587);
        js(n, "euser", d->net.email_user, sizeof(d->net.email_user));
        js(n, "epass", d->net.email_pass, sizeof(d->net.email_pass));
        js(n, "eto", d->net.email_to, sizeof(d->net.email_to));
        d->net.telegram_enabled = jb(n, "tg", false);
        js(n, "tgtok", d->net.telegram_token, sizeof(d->net.telegram_token));
        js(n, "tgchat", d->net.telegram_chat, sizeof(d->net.telegram_chat));
    }

    const cJSON *el;
    const cJSON *sa = cJSON_GetObjectItem(r, "sensors");
    cJSON_ArrayForEach(el, sa) {
        int i = (int)json_num(el, "i", -1);
        if (i < 0 || i >= DM_MAX_SENSORS) continue;
        dm_sensor_t *s = &d->sensor[i];
        js(el, "name", s->name, DM_NAME_LEN);
        s->src = (sensor_src_t)json_num(el, "src", SRC_NONE);
        s->role = (sensor_role_t)json_num(el, "role", ROLE_NONE);
        s->role_idx = (int)json_num(el, "ridx", 0);
        s->offset = (float)json_num(el, "off", 0);
        s->enabled = jb(el, "en", true);
        char refhex[20] = "0";
        js(el, "ref", refhex, sizeof(refhex));
        s->ref = strtoull(refhex, NULL, 16);
        s->value = NAN;
    }

    const cJSON *ra = cJSON_GetObjectItem(r, "relays");
    int ri = 0;
    cJSON_ArrayForEach(el, ra) {
        if (ri >= 8) break;
        d->relay[ri].fn = (actor_fn_t)json_num(el, "fn", ACT_NONE);
        d->relay[ri].fn_idx = (int)json_num(el, "fi", 0);
        js(el, "name", d->relay[ri].name, DM_NAME_LEN);
        d->relay[ri].runtime_s = (uint32_t)json_num(el, "rt", 0);
        d->relay[ri].starts = (uint32_t)json_num(el, "st", 0);
        ri++;
    }

    const cJSON *b = cJSON_GetObjectItem(r, "boiler");
    if (b) {
        d->boiler.enabled = jb(b, "en", true);
        d->boiler.type = (boiler_type_t)json_num(b, "type", BOILER_PELLET);
        js(b, "name", d->boiler.name, DM_NAME_LEN);
        d->boiler.setpoint = (float)json_num(b, "set", 72);
        d->boiler.hysteresis = (float)json_num(b, "hyst", 4);
        d->boiler.min_temp = (float)json_num(b, "tmin", 55);
        d->boiler.max_temp = (float)json_num(b, "tmax", 90);
        d->boiler.return_min = (float)json_num(b, "rmin", 55);
        d->boiler.tp_index = (int)json_num(b, "tp", -1);
        d->boiler.kg_per_h_full = (float)json_num(b, "kgph", 3.4);
        d->boiler.runtime_s = (uint32_t)json_num(b, "rt", 0);
        d->boiler.starts = (uint32_t)json_num(b, "starts", 0);
        d->boiler.consumption_kg = (float)json_num(b, "cons", 0);
    }

    const cJSON *p = cJSON_GetObjectItem(r, "buffer");
    if (p) {
        d->buffer.enabled = jb(p, "en", true);
        js(p, "name", d->buffer.name, DM_NAME_LEN);
        d->buffer.volume_l = (float)json_num(p, "vol", 1000);
        d->buffer.t_min = (float)json_num(p, "tmin", 40);
        d->buffer.t_max = (float)json_num(p, "tmax", 80);
    }

    const cJSON *w = cJSON_GetObjectItem(r, "dhw");
    if (w) {
        d->dhw.enabled = jb(w, "en", true);
        js(w, "name", d->dhw.name, DM_NAME_LEN);
        d->dhw.setpoint = (float)json_num(w, "set", 55);
        d->dhw.hysteresis = (float)json_num(w, "hyst", 5);
        d->dhw.priority = jb(w, "prio", true);
        d->dhw.tp_index = (int)json_num(w, "tp", 0);
        d->dhw.legio_enabled = jb(w, "legio", true);
        d->dhw.legio_weekday = (int)json_num(w, "lwd", 1);
        d->dhw.legio_hour = (int)json_num(w, "lh", 2);
        d->dhw.legio_temp = (float)json_num(w, "lt", 65);
    }

    const cJSON *s = cJSON_GetObjectItem(r, "solar");
    if (s) {
        d->solar.enabled = jb(s, "en", false);
        js(s, "name", d->solar.name, DM_NAME_LEN);
        d->solar.dt_on = (float)json_num(s, "dton", 7);
        d->solar.dt_off = (float)json_num(s, "dtoff", 3);
        d->solar.max_store_temp = (float)json_num(s, "smax", 85);
        d->solar.collector_max = (float)json_num(s, "cmax", 130);
        d->solar.flow_lpm = (float)json_num(s, "lpm", 6);
        d->solar.yield_day_kwh = (float)json_num(s, "yd", 0);
        d->solar.yield_month_kwh = (float)json_num(s, "ym", 0);
        d->solar.yield_year_kwh = (float)json_num(s, "yy", 0);
        d->solar.yield_total_kwh = (float)json_num(s, "yt", 0);
    }

    const cJSON *ha = cJSON_GetObjectItem(r, "hc");
    cJSON_ArrayForEach(el, ha) {
        int i = (int)json_num(el, "i", -1);
        if (i < 0 || i >= DM_MAX_HC) continue;
        dm_hc_t *h = &d->hc[i];
        h->enabled = jb(el, "en", false);
        js(el, "name", h->name, DM_NAME_LEN);
        h->type = (hc_type_t)json_num(el, "type", HC_MIXED);
        h->mode = (hc_mode_t)json_num(el, "mode", HCM_AUTO);
        h->curve_slope = (float)json_num(el, "slope", 1.2);
        h->curve_offset = (float)json_num(el, "offs", 0);
        h->flow_min = (float)json_num(el, "fmin", 20);
        h->flow_max = (float)json_num(el, "fmax", 60);
        h->room_set_day = (float)json_num(el, "rday", 21);
        h->room_set_night = (float)json_num(el, "rnight", 17);
        h->heat_limit = (float)json_num(el, "hlim", 17);
        h->frost_limit = (float)json_num(el, "frost", 3);
        h->room_influence = (float)json_num(el, "rinf", 0);
        h->tp_index = (int)json_num(el, "tp", -1);
        h->mixer_runtime_s = (int)json_num(el, "mrt", 120);
        h->n_curve_pts = (int)json_num(el, "npts", 0);
        const cJSON *pts = cJSON_GetObjectItem(el, "pts"), *pe;
        int pi = 0;
        cJSON_ArrayForEach(pe, pts) {
            if (pi >= DM_MAX_CURVE_PTS) break;
            h->curve_out[pi] = (float)json_num(pe, "o", 0);
            h->curve_flow[pi] = (float)json_num(pe, "f", 0);
            pi++;
        }
    }

    const cJSON *ta = cJSON_GetObjectItem(r, "tp");
    cJSON_ArrayForEach(el, ta) {
        int i = (int)json_num(el, "i", -1);
        if (i < 0 || i >= DM_MAX_TIMEPROG) continue;
        dm_timeprog_t *t = &d->tp[i];
        js(el, "name", t->name, DM_NAME_LEN);
        t->enabled = jb(el, "en", false);
        t->n_switch = 0;
        const cJSON *swa = cJSON_GetObjectItem(el, "sw"), *se;
        cJSON_ArrayForEach(se, swa) {
            if (t->n_switch >= DM_MAX_SWITCH) break;
            dm_switch_t *sw = &t->sw[t->n_switch++];
            sw->dow_mask = (uint8_t)json_num(se, "d", 0x7F);
            sw->on_min = (uint16_t)json_num(se, "on", 360);
            sw->off_min = (uint16_t)json_num(se, "off", 1320);
        }
    }

    const cJSON *ua = cJSON_GetObjectItem(r, "users");
    cJSON_ArrayForEach(el, ua) {
        int i = (int)json_num(el, "i", -1);
        if (i < 0 || i >= DM_MAX_USERS) continue;
        dm_user_t *u = &d->user[i];
        js(el, "name", u->name, DM_NAME_LEN);
        js(el, "hash", u->pw_hash, sizeof(u->pw_hash));
        js(el, "salt", u->salt, sizeof(u->salt));
        u->role = (user_role_t)json_num(el, "role", ROLE_GUEST);
        u->enabled = true;
    }
}

/* ---------- Öffentliche API ------------------------------------------ */
const dm_t *cfg_get(void)
{
    dm_t *d = dm_lock();
    memcpy(&s_cfg_view, d, sizeof(dm_t));
    dm_unlock();
    return &s_cfg_view;
}

esp_err_t cfg_load(void)
{
    FILE *f = fopen(CFG_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "Keine Konfiguration – Werkszustand aktiv");
        return cfg_save();
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    fread(buf, 1, len, f);
    buf[len] = 0;
    fclose(f);

    cJSON *r = cJSON_Parse(buf);
    free(buf);
    if (!r) {
        ESP_LOGE(TAG, "config.json beschädigt – versuche Backup");
        rename(CFG_BAK, CFG_PATH);
        return ESP_FAIL;
    }
    dm_t *d = dm_lock();
    deserialize(d, r);
    dm_unlock();
    cJSON_Delete(r);
    ESP_LOGI(TAG, "Konfiguration geladen (Revision %u)", (unsigned)cfg_get()->cfg_revision);
    return ESP_OK;
}

esp_err_t cfg_save(void)
{
    dm_t *d = dm_lock();
    d->cfg_revision++;
    cJSON *r = serialize(d);
    dm_unlock();
    char *txt = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (!txt) return ESP_ERR_NO_MEM;

    FILE *f = fopen(CFG_TMP, "w");
    if (!f) { free(txt); return ESP_FAIL; }
    size_t n = fwrite(txt, 1, strlen(txt), f);
    fclose(f);
    bool ok = (n == strlen(txt));
    free(txt);
    if (!ok) { unlink(CFG_TMP); return ESP_FAIL; }

    rename(CFG_PATH, CFG_BAK);     /* alte Version als Backup behalten */
    rename(CFG_TMP, CFG_PATH);
    ESP_LOGI(TAG, "Konfiguration gespeichert");
    return ESP_OK;
}

char *cfg_export_json(void)       /* Aufrufer gibt frei */
{
    dm_t *d = dm_lock();
    cJSON *r = serialize(d);
    dm_unlock();
    char *txt = cJSON_Print(r);
    cJSON_Delete(r);
    return txt;
}

esp_err_t cfg_import_json(const char *json)
{
    cJSON *r = cJSON_Parse(json);
    if (!r) return ESP_ERR_INVALID_ARG;
    dm_t *d = dm_lock();
    deserialize(d, r);
    dm_unlock();
    cJSON_Delete(r);
    return cfg_save();
}
