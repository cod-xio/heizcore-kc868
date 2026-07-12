#pragma once
#include <stdbool.h>
#include <time.h>

#define ALARM_MAX_ACTIVE 32

typedef enum { ALARM_INFO = 0, ALARM_WARNING, ALARM_CRITICAL } alarm_level_t;

/* Alarm-Codes (stabil halten – werden geloggt und per MQTT gemeldet) */
enum {
    ALM_SENSOR_FAIL      = 100,  /* +Sensorindex                        */
    ALM_BOILER_OVERTEMP  = 200,
    ALM_BOILER_NO_RISE   = 201,  /* Brenner an, Temperatur steigt nicht */
    ALM_BOILER_FLUE_HIGH = 202,
    ALM_BUFFER_OVERTEMP  = 300,
    ALM_DHW_LEGIO_FAIL   = 400,
    ALM_SOLAR_COLL_MAX   = 500,
    ALM_HC_FROST         = 600,  /* +Heizkreisindex                     */
    ALM_NET_MQTT_DOWN    = 700,
    ALM_SYS_FS_FULL      = 800,
    ALM_SYS_TIME_INVALID = 801,
};

typedef struct {
    alarm_level_t level;
    int           code;
    time_t        timestamp;
    char          message[96];
} alarm_t;

typedef void (*alarm_notify_cb_t)(const alarm_t *a);

void alarms_init(void);
void alarm_raise(alarm_level_t lvl, int code, const char *msg);
void alarm_clear(int code);
int  alarms_get_active(alarm_t *out, int max);
bool alarms_has_critical(void);
void alarms_register_notify(alarm_notify_cb_t cb);
