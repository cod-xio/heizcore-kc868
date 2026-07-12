/* buffer.c – Pufferspeicher: Mehrpunktmessung, Ladezustand, Restenergie */
#include <math.h>
#include "control.h"
#include "alarms.h"

#define T_REF 30.0f   /* Referenztemperatur für Restenergie */

void buffer_step(dm_t *d, float dt)
{
    (void)dt;
    dm_buffer_t *p = &d->buffer;
    if (!p->enabled) return;

    static const sensor_role_t roles[5] = {
        ROLE_BUFFER_TOP, ROLE_BUFFER_MID_TOP, ROLE_BUFFER_MID,
        ROLE_BUFFER_MID_BOT, ROLE_BUFFER_BOTTOM,
    };
    int valid = 0;
    float sum = 0;
    for (int i = 0; i < 5; i++) {
        p->t[i] = dm_sensor_by_role(roles[i], 0);
        if (!isnan(p->t[i])) { valid++; sum += p->t[i]; }
    }
    if (valid == 0) { p->charge_pct = 0; p->energy_kwh = 0; return; }

    float t_avg = sum / valid;

    /* Ladezustand: linear zwischen t_min (0 %) und t_max (100 %) */
    float pct = (t_avg - p->t_min) / (p->t_max - p->t_min) * 100.0f;
    p->charge_pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);

    /* Restenergie ggü. 30 °C: E = m·c·ΔT (Wasser: 1,163 Wh/kg·K) */
    float dT = t_avg - T_REF;
    if (dT < 0) dT = 0;
    p->energy_kwh = p->volume_l * 1.163f * dT / 1000.0f;

    if (!isnan(p->t[0]) && p->t[0] > p->t_max + 8)
        alarm_raise(ALARM_WARNING, ALM_BUFFER_OVERTEMP, "Pufferspeicher überhitzt");
    else
        alarm_clear(ALM_BUFFER_OVERTEMP);
}
