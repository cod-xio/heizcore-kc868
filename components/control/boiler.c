/* boiler.c – Kesselregelung (Zustandsautomat + Rücklaufanhebung)
 *
 * Zustände: AUS → BEREIT → ZÜNDUNG → LEISTUNGSBRAND → TEILLAST/VOLLLAST
 *           → GLUTERHALTUNG → BEREIT   (STÖRUNG aus jedem Zustand)
 *
 * Die Brenneranforderung wird als Relais (ACT_BURNER) ausgegeben –
 * geeignet für Kessel mit Freigabekontakt. Bei modulierenden Kesseln
 * kann die Leistung zusätzlich über Modbus geschrieben werden
 * (modbus_svc.c, Registerkarte "Modbus" der Weboberfläche).
 *
 * Überwachung:
 *   - Übertemperatur  → Störung + Alarm
 *   - Zündüberwachung → Brenner an, aber kein Temperaturanstieg
 *   - Abgastemperatur → Warnung bei Überschreitung
 */
#include <math.h>
#include "control.h"
#include "alarms.h"

#define IGNITION_TIMEOUT_S   (20 * 60)  /* max. Zeit ohne Temperaturanstieg */
#define IGNITION_MIN_RISE    3.0f       /* K Anstieg zum Nachweis der Zündung */
#define FLUE_WARN_C          280.0f
#define EMBER_HOLD_S         (15 * 60)

static float    s_ignition_start_temp;
static uint32_t s_state_timer;

static void set_state(dm_boiler_t *b, boiler_state_t st)
{
    if (b->state != st) {
        b->state = st;
        s_state_timer = 0;
        if (st == BST_IGNITION) {
            b->starts++;
            s_ignition_start_temp = b->temp;
        }
    }
}

void boiler_step(dm_t *d, const struct tm *now, float dt)
{
    dm_boiler_t *b = &d->boiler;
    if (!b->enabled) { ctl_actor(d, ACT_BURNER, 0, false); return; }

    b->temp        = dm_sensor_by_role(ROLE_BOILER, 0);
    b->return_temp = dm_sensor_by_role(ROLE_RETURN, -1);
    b->flue_temp   = dm_sensor_by_role(ROLE_FLUE_GAS, 0);
    s_state_timer += (uint32_t)dt;

    bool sensor_ok = !isnan(b->temp);
    if (!sensor_ok) {
        alarm_raise(ALARM_CRITICAL, ALM_SENSOR_FAIL + 1, "Kesselfühler ausgefallen");
        set_state(b, BST_FAULT);
    }

    /* Sicherheitsabschaltung hat immer Vorrang */
    if (sensor_ok && b->temp >= b->max_temp) {
        alarm_raise(ALARM_CRITICAL, ALM_BOILER_OVERTEMP, "Kessel-Übertemperatur");
        set_state(b, BST_FAULT);
    }
    if (!isnan(b->flue_temp) && b->flue_temp > FLUE_WARN_C)
        alarm_raise(ALARM_WARNING, ALM_BOILER_FLUE_HIGH, "Abgastemperatur zu hoch");
    else
        alarm_clear(ALM_BOILER_FLUE_HIGH);

    /* Wärmeanforderung: Zeitprogramm + (Puffer unter Minimum ODER
     * mind. ein Heizkreis/WW fordert an) */
    bool tp_ok = timeprog_active(&d->tp[b->tp_index < 0 ? 0 : b->tp_index], now) ||
                 b->tp_index < 0;
    bool demand = false;
    if (d->buffer.enabled && !isnan(d->buffer.t[0]))
        demand = d->buffer.t[0] < d->buffer.t_min;
    for (int i = 0; i < DM_MAX_HC; i++)
        if (d->hc[i].enabled && d->hc[i].demand) demand = true;
    if (d->dhw.enabled && d->dhw.charging) demand = true;
    demand = demand && tp_ok;

    switch (b->state) {
    case BST_FAULT:
        ctl_actor(d, ACT_BURNER, 0, false);
        b->power_pct = 0;
        /* Störung wird über die Weboberfläche quittiert (REST /api/boiler/reset) */
        return;

    case BST_OFF:
        ctl_actor(d, ACT_BURNER, 0, false);
        b->power_pct = 0;
        if (demand) set_state(b, BST_READY);
        break;

    case BST_READY:
        ctl_actor(d, ACT_BURNER, 0, false);
        b->power_pct = 0;
        if (demand && sensor_ok && b->temp < b->setpoint - b->hysteresis)
            set_state(b, BST_IGNITION);
        else if (!demand)
            set_state(b, BST_OFF);
        break;

    case BST_IGNITION:
        ctl_actor(d, ACT_BURNER, 0, true);
        b->power_pct = 100;
        if (sensor_ok && b->temp > s_ignition_start_temp + IGNITION_MIN_RISE)
            set_state(b, BST_FIRING);
        else if (s_state_timer > IGNITION_TIMEOUT_S) {
            alarm_raise(ALARM_CRITICAL, ALM_BOILER_NO_RISE,
                        "Zündung fehlgeschlagen – kein Temperaturanstieg");
            set_state(b, BST_FAULT);
        }
        break;

    case BST_FIRING:
    case BST_PART_LOAD:
    case BST_FULL_LOAD: {
        ctl_actor(d, ACT_BURNER, 0, true);
        /* Pseudo-Modulation über Abstand zum Sollwert */
        float gap = b->setpoint - b->temp;
        if (gap > b->hysteresis)        { b->power_pct = 100; set_state(b, BST_FULL_LOAD); }
        else if (gap > 0)               { b->power_pct = 30 + 70 * gap / b->hysteresis;
                                          set_state(b, BST_PART_LOAD); }
        else                            { set_state(b, BST_EMBER_HOLD); }
        if (!demand) set_state(b, BST_EMBER_HOLD);
        break;
    }

    case BST_EMBER_HOLD:
        ctl_actor(d, ACT_BURNER, 0, false);
        b->power_pct = 0;
        if (demand && sensor_ok && b->temp < b->setpoint - b->hysteresis)
            set_state(b, BST_IGNITION);
        else if (s_state_timer > EMBER_HOLD_S)
            set_state(b, demand ? BST_READY : BST_OFF);
        break;
    }

    /* Kesselpumpe mit Rücklaufanhebung: Pumpe erst freigeben, wenn der
     * Kessel über der Mindesttemperatur liegt (Kondensatschutz). */
    bool burner_on = ctl_actor_get(d, ACT_BURNER, 0);
    bool pump = false;
    if (sensor_ok) {
        if (burner_on && b->temp >= b->min_temp) pump = true;
        if (!isnan(b->return_temp) && b->return_temp < b->return_min - 5 &&
            b->temp < b->min_temp) pump = false;      /* Anhebung schützen */
        if (!burner_on && b->temp > b->setpoint + 5) pump = true; /* Restwärme */
    }
    ctl_actor(d, ACT_BOILER_PUMP, 0, pump);

    /* Statistik */
    if (burner_on) {
        b->runtime_s += (uint32_t)dt;
        b->consumption_kg += b->kg_per_h_full * (b->power_pct / 100.0f) * dt / 3600.0f;
    }
}
