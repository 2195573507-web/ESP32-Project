#ifndef IIC_H
#define IIC_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IIC_BME690_PORT：ESP32-C5 的 GPIO2/GPIO3 对应 LP_I2C 固定引脚。 */
#define IIC_BME690_PORT              LP_I2C_NUM_0

/* IIC_BME690_SDA_GPIO：PDF 原理图中 BM_SDI/SDA 连接到 GPIO2。 */
#define IIC_BME690_SDA_GPIO          GPIO_NUM_2

/* IIC_BME690_SCL_GPIO：PDF 原理图中 BM_SCK/SCL 连接到 GPIO3。 */
#define IIC_BME690_SCL_GPIO          GPIO_NUM_3

/* IIC_BME690_FREQ_HZ：BME690 I2C 标准模式时钟，先用 100 kHz 保守启动。 */
#define IIC_BME690_FREQ_HZ           100000U

/* IIC_BME690_PROBE_TIMEOUT_MS：探测 0x76/0x77 的单次等待时间，避免无响应时长时间卡住。 */
#define IIC_BME690_PROBE_TIMEOUT_MS  100U

/* IIC_BME690_CSB_GPIO：PDF 原理图中 BM_CS 连接到 GPIO10，I2C 模式必须保持高电平。 */
#define IIC_BME690_CSB_GPIO          GPIO_NUM_10

/* IIC_BME690_SDO_GPIO：PDF 原理图中 BM_SDO 连接到 GPIO9，用来选择 I2C 地址。 */
#define IIC_BME690_SDO_GPIO          GPIO_NUM_9

/* IIC_BME690_CSB_I2C_LEVEL：CSB 为高时 BME690 进入 I2C 模式。 */
#define IIC_BME690_CSB_I2C_LEVEL     1

/* IIC_BME690_SDO_ADDR_LEVEL：SDO 为低时 BME690 使用 0x76 地址。 */
#define IIC_BME690_SDO_ADDR_LEVEL    0

/* IIC_BME690_ADDR_LOW：SDO=0 时 BME690 的 7 位 I2C 地址。 */
#define IIC_BME690_ADDR_LOW          0x76U

/* IIC_BME690_ADDR_HIGH：SDO=1 时 BME690 的 7 位 I2C 地址。 */
#define IIC_BME690_ADDR_HIGH         0x77U

typedef struct {
    i2c_port_num_t port;
    gpio_num_t sda_io_num;
    gpio_num_t scl_io_num;
    uint32_t clk_speed_hz;
    i2c_master_bus_handle_t bus_handle;
    bool initialized;
} iic_bus_t;

esp_err_t iic_bme690_prepare_pins(void);
esp_err_t iic_init(void);
void iic_deinit(void);
i2c_master_bus_handle_t iic_get_bus_handle(void);
esp_err_t iic_probe(uint8_t addr, uint32_t timeout_ms);
esp_err_t iic_add_device(uint8_t addr, i2c_master_dev_handle_t *out_dev);
uint8_t iic_find_bme690_addr(uint8_t *out_addr);
void iic_log_line_levels(const char *hint);
void iic_log_bme690_control_levels(const char *hint);

#ifdef __cplusplus
}
#endif

#endif
