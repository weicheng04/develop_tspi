#include "dev_lcd.h"
#include "dev_pca9557.h"
#include "esp_lvgl_port.h"

#define TAG "LCD"

esp_lcd_panel_io_handle_t lcd_io_handle = NULL;
esp_lcd_panel_handle_t lcd_panel_handle = NULL;
esp_lcd_touch_handle_t lcd_touch_handle = NULL;

lv_disp_t *disp = NULL;
lv_indev_t *indev_touch = NULL;

/**
 * @brief Initialize the LCD backlight using the LEDC peripheral.
 * 
 */
static void dev_lcd_backlight_init(void)
{
    // Initialize the LEDC peripheral for backlight control
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LCD_BACKLIGHT_GPIO, // Backlight control pin
        .duty = 0, // Start with backlight off
        .hpoint = 0,
        .flags.output_invert = true, // output inversion
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

/**
 * @brief Set the brightness of the LCD backlight.
 * 
 * @param brightness_percent Brightness level as a percentage (0-100).
 */
void dev_lcd_backlight_set_brightness(uint8_t brightness_percent)
{
    if (brightness_percent > 100) {
        brightness_percent = 100; // Cap the brightness at 100%
    }
    else if (brightness_percent < 0) {
        brightness_percent = 0; // Minimum brightness is 0%
    }
    // Set the duty cycle to control the backlight brightness
    // The duty cycle range is [0, (2^duty_resolution)-1], which is [0, 1023] for 10-bit resolution
    uint32_t duty = (brightness_percent * 1023) / 100; // Convert percentage to duty cycle
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
}

/**
 * @brief Initialize the LCD panel and its SPI interface.
 * 
 * @return esp_err_t 
 */
static esp_err_t dev_lcd_init(void)
{
    dev_lcd_backlight_init();
    ESP_LOGD(TAG, "LCD initialized");
    spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_SPI_SCLK_GPIO,
        .mosi_io_num = LCD_SPI_MOSI_GPIO,
        .miso_io_num = LCD_SPI_MISO_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t), // Assuming 16 bits per pixel and 40 lines per transfer
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO), TAG, "Failed to initialize SPI bus for LCD");
    
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_SPI_CS_GPIO,
        .dc_gpio_num = LCD_DC_GPIO,
        .spi_mode = 2,
        .pclk_hz = 80 * 1000 * 1000, // 80 MHz
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &lcd_io_handle), TAG, "Failed to create LCD panel IO");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(lcd_io_handle, &panel_config, &lcd_panel_handle), TAG, "Failed to create LCD panel device");

    esp_lcd_panel_reset(lcd_panel_handle);
    dev_pca9557_set_output_status(LCD_SPI_CS_GPIO, 0);
    esp_lcd_panel_init(lcd_panel_handle);
    esp_lcd_panel_invert_color(lcd_panel_handle, true);
    esp_lcd_panel_swap_xy(lcd_panel_handle, true);
    esp_lcd_panel_mirror(lcd_panel_handle, true, false);
    dev_lcd_set_color(0xFFFF); // Fill the screen with white color
    esp_lcd_panel_disp_on_off(lcd_panel_handle, true);
    dev_lcd_backlight_set_brightness(100); // Set backlight to maximum brightness

    return ESP_OK;
}

/**
 * @brief Fill the entire LCD screen with a specific color.
 * 
 * @param color The color to fill the screen with, in RGB565 format.
 */
void dev_lcd_set_color(uint16_t color)
{
    uint16_t *color_buffer = (uint16_t *)heap_caps_malloc(LCD_H_RES * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);

    if (NULL == color_buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory for color buffer");
        return;
    }
    else
    {
        for (size_t i = 0; i < LCD_H_RES; i++) {
            color_buffer[i] = color;
        }

        for (size_t line = 0; line < LCD_V_RES; line++) {
            esp_lcd_panel_draw_bitmap(lcd_panel_handle, 0, line, LCD_H_RES, line + 1, color_buffer);
        }
        free(color_buffer);
    }
}

/**
 * @brief Initialize the touch panel for the LCD.
 * 
 * @param touch_handle Pointer to store the created touch panel handle.
 * @return esp_err_t 
 */
static esp_err_t dev_lcd_touch_init(esp_lcd_touch_handle_t *touch_handle)
{
    esp_lcd_touch_config_t touch_config = {
       .x_max = LCD_V_RES,
       .y_max = LCD_H_RES,
       .rst_gpio_num = GPIO_NUM_NC,
       .int_gpio_num = GPIO_NUM_NC,
       .levels = {
            .reset = 0,
            .interrupt = 0,
       },
       .flags = {
            .swap_xy = 1,
            .mirror_x = 1,
            .mirror_y = 0,
       },
    };
    esp_lcd_panel_io_handle_t touch_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)I2C_NUM_0, &touch_io_config, &touch_io_handle), TAG, "Failed to create touch panel IO");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(touch_io_handle, &touch_config, touch_handle), TAG, "Failed to create touch panel handle");
    return ESP_OK;
}

/**
 * @brief Add the LCD panel as a display to the LVGL library.
 * 
 * @return lv_disp_t* Pointer to the created LVGL display object.
 */
static lv_disp_t* dev_lcd_add_display_lvgl_port(void)
{
    dev_lcd_init();
    lvgl_port_display_cfg_t display_cfg = {
        .io_handle = lcd_io_handle,
        .panel_handle = lcd_panel_handle,
        .buffer_size = LCD_H_RES * 20,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .mirror_x = true,
            .mirror_y = false,
            .swap_xy = true,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        }
    };
    return lvgl_port_add_disp(&display_cfg);
}

/**
 * @brief Add the touch panel as an input device to the LVGL library.
 * 
 * @param disp Pointer to the LVGL display object to associate the touch input with.
 * @return lv_indev_t* Pointer to the created LVGL input device object.
 */
static lv_indev_t* dev_lcd_add_touch_lvgl_port(lv_disp_t *disp)
{
    ESP_ERROR_CHECK(dev_lcd_touch_init(&lcd_touch_handle));
    assert(lcd_touch_handle != NULL);

    lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = lcd_touch_handle,
    };

    return lvgl_port_add_touch(&touch_cfg);
}

/**
 * @brief Initialize the LCD panel and its touch input for use with the LVGL library.
 * 
 */
void dev_lcd_lvgl_init(void)
{
    lvgl_port_cfg_t lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&lvgl_port_cfg);

    disp = dev_lcd_add_display_lvgl_port();
    assert(disp != NULL);

    indev_touch = dev_lcd_add_touch_lvgl_port(disp);
    assert(indev_touch != NULL);
}