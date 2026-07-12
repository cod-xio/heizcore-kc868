/* heating_circuit.c – bis zu 20 Heizkreise, gemischt/ungemischt
 *
 * Gemischte Kreise: Dreipunkt-Mischersteuerung (Auf/Zu-Relais) mit
 * PI-Regler; die Impulsdauer wird aus der Regelabweichung abgeleitet
 * und über die Mischer-Stellzeit normiert. Die Mischerstellung wird
 * als Laufzeitintegral geschätzt (mixer_pos_pct).
 *
 * Betriebsarten: Automatik (Zeitprogramm Tag/Nacht), Tag, Nacht,
 * Sommer (aus, nur Frostschutz), Urlaub (Absenkung bis Datum),
 * Frostschutz, Party (Tagbetrieb bis Uhrzeit), Aus.
 */
#include <math.h>
#include <string.h>
#include "control.h"
#include "alarms.h"

typedef struct {
    pid_t_ pid;
    float  pulse_budget_s;   /* auszugebende Mischer-Fahrzeit (+auf/−zu) */
    bool   pid_ready;
} hc_rt_t;

static hc_rt_t s_rt[DM_MAX_HC];

static bool is_day(const dm_hc_t *h, const struct tm *now)
{
    switch (h->mode) {
    case HCM_DAY:    return true;
    case HCM_NIGHT:  return false;
    case HCM_PARTY:  return time(NULL) < h->party_until ? true
                            : timeprog_active_idx(h->tp_index, now);
    case HCM_AUTO:
    default:         return timeprog_active_idx(h->tp_index, now);
    }
}

void hc_step(dm_t *d, int i, const struct tm *now, float dt)
{
    dm_hc_t *h = &d->hc[i];
    hc_rt_t *rt = &s_rt[i];
    if (!h->enabled) return;

    h->flow_act   = dm_sensor_by_role(ROLE_FLOW, i);
    h->return_act = dm_sensor_by_role(ROLE_RETURN, i);
    h->room_act   = dm_sensor_by_role(ROLE_ROOM, i);
    float outdoor = d->outdoor_temp;

    /* Urlaubs-/Party-Ende automatisch zurücksetzen */
    time_t t = time(NULL);
    if (h->mode == HCM_HOLIDAY && h->holiday_until && t > h->holiday_until) h->mode = HCM_AUTO;
    if (h->mode == HCM_PARTY   && h->party_until   && t > h->party_until)   h->mode = HCM_AUTO;

    bool day = is_day(h, now);
    float room_set = day ? h->room_set_day : h->room_set_night;
    if (h->mode == HCM_HOLIDAY) room_set = h->room_set_night;

    /* Heizgrenze / Sommer / Frost */
    bool heating_off = false;
    if (h->mode == HCM_OFF || h->mode == HCM_SUMMER) heating_off = true;
    if (!isnan(outdoor) && outdoor >= h->heat_limit)  heating_off = true;

    bool frost = !isnan(outdoor) && outdoor <= h->frost_limit;
    if (h->mode == HCM_FROST) { heating_off = !frost; room_set = 8; }
    if (heating_off && frost) { heating_off = false; room_set = 8; } /* Frostschutz übersteuert */

    if (heating_off) {
        h->pump_on = false;
        h->demand = false;
        h->flow_set = 0;
        ctl_actor(d, ACT_HC_PUMP, i, false);
        if (h->type == HC_MIXED) {                    /* Mischer zufahren */
            ctl_actor(d, ACT_MIXER_OPEN, i, false);
            ctl_actor(d, ACT_MIXER_CLOSE, i, true);
            h->mixer_pos_pct -= 100.0f * dt / h->mixer_runtime_s;
            if (h->mixer_pos_pct <= 0) {
                h->mixer_pos_pct = 0;
                ctl_actor(d, ACT_MIXER_CLOSE, i, false);
            }
        }
        pid_reset(&rt->pid);
        rt->pid_ready = false;
        return;
    }

    /* Soll-Vorlauf aus Heizkurve + optionaler Raumeinfluss */
    float flow_set = heat_curve_flow(h, isnan(outdoor) ? 0 : outdoor, room_set);
    if (h->room_influence > 0 && !isnan(h->room_act))
        flow_set += h->room_influence * (room_set - h->room_act);
    if (flow_set > h->flow_max) flow_set = h->flow_max;
    if (flow_set < h->flow_min) flow_set = h->flow_min;
    h->flow_set = flow_set;

    /* WW-Vorrang: Kreis pausiert während Boilerladung */
    if (d->dhw.enabled && d->dhw.priority && d->dhw.charging) {
        h->pump_on = false;
        h->demand = false;
        ctl_actor(d, ACT_HC_PUMP, i, false);
        return;
    }

    h->pump_on = true;
    h->demand = true;
    ctl_actor(d, ACT_HC_PUMP, i, true);

    if (h->type == HC_UNMIXED) return;   /* fertig: Pumpe genügt */

    /* --- Dreipunkt-Mischer --------------------------------------- */
    if (isnan(h->flow_act)) {
        alarm_raise(ALARM_WARNING, ALM_SENSOR_FAIL + 10 + i, "Vorlauffühler fehlt");
        ctl_actor(d, ACT_MIXER_OPEN, i, false);
        ctl_actor(d, ACT_MIXER_CLOSE, i, false);
        return;
    }
    alarm_clear(ALM_SENSOR_FAIL + 10 + i);

    if (!rt->pid_ready) {
        /* PI: Ausgang = Mischer-Fahrzeit in s pro Zyklus */
        pid_init(&rt->pid, 1.2f, 0.02f, 0.0f, -8.0f, 8.0f);
        rt->pid_ready = true;
    }
    float drive_s = pid_step(&rt->pid, flow_set, h->flow_act, dt);

    /* Totband: kleine Abweichungen nicht ausregeln (Relais schonen) */
    if (fabsf(flow_set - h->flow_act) < 1.0f) drive_s = 0;
    rt->pulse_budget_s += drive_s * dt / 10.0f;   /* sanft dosieren */
    if (rt->pulse_budget_s > 5)  rt->pulse_budget_s = 5;
    if (rt->pulse_budget_s < -5) rt->pulse_budget_s = -5;

    bool open_ = rt->pulse_budget_s > 0.5f;
    bool close = rt->pulse_budget_s < -0.5f;
    ctl_actor(d, ACT_MIXER_OPEN, i, open_);
    ctl_actor(d, ACT_MIXER_CLOSE, i, close);
    if (open_) {
        rt->pulse_budget_s -= dt;
        h->mixer_pos_pct += 100.0f * dt / h->mixer_runtime_s;
    } else if (close) {
        rt->pulse_budget_s += dt;
        h->mixer_pos_pct -= 100.0f * dt / h->mixer_runtime_s;
    }
    if (h->mixer_pos_pct > 100) h->mixer_pos_pct = 100;
    if (h->mixer_pos_pct < 0)   h->mixer_pos_pct = 0;
}
