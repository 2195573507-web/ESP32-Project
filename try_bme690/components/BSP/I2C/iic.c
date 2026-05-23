#include "iic.h"

#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "IIC";

i2c_obj_t iic_master[I2C_NUM_MAX] = {0};

/* 将ESP-IDF的I2C端口号转换成本地对象数组下标 */
static esp_err_t iic_get_index(i2c_port_t iic_port, int *index)
{
    if (index == NULL || iic_port < I2C_NUM_0 || iic_port >= I2C_NUM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    *index = (int)iic_port;
    return ESP_OK;
}

/* 所有读写接口先检查总线是否已经初始化，避免误操作 */
static esp_err_t iic_check_ready(i2c_port_t iic_port)
{
    int index = 0;
    esp_err_t ret = iic_get_index(iic_port, &index);
    if (ret != ESP_OK) {
        return ret;
    }

    if (iic_master[index].init_flag != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

i2c_obj_t iic_init(i2c_port_t iic_port)
{
    int index = 0;
    i2c_obj_t invalid_obj = {
        .port = iic_port,
        .sda_io_num = IIC_SDA_GPIO_PIN,
        .scl_io_num = IIC_SCL_GPIO_PIN,
        .clk_speed = IIC_FREQ,
        .init_flag = ESP_ERR_INVALID_ARG,
    };

    if (iic_get_index(iic_port, &index) != ESP_OK) {
        ESP_LOGE(TAG, "invalid I2C port: %d", iic_port);
        return invalid_obj;
    }

    /* 重复初始化时直接返回已有对象，避免重复安装驱动 */
    if (iic_master[index].init_flag == ESP_OK) {
        return iic_master[index];
    }

    iic_master[index].port = iic_port;
    iic_master[index].sda_io_num = IIC_SDA_GPIO_PIN;
    iic_master[index].scl_io_num = IIC_SCL_GPIO_PIN;
    iic_master[index].clk_speed = IIC_FREQ;
    iic_master[index].init_flag = ESP_FAIL;

    /* 配置ESP-IDF旧版I2C主机驱动 */
    i2c_config_t iic_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = iic_master[index].sda_io_num,
        .scl_io_num = iic_master[index].scl_io_num,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = iic_master[index].clk_speed,
        .clk_flags = 0,
    };

    esp_err_t ret = i2c_param_config(iic_port, &iic_config);
    if (ret != ESP_OK) {
        iic_master[index].init_flag = ret;
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(ret));
        return iic_master[index];
    }

    /* 参数配置成功后再安装I2C驱动 */
    ret = i2c_driver_install(iic_port,
                             I2C_MODE_MASTER,
                             I2C_MASTER_RX_BUF_DISABLE,
                             I2C_MASTER_TX_BUF_DISABLE,
                             0);
    iic_master[index].init_flag = ret;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(ret));
    }

    return iic_master[index];
}

esp_err_t iic_deinit(i2c_port_t iic_port)
{
    int index = 0;
    esp_err_t ret = iic_get_index(iic_port, &index);
    if (ret != ESP_OK) {
        return ret;
    }

    if (iic_master[index].init_flag != ESP_OK) {
        return ESP_OK;
    }

    /* 释放该端口的ESP-IDF I2C驱动资源 */
    ret = i2c_driver_delete(iic_port);
    if (ret == ESP_OK) {
        iic_master[index].init_flag = ESP_FAIL;
    }

    return ret;
}

esp_err_t iic_write(i2c_port_t iic_port, uint8_t dev_addr, const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* ESP-IDF这里需要7位设备地址，不需要包含读写位 */
    esp_err_t ret = iic_check_ready(iic_port);
    if (ret != ESP_OK) {
        return ret;
    }

    return i2c_master_write_to_device(iic_port, dev_addr, data, len, pdMS_TO_TICKS(IIC_TIMEOUT_MS));
}

esp_err_t iic_read(i2c_port_t iic_port, uint8_t dev_addr, uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* ESP-IDF这里需要7位设备地址，不需要包含读写位 */
    esp_err_t ret = iic_check_ready(iic_port);
    if (ret != ESP_OK) {
        return ret;
    }

    return i2c_master_read_from_device(iic_port, dev_addr, data, len, pdMS_TO_TICKS(IIC_TIMEOUT_MS));
}

esp_err_t iic_write_read(i2c_port_t iic_port,
                         uint8_t dev_addr,
                         const uint8_t *write_data,
                         size_t write_len,
                         uint8_t *read_data,
                         size_t read_len)
{
    if (write_data == NULL || write_len == 0 || read_data == NULL || read_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 该接口内部会发送重复起始信号 */
    esp_err_t ret = iic_check_ready(iic_port);
    if (ret != ESP_OK) {
        return ret;
    }

    return i2c_master_write_read_device(iic_port,
                                        dev_addr,
                                        write_data,
                                        write_len,
                                        read_data,
                                        read_len,
                                        pdMS_TO_TICKS(IIC_TIMEOUT_MS));
}

esp_err_t iic_write_reg(i2c_port_t iic_port, uint8_t dev_addr, uint8_t reg_addr, const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = iic_check_ready(iic_port);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 手动构造一次写寄存器事务：起始信号、设备写地址、寄存器地址、数据、停止信号 */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
    i2c_master_write_byte(cmd, reg_addr, ACK_CHECK_EN);
    i2c_master_write(cmd, data, len, ACK_CHECK_EN);
    i2c_master_stop(cmd);

    ret = i2c_master_cmd_begin(iic_port, cmd, pdMS_TO_TICKS(IIC_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);

    return ret;
}

esp_err_t iic_read_reg(i2c_port_t iic_port, uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, size_t len)
{
    /* 读寄存器等价于先写寄存器地址，再用重复起始信号读取数据 */
    return iic_write_read(iic_port, dev_addr, &reg_addr, 1, data, len);
}
