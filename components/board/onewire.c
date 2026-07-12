/* onewire.c – Bit-Bang-OneWire + DS18B20 (Suche, Konvertierung, CRC) */
#include "onewire.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void ow_pin_out_low(int g) { gpio_set_direction(g, GPIO_MODE_OUTPUT_OD); gpio_set_level(g, 0); }
static void ow_pin_release(int g) { gpio_set_direction(g, GPIO_MODE_INPUT); }

void ow_init(int gpio)
{
    gpio_reset_pin(gpio);
    gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
    ow_pin_release(gpio);
}

bool ow_reset(int g)
{
    portDISABLE_INTERRUPTS();
    ow_pin_out_low(g); ets_delay_us(480);
    ow_pin_release(g); ets_delay_us(70);
    bool presence = (gpio_get_level(g) == 0);
    portENABLE_INTERRUPTS();
    ets_delay_us(410);
    return presence;
}

static void ow_write_bit(int g, int bit)
{
    portDISABLE_INTERRUPTS();
    ow_pin_out_low(g);
    ets_delay_us(bit ? 6 : 60);
    ow_pin_release(g);
    ets_delay_us(bit ? 64 : 10);
    portENABLE_INTERRUPTS();
}

static int ow_read_bit(int g)
{
    portDISABLE_INTERRUPTS();
    ow_pin_out_low(g); ets_delay_us(6);
    ow_pin_release(g); ets_delay_us(9);
    int bit = gpio_get_level(g);
    portENABLE_INTERRUPTS();
    ets_delay_us(55);
    return bit;
}

void ow_write_byte(int g, uint8_t v)
{
    for (int i = 0; i < 8; i++) { ow_write_bit(g, v & 1); v >>= 1; }
}

uint8_t ow_read_byte(int g)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) v |= (ow_read_bit(g) << i);
    return v;
}

uint8_t ow_crc8(const uint8_t *d, int len)
{
    uint8_t crc = 0;
    while (len--) {
        uint8_t b = *d++;
        for (int i = 0; i < 8; i++) {
            uint8_t mix = (crc ^ b) & 1;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            b >>= 1;
        }
    }
    return crc;
}

/* --- ROM-Suche (Maxim App-Note 187) ---------------------------------- */
int ow_search(int g, uint64_t *roms, int max)
{
    int found = 0;
    int last_discrepancy = 0;
    bool last_device = false;
    uint8_t rom[8] = {0};

    while (!last_device && found < max) {
        if (!ow_reset(g)) break;
        ow_write_byte(g, 0xF0); /* SEARCH ROM */

        int discrepancy = 0;
        for (int bitpos = 1; bitpos <= 64; bitpos++) {
            int idx = (bitpos - 1) / 8, mask = 1 << ((bitpos - 1) % 8);
            int b  = ow_read_bit(g);
            int nb = ow_read_bit(g);
            int dir;
            if (b && nb) { found = found; goto done; }     /* kein Gerät */
            if (b != nb) dir = b;
            else {
                if (bitpos < last_discrepancy)      dir = (rom[idx] & mask) ? 1 : 0;
                else if (bitpos == last_discrepancy) dir = 1;
                else                                 dir = 0;
                if (dir == 0) discrepancy = bitpos;
            }
            if (dir) rom[idx] |= mask; else rom[idx] &= ~mask;
            ow_write_bit(g, dir);
        }
        last_discrepancy = discrepancy;
        if (last_discrepancy == 0) last_device = true;
        if (ow_crc8(rom, 7) == rom[7]) {
            uint64_t r = 0;
            for (int i = 7; i >= 0; i--) r = (r << 8) | rom[i];
            roms[found++] = r;
        }
    }
done:
    return found;
}

/* --- DS18B20 ---------------------------------------------------------- */
static void ow_select(int g, uint64_t rom)
{
    ow_write_byte(g, 0x55); /* MATCH ROM */
    for (int i = 0; i < 8; i++) ow_write_byte(g, (rom >> (8 * i)) & 0xFF);
}

void ds18b20_start_convert_all(int g)
{
    if (!ow_reset(g)) return;
    ow_write_byte(g, 0xCC); /* SKIP ROM */
    ow_write_byte(g, 0x44); /* CONVERT T (max. 750 ms, extern abwarten) */
}

bool ds18b20_read_temp(int g, uint64_t rom, float *out)
{
    if (!ow_reset(g)) return false;
    ow_select(g, rom);
    ow_write_byte(g, 0xBE); /* READ SCRATCHPAD */
    uint8_t sp[9];
    for (int i = 0; i < 9; i++) sp[i] = ow_read_byte(g);
    if (ow_crc8(sp, 8) != sp[8]) return false;
    int16_t raw = (sp[1] << 8) | sp[0];
    float t = raw / 16.0f;
    if (t < -55.0f || t > 125.0f) return false;
    *out = t;
    return true;
}
