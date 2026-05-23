#ifndef __SPI_H__
#define __SPI_H__

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"


/* 引脚定义 */
#define SPI2_MOSI_PIN  GPIO_NUM_7
#define SPI2_CLK_PIN   GPIO_NUM_6
#define SPI2_MISO_PIN    GPIO_NUM_8


void spi2_init(void);
void spi2_write_cmd(spi_device_handle_t spi, uint8_t cmd);
void spi2_write_mydata(spi_device_handle_t spi, const uint8_t *data, int len);
uint8_t spi2_transfer_byte(spi_device_handle_t handle, uint8_t data);


#endif /* __SPI_H__ */