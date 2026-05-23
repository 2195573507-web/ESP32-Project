#ifndef __BME_H__
#define __BME_H__

/*
 * BME690 命令/寄存器/位域宏定义（I2C 地址为 7-bit 地址）
 *
 * 约定：
 * - 本文件只放 “寄存器地址 / 命令值 / 位域掩码与位移 / 取值枚举” 等宏，不实现任何函数。
 * - BME690 读数使用 Field 数据区（Field0/Field1/Field2），不同 Field 的寄存器地址按固定步进排列。
 */

#include <stdint.h>

/* =========================
 * I2C 地址（7-bit）
 * =========================
 * SDO/ADDR 接 GND 通常为 0x76，接 VDDIO 通常为 0x77（以模块/硬件接法为准）。
 */
#define BME690_I2C_ADDR_LOW  0x76 /* 7-bit I2C address when SDO/ADDR=0 */
#define BME690_I2C_ADDR_HIGH 0x77 /* 7-bit I2C address when SDO/ADDR=1 */

/* =========================
 * 基础识别/复位寄存器
 * ========================= */

#define BME690_REG_CHIP_ID 0xD0 /* Chip ID 寄存器地址 */
#define BME690_CHIP_ID     0x61 /* BME690 期望的 Chip ID 值 */

#define BME690_REG_VARIANT_ID 0xF0 /* Variant ID 寄存器地址 */
#define BME690_VARIANT_ID     0x02 /* BME690 期望的 Variant ID 值 */

#define BME690_REG_SOFT_RESET 0xE0 /* Soft reset 寄存器地址 */
#define BME690_CMD_SOFT_RESET 0xB6 /* Soft reset 命令：写入该值触发复位 */

#define BME690_REG_STATUS 0x73                 /* Status 寄存器地址 */
#define BME690_STATUS_SPI_MEM_PAGE_POS 4       /* SPI memory page bit 位置（I2C 一般无需关心） */
#define BME690_STATUS_SPI_MEM_PAGE_MSK (1U << BME690_STATUS_SPI_MEM_PAGE_POS) /* SPI memory page bit 掩码 */

/* =========================
 * 通用控制寄存器（BME690）
 * ========================= */

#define BME690_REG_CTRL_GAS_0 0x70 /* ctrl_gas_0：气体测量/加热器控制 */
#define BME690_REG_CTRL_GAS_1 0x71 /* ctrl_gas_1：气体测量控制（run_gas/nb_conv） */

#define BME690_REG_CTRL_HUM  0x72 /* ctrl_hum：湿度过采样配置 */
#define BME690_REG_CTRL_MEAS 0x74 /* ctrl_meas：温度/压力过采样 + 模式 */
#define BME690_REG_CONFIG    0x75 /* config：IIR 滤波等 */

/* Heater 配置寄存器组（通常 index=0~9，共 10 组） */
#define BME690_REG_GAS_WAIT_SHARED 0x6E /* gas_wait_shared：共享等待时间/倍率配置 */
#define BME690_REG_GAS_WAIT_BASE   0x64 /* gas_wait_0 起始地址（每组 1 字节） */
#define BME690_REG_RES_HEAT_BASE   0x5A /* res_heat_0 起始地址（每组 1 字节） */
#define BME690_REG_IDAC_HEAT_BASE  0x50 /* idac_heat_0 起始地址（每组 1 字节） */

/* 通过索引计算对应寄存器地址（index: 0~9） */
#define BME690_REG_GAS_WAIT(index) ((uint8_t)(BME690_REG_GAS_WAIT_BASE + (uint8_t)(index))) /* gas_wait_index */
#define BME690_REG_RES_HEAT(index) ((uint8_t)(BME690_REG_RES_HEAT_BASE + (uint8_t)(index))) /* res_heat_index */
#define BME690_REG_IDAC_HEAT(index) ((uint8_t)(BME690_REG_IDAC_HEAT_BASE + (uint8_t)(index))) /* idac_heat_index */

/* -------------------------
 * ctrl_meas：过采样与模式
 * ------------------------- */

/* mode<1:0>：工作模式取值（写入 ctrl_meas 的 mode 位域） */
#define BME690_MODE_SLEEP    0x00 /* 00b: Sleep mode */
#define BME690_MODE_FORCED   0x01 /* 01b: Forced mode */
#define BME690_MODE_PARALLEL 0x02 /* 10b: Parallel mode */

/* osrs_*：过采样倍率（写入 ctrl_meas/ctrl_hum 的 osrs 位域） */
#define BME690_OSRS_SKIP 0x00 /* 000b: 跳过该通道（输出通常为 0x8000） */
#define BME690_OSRS_X1   0x01 /* 001b: ×1 */
#define BME690_OSRS_X2   0x02 /* 010b: ×2 */
#define BME690_OSRS_X4   0x03 /* 011b: ×4 */
#define BME690_OSRS_X8   0x04 /* 100b: ×8 */
#define BME690_OSRS_X16  0x05 /* 101b (以及更高编码): ×16 */

/* ctrl_meas 位域（掩码/位移） */
#define BME690_CTRL_MEAS_MODE_MSK   0x03 /* mode<1:0> mask */
#define BME690_CTRL_MEAS_OSRS_P_MSK 0x1C /* osrs_p<2:0> mask (bits[4:2]) */
#define BME690_CTRL_MEAS_OSRS_T_MSK 0xE0 /* osrs_t<2:0> mask (bits[7:5]) */
#define BME690_CTRL_MEAS_OSRS_P_POS 2    /* osrs_p shift */
#define BME690_CTRL_MEAS_OSRS_T_POS 5    /* osrs_t shift */

/* ctrl_hum：osrs_h<2:0> 位域（掩码/位移） */
#define BME690_CTRL_HUM_OSRS_H_MSK 0x07 /* osrs_h<2:0> mask */
#define BME690_CTRL_HUM_OSRS_H_POS 0    /* osrs_h shift */

/* ctrl_hum：SPI 3-wire interrupt enable（I2C 一般不用） */
#define BME690_CTRL_HUM_SPI_3W_INT_EN_POS 6 /* SPI_3W_INT_EN bit position */
#define BME690_CTRL_HUM_SPI_3W_INT_EN_MSK (1U << BME690_CTRL_HUM_SPI_3W_INT_EN_POS) /* SPI_3W_INT_EN mask */

/* config：IIR 滤波 + SPI 3-wire enable */
#define BME690_CONFIG_FILTER_MSK 0x1C /* filter<2:0> mask (bits[4:2]) */
#define BME690_CONFIG_FILTER_POS 2    /* filter shift */
#define BME690_CONFIG_SPI_3W_EN_POS 0 /* SPI_3W_EN bit position */
#define BME690_CONFIG_SPI_3W_EN_MSK (1U << BME690_CONFIG_SPI_3W_EN_POS) /* SPI_3W_EN mask */

/* IIR 滤波系数（写入 config 的 filter<2:0> 位域） */
#define BME690_FILTER_OFF 0x00 /* 000b: 关闭滤波 */
#define BME690_FILTER_2   0x01 /* 001b: filter = 2 */
#define BME690_FILTER_4   0x02 /* 010b: filter = 4 */
#define BME690_FILTER_8   0x03 /* 011b: filter = 8 */
#define BME690_FILTER_16  0x04 /* 100b: filter = 16 */

/* ctrl_gas_1：run_gas / nb_conv */
#define BME690_CTRL_GAS_1_NB_CONV_MSK 0x0F /* nb_conv<3:0> mask：并行模式下转换次数 */
#define BME690_CTRL_GAS_1_RUN_GAS_POS 5    /* run_gas bit position：使能气体测量 */
#define BME690_CTRL_GAS_1_RUN_GAS_MSK (1U << BME690_CTRL_GAS_1_RUN_GAS_POS) /* run_gas mask */

/* ctrl_gas_0：heat_off */
#define BME690_CTRL_GAS_0_HEAT_OFF_POS 3 /* heat_off bit position：关闭加热器 */
#define BME690_CTRL_GAS_0_HEAT_OFF_MSK (1U << BME690_CTRL_GAS_0_HEAT_OFF_POS) /* heat_off mask */

/*
 * gas_wait_shared：气体测量等待时间（共享值）
 * - gas_wait_shared<5:0>：基础时间值（单位约 0.477ms，具体以数据手册为准）
 * - gas_wait_shared_mult<7:6>：倍率（00=×1，01=×4，10=×16，11=×64）
 */
#define BME690_GAS_WAIT_SHARED_VAL_MSK  0x3F /* gas_wait_shared<5:0> mask */
#define BME690_GAS_WAIT_SHARED_MULT_POS 6    /* gas_wait_shared_mult shift */
#define BME690_GAS_WAIT_SHARED_MULT_MSK (0x03U << BME690_GAS_WAIT_SHARED_MULT_POS) /* mult mask */
#define BME690_GAS_WAIT_SHARED_MULT_1   (0x00U << BME690_GAS_WAIT_SHARED_MULT_POS) /* 00b: ×1 */
#define BME690_GAS_WAIT_SHARED_MULT_4   (0x01U << BME690_GAS_WAIT_SHARED_MULT_POS) /* 01b: ×4 */
#define BME690_GAS_WAIT_SHARED_MULT_16  (0x02U << BME690_GAS_WAIT_SHARED_MULT_POS) /* 10b: ×16 */
#define BME690_GAS_WAIT_SHARED_MULT_64  (0x03U << BME690_GAS_WAIT_SHARED_MULT_POS) /* 11b: ×64 */

/* =========================
 * Field 数据寄存器（BME690）
 * =========================
 * BME690 有 3 个 Field 数据区：Field0 / Field1 / Field2
 * 每个 Field 的寄存器块之间步进为 0x11：
 * - Field0 base = 0x1D
 * - Field1 base = 0x1D + 0x11
 * - Field2 base = 0x1D + 0x22
 */

#define BME690_FIELD0_BASE  0x1D /* Field0 起始地址 */
#define BME690_FIELD_STRIDE 0x11 /* 相邻 Field 的地址步进 */

#define BME690_FIELD_BASE(field_index) ((uint8_t)(BME690_FIELD0_BASE + (uint8_t)(field_index) * BME690_FIELD_STRIDE)) /* Field base by index */

/* Field 内各数据偏移（相对 BME690_FIELD_BASE） */
#define BME690_FIELD_OFF_MEAS_STATUS  0x00 /* meas_status */
#define BME690_FIELD_OFF_SUB_MEAS_IDX 0x01 /* sub_meas_index */
#define BME690_FIELD_OFF_PRESS_MSB    0x02 /* press[19:12] */
#define BME690_FIELD_OFF_PRESS_LSB    0x03 /* press[11:4] */
#define BME690_FIELD_OFF_PRESS_XLSB   0x04 /* press[3:0] (upper nibble) */
#define BME690_FIELD_OFF_TEMP_MSB     0x05 /* temp[19:12] */
#define BME690_FIELD_OFF_TEMP_LSB     0x06 /* temp[11:4] */
#define BME690_FIELD_OFF_TEMP_XLSB    0x07 /* temp[3:0] (upper nibble) */
#define BME690_FIELD_OFF_HUM_MSB      0x08 /* hum[15:8] */
#define BME690_FIELD_OFF_HUM_LSB      0x09 /* hum[7:0] */
#define BME690_FIELD_OFF_GAS_R_MSB    0x0F /* gas_resistance MSB (+range bits) */
#define BME690_FIELD_OFF_GAS_R_LSB    0x10 /* gas_resistance LSB (+valid/stab/range bits) */

/* 通过 Field 索引计算具体寄存器地址（field_index: 0~2） */
#define BME690_REG_MEAS_STATUS(field_index) ((uint8_t)(BME690_FIELD_BASE(field_index) + BME690_FIELD_OFF_MEAS_STATUS)) /* meas_status reg */
#define BME690_REG_SUB_MEAS_INDEX(field_index) ((uint8_t)(BME690_FIELD_BASE(field_index) + BME690_FIELD_OFF_SUB_MEAS_IDX)) /* sub_meas_index reg */

#define BME690_REG_PRESS_MSB(field_index)  ((uint8_t)(BME690_FIELD_BASE(field_index) + BME690_FIELD_OFF_PRESS_MSB))  /* pressure MSB reg */
#define BME690_REG_PRESS_LSB(field_index)  ((uint8_t)(BME690_FIELD_BASE(field_index) + BME690_FIELD_OFF_PRESS_LSB))  /* pressure LSB reg */
#define BME690_REG_PRESS_XLSB(field_index) ((uint8_t)(BME690_FIELD_BASE(field_index) + BME690_FIELD_OFF_PRESS_XLSB)) /* pressure XLSB reg */

#define BME690_REG_TEMP_MSB(field_index)  ((uint8_t)(BME690_FIELD_BASE(field_index) + BME690_FIELD_OFF_TEMP_MSB))  /* temperature MSB reg */
#define BME690_REG_TEMP_LSB(field_index)  ((uint8_t)(BME690_FIELD_BASE(field_index) + BME690_FIELD_OFF_TEMP_LSB))  /* temperature LSB reg */
#define BME690_REG_TEMP_XLSB(field_index) ((uint8_t)(BME690_FIELD_BASE(field_index) + BME690_FIELD_OFF_TEMP_XLSB)) /* temperature XLSB reg */

#define BME690_REG_HUM_MSB(field_index) ((uint8_t)(BME690_FIELD_BASE(field_index) + BME690_FIELD_OFF_HUM_MSB)) /* humidity MSB reg */
#define BME690_REG_HUM_LSB(field_index) ((uint8_t)(BME690_FIELD_BASE(field_index) + BME690_FIELD_OFF_HUM_LSB)) /* humidity LSB reg */

#define BME690_REG_GAS_R_MSB(field_index) ((uint8_t)(BME690_FIELD_BASE(field_index) + BME690_FIELD_OFF_GAS_R_MSB)) /* gas resistance MSB reg */
#define BME690_REG_GAS_R_LSB(field_index) ((uint8_t)(BME690_FIELD_BASE(field_index) + BME690_FIELD_OFF_GAS_R_LSB)) /* gas resistance LSB reg */

/* meas_status 位域 */
#define BME690_MEAS_STATUS_NEW_DATA_POS      7     /* new_data bit position */
#define BME690_MEAS_STATUS_GAS_MEASURING_POS 6     /* gas_measuring bit position */
#define BME690_MEAS_STATUS_MEASURING_POS     5     /* measuring bit position */
#define BME690_MEAS_STATUS_GAS_MEAS_INDEX_MSK 0x0F /* gas_meas_index<3:0> mask */

#define BME690_MEAS_STATUS_NEW_DATA_MSK      (1U << BME690_MEAS_STATUS_NEW_DATA_POS)      /* new_data mask */
#define BME690_MEAS_STATUS_GAS_MEASURING_MSK (1U << BME690_MEAS_STATUS_GAS_MEASURING_POS) /* gas_measuring mask */
#define BME690_MEAS_STATUS_MEASURING_MSK     (1U << BME690_MEAS_STATUS_MEASURING_POS)     /* measuring mask */

/* gas_r_lsb 常用状态位域 */
#define BME690_GAS_R_LSB_GAS_VALID_POS 5    /* gas_valid bit position：本次 gas 数据有效 */
#define BME690_GAS_R_LSB_HEAT_STAB_POS 4    /* heat_stab bit position：加热器稳定 */
#define BME690_GAS_R_LSB_GAS_RANGE_MSK 0x0F /* gas_range<3:0> mask */

#define BME690_GAS_R_LSB_GAS_VALID_MSK (1U << BME690_GAS_R_LSB_GAS_VALID_POS) /* gas_valid mask */
#define BME690_GAS_R_LSB_HEAT_STAB_MSK (1U << BME690_GAS_R_LSB_HEAT_STAB_POS) /* heat_stab mask */

#endif /* __BME_H__ */
