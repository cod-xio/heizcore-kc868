/* heat_curve.c – witterungsgeführte Vorlauftemperatur
 *
 * Zwei Betriebsarten:
 *   1. Klassische Kurve (Steigung/Niveau) nach der in Reglern wie dem
 *      KMS-D üblichen Kennlinienform:
 *        VL = Raumsoll + Niveau
 *             + Steigung · 1,8317984 · (Raumsoll − AT)^0,8281902
 *      (Exponentialform, liefert die typische gekrümmte Kennlinie)
 *   2. Mehrpunkt-Kurve: lineare Interpolation zwischen bis zu 6
 *      frei definierten Stützstellen (Außentemp. → Vorlauf).
 *
 * Ergebnis wird auf flow_min/flow_max begrenzt.
 */
#include <math.h>
#include <time.h>
#include "control.h"

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

float heat_curve_flow(const dm_hc_t *hc, float outdoor, float room_set)
{
    float flow;

    if (hc->n_curve_pts >= 2) {
        /* Mehrpunkt: Stützstellen sind nach Außentemp. sortiert erwartet */
        int n = hc->n_curve_pts;
        if (outdoor <= hc->curve_out[0]) {
            flow = hc->curve_flow[0];
        } else if (outdoor >= hc->curve_out[n - 1]) {
            flow = hc->curve_flow[n - 1];
        } else {
            flow = hc->curve_flow[n - 1];
            for (int i = 0; i < n - 1; i++) {
                if (outdoor >= hc->curve_out[i] && outdoor <= hc->curve_out[i + 1]) {
                    float f = (outdoor - hc->curve_out[i]) /
                              (hc->curve_out[i + 1] - hc->curve_out[i]);
                    flow = hc->curve_flow[i] +
                           f * (hc->curve_flow[i + 1] - hc->curve_flow[i]);
                    break;
                }
            }
        }
        flow += hc->curve_offset;
    } else {
        float dt = room_set - outdoor;
        if (dt < 0) dt = 0;
        flow = room_set + hc->curve_offset +
               hc->curve_slope * 1.8317984f * powf(dt, 0.8281902f);
    }

    return clampf(flow, hc->flow_min, hc->flow_max);
}

/* ── Zeitprogramme ────────────────────────────────────────────────── */
bool timeprog_active(const dm_timeprog_t *tp, const struct tm *now)
{
    if (!tp->enabled) return false;
    int dow = (now->tm_wday + 6) % 7;          /* tm: 0=So → 0=Mo       */
    int minutes = now->tm_hour * 60 + now->tm_min;
    for (int i = 0; i < tp->n_switch; i++) {
        const dm_switch_t *s = &tp->sw[i];
        if (!(s->dow_mask & (1 << dow))) continue;
        if (s->on_min <= s->off_min) {
            if (minutes >= s->on_min && minutes < s->off_min) return true;
        } else {                                /* über Mitternacht      */
            if (minutes >= s->on_min || minutes < s->off_min) return true;
        }
    }
    return false;
}

bool timeprog_active_idx(int tp_index, const struct tm *now)
{
    if (tp_index < 0 || tp_index >= DM_MAX_TIMEPROG) return true;
    extern dm_t *dm_lock(void);
    extern void dm_unlock(void);
    dm_t *d = dm_lock();
    bool r = timeprog_active(&d->tp[tp_index], now);
    dm_unlock();
    return r;
}
