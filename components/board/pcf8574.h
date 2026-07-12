#pragma once
#include <stdint.h>
#include "esp_err.h"

esp_err_t pcf8574_bus_init(int sda, int scl);
esp_err_t pcf8574_write(uint8_t addr, uint8_t val);
esp_err_t pcf8574_read(uint8_t addr, uint8_t *val);
