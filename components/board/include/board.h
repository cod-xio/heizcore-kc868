/**
 * board.h – Hardware-Abstraktion für KinCony KC868-A6 / KC868-A8
 *
 * Beide Boards nutzen PCF8574-Portexpander am I²C-Bus:
 *   - Eingänge  @ 0x22 (galvanisch getrennte Digitaleingänge)
 *   - Ausgänge  @ 0x24 (Relais, low-aktiv)
 *
 * Temperaturfühler:
 *   - DS18B20 an bis zu 2 OneWire-Bussen (GPIO konfigurierbar)
 *   - NTC 10k an den Analogeingängen (0–5 V, Spannungsteiler)
 *
 * Alle Pinbelegungen sind über die Weboberfläche änderbar und werden
 * in der Konfiguration persistiert (Werksdefaults siehe board.c).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define BOARD_MAX_RELAYS      8
#define BOARD_MAX_DIN         8
#define BOARD_MAX_AIN         4
#define BOARD_MAX_ONEWIRE     2
#define BOARD_MAX_DS18B20     32   /* Fühler gesamt über alle Busse */

typedef enum {
    BOARD_KC868_A6 = 6,
    BOARD_KC868_A8 = 8,
} board_model_t;

typedef struct {
    uint64_t rom;          /* DS18B20 ROM-Code                     */
    float    temp_c;       /* letzter Messwert                     */
    bool     valid;        /* CRC ok + Fühler antwortet            */
    uint8_t  bus;          /* OneWire-Busindex                     */
    int64_t  last_seen_us;
} ds18b20_t;

typedef struct {
    board_model_t model;
    /* I²C */
    int gpio_sda, gpio_scl;
    uint8_t addr_in, addr_out;
    /* OneWire-Busse */
    int gpio_onewire[BOARD_MAX_ONEWIRE];
    /* Analogeingänge (ADC1-Kanäle als GPIO) */
    int gpio_ain[BOARD_MAX_AIN];
    /* RS485 */
    int gpio_rs485_tx, gpio_rs485_rx;
    /* Ethernet (nur A8) */
    bool has_eth;
} board_pins_t;

esp_err_t board_init(board_model_t model);
const board_pins_t *board_pins(void);

/* Relais: idx 0-basiert, true = angezogen */
esp_err_t board_relay_set(int idx, bool on);
bool      board_relay_get(int idx);
int       board_relay_count(void);

/* Digitaleingänge: true = aktiv (Kontakt geschlossen) */
bool board_din_get(int idx);
int  board_din_count(void);

/* Analog: Rohspannung in mV bzw. NTC-Temperatur */
int   board_ain_mv(int idx);
float board_ain_ntc10k(int idx);

/* DS18B20 */
int               board_ds18b20_count(void);
const ds18b20_t  *board_ds18b20_get(int idx);
const ds18b20_t  *board_ds18b20_by_rom(uint64_t rom);
void              board_ds18b20_rescan(void);

/* Zyklische Messung – wird vom Regelungs-Task aufgerufen */
void board_poll(void);
