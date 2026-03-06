#ifndef __DEV_LCD_H__
#define __DEV_LCD_H__

#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#include "esp_lcd_touch_ft5x06.h"


#define LCD_BACKLIGHT_GPIO GPIO_NUM_42
#define LCD_SPI_HOST SPI3_HOST
#define LCD_SPI_SCLK_GPIO GPIO_NUM_41
#define LCD_SPI_MOSI_GPIO GPIO_NUM_40
#define LCD_SPI_MISO_GPIO GPIO_NUM_NC
#define LCD_SPI_CS_GPIO GPIO_NUM_NC
#define LCD_DC_GPIO GPIO_NUM_39
#define LCD_RESET_GPIO GPIO_NUM_NC

#define LCD_H_RES 320
#define LCD_V_RES 240


/**
 * @brief Fill the entire LCD screen with a specific color.
 * 
 * @param color The color to fill the screen with, in RGB565 format.
 */
void dev_lcd_set_color(uint16_t color);

/**
 * @brief Set the brightness of the LCD backlight.
 * 
 * @param brightness_percent Brightness level as a percentage (0-100).
 */
void dev_lcd_backlight_set_brightness(uint8_t brightness_percent);

/**
 * @brief Initialize the LCD panel and its touch input for use with the LVGL library.
 * 
 */
void dev_lcd_lvgl_init(void);

#endif