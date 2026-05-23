#ifndef __IIC_H__
#define __IIC_H__

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"

/* I2C主机总线对象 */
typedef struct {
    i2c_port_t port;        /* I2C端口号，例如I2C_NUM_0 */
    gpio_num_t sda_io_num;  /* SDA引脚 */
    gpio_num_t scl_io_num;  /* SCL引脚 */
    uint32_t clk_speed;     /* I2C时钟频率，单位Hz */
    esp_err_t init_flag;    /* ESP_OK表示该总线已经初始化 */
} i2c_obj_t;

/* I2C数据缓冲区描述 */
typedef struct {
    size_t len;   /* 缓冲区长度，单位字节 */
    uint8_t *buf; /* 缓冲区指针 */
} i2c_buf_t;

extern i2c_obj_t iic_master[I2C_NUM_MAX];

/* 操作标志，保留给旧代码或组合传输时使用 */
#define I2C_FLAG_READ  0x01
#define I2C_FLAG_STOP  0x02
#define I2C_FLAG_WRITE 0x04

/* 默认I2C引脚和频率，如果板子接线不同，改这里即可 */
#define IIC_SDA_GPIO_PIN GPIO_NUM_2
#define IIC_SCL_GPIO_PIN GPIO_NUM_3
#define IIC_FREQ           100000

/* I2C主机模式不需要硬件收发环形缓冲区 */
#define I2C_MASTER_TX_BUF_DISABLE 0
#define I2C_MASTER_RX_BUF_DISABLE 0
#define ACK_CHECK_EN              1
#define IIC_TIMEOUT_MS            1000

/* 初始化指定I2C主机端口，使用上面的默认引脚和频率 */
i2c_obj_t iic_init(i2c_port_t iic_port);

/* 释放指定I2C端口，即使没有初始化也可以安全调用 */
esp_err_t iic_deinit(i2c_port_t iic_port);

/* 向7位I2C设备地址写入原始数据 */
esp_err_t iic_write(i2c_port_t iic_port, uint8_t dev_addr, const uint8_t *data, size_t len);

/* 从7位I2C设备地址读取原始数据 */
esp_err_t iic_read(i2c_port_t iic_port, uint8_t dev_addr, uint8_t *data, size_t len);

/* 先写后读，中间自动重复起始信号，常用于寄存器读取 */
esp_err_t iic_write_read(i2c_port_t iic_port,
                         uint8_t dev_addr,
                         const uint8_t *write_data,
                         size_t write_len,
                         uint8_t *read_data,
                         size_t read_len);

/* 向8位寄存器地址写入数据 */
esp_err_t iic_write_reg(i2c_port_t iic_port, uint8_t dev_addr, uint8_t reg_addr, const uint8_t *data, size_t len);

/* 从8位寄存器地址读取数据 */
esp_err_t iic_read_reg(i2c_port_t iic_port, uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, size_t len);

#endif
