#include "iic.h"

#include <stddef.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

static const char *TAG = "IIC";

static iic_bus_t s_iic_bus = {
    .port = IIC_BME690_PORT,
    .sda_io_num = IIC_BME690_SDA_GPIO,
    .scl_io_num = IIC_BME690_SCL_GPIO,
    .clk_speed_hz = IIC_BME690_FREQ_HZ,
    .bus_handle = NULL,
    .initialized = false,
};

static bool s_bme690_pins_prepared = false;

esp_err_t iic_bme690_prepare_pins(void)
{
    if (s_bme690_pins_prepared) {
        return ESP_OK;
    }

    /* 只配置 BME690 模式脚，不手动配置 GPIO2/GPIO3，SDA/SCL 必须交给 I2C 驱动。
     * 控制脚使用输入输出模式，既能驱动 CSB/SDO，也能读取实际管脚电平用于诊断。
     */
    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(IIC_BME690_CSB_GPIO) |
                        BIT64(IIC_BME690_SDO_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        return ret;
    }

    /* BME690 上电时采样 CSB：高电平为 I2C 模式，低电平会进入 SPI 模式。 */
    ESP_RETURN_ON_ERROR(gpio_set_level(IIC_BME690_CSB_GPIO, IIC_BME690_CSB_I2C_LEVEL), TAG, "set CSB failed");

    /* SDO 决定 I2C 地址：0 为 0x76，1 为 0x77。 */
    ESP_RETURN_ON_ERROR(gpio_set_level(IIC_BME690_SDO_GPIO, IIC_BME690_SDO_ADDR_LEVEL), TAG, "set SDO failed");

    vTaskDelay(pdMS_TO_TICKS(10));

    s_bme690_pins_prepared = true;
    ESP_LOGI(TAG,
             "BME690 pins ready: SDA=%d SCL=%d CSB=%d SDO=%d",
             IIC_BME690_SDA_GPIO,
             IIC_BME690_SCL_GPIO,
             IIC_BME690_CSB_GPIO,
             IIC_BME690_SDO_GPIO);
    return ESP_OK;
}

esp_err_t iic_init(void)
{
    if (s_iic_bus.initialized) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = s_iic_bus.port,
        .sda_io_num = s_iic_bus.sda_io_num,
        .scl_io_num = s_iic_bus.scl_io_num,
#if defined(SOC_LP_I2C_SUPPORTED) && SOC_LP_I2C_SUPPORTED
        .lp_source_clk = LP_I2C_SCLK_DEFAULT,
#else
        .clk_source = I2C_CLK_SRC_DEFAULT,
#endif
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = false,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_iic_bus.bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_iic_bus.initialized = true;
    ESP_LOGI(TAG,
             "I2C ready: port=%d sda=%d scl=%d freq=%lu",
             s_iic_bus.port,
             s_iic_bus.sda_io_num,
             s_iic_bus.scl_io_num,
             (unsigned long)s_iic_bus.clk_speed_hz);
    return ESP_OK;
}

void iic_deinit(void)
{
    if (!s_iic_bus.initialized) {
        return;
    }

    esp_err_t ret = i2c_del_master_bus(s_iic_bus.bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "i2c_del_master_bus failed: %s", esp_err_to_name(ret));
        return;
    }

    s_iic_bus.bus_handle = NULL;
    s_iic_bus.initialized = false;
}

i2c_master_bus_handle_t iic_get_bus_handle(void)
{
    return s_iic_bus.bus_handle;
}

esp_err_t iic_probe(uint8_t addr, uint32_t timeout_ms)
{
    if (!s_iic_bus.initialized || s_iic_bus.bus_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_probe(s_iic_bus.bus_handle, addr, (int)timeout_ms);
}

esp_err_t iic_add_device(uint8_t addr, i2c_master_dev_handle_t *out_dev)
{
    if (!s_iic_bus.initialized || s_iic_bus.bus_handle == NULL || out_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = s_iic_bus.clk_speed_hz,
    };

    return i2c_master_bus_add_device(s_iic_bus.bus_handle, &dev_cfg, out_dev);
}

uint8_t iic_find_bme690_addr(uint8_t *out_addr)
{
    const uint8_t addrs[] = {
        IIC_BME690_ADDR_LOW,
        IIC_BME690_ADDR_HIGH,
    };

    uint8_t found = 0;
    for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        esp_err_t ret = iic_probe(addrs[i], IIC_BME690_PROBE_TIMEOUT_MS);
        if (ret == ESP_OK) {
            if (out_addr != NULL && found == 0) {
                *out_addr = addrs[i];
            }
            found++;
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addrs[i]);
        }
    }

    ESP_LOGI(TAG, "I2C scan finished, found=%u", found);
    return found;
}

void iic_log_line_levels(const char *hint)
{
    if (hint == NULL) {
        hint = "";
    }

    ESP_LOGW(TAG,
             "I2C lines%s%s: SDA=%d SCL=%d",
             (hint[0] != '\0') ? " " : "",
             hint,
             gpio_get_level(s_iic_bus.sda_io_num),
             gpio_get_level(s_iic_bus.scl_io_num));
}

void iic_log_bme690_control_levels(const char *hint)
{
    if (hint == NULL) {
        hint = "";
    }

    ESP_LOGW(TAG,
             "BME690 ctrl%s%s: CSB=%d SDO=%d",
             (hint[0] != '\0') ? " " : "",
             hint,
             gpio_get_level(IIC_BME690_CSB_GPIO),
             gpio_get_level(IIC_BME690_SDO_GPIO));
}
