/* pcf8574.c – minimaler, threadsicherer PCF8574-Treiber (I²C) */
#include "pcf8574.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define I2C_PORT   I2C_NUM_0
#define I2C_TO_MS  50

static SemaphoreHandle_t s_lock;

esp_err_t pcf8574_bus_init(int sda, int scl)
{
    s_lock = xSemaphoreCreateMutex();
    i2c_config_t c = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    esp_err_t err = i2c_param_config(I2C_PORT, &c);
    if (err != ESP_OK) return err;
    return i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}

esp_err_t pcf8574_write(uint8_t addr, uint8_t val)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_write_to_device(I2C_PORT, addr, &val, 1,
                                               pdMS_TO_TICKS(I2C_TO_MS));
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t pcf8574_read(uint8_t addr, uint8_t *val)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_read_from_device(I2C_PORT, addr, val, 1,
                                                pdMS_TO_TICKS(I2C_TO_MS));
    xSemaphoreGive(s_lock);
    return err;
}
