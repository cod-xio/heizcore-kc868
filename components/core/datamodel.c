/* datamodel.c – zentrales Datenmodell + Werkszustand */
#include <string.h>
#include <math.h>
#include "datamodel.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static dm_t s_dm;
static SemaphoreHandle_t s_mutex;

void dm_init(void)
{
    s_mutex = xSemaphoreCreateRecursiveMutex();
    dm_factory_defaults(&s_dm);
}

dm_t *dm_lock(void)
{
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    return &s_dm;
}

void dm_unlock(void)
{
    xSemaphoreGiveRecursive(s_mutex);
}

void dm_factory_defaults(dm_t *d)
{
    memset(d, 0, sizeof(*d));
    d->board_model = 8;
    strcpy(d->plant_name, "Meine Heizungsanlage");
    strcpy(d->language, "de");

    /* Netzwerk */
    strcpy(d->net.hostname, "heizcore");
    d->net.eth_enabled = true;
    d->net.wifi_enabled = true;
    d->net.dhcp = true;
    strcpy(d->net.ntp_server, "pool.ntp.org");
    strcpy(d->net.timezone, "CET-1CEST,M3.5.0,M10.5.0/3");   /* Europe/Vienna */
    strcpy(d->net.mqtt_prefix, "heizcore");
    d->net.mqtt_discovery = true;
    d->net.mb_rtu_baud = 19200;
    d->net.mb_tcp_port = 502;
    d->net.mb_slave_id = 1;
    d->net.email_port = 587;

    /* Kessel: Pelletkessel, betriebsbereit vorkonfiguriert */
    d->boiler.enabled   = true;
    d->boiler.type      = BOILER_PELLET;
    strcpy(d->boiler.name, "Pelletkessel");
    d->boiler.setpoint  = 72.0f;
    d->boiler.hysteresis = 4.0f;
    d->boiler.min_temp  = 55.0f;
    d->boiler.max_temp  = 90.0f;
    d->boiler.return_min = 55.0f;
    d->boiler.tp_index  = -1;
    d->boiler.kg_per_h_full = 3.4f;

    /* Puffer */
    d->buffer.enabled = true;
    strcpy(d->buffer.name, "Pufferspeicher");
    d->buffer.volume_l = 1000;
    d->buffer.t_min = 40; d->buffer.t_max = 80;
    for (int i = 0; i < 5; i++) d->buffer.t[i] = NAN;

    /* Warmwasser */
    d->dhw.enabled = true;
    strcpy(d->dhw.name, "Warmwasserboiler");
    d->dhw.setpoint = 55; d->dhw.hysteresis = 5;
    d->dhw.priority = true;
    d->dhw.tp_index = 0;
    d->dhw.legio_enabled = true;
    d->dhw.legio_weekday = 1;      /* Dienstag */
    d->dhw.legio_hour = 2;
    d->dhw.legio_temp = 65;
    d->dhw.temp = NAN;

    /* Solar */
    d->solar.enabled = false;
    strcpy(d->solar.name, "Solaranlage");
    d->solar.dt_on = 7; d->solar.dt_off = 3;
    d->solar.max_store_temp = 85;
    d->solar.collector_max = 130;
    d->solar.flow_lpm = 6;

    /* Heizkreise: HK1 gemischt, HK2 ungemischt vorkonfiguriert */
    for (int i = 0; i < DM_MAX_HC; i++) {
        dm_hc_t *h = &d->hc[i];
        snprintf(h->name, DM_NAME_LEN, "Heizkreis %d", i + 1);
        h->type = HC_MIXED;
        h->mode = HCM_AUTO;
        h->curve_slope = 1.2f;
        h->curve_offset = 0;
        h->flow_min = 20; h->flow_max = 60;
        h->room_set_day = 21; h->room_set_night = 17;
        h->heat_limit = 17;
        h->frost_limit = 3;
        h->room_influence = 0;
        h->tp_index = 1;
        h->mixer_runtime_s = 120;
        h->flow_act = h->return_act = h->room_act = NAN;
    }
    d->hc[0].enabled = true;
    d->hc[1].enabled = true;
    d->hc[1].type = HC_UNMIXED;
    d->hc[1].flow_max = 70;

    /* Zeitprogramme: 0 = Warmwasser, 1 = Heizen */
    strcpy(d->tp[0].name, "Warmwasser");
    d->tp[0].enabled = true;
    d->tp[0].n_switch = 2;
    d->tp[0].sw[0] = (dm_switch_t){ .dow_mask = 0x7F, .on_min = 5 * 60,  .off_min = 8 * 60 };
    d->tp[0].sw[1] = (dm_switch_t){ .dow_mask = 0x7F, .on_min = 16 * 60, .off_min = 21 * 60 };

    strcpy(d->tp[1].name, "Heizbetrieb");
    d->tp[1].enabled = true;
    d->tp[1].n_switch = 2;
    d->tp[1].sw[0] = (dm_switch_t){ .dow_mask = 0x1F, .on_min = 5 * 60 + 30, .off_min = 22 * 60 };
    d->tp[1].sw[1] = (dm_switch_t){ .dow_mask = 0x60, .on_min = 6 * 60 + 30, .off_min = 23 * 60 };

    /* Fühlerplätze: sinnvolle Rollen vorbelegt, Zuordnung per Web-UI */
    static const struct { const char *n; sensor_role_t r; int ri; } pre[] = {
        { "Außentemperatur",  ROLE_OUTDOOR,        0 },
        { "Kessel",           ROLE_BOILER,         0 },
        { "Kessel Rücklauf",  ROLE_RETURN,        -1 },
        { "Abgas",            ROLE_FLUE_GAS,       0 },
        { "Puffer oben",      ROLE_BUFFER_TOP,     0 },
        { "Puffer Mitte",     ROLE_BUFFER_MID,     0 },
        { "Puffer unten",     ROLE_BUFFER_BOTTOM,  0 },
        { "Boiler",           ROLE_DHW,            0 },
        { "Kollektor",        ROLE_COLLECTOR,      0 },
        { "Vorlauf HK1",      ROLE_FLOW,           0 },
        { "Vorlauf HK2",      ROLE_FLOW,           1 },
        { "Raum HK1",         ROLE_ROOM,           0 },
    };
    for (size_t i = 0; i < sizeof(pre) / sizeof(pre[0]); i++) {
        dm_sensor_t *s = &d->sensor[i];
        strncpy(s->name, pre[i].n, DM_NAME_LEN - 1);
        s->role = pre[i].r; s->role_idx = pre[i].ri;
        s->src = SRC_NONE; s->enabled = true; s->value = NAN;
    }

    /* Relais-Vorbelegung (KC868-A8; bei A6 entfallen R7/R8) */
    static const struct { actor_fn_t f; int fi; const char *n; } rel[] = {
        { ACT_BURNER,      0, "Brenneranforderung" },
        { ACT_BOILER_PUMP, 0, "Kesselpumpe" },
        { ACT_HC_PUMP,     0, "Pumpe HK1" },
        { ACT_MIXER_OPEN,  0, "Mischer HK1 auf" },
        { ACT_MIXER_CLOSE, 0, "Mischer HK1 zu" },
        { ACT_HC_PUMP,     1, "Pumpe HK2" },
        { ACT_DHW_PUMP,    0, "Boilerladepumpe" },
        { ACT_SOLAR_PUMP,  0, "Solarpumpe" },
    };
    for (int i = 0; i < 8; i++) {
        d->relay[i].fn = rel[i].f; d->relay[i].fn_idx = rel[i].fi;
        strncpy(d->relay[i].name, rel[i].n, DM_NAME_LEN - 1);
    }

    /* Benutzer: admin/admin (Hash wird bei erster Anmeldung erzwungen
     * geändert – siehe auth.c), Gast ohne Passwort nur lesend */
    strcpy(d->user[0].name, "admin");
    d->user[0].role = ROLE_ADMIN;
    d->user[0].enabled = true;
    /* pw_hash/salt werden von auth_set_password() beim ersten Start gesetzt */

    d->outdoor_temp = NAN;
}

float dm_sensor_by_role(sensor_role_t role, int idx)
{
    float v = NAN;
    dm_t *d = dm_lock();
    for (int i = 0; i < DM_MAX_SENSORS; i++) {
        dm_sensor_t *s = &d->sensor[i];
        if (s->enabled && s->valid && s->role == role &&
            (s->role_idx == idx || s->role_idx == -1)) { v = s->value; break; }
    }
    dm_unlock();
    return v;
}

int dm_relay_by_fn(actor_fn_t fn, int idx)
{
    int r = -1;
    dm_t *d = dm_lock();
    for (int i = 0; i < 8; i++)
        if (d->relay[i].fn == fn && d->relay[i].fn_idx == idx) { r = i; break; }
    dm_unlock();
    return r;
}
