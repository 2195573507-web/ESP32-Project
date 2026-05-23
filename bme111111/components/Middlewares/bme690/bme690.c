#include "bme690.h"

#include <string.h>

#include "bme69x.h"
#include "bme69x_defs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iic.h"

static const char *TAG = "BME690";

typedef struct {
    struct bme69x_dev dev;
    struct bme69x_conf conf;
    struct bme69x_heatr_conf heatr_conf;
    i2c_master_dev_handle_t i2c_dev;
    uint8_t i2c_addr;
    bool initialized;
} bme690_ctx_t;

static bme690_ctx_t s_bme690;

static esp_err_t bme690_rslt_to_err(int8_t rslt)
{
    switch (rslt) {
    case BME69X_OK:
        return ESP_OK;
    case BME69X_E_NULL_PTR:
        return ESP_ERR_INVALID_ARG;
    case BME69X_E_DEV_NOT_FOUND:
        return ESP_ERR_NOT_FOUND;
    case BME69X_W_NO_NEW_DATA:
        return ESP_ERR_TIMEOUT;
    default:
        return ESP_FAIL;
    }
}

static void bme690_delay_us(uint32_t period, void *intf_ptr)
{
    (void)intf_ptr;

    if (period < 1000U) {
        esp_rom_delay_us(period);
    } else {
        vTaskDelay(pdMS_TO_TICKS((period + 999U) / 1000U));
    }
}

static BME69X_INTF_RET_TYPE bme690_i2c_read(uint8_t reg_addr,
                                            uint8_t *reg_data,
                                            uint32_t len,
                                            void *intf_ptr)
{
    bme690_ctx_t *ctx = (bme690_ctx_t *)intf_ptr;
    if (ctx == NULL || ctx->i2c_dev == NULL || reg_data == NULL || len == 0U) {
        return BME69X_E_NULL_PTR;
    }

    esp_err_t ret = i2c_master_transmit_receive(ctx->i2c_dev,
                                                &reg_addr,
                                                1,
                                                reg_data,
                                                (size_t)len,
                                                IIC_BME690_PROBE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read reg 0x%02X failed: %s", reg_addr, esp_err_to_name(ret));
        return BME69X_E_COM_FAIL;
    }

    return BME69X_OK;
}

static BME69X_INTF_RET_TYPE bme690_i2c_write(uint8_t reg_addr,
                                             const uint8_t *reg_data,
                                             uint32_t len,
                                             void *intf_ptr)
{
    bme690_ctx_t *ctx = (bme690_ctx_t *)intf_ptr;
    if (ctx == NULL || ctx->i2c_dev == NULL || reg_data == NULL || len == 0U) {
        return BME69X_E_NULL_PTR;
    }

    uint8_t write_buf[16];
    if ((len + 1U) > sizeof(write_buf)) {
        return BME69X_E_INVALID_LENGTH;
    }

    write_buf[0] = reg_addr;
    memcpy(&write_buf[1], reg_data, (size_t)len);

    esp_err_t ret = i2c_master_transmit(ctx->i2c_dev,
                                        write_buf,
                                        (size_t)len + 1U,
                                        IIC_BME690_PROBE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "write reg 0x%02X failed: %s", reg_addr, esp_err_to_name(ret));
        return BME69X_E_COM_FAIL;
    }

    return BME69X_OK;
}

esp_err_t bme690_init(void)
{
    if (s_bme690.initialized) {
        return ESP_OK;
    }

    memset(&s_bme690, 0, sizeof(s_bme690));

    /* 先准备 CSB/SDO 模式脚，再创建 I2C 总线。 */
    ESP_RETURN_ON_ERROR(iic_bme690_prepare_pins(), TAG, "prepare pins failed");
    ESP_RETURN_ON_ERROR(iic_init(), TAG, "iic init failed");
    iic_log_bme690_control_levels("before probe");
    iic_log_line_levels("before probe");

    uint8_t addr = 0;
    uint8_t found = iic_find_bme690_addr(&addr);
    if (found == 0U) {
        iic_log_line_levels("after probe");
        iic_log_bme690_control_levels("after probe");
        ESP_LOGE(TAG, "BME690 not found at 0x%02X or 0x%02X",
                 IIC_BME690_ADDR_LOW,
                 IIC_BME690_ADDR_HIGH);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(iic_add_device(addr, &s_bme690.i2c_dev), TAG, "add device failed");
    s_bme690.i2c_addr = addr;

    s_bme690.dev.intf = BME69X_I2C_INTF;
    s_bme690.dev.read = bme690_i2c_read;
    s_bme690.dev.write = bme690_i2c_write;
    s_bme690.dev.delay_us = bme690_delay_us;
    s_bme690.dev.intf_ptr = &s_bme690;
    s_bme690.dev.amb_temp = 25;

    int8_t rslt = bme69x_init(&s_bme690.dev);
    esp_err_t ret = bme690_rslt_to_err(rslt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bme69x_init failed: %d", rslt);
        return ret;
    }

    /* 传感器配置参考 managed_components/espressif__bme690 的 forced mode 示例。 */
    s_bme690.conf.filter = BME69X_FILTER_OFF;
    s_bme690.conf.odr = BME69X_ODR_NONE;
    s_bme690.conf.os_hum = BME69X_OS_16X;
    s_bme690.conf.os_pres = BME69X_OS_1X;
    s_bme690.conf.os_temp = BME69X_OS_2X;

    rslt = bme69x_set_conf(&s_bme690.conf, &s_bme690.dev);
    ret = bme690_rslt_to_err(rslt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bme69x_set_conf failed: %d", rslt);
        return ret;
    }

    s_bme690.heatr_conf.enable = BME69X_ENABLE;
    s_bme690.heatr_conf.heatr_temp = BME690_HEATER_TEMP_C;
    s_bme690.heatr_conf.heatr_dur = BME690_HEATER_DUR_MS;

    rslt = bme69x_set_heatr_conf(BME69X_FORCED_MODE, &s_bme690.heatr_conf, &s_bme690.dev);
    ret = bme690_rslt_to_err(rslt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bme69x_set_heatr_conf failed: %d", rslt);
        return ret;
    }

    s_bme690.initialized = true;
    ESP_LOGI(TAG, "BME690 ready: addr=0x%02X chip_id=0x%02X", s_bme690.i2c_addr, s_bme690.dev.chip_id);
    return ESP_OK;
}

esp_err_t bme690_read_forced(bme690_data_t *out_data)
{
    if (out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_bme690.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int8_t rslt = bme69x_set_op_mode(BME69X_FORCED_MODE, &s_bme690.dev);
    esp_err_t ret = bme690_rslt_to_err(rslt);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t wait_us = bme69x_get_meas_dur(BME69X_FORCED_MODE, &s_bme690.conf, &s_bme690.dev) +
                       (s_bme690.heatr_conf.heatr_dur * 1000U);
    s_bme690.dev.delay_us(wait_us, s_bme690.dev.intf_ptr);

    struct bme69x_data raw_data = {0};
    uint8_t n_fields = 0;
    rslt = bme69x_get_data(BME69X_FORCED_MODE, &raw_data, &n_fields, &s_bme690.dev);
    ret = bme690_rslt_to_err(rslt);
    if (ret != ESP_OK) {
        return ret;
    }
    if (n_fields == 0U) {
        return ESP_ERR_TIMEOUT;
    }

    out_data->temperature_c = raw_data.temperature;
    out_data->pressure_pa = raw_data.pressure;
    out_data->humidity_percent = raw_data.humidity;
    out_data->gas_resistance_ohm = raw_data.gas_resistance;
    out_data->status = raw_data.status;
    return ESP_OK;
}

void bme690_deinit(void)
{
    if (s_bme690.i2c_dev != NULL) {
        (void)i2c_master_bus_rm_device(s_bme690.i2c_dev);
    }

    memset(&s_bme690, 0, sizeof(s_bme690));
    iic_deinit();
}

bool bme690_is_ready(void)
{
    return s_bme690.initialized;
}
