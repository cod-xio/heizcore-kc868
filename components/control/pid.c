/* pid.c – PID mit Anti-Windup (Clamping) und stoßfreiem Start */
#include "control.h"

void pid_init(pid_t_ *p, float kp, float ki, float kd, float omin, float omax)
{
    p->kp = kp; p->ki = ki; p->kd = kd;
    p->out_min = omin; p->out_max = omax;
    pid_reset(p);
}

void pid_reset(pid_t_ *p)
{
    p->integral = 0;
    p->prev_err = 0;
    p->first = true;
}

float pid_step(pid_t_ *p, float setpoint, float actual, float dt)
{
    float err = setpoint - actual;
    float d = 0;
    if (!p->first && dt > 0) d = (err - p->prev_err) / dt;
    p->first = false;
    p->prev_err = err;

    p->integral += err * dt;

    float out = p->kp * err + p->ki * p->integral + p->kd * d;

    /* Anti-Windup: Integral zurücknehmen, wenn Ausgang begrenzt */
    if (out > p->out_max) {
        if (p->ki > 0) p->integral -= (out - p->out_max) / p->ki;
        out = p->out_max;
    } else if (out < p->out_min) {
        if (p->ki > 0) p->integral -= (out - p->out_min) / p->ki;
        out = p->out_min;
    }
    return out;
}
