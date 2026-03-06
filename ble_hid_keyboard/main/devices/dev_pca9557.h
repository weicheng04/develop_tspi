#ifndef __DEV_PCA9557_H__
#define __DEV_PCA9557_H__

#include "esp_err.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SET_BITS(_m, _s, _v)  ((_v) ? (_m)|((_s)) : (_m)&~((_s)))

#define PCA9557_DEV_ADDR 0x19

#define PCA9557_INPUT_PORT              0x00
#define PCA9557_OUTPUT_PORT             0x01
#define PCA9557_POLARITY_INVERSION_PORT 0x02
#define PCA9557_CONFIGURATION_PORT      0x03

/**
 * @brief Initialize the PCA9557 device.
 * 
 * @return esp_err_t 
 */
esp_err_t dev_pca9557_init(void);

/**
 * @brief Set the output status of a specific GPIO pin on the PCA9557 device.
 * 
 * @param gpio_bit The bit position of the GPIO pin (0-7).
 * @param level The desired output level (0 or 1).
 * @return esp_err_t 
 */
esp_err_t dev_pca9557_set_output_status(uint8_t gpio_bit, uint8_t level);

#endif
