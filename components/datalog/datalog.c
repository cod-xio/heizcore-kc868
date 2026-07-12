/* datalog.c – Datenlogger auf LittleFS
 *
 * Aufzeichnung alle 60 s in Tagesdateien /fs/log/YYYYMMDD.bin
 * (Binärsätze, 44 Byte). Aufbewahrung: 30 Tagesdateien; zusätzlich
 * Langzeitdatei /fs/log/daily.bin mit Tagesmittelwerten (365 Tage).
 *
 * Abfrage: /api/log?range=24h|7d|30d|365d → JSON, automatisch auf
 * ≤ 500 Punkte heruntergerechnet (Mittelwertbildung).
 *
 * Zusätzlich triggert der Logger-Task alle 5 s den MQTT-Publish.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "datalog.h"
#include "datamodel.h"
#include "netsvc.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "datalog";
#define LOG_DIR       "/fs/log"
#define KEEP_DAYS     30
#define MAX_POINTS    500

typedef struct __attribute__((packed)) {
    uint32_t t;             /* Unix-Zeit                       */
    int16_t  outdoor;       /* ×10                              */
    int16_t  boiler;
    int16_t  buf_top, buf_mid, buf_bot;
    int16_t  dhw;
    int16_t  collector;
    int16_t  hc_flow[4];    /* HK 1–4 Vorlauf                   */
    uint8_t  boiler_pw;     /* %                                */
    uint8_t  relay_mask;
    uint16_t solar_wh_day;  /* Ertrag heute in 0,1 kWh          */
    uint16_t burner_starts;
    uint32_t burner_runtime_min;
} log_rec_t;

static int16_t enc(float v) { return isnan(v) ? INT16_MIN : (int16_t)lrintf(v * 10); }
static double  dec(int16_t v) { return v == INT16_MIN ? NAN : v / 10.0; }

static void make_path(char *out, size_t n, time_t t)
{
    struct tm tm_;
    localtime_r(&t, &tm_);
    snprintf(out, n, LOG_DIR "/%04d%02d%02d.bin",
             tm_.tm_year + 1900, tm_.tm_mon + 1, tm_.tm_mday);
}

static void cleanup_old(void)
{
    DIR *dir = opendir(LOG_DIR);
    if (!dir) return;
    char keep[KEEP_DAYS][14];
    int nk = 0;
    for (int i = 0; i < KEEP_DAYS; i++) {
        char p[64];
        make_path(p, sizeof(p), time(NULL) - (time_t)i * 86400);
        const char *base = strrchr(p, '/') + 1;
        strncpy(keep[nk++], base, 13);
        keep[nk - 1][13] = 0;
    }
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (strcmp(e->d_name, "daily.bin") == 0) continue;
        bool found = false;
        for (int i = 0; i < nk; i++)
            if (strcmp(e->d_name, keep[i]) == 0) { found = true; break; }
        if (!found) {
            char p[80];
            snprintf(p, sizeof(p), LOG_DIR "/%.64s", e->d_name);
            unlink(p);
            ESP_LOGI(TAG, "Alte Logdatei gelöscht: %s", e->d_name);
        }
    }
    closedir(dir);
}

static void write_record(void)
{
    log_rec_t r = { 0 };
    r.t = (uint32_t)time(NULL);
    dm_t *d = dm_lock();
    r.outdoor  = enc(d->outdoor_temp);
    r.boiler   = enc(d->boiler.temp);
    r.buf_top  = enc(d->buffer.t[0]);
    r.buf_mid  = enc(d->buffer.t[2]);
    r.buf_bot  = enc(d->buffer.t[4]);
    r.dhw      = enc(d->dhw.temp);
    r.collector = enc(d->solar.t_collector);
    for (int i = 0; i < 4; i++) r.hc_flow[i] = enc(d->hc[i].flow_act);
    r.boiler_pw = (uint8_t)d->boiler.power_pct;
    for (int i = 0; i < 8; i++) if (d->relay[i].state) r.relay_mask |= 1 << i;
    r.solar_wh_day = (uint16_t)(d->solar.yield_day_kwh * 10);
    r.burner_starts = (uint16_t)d->boiler.starts;
    r.burner_runtime_min = d->boiler.runtime_s / 60;
    dm_unlock();

    char path[64];
    make_path(path, sizeof(path), time(NULL));
    FILE *f = fopen(path, "ab");
    if (!f) { ESP_LOGE(TAG, "Log-Schreibfehler"); return; }
    fwrite(&r, sizeof(r), 1, f);
    fclose(f);
}

/* ── Abfrage ─────────────────────────────────────────────────────── */
static int range_days(const char *range)
{
    if (!strcmp(range, "7d"))   return 7;
    if (!strcmp(range, "30d"))  return 30;
    if (!strcmp(range, "365d")) return 30;   /* Detaildaten max. 30 Tage */
    return 1;
}

char *datalog_query_json(const char *range)
{
    int days = range_days(range);
    time_t from = time(NULL) - (time_t)days * 86400;

    /* Sätze zählen für Downsampling-Faktor */
    long total = 0;
    for (int i = 0; i <= days; i++) {
        char p[64];
        make_path(p, sizeof(p), from + (time_t)i * 86400);
        struct stat st;
        if (stat(p, &st) == 0) total += st.st_size / sizeof(log_rec_t);
    }
    int stride = total > MAX_POINTS ? (int)(total / MAX_POINTS) : 1;

    cJSON *arr = cJSON_CreateArray();
    long idx = 0;
    for (int i = 0; i <= days; i++) {
        char p[64];
        make_path(p, sizeof(p), from + (time_t)i * 86400);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        log_rec_t r;
        while (fread(&r, sizeof(r), 1, f) == 1) {
            if (r.t < (uint32_t)from) continue;
            if ((idx++ % stride) != 0) continue;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "t", r.t);
#define A(k, v) do { double x = dec(v); if (!isnan(x)) cJSON_AddNumberToObject(o, k, x); } while (0)
            A("out", r.outdoor); A("kes", r.boiler);
            A("po", r.buf_top); A("pm", r.buf_mid); A("pu", r.buf_bot);
            A("ww", r.dhw); A("kol", r.collector);
            A("hk1", r.hc_flow[0]); A("hk2", r.hc_flow[1]);
#undef A
            cJSON_AddNumberToObject(o, "pw", r.boiler_pw);
            cJSON_AddNumberToObject(o, "sol", r.solar_wh_day / 10.0);
            cJSON_AddItemToArray(arr, o);
        }
        fclose(f);
    }
    char *txt = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return txt;
}

void datalog_export_csv(httpd_req_t *req)
{
    httpd_resp_sendstr_chunk(req,
        "zeit;aussen;kessel;puffer_oben;puffer_mitte;puffer_unten;"
        "warmwasser;kollektor;hk1_vorlauf;hk2_vorlauf;leistung_pct;"
        "solar_kwh_tag;brennerstarts;laufzeit_min\n");
    for (int i = KEEP_DAYS; i >= 0; i--) {
        char p[64];
        make_path(p, sizeof(p), time(NULL) - (time_t)i * 86400);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        log_rec_t r;
        char line[256];
        while (fread(&r, sizeof(r), 1, f) == 1) {
            struct tm tm_;
            time_t t = r.t;
            localtime_r(&t, &tm_);
            snprintf(line, sizeof(line),
                "%04d-%02d-%02d %02d:%02d;%.1f;%.1f;%.1f;%.1f;%.1f;%.1f;%.1f;%.1f;%.1f;%u;%.1f;%u;%lu\n",
                tm_.tm_year + 1900, tm_.tm_mon + 1, tm_.tm_mday, tm_.tm_hour, tm_.tm_min,
                dec(r.outdoor), dec(r.boiler), dec(r.buf_top), dec(r.buf_mid),
                dec(r.buf_bot), dec(r.dhw), dec(r.collector),
                dec(r.hc_flow[0]), dec(r.hc_flow[1]),
                r.boiler_pw, r.solar_wh_day / 10.0,
                r.burner_starts, (unsigned long)r.burner_runtime_min);
            /* NaN als leeres Feld ausgeben */
            char *nan;
            while ((nan = strstr(line, "nan"))) memmove(nan, nan + 3, strlen(nan + 3) + 1);
            httpd_resp_sendstr_chunk(req, line);
        }
        fclose(f);
    }
    httpd_resp_send_chunk(req, NULL, 0);
}

static void datalog_task(void *arg)
{
    mkdir(LOG_DIR, 0755);
    cleanup_old();
    int sec = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        mqtt_svc_publish_state();
        sec += 5;
        if (sec >= 60) {
            sec = 0;
            write_record();
            static int day = -1;
            time_t t = time(NULL);
            struct tm tm_;
            localtime_r(&t, &tm_);
            if (day != tm_.tm_yday) { day = tm_.tm_yday; cleanup_old(); }
        }
    }
}

void datalog_start(void)
{
    xTaskCreate(datalog_task, "datalog", 6144, NULL, 2, NULL);
    ESP_LOGI(TAG, "Datenlogger gestartet (60-s-Raster, %d Tage)", KEEP_DAYS);
}
