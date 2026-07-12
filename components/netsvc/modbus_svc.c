/* modbus_svc.c – Modbus RTU (RS485) und Modbus TCP, Master oder Slave
 *
 * API: espressif/esp-modbus >= 2.0 (Kontext-Handle-API: mbc_*_create_serial/tcp,
 * mbc_*_start(ctx), mbc_slave_set_descriptor(ctx, area), mbc_master_send_request(ctx, req, data))
 * Slave-Registerkarte: siehe README.md
 */
#include <string.h>
#include <math.h>
#include "netsvc.h"
#include "datamodel.h"
#include "config_store.h"
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

/* esp-modbus public header (espressif__esp-modbus) */
#include "esp_modbus_common.h"
#include "esp_modbus_slave.h"
#include "esp_modbus_master.h"

static const char *TAG = "modbus";

/* ── Slave-Register ──────────────────────────────────────────────── */
static uint16_t s_input_reg[128];
static uint16_t s_holding_reg[160];
static uint8_t  s_coils[2];
static void    *s_slave_handle;
static bool     s_slave_running;
static void    *s_master_handle;
static bool     s_master_running;

static uint16_t t10(float v)
{
    return isnan(v) ? 0x8000u : (uint16_t)(int16_t)lrintf(v * 10.0f);
}
static double dec(uint16_t v)
{
    return v == 0x8000u ? NAN : (double)(int16_t)v / 10.0;
}

/* ── Slave → Register spiegeln (1×/s) ───────────────────────────── */
static void slave_update_regs(void)
{
    dm_t *d = dm_lock();

    s_input_reg[0]  = t10(d->outdoor_temp);
    s_input_reg[1]  = t10(d->boiler.temp);
    s_input_reg[2]  = t10(d->boiler.return_temp);
    s_input_reg[3]  = (uint16_t)d->boiler.power_pct;
    s_input_reg[4]  = (uint16_t)d->boiler.state;
    for (int i = 0; i < 5; i++)
        s_input_reg[10 + i] = t10(d->buffer.t[i]);
    s_input_reg[15] = (uint16_t)d->buffer.charge_pct;
    s_input_reg[20] = t10(d->dhw.temp);
    s_input_reg[21] = d->dhw.charging ? 1u : 0u;
    s_input_reg[30] = t10(d->solar.t_collector);
    s_input_reg[31] = t10(d->solar.t_store);
    for (int i = 0; i < DM_MAX_HC && 40 + 4 * i + 3 < 128; i++) {
        s_input_reg[40 + 4 * i]     = t10(d->hc[i].flow_act);
        s_input_reg[40 + 4 * i + 1] = t10(d->hc[i].flow_set);
        s_input_reg[40 + 4 * i + 2] = t10(d->hc[i].room_act);
        s_input_reg[40 + 4 * i + 3] = (uint16_t)d->hc[i].mixer_pos_pct;
    }
    s_coils[0] = 0;
    for (int i = 0; i < 8; i++)
        if (d->relay[i].state) s_coils[0] |= (uint8_t)(1u << i);

    /* Holding-Register: Schreibzugriffe der Gegenstelle übernehmen */
    float bs = (float)(int16_t)s_holding_reg[100] / 10.0f;
    if (bs >= 40.0f && bs <= 90.0f && fabsf(bs - d->boiler.setpoint) > 0.05f)
        d->boiler.setpoint = bs;
    else
        s_holding_reg[100] = t10(d->boiler.setpoint);

    float ws = (float)(int16_t)s_holding_reg[101] / 10.0f;
    if (ws >= 30.0f && ws <= 70.0f && fabsf(ws - d->dhw.setpoint) > 0.05f)
        d->dhw.setpoint = ws;
    else
        s_holding_reg[101] = t10(d->dhw.setpoint);

    for (int i = 0; i < DM_MAX_HC && 110 + 2 * i + 1 < 160; i++) {
        uint16_t m = s_holding_reg[110 + 2 * i];
        if (m <= HCM_OFF && m != (uint16_t)d->hc[i].mode)
            d->hc[i].mode = (hc_mode_t)m;
        else
            s_holding_reg[110 + 2 * i] = (uint16_t)d->hc[i].mode;

        float rs = (float)(int16_t)s_holding_reg[110 + 2 * i + 1] / 10.0f;
        if (rs >= 5.0f && rs <= 30.0f && fabsf(rs - d->hc[i].room_set_day) > 0.05f)
            d->hc[i].room_set_day = rs;
        else
            s_holding_reg[110 + 2 * i + 1] = t10(d->hc[i].room_set_day);
    }
    dm_unlock();
}

/* ── Slave starten ───────────────────────────────────────────────── */
static void slave_start(const dm_t *cfg, bool tcp)
{
    esp_err_t err;

    if (tcp) {
        mb_communication_info_t comm = {
            .tcp_opts.port         = (uint16_t)cfg->net.mb_tcp_port,
            .tcp_opts.mode         = MB_TCP,
            .tcp_opts.addr_type    = MB_IPV4,
            .tcp_opts.ip_addr_table = NULL,   /* an beliebige Adresse binden */
            .tcp_opts.ip_netif_ptr = NULL,    /* gesetzt nach netif-Start */
            .tcp_opts.uid          = (uint8_t)cfg->net.mb_slave_id,
        };
        err = mbc_slave_create_tcp(&comm, &s_slave_handle);
    } else {
        mb_communication_info_t comm = {
            .ser_opts.port      = UART_NUM_2,
            .ser_opts.mode      = MB_RTU,
            .ser_opts.baudrate  = (uint32_t)cfg->net.mb_rtu_baud,
            .ser_opts.parity    = MB_PARITY_NONE,
            .ser_opts.uid       = (uint8_t)cfg->net.mb_slave_id,
            .ser_opts.data_bits = UART_DATA_8_BITS,
            .ser_opts.stop_bits = UART_STOP_BITS_1,
        };
        err = mbc_slave_create_serial(&comm, &s_slave_handle);
    }
    if (err != ESP_OK || s_slave_handle == NULL) {
        ESP_LOGE(TAG, "Slave-%s init: %s", tcp ? "TCP" : "RTU", esp_err_to_name(err));
        return;
    }

    mb_register_area_descriptor_t area;

    area.type         = MB_PARAM_INPUT;
    area.start_offset = 0;
    area.address      = (void *)s_input_reg;
    area.size         = sizeof(s_input_reg);
    area.access       = MB_ACCESS_RW;
    mbc_slave_set_descriptor(s_slave_handle, area);

    area.type    = MB_PARAM_HOLDING;
    area.address = (void *)s_holding_reg;
    area.size    = sizeof(s_holding_reg);
    mbc_slave_set_descriptor(s_slave_handle, area);

    area.type    = MB_PARAM_COIL;
    area.address = (void *)s_coils;
    area.size    = sizeof(s_coils);
    mbc_slave_set_descriptor(s_slave_handle, area);

    if (!tcp) {
        const board_pins_t *p = board_pins();
        uart_set_pin(UART_NUM_2,
                     p->gpio_rs485_tx, p->gpio_rs485_rx,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        uart_set_mode(UART_NUM_2, UART_MODE_RS485_HALF_DUPLEX);
    }

    err = mbc_slave_start(s_slave_handle);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Slave start: %s", esp_err_to_name(err)); return; }
    s_slave_running = true;
    ESP_LOGI(TAG, "Modbus-%s-Slave gestartet (ID %d)", tcp ? "TCP" : "RTU", cfg->net.mb_slave_id);
}

/* ── Master starten ──────────────────────────────────────────────── */
static void master_start(const dm_t *cfg, bool tcp)
{
    esp_err_t err;

    if (tcp) {
        mb_communication_info_t comm = {
            .tcp_opts.port          = (uint16_t)cfg->net.mb_tcp_port,
            .tcp_opts.mode          = MB_TCP,
            .tcp_opts.addr_type     = MB_IPV4,
            .tcp_opts.ip_addr_table = NULL,   /* je Fühler per "slave:register" adressiert */
            .tcp_opts.ip_netif_ptr  = NULL,
            .tcp_opts.uid           = 0,      /* ungenutzt beim Master */
        };
        err = mbc_master_create_tcp(&comm, &s_master_handle);
    } else {
        mb_communication_info_t comm = {
            .ser_opts.port      = UART_NUM_2,
            .ser_opts.mode      = MB_RTU,
            .ser_opts.baudrate  = (uint32_t)cfg->net.mb_rtu_baud,
            .ser_opts.parity    = MB_PARITY_NONE,
            .ser_opts.uid       = 0,
            .ser_opts.data_bits = UART_DATA_8_BITS,
            .ser_opts.stop_bits = UART_STOP_BITS_1,
        };
        err = mbc_master_create_serial(&comm, &s_master_handle);
    }
    if (err != ESP_OK || s_master_handle == NULL) {
        ESP_LOGE(TAG, "Master-%s init: %s", tcp ? "TCP" : "RTU", esp_err_to_name(err));
        return;
    }

    if (!tcp) {
        const board_pins_t *p = board_pins();
        uart_set_pin(UART_NUM_2,
                     p->gpio_rs485_tx, p->gpio_rs485_rx,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        uart_set_mode(UART_NUM_2, UART_MODE_RS485_HALF_DUPLEX);
    }

    err = mbc_master_start(s_master_handle);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Master start: %s", esp_err_to_name(err)); return; }
    s_master_running = true;
    ESP_LOGI(TAG, "Modbus-%s-Master gestartet", tcp ? "TCP" : "RTU");
}

/* ── Master: SRC_MODBUS-Fühler abfragen ─────────────────────────── */
static void master_poll_sensors(void)
{
    /* Jobs ohne Lock sammeln */
    struct { int idx; uint8_t sid; uint16_t reg; } jobs[DM_MAX_SENSORS];
    int nj = 0;
    dm_t *d = dm_lock();
    for (int i = 0; i < DM_MAX_SENSORS && nj < DM_MAX_SENSORS; i++) {
        dm_sensor_t *s = &d->sensor[i];
        if (s->enabled && s->src == SRC_MODBUS) {
            jobs[nj].idx = i;
            jobs[nj].sid = (uint8_t)((s->ref >> 16) & 0xFF);
            jobs[nj].reg = (uint16_t)(s->ref & 0xFFFF);
            nj++;
        }
    }
    dm_unlock();

    for (int j = 0; j < nj; j++) {
        uint16_t val = 0;
        mb_param_request_t req = {
            .slave_addr = jobs[j].sid,
            .command    = 0x04,          /* Read Input Registers */
            .reg_start  = jobs[j].reg,
            .reg_size   = 1,
        };
        esp_err_t err = mbc_master_send_request(s_master_handle, &req, &val);
        d = dm_lock();
        dm_sensor_t *s = &d->sensor[jobs[j].idx];
        if (err == ESP_OK && val != 0x8000u) {
            s->value = (float)(int16_t)val / 10.0f + s->offset;
            s->valid = true;
        } else {
            s->valid = false;
        }
        dm_unlock();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ── Haupt-Task ──────────────────────────────────────────────────── */
static void modbus_task(void *arg)
{
    for (;;) {
        if (s_slave_running)  slave_update_regs();
        if (s_master_running) master_poll_sensors();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void modbus_svc_start(void)
{
    const dm_t *cfg = cfg_get();

    if (cfg->net.mb_rtu_enabled) {
        if (cfg->net.mb_rtu_mode == 1) slave_start(cfg, false);
        else                           master_start(cfg, false);
    }
    if (cfg->net.mb_tcp_enabled && !s_slave_running && !s_master_running) {
        if (cfg->net.mb_tcp_mode == 1) slave_start(cfg, true);
        else                           master_start(cfg, true);
    }
    if (s_slave_running || s_master_running)
        xTaskCreate(modbus_task, "modbus", 4096, NULL, 4, NULL);
}

void modbus_svc_reconfigure(void)
{
    /* Modbus-Stack sauber neu starten erfordert Reboot – nach Konfig-
     * Änderung über die Weboberfläche angeboten. */
}
