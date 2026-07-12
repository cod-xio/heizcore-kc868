/* dhw.c – Warmwasser: Boilerladung, Zeitprogramm, Legionellenschutz */
#include <math.h>
#include "control.h"
#include "alarms.h"

#define LEGIO_MAX_DURATION_S (3 * 3600)

static uint32_t s_legio_timer;
static int      s_legio_last_day = -1;

void dhw_step(dm_t *d, const struct tm *now, float dt)
{
    dm_dhw_t *w = &d->dhw;
    if (!w->enabled) { ctl_actor(d, ACT_DHW_PUMP, 0, false); return; }

    w->temp = dm_sensor_by_role(ROLE_DHW, 0);
    if (isnan(w->temp)) {
        alarm_raise(ALARM_WARNING, ALM_SENSOR_FAIL + 8, "Boilerfühler fehlt");
        ctl_actor(d, ACT_DHW_PUMP, 0, false);
        w->charging = false;
        return;
    }
    alarm_clear(ALM_SENSOR_FAIL + 8);

    /* Urlaubsmodus: keine Ladung, Legionellenschutz bleibt aktiv */
    bool holiday = w->holiday_until && time(NULL) < w->holiday_until;

    /* ── Legionellenschutz: 1×/Woche auf legio_temp aufheizen ───── */
    int dow = (now->tm_wday + 6) % 7;
    if (w->legio_enabled && !w->legio_active &&
        dow == w->legio_weekday && now->tm_hour == w->legio_hour &&
        s_legio_last_day != now->tm_yday) {
        w->legio_active = true;
        s_legio_timer = 0;
        s_legio_last_day = now->tm_yday;
    }
    if (w->legio_active) {
        s_legio_timer += (uint32_t)dt;
        if (w->temp >= w->legio_temp) {
            w->legio_active = false;
            alarm_clear(ALM_DHW_LEGIO_FAIL);
        } else if (s_legio_timer > LEGIO_MAX_DURATION_S) {
            w->legio_active = false;
            alarm_raise(ALARM_WARNING, ALM_DHW_LEGIO_FAIL,
                        "Legionellenschutz: Zieltemperatur nicht erreicht");
        }
    }

    float set  = w->legio_active ? w->legio_temp : w->setpoint;
    bool  tp_ok = timeprog_active_idx(w->tp_index, now);

    /* Zweipunktregelung mit Hysterese */
    bool want = w->charging;
    if (w->temp < set - w->hysteresis) want = true;
    if (w->temp >= set)                want = false;
    if (!tp_ok && !w->legio_active)    want = false;
    if (holiday && !w->legio_active)   want = false;

    /* Quelle muss wärmer sein als der Boiler (Puffer oben bzw. Kessel) */
    float src = d->buffer.enabled ? d->buffer.t[0] : d->boiler.temp;
    if (!isnan(src) && src < w->temp + 5) want = false;

    w->charging = want;
    ctl_actor(d, ACT_DHW_PUMP, 0, want);
}
