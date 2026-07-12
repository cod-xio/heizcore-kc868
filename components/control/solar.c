/* solar.c – Solarthermie: Differenzregelung + Ertragsbilanz */
#include <math.h>
#include "control.h"
#include "alarms.h"

void solar_step(dm_t *d, const struct tm *now, float dt)
{
    dm_solar_t *s = &d->solar;
    if (!s->enabled) { ctl_actor(d, ACT_SOLAR_PUMP, 0, false); return; }

    s->t_collector = dm_sensor_by_role(ROLE_COLLECTOR, 0);
    s->t_store     = d->buffer.enabled ? d->buffer.t[4] : d->dhw.temp; /* unten */
    if (isnan(s->t_store)) s->t_store = d->dhw.temp;

    if (isnan(s->t_collector) || isnan(s->t_store)) {
        ctl_actor(d, ACT_SOLAR_PUMP, 0, false);
        s->pump_on = false;
        return;
    }

    /* Kollektor-Schutz: über Maximum immer pumpen (Wärme abführen),
     * außer der Speicher ist selbst am Limit. */
    bool protect = s->t_collector >= s->collector_max;
    if (protect)
        alarm_raise(ALARM_WARNING, ALM_SOLAR_COLL_MAX, "Kollektor-Übertemperatur");
    else
        alarm_clear(ALM_SOLAR_COLL_MAX);

    bool want = s->pump_on;
    float dT = s->t_collector - s->t_store;
    if (dT >= s->dt_on)  want = true;
    if (dT <= s->dt_off) want = false;
    if (s->t_store >= s->max_store_temp && !protect) want = false;
    if (protect && s->t_store < s->max_store_temp + 5) want = true;

    s->pump_on = want;
    ctl_actor(d, ACT_SOLAR_PUMP, 0, want);

    /* ── Ertragsbilanz: P = ṁ·c·ΔT (Wasser/Glykol vereinfacht) ──── */
    if (want && dT > 0) {
        float kw = (s->flow_lpm / 60.0f) * 4.19f * dT / 1.0f / 1000.0f * 1000.0f;
        /* l/min → kg/s ≈ /60; c=4,19 kJ/kgK; kW = kg/s·kJ/kgK·K */
        kw = (s->flow_lpm / 60.0f) * 4.19f * dT;      /* kW */
        float kwh = kw * dt / 3600.0f;
        s->yield_day_kwh   += kwh;
        s->yield_month_kwh += kwh;
        s->yield_year_kwh  += kwh;
        s->yield_total_kwh += kwh;
    }

    /* Kalenderwechsel: Zähler zurücksetzen */
    if (s->yield_day != now->tm_yday)   { s->yield_day = now->tm_yday;     s->yield_day_kwh = 0; }
    if (s->yield_month != now->tm_mon)  { s->yield_month = now->tm_mon;    s->yield_month_kwh = 0; }
    if (s->yield_year != now->tm_year)  { s->yield_year = now->tm_year;    s->yield_year_kwh = 0; }
}
