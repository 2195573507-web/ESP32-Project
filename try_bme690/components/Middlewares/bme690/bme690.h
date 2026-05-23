#ifndef __BME690_H__
#define __BME690_H__

#include <stdint.h>

#include "esp_err.h"

/*
 * BME690 的 “命令/寄存器/位域” 宏定义统一放在 `bme.h` 里，
 * 便于把寄存器表与驱动逻辑解耦。
 */
#include "bme.h"

/*
 * I2C 读写底层使用 BSP/I2C 的封装（`components/BSP/I2C/iic.h`）。
 * 注意：`iic_*` 接口使用 7-bit 地址（不包含 R/W 位），与本文件的地址宏保持一致。
 */
#include "iic.h"

#endif /* __BME690_H__ */

