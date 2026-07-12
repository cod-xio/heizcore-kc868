/* alarms.c – Alarm-/Störungsverwaltung mit Historie auf LittleFS
 *
 * Stufen: INFO, WARNUNG, KRITISCH. Aktive Alarme werden dedupliziert
 * (gleicher Code wird nicht erneut gemeldet, solange er ansteht).
 * Historie: /fs/alarms.log (JSON-Zeilen, Ringkürzung bei 64 kB).
 * Benachrichtigung: Callback-Hooks für MQTT / E-Mail / Telegram.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include "alarms.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "alarm";
#define LOG_PATH   "/fs/alarms.log"
#define LOG_MAX    (64 * 1024)

static alarm_t s_active[ALARM_MAX_ACTIVE];
static int s_count = 0;
static SemaphoreHandle_t s_lock;
static alarm_notify_cb_t s_cb[4];
static int s_cb_count = 0;

void alarms_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

void alarms_register_notify(alarm_notify_cb_t cb)
{
    if (s_cb_count < 4) s_cb[s_cb_count++] = cb;
}

static void append_log(const alarm_t *a, bool cleared)
{
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > LOG_MAX) {
        /* Ringkürzung: hintere Hälfte behalten */
        FILE *f = fopen(LOG_PATH, "r");
        if (f) {
            fseek(f, -(LOG_MAX / 2), SEEK_END);
            char *buf = malloc(LOG_MAX / 2 + 1);
            if (buf) {
                size_t n = fread(buf, 1, LOG_MAX / 2, f);
                fclose(f);
                char *nl = memchr(buf, '\n', n);
                FILE *o = fopen(LOG_PATH, "w");
                if (o && nl) { fwrite(nl + 1, 1, n - (nl - buf) - 1, o); }
                if (o) fclose(o);
                free(buf);
            } else fclose(f);
        }
    }
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    fprintf(f, "{\"t\":%lld,\"lvl\":%d,\"code\":%d,\"msg\":\"%s\",\"clr\":%d}\n",
            (long long)a->timestamp, a->level, a->code, a->message, cleared ? 1 : 0);
    fclose(f);
}

void alarm_raise(alarm_level_t lvl, int code, const char *msg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (s_active[i].code == code) { xSemaphoreGive(s_lock); return; }
    }
    if (s_count < ALARM_MAX_ACTIVE) {
        alarm_t *a = &s_active[s_count++];
        a->level = lvl; a->code = code;
        a->timestamp = time(NULL);
        strncpy(a->message, msg, sizeof(a->message) - 1);
        a->message[sizeof(a->message) - 1] = 0;
        append_log(a, false);
        ESP_LOGW(TAG, "[%s] %d: %s",
                 lvl == ALARM_CRITICAL ? "KRITISCH" : lvl == ALARM_WARNING ? "WARNUNG" : "INFO",
                 code, msg);
        for (int i = 0; i < s_cb_count; i++) s_cb[i](a);
    }
    xSemaphoreGive(s_lock);
}

void alarm_clear(int code)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (s_active[i].code == code) {
            append_log(&s_active[i], true);
            memmove(&s_active[i], &s_active[i + 1], (s_count - i - 1) * sizeof(alarm_t));
            s_count--;
            break;
        }
    }
    xSemaphoreGive(s_lock);
}

int alarms_get_active(alarm_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_count < max ? s_count : max;
    memcpy(out, s_active, n * sizeof(alarm_t));
    xSemaphoreGive(s_lock);
    return n;
}

bool alarms_has_critical(void)
{
    bool r = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++)
        if (s_active[i].level == ALARM_CRITICAL) { r = true; break; }
    xSemaphoreGive(s_lock);
    return r;
}
