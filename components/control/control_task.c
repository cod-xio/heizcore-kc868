/* control_task.c – zentraler Regelungs-Task (1-s-Zyklus, Priorität 5)
 *
 * Zyklus:
 *   1. board_poll()           – Hardware-Messung (DS18B20, DIN)
 *   2. Fühler → Datenmodell   – Rollenzuordnung, Offsets, Gültigkeit
 *   3. Teilregler             – Puffer, WW, Heizkreise, Solar, Kessel
 *   4. Relaisausgabe          – Wunschzustände auf PCF8574 schreiben
 *   5. Statistik              – Laufzeiten, Starts
 */
#include <math.h>
#include <string.h>
#include "control.h"
#include "board.h"
#include "alarms.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "control";
static bool s_actor_want[8];

void ctl_actor(dm_t *d, actor_fn_t fn, int idx, bool on)
{
    for (int i = 0; i < 8; i++)
        if (d->relay[i].fn == fn && d->relay[i].fn_idx == idx)
            s_actor_want[i] = on;
}

bool ctl_actor_get(dm_t *d, actor_fn_t fn, int idx)
{
    for (int i = 0; i < 8; i++)
        if (d->relay[i].fn == fn && d->relay[i].fn_idx == idx)
            return s_actor_want[i];
    return false;
}

/* Fühlerwerte aus HAL/Modbus/MQTT ins Datenmodell übernehmen */
static void update_sensors(dm_t *d)
{
    for (int i = 0; i < DM_MAX_SENSORS; i++) {
        dm_sensor_t *s = &d->sensor[i];
        if (!s->enabled || s->src == SRC_NONE) { continue; }
        float v = NAN;
        switch (s->src) {
        case SRC_DS18B20: {
            const ds18b20_t *ds = board_ds18b20_by_rom(s->ref);
            if (ds && ds->valid) v = ds->temp_c;
            break;
        }
        case SRC_NTC:
            v = board_ain_ntc10k((int)s->ref);
            break;
        case SRC_MODBUS:
        case SRC_MQTT:
            /* Wert wird asynchron von modbus_svc/mqtt_svc geschrieben;
             * hier nur Gültigkeit anhand des Alters prüfen. */
            v = s->value;
            break;
        case SRC_FIXED:
            v = (float)(int64_t)s->ref / 10.0f;
            break;
        default: break;
        }
        if (!isnan(v)) {
            s->value = v + s->offset;
            s->valid = true;
            alarm_clear(ALM_SENSOR_FAIL + 100 + i);
        } else if (s->src == SRC_DS18B20 || s->src == SRC_NTC) {
            if (s->valid)
                alarm_raise(ALARM_WARNING, ALM_SENSOR_FAIL + 100 + i, s->name);
            s->valid = false;
        }
    }

    /* Außentemperatur gleitend mitteln (dämpft Kurzzeitschwankungen) */
    float ot = dm_sensor_by_role(ROLE_OUTDOOR, 0);
    if (!isnan(ot)) {
        if (isnan(d->outdoor_temp)) d->outdoor_temp = ot;
        else d->outdoor_temp += (ot - d->outdoor_temp) * 0.02f;  /* τ ≈ 50 s */
    }
}

static void write_relays(dm_t *d, float dt)
{
    for (int i = 0; i < board_relay_count(); i++) {
        bool cur = board_relay_get(i);
        if (s_actor_want[i] != cur) {
            board_relay_set(i, s_actor_want[i]);
            if (s_actor_want[i]) d->relay[i].starts++;
        }
        d->relay[i].state = s_actor_want[i];
        if (s_actor_want[i]) d->relay[i].runtime_s += (uint32_t)dt;
    }
}

static void control_task(void *arg)
{
    ESP_LOGI(TAG, "Regelungs-Task gestartet (1-s-Zyklus)");
    TickType_t last_wake = xTaskGetTickCount();
    int64_t prev_us = esp_timer_get_time();
    uint32_t save_counter = 0;

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000));
        int64_t now_us = esp_timer_get_time();
        float dt = (now_us - prev_us) / 1e6f;
        prev_us = now_us;
        if (dt <= 0 || dt > 10) dt = 1.0f;

        board_poll();

        time_t t = time(NULL);
        struct tm now;
        localtime_r(&t, &now);
        if (now.tm_year < 120)
            alarm_raise(ALARM_INFO, ALM_SYS_TIME_INVALID, "Systemzeit nicht gesetzt (NTP?)");
        else
            alarm_clear(ALM_SYS_TIME_INVALID);

        dm_t *d = dm_lock();
        update_sensors(d);
        buffer_step(d, dt);
        dhw_step(d, &now, dt);
        for (int i = 0; i < DM_MAX_HC; i++)
            hc_step(d, i, &now, dt);
        solar_step(d, &now, dt);
        boiler_step(d, &now, dt);   /* zuletzt: sieht alle Anforderungen */
        write_relays(d, dt);
        dm_unlock();

        /* Betriebsstunden alle 15 min persistieren */
        if (++save_counter >= 900) {
            save_counter = 0;
            extern esp_err_t cfg_save(void);
            cfg_save();
        }
    }
}

void control_start(void)
{
    xTaskCreatePinnedToCore(control_task, "control", 6144, NULL, 5, NULL, 1);
}
