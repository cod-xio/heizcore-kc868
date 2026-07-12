/**
 * datamodel.h – zentrales Datenmodell (Konfiguration + Laufzeitzustand)
 *
 * Der gesamte Zustand der Anlage liegt in einer mutex-geschützten
 * Struktur. Regelung, Webserver, MQTT, Modbus und Logger arbeiten
 * ausschließlich über dm_lock()/dm_unlock() bzw. die Snapshot-API.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define FIRMWARE_VERSION   "1.0.0"

#define DM_MAX_HC          20   /* Heizkreise                          */
#define DM_MAX_SENSORS     40   /* logische Fühler                     */
#define DM_MAX_TIMEPROG    24   /* Zeitprogramme                       */
#define DM_MAX_SWITCH      42   /* Schaltfenster pro Programm (6/Tag)  */
#define DM_MAX_USERS       8
#define DM_MAX_CURVE_PTS   6    /* Mehrpunkt-Heizkurve                 */
#define DM_NAME_LEN        32

/* ── Fühlerquellen ─────────────────────────────────────────────── */
typedef enum {
    SRC_NONE = 0,
    SRC_DS18B20,        /* ref = ROM-Code                             */
    SRC_NTC,            /* ref = Analogeingang 0-3                    */
    SRC_MODBUS,         /* ref = (slave<<16)|register                 */
    SRC_MQTT,           /* Wert wird per MQTT geschrieben             */
    SRC_FIXED,          /* Festwert (Simulation/Ersatzwert)           */
} sensor_src_t;

typedef enum {
    ROLE_NONE = 0, ROLE_OUTDOOR, ROLE_ROOM, ROLE_BOILER, ROLE_FLOW,
    ROLE_RETURN, ROLE_BUFFER_TOP, ROLE_BUFFER_MID_TOP, ROLE_BUFFER_MID,
    ROLE_BUFFER_MID_BOT, ROLE_BUFFER_BOTTOM, ROLE_DHW, ROLE_COLLECTOR,
    ROLE_FLUE_GAS,
} sensor_role_t;

typedef struct {
    char          name[DM_NAME_LEN];
    sensor_src_t  src;
    sensor_role_t role;
    int           role_idx;      /* z. B. Heizkreis-Nr. bei ROLE_FLOW  */
    uint64_t      ref;
    float         offset;        /* Kalibrier-Offset                   */
    float         value;         /* Laufzeit: letzter Wert (°C)        */
    bool          valid;
    bool          enabled;
} dm_sensor_t;

/* ── Relais-/Aktorzuordnung ────────────────────────────────────── */
typedef enum {
    ACT_NONE = 0, ACT_BURNER, ACT_BOILER_PUMP, ACT_HC_PUMP, ACT_MIXER_OPEN,
    ACT_MIXER_CLOSE, ACT_DHW_PUMP, ACT_SOLAR_PUMP, ACT_VALVE, ACT_ALARM_OUT,
} actor_fn_t;

typedef struct {
    actor_fn_t fn;
    int        fn_idx;           /* Heizkreis-Nr. etc.                 */
    char       name[DM_NAME_LEN];
    bool       state;            /* Laufzeit                            */
    uint32_t   runtime_s;        /* Betriebsstundenzähler               */
    uint32_t   starts;
} dm_relay_t;

/* ── Zeitprogramme ─────────────────────────────────────────────── */
typedef struct {
    uint8_t  dow_mask;           /* Bit0=Mo … Bit6=So                  */
    uint16_t on_min, off_min;    /* Minuten seit 0:00                  */
} dm_switch_t;

typedef struct {
    char        name[DM_NAME_LEN];
    bool        enabled;
    int         n_switch;
    dm_switch_t sw[DM_MAX_SWITCH];
} dm_timeprog_t;

/* ── Kessel ─────────────────────────────────────────────────────── */
typedef enum {
    BOILER_PELLET = 0, BOILER_LOG_WOOD, BOILER_WOODCHIP, BOILER_HEATPUMP,
    BOILER_GAS, BOILER_OIL, BOILER_ELECTRIC, BOILER_HYBRID,
} boiler_type_t;

typedef enum {
    BST_OFF = 0, BST_READY, BST_IGNITION, BST_FIRING, BST_PART_LOAD,
    BST_FULL_LOAD, BST_EMBER_HOLD, BST_FAULT,
} boiler_state_t;

typedef struct {
    bool          enabled;
    boiler_type_t type;
    char          name[DM_NAME_LEN];
    float         setpoint;        /* Soll-Kesseltemperatur             */
    float         hysteresis;
    float         min_temp;        /* Mindesttemp. (Kondensatschutz)    */
    float         max_temp;        /* Sicherheitsabschaltung            */
    float         return_min;      /* Rücklaufanhebung                  */
    int           tp_index;        /* Zeitprogramm (-1 = keins)         */
    /* Laufzeit */
    boiler_state_t state;
    float          temp, return_temp, flue_temp;
    float          power_pct;      /* Modulationsgrad                    */
    uint32_t       runtime_s, starts;
    float          consumption_kg; /* Verbrauchsschätzung (Pellets)      */
    float          kg_per_h_full;  /* Verbrauch bei Volllast             */
} dm_boiler_t;

/* ── Pufferspeicher ─────────────────────────────────────────────── */
typedef struct {
    bool  enabled;
    char  name[DM_NAME_LEN];
    float volume_l;
    float t_min, t_max;           /* Lade-Sollfenster                    */
    /* Laufzeit */
    float t[5];                   /* oben … unten                        */
    float charge_pct;             /* Ladezustand                         */
    float energy_kwh;             /* Restenergie ggü. 30 °C Referenz     */
} dm_buffer_t;

/* ── Heizkreis ──────────────────────────────────────────────────── */
typedef enum { HC_UNMIXED = 0, HC_MIXED } hc_type_t;
typedef enum {
    HCM_AUTO = 0, HCM_DAY, HCM_NIGHT, HCM_SUMMER, HCM_HOLIDAY,
    HCM_FROST, HCM_PARTY, HCM_OFF,
} hc_mode_t;

typedef struct {
    bool      enabled;
    char      name[DM_NAME_LEN];
    hc_type_t type;
    hc_mode_t mode;
    /* Heizkurve */
    float curve_slope;            /* 0,2 … 3,5                           */
    float curve_offset;           /* Parallelverschiebung                */
    int   n_curve_pts;            /* 0 = klassische Kurve, sonst Punkte  */
    float curve_out[DM_MAX_CURVE_PTS], curve_flow[DM_MAX_CURVE_PTS];
    float flow_min, flow_max;
    float room_set_day, room_set_night;
    float heat_limit;             /* Heizgrenze (Sommerabschaltung)      */
    float frost_limit;
    float room_influence;         /* K Vorlauf je K Raumabweichung       */
    int   tp_index;
    int   mixer_runtime_s;        /* Stellzeit des Mischers              */
    time_t holiday_until;
    time_t party_until;
    /* Laufzeit */
    float flow_set, flow_act, return_act, room_act;
    float mixer_pos_pct;
    bool  pump_on;
    bool  demand;                 /* Wärmeanforderung an Erzeuger        */
} dm_hc_t;

/* ── Warmwasser ─────────────────────────────────────────────────── */
typedef struct {
    bool  enabled;
    char  name[DM_NAME_LEN];
    float setpoint, hysteresis;
    bool  priority;               /* WW-Vorrang vor Heizkreisen          */
    int   tp_index;
    /* Legionellenschutz */
    bool  legio_enabled;
    int   legio_weekday;          /* 0=Mo … 6=So                         */
    int   legio_hour;
    float legio_temp;
    time_t holiday_until;
    /* Laufzeit */
    float temp;
    bool  charging;
    bool  legio_active;
} dm_dhw_t;

/* ── Solar ───────────────────────────────────────────────────────── */
typedef struct {
    bool  enabled;
    char  name[DM_NAME_LEN];
    float dt_on, dt_off;          /* Differenzregelung                   */
    float max_store_temp;
    float collector_max;          /* Kollektor-Schutzabschaltung         */
    float flow_lpm;               /* für Ertragsschätzung                */
    /* Laufzeit */
    float t_collector, t_store;
    bool  pump_on;
    float yield_day_kwh, yield_month_kwh, yield_year_kwh, yield_total_kwh;
    int   yield_day, yield_month, yield_year;   /* Kalenderreferenzen    */
} dm_solar_t;

/* ── Benutzer ────────────────────────────────────────────────────── */
typedef enum { ROLE_ADMIN = 0, ROLE_SERVICE, ROLE_USER, ROLE_GUEST } user_role_t;

typedef struct {
    char        name[DM_NAME_LEN];
    char        pw_hash[65];      /* SHA-256 hex                          */
    char        salt[17];
    user_role_t role;
    bool        enabled;
} dm_user_t;

/* ── Netzwerk / Dienste ──────────────────────────────────────────── */
typedef struct {
    char hostname[DM_NAME_LEN];
    char wifi_ssid[33], wifi_pass[65];
    bool eth_enabled, wifi_enabled;
    bool dhcp;
    char ip[16], gw[16], mask[16], dns[16];
    char ntp_server[64];
    char timezone[48];            /* z. B. "CET-1CEST,M3.5.0,M10.5.0/3"  */
    /* MQTT */
    bool mqtt_enabled, mqtt_discovery;
    char mqtt_uri[96], mqtt_user[33], mqtt_pass[65], mqtt_prefix[33];
    /* Modbus */
    bool mb_rtu_enabled;  int mb_rtu_baud; int mb_rtu_mode;   /* 0=Master 1=Slave */
    bool mb_tcp_enabled;  int mb_tcp_mode; int mb_tcp_port;
    int  mb_slave_id;
    /* Benachrichtigung */
    bool  email_enabled;   char email_smtp[64]; int email_port;
    char  email_user[64], email_pass[64], email_to[64];
    bool  telegram_enabled; char telegram_token[64], telegram_chat[24];
} dm_net_t;

/* ── Gesamtsystem ───────────────────────────────────────────────── */
typedef struct {
    int         board_model;     /* 6 oder 8                             */
    char        plant_name[48];
    char        language[3];     /* "de" / "en"                          */
    dm_net_t    net;
    dm_sensor_t sensor[DM_MAX_SENSORS];
    dm_relay_t  relay[8];
    dm_boiler_t boiler;
    dm_buffer_t buffer;
    dm_dhw_t    dhw;
    dm_solar_t  solar;
    dm_hc_t     hc[DM_MAX_HC];
    dm_timeprog_t tp[DM_MAX_TIMEPROG];
    dm_user_t   user[DM_MAX_USERS];
    float       outdoor_temp;    /* Laufzeit: gemittelte Außentemp.      */
    uint32_t    cfg_revision;    /* wird bei jedem Speichern erhöht      */
} dm_t;

void  dm_init(void);
dm_t *dm_lock(void);             /* Mutex nehmen, Zeiger liefern         */
void  dm_unlock(void);
void  dm_factory_defaults(dm_t *d);

/* Hilfen (nehmen selbst den Lock) */
float dm_sensor_by_role(sensor_role_t role, int idx);   /* NAN wenn fehlt */
int   dm_relay_by_fn(actor_fn_t fn, int idx);           /* -1 wenn fehlt  */
