/* board.c – KC868-A6/A8: Relais, Eingänge, NTC-ADC, DS18B20-Verwaltung */
#include <string.h>
#include <math.h>
#include "board.h"
#include "pcf8574.h"
#include "onewire.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

static board_pins_t s_pins;
static uint8_t  s_relay_shadow = 0xFF;      /* PCF8574 low-aktiv          */
static uint8_t  s_din_raw = 0xFF;
static ds18b20_t s_ds[BOARD_MAX_DS18B20];
static int       s_ds_count = 0;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok = false;

/* Werksdefaults – über Weboberfläche/Konfiguration überschreibbar.
 * Quelle: KinCony-Dokumentation. Vor Inbetriebnahme gegen die
 * Revision des eigenen Boards prüfen (siehe README). */
static void load_defaults(board_model_t m)
{
    memset(&s_pins, 0, sizeof(s_pins));
    s_pins.model     = m;
    s_pins.gpio_sda  = 4;
    s_pins.gpio_scl  = 5;
    s_pins.addr_in   = 0x22;
    s_pins.addr_out  = 0x24;
    s_pins.gpio_onewire[0] = 32;
    s_pins.gpio_onewire[1] = 33;
    s_pins.gpio_ain[0] = 36;
    s_pins.gpio_ain[1] = 39;
    s_pins.gpio_ain[2] = 34;
    s_pins.gpio_ain[3] = 35;
    s_pins.gpio_rs485_tx = 27;
    s_pins.gpio_rs485_rx = 14;
    s_pins.has_eth = (m == BOARD_KC868_A8);
}

const board_pins_t *board_pins(void) { return &s_pins; }
int board_relay_count(void) { return (int)s_pins.model; }
int board_din_count(void)   { return (int)s_pins.model; }

esp_err_t board_init(board_model_t model)
{
    load_defaults(model);

    ESP_ERROR_CHECK(pcf8574_bus_init(s_pins.gpio_sda, s_pins.gpio_scl));
    pcf8574_write(s_pins.addr_out, 0xFF);   /* alle Relais aus (low-aktiv) */
    pcf8574_write(s_pins.addr_in, 0xFF);    /* Eingänge als Eingang        */

    for (int b = 0; b < BOARD_MAX_ONEWIRE; b++)
        if (s_pins.gpio_onewire[b] >= 0) ow_init(s_pins.gpio_onewire[b]);
    board_ds18b20_rescan();

    /* ADC1 für NTC-Eingänge */
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc));
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    for (int i = 0; i < BOARD_MAX_AIN; i++) {
        adc_channel_t ch; adc_unit_t un;
        if (adc_oneshot_io_to_channel(s_pins.gpio_ain[i], &un, &ch) == ESP_OK)
            adc_oneshot_config_channel(s_adc, ch, &ccfg);
    }
    adc_cali_line_fitting_config_t lcfg = {
        .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    s_cali_ok = (adc_cali_create_scheme_line_fitting(&lcfg, &s_cali) == ESP_OK);

    ESP_LOGI(TAG, "KC868-A%d initialisiert: %d Relais, %d Eingänge, %d DS18B20",
             (int)model, board_relay_count(), board_din_count(), s_ds_count);
    return ESP_OK;
}

/* --- Relais ------------------------------------------------------------ */
esp_err_t board_relay_set(int idx, bool on)
{
    if (idx < 0 || idx >= board_relay_count()) return ESP_ERR_INVALID_ARG;
    uint8_t v = s_relay_shadow;
    if (on) v &= ~(1 << idx); else v |= (1 << idx);   /* low-aktiv */
    if (v == s_relay_shadow) return ESP_OK;
    esp_err_t err = pcf8574_write(s_pins.addr_out, v);
    if (err == ESP_OK) s_relay_shadow = v;
    return err;
}

bool board_relay_get(int idx)
{
    if (idx < 0 || idx >= board_relay_count()) return false;
    return !(s_relay_shadow & (1 << idx));
}

/* --- Digitaleingänge ---------------------------------------------------- */
bool board_din_get(int idx)
{
    if (idx < 0 || idx >= board_din_count()) return false;
    return !(s_din_raw & (1 << idx));                 /* low-aktiv */
}

/* --- Analog / NTC -------------------------------------------------------- */
int board_ain_mv(int idx)
{
    if (idx < 0 || idx >= BOARD_MAX_AIN) return -1;
    adc_channel_t ch; adc_unit_t un;
    if (adc_oneshot_io_to_channel(s_pins.gpio_ain[idx], &un, &ch) != ESP_OK) return -1;
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, ch, &raw) != ESP_OK) return -1;
    if (s_cali_ok && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) return mv;
    return raw * 3300 / 4095;
}

float board_ain_ntc10k(int idx)
{
    /* Spannungsteiler 10k gegen 3,3 V; NTC 10k B=3950 (Beta-Gleichung) */
    int mv = board_ain_mv(idx);
    if (mv <= 5 || mv >= 3290) return NAN;
    float r = 10000.0f * mv / (3300.0f - mv);
    float t = 1.0f / (1.0f / 298.15f + logf(r / 10000.0f) / 3950.0f) - 273.15f;
    return t;
}

/* --- DS18B20 -------------------------------------------------------------- */
void board_ds18b20_rescan(void)
{
    s_ds_count = 0;
    for (int b = 0; b < BOARD_MAX_ONEWIRE; b++) {
        int g = s_pins.gpio_onewire[b];
        if (g < 0) continue;
        uint64_t roms[BOARD_MAX_DS18B20];
        int n = ow_search(g, roms, BOARD_MAX_DS18B20 - s_ds_count);
        for (int i = 0; i < n && s_ds_count < BOARD_MAX_DS18B20; i++) {
            ds18b20_t *d = &s_ds[s_ds_count++];
            d->rom = roms[i]; d->bus = b; d->valid = false; d->temp_c = NAN;
        }
    }
    ESP_LOGI(TAG, "OneWire-Scan: %d Fühler gefunden", s_ds_count);
}

int board_ds18b20_count(void) { return s_ds_count; }
const ds18b20_t *board_ds18b20_get(int i) { return (i >= 0 && i < s_ds_count) ? &s_ds[i] : NULL; }

const ds18b20_t *board_ds18b20_by_rom(uint64_t rom)
{
    for (int i = 0; i < s_ds_count; i++)
        if (s_ds[i].rom == rom) return &s_ds[i];
    return NULL;
}

/* --- zyklische Messung (vom Regelungs-Task, 1-s-Raster) ------------------- */
void board_poll(void)
{
    static int phase = 0;
    /* Eingänge jede Sekunde lesen */
    uint8_t v;
    if (pcf8574_read(s_pins.addr_in, &v) == ESP_OK) s_din_raw = v;

    /* DS18B20: Sekunde 0 = Konvertierung starten, Sekunde 1 = auslesen */
    if (phase == 0) {
        for (int b = 0; b < BOARD_MAX_ONEWIRE; b++)
            if (s_pins.gpio_onewire[b] >= 0)
                ds18b20_start_convert_all(s_pins.gpio_onewire[b]);
    } else {
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < s_ds_count; i++) {
            float t;
            if (ds18b20_read_temp(s_pins.gpio_onewire[s_ds[i].bus], s_ds[i].rom, &t)) {
                s_ds[i].temp_c = t; s_ds[i].valid = true; s_ds[i].last_seen_us = now;
            } else if (now - s_ds[i].last_seen_us > 30 * 1000000LL) {
                s_ds[i].valid = false;      /* 30 s keine Antwort → Fühlerfehler */
            }
        }
    }
    phase ^= 1;
}
