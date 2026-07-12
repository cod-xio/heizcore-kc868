#pragma once
#include <stdbool.h>
#include <time.h>
#include "datamodel.h"

/* ── PID-Regler ─────────────────────────────────────────────────── */
typedef struct {
    float kp, ki, kd;
    float out_min, out_max;
    float integral;
    float prev_err;
    bool  first;
} pid_t_;

void  pid_init(pid_t_ *p, float kp, float ki, float kd, float omin, float omax);
float pid_step(pid_t_ *p, float setpoint, float actual, float dt);
void  pid_reset(pid_t_ *p);

/* ── Heizkurve ──────────────────────────────────────────────────── */
float heat_curve_flow(const dm_hc_t *hc, float outdoor, float room_set);

/* ── Zeitprogramm ───────────────────────────────────────────────── */
bool timeprog_active(const dm_timeprog_t *tp, const struct tm *now);
bool timeprog_active_idx(int tp_index, const struct tm *now); /* -1 → true */

/* ── Teilregler (werden vom control_task 1×/s aufgerufen) ───────── */
void boiler_step(dm_t *d, const struct tm *now, float dt);
void buffer_step(dm_t *d, float dt);
void hc_step(dm_t *d, int idx, const struct tm *now, float dt);
void dhw_step(dm_t *d, const struct tm *now, float dt);
void solar_step(dm_t *d, const struct tm *now, float dt);

/* ── Task-Start ─────────────────────────────────────────────────── */
void control_start(void);

/* Relais-Wunschzustand setzen (wird am Zyklusende auf HW geschrieben) */
void ctl_actor(dm_t *d, actor_fn_t fn, int idx, bool on);
bool ctl_actor_get(dm_t *d, actor_fn_t fn, int idx);
