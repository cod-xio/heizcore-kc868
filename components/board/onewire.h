#pragma once
#include <stdint.h>
#include <stdbool.h>

void    ow_init(int gpio);
bool    ow_reset(int gpio);
void    ow_write_byte(int gpio, uint8_t v);
uint8_t ow_read_byte(int gpio);
uint8_t ow_crc8(const uint8_t *d, int len);
int     ow_search(int gpio, uint64_t *roms, int max);

void ds18b20_start_convert_all(int gpio);
bool ds18b20_read_temp(int gpio, uint64_t rom, float *out);
