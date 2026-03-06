#include "dev_pca9557.h"

/**
 * @brief Initialize the I2C master bus and add the PCA9557 device.
 * 
 */
static void bsp_i2c_master_init(void)
{
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GPIO_NUM_1,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = GPIO_NUM_2,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100 * 1000, // 100kHz
    };
    i2c_param_config(I2C_NUM_0, &i2c_conf);
    i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
}

/**
 * @brief Write a byte to a specific register of the PCA9557 device.
 * 
 * @param reg_addr The register address to write to.
 * @param data The byte of data to write.`
 * @return esp_err_t 
 */
esp_err_t dev_pca9557_write_byte(uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_write_to_device(I2C_NUM_0, PCA9557_DEV_ADDR, write_buf, sizeof(write_buf), pdMS_TO_TICKS(1000));
}

/**
 * @brief Read data from a specific register of the PCA9557 device.
 * 
 * @param reg_addr The register address to read from.
 * @param data Buffer to store the read data.
 * @param data_len Length of the data to read.
 * @return esp_err_t 
 */
esp_err_t dev_pca9557_read_register(uint8_t reg_addr, uint8_t *data, size_t data_len)
{
    return i2c_master_write_read_device(I2C_NUM_0, PCA9557_DEV_ADDR, &reg_addr, 1, data, data_len, pdMS_TO_TICKS(1000));
}

/**
 * @brief Initialize the PCA9557 device.
 * 
 * @return esp_err_t 
 */
esp_err_t dev_pca9557_init(void)
{
    esp_err_t ret = ESP_OK;
    bsp_i2c_master_init();

    ret = dev_pca9557_write_byte(PCA9557_OUTPUT_PORT, 0x05);
    ret = dev_pca9557_write_byte(PCA9557_CONFIGURATION_PORT, 0xf8); 
    return ret;
}

/**
 * @brief Set the output status of a specific GPIO pin on the PCA9557 device.
 * 
 * @param gpio_bit The bit position of the GPIO pin (0-7).
 * @param level The desired output level (0 or 1).
 * @return esp_err_t 
 */
esp_err_t dev_pca9557_set_output_status(uint8_t gpio_bit, uint8_t level)
{
    uint8_t output;
    esp_err_t ret = ESP_OK;

    dev_pca9557_read_register(PCA9557_OUTPUT_PORT, &output, 1);
    ret = dev_pca9557_write_byte(PCA9557_OUTPUT_PORT, SET_BITS(output, gpio_bit, level));
    return ret;
}

