#ifndef CSI_PRINT_CONFIG_H
#define CSI_PRINT_CONFIG_H

/*
 * CSI 串口打印开关集中配置。
 *
 * 后续需要调整串口负载时，优先只改本文件：
 * 1. CSI_SERIAL_OUTPUT_ENABLE 控制是否启动串口输出任务。
 * 2. CSI_PRINT_FEATURE_RAW 控制是否输出 CSI_FEATURE_RAW 特征行。
 * 3. CSI_PRINT_DEBUG 控制是否输出 CSI_DBG 诊断行。
 * 4. CSI_PRINT_SUBBAND_DATA 控制是否把 subband 扩展字段追加到 CSI_FEATURE_RAW。
 * 5. CSI_PRINT_RAW_DATA 控制是否输出原始 CSI_DATA 数组，默认关闭，避免串口负载过大。
 * 6. CSI_FEATURE_PRINT_INTERVAL_MS 控制串口输出周期；越小曲线越密，串口负载越高。
 */
#define CSI_SERIAL_OUTPUT_ENABLE 1
#define CSI_PRINT_FEATURE_RAW 1
#define CSI_PRINT_DEBUG 1
#define CSI_PRINT_SUBBAND_DATA 1
#define CSI_PRINT_RAW_DATA 0
#define CSI_FEATURE_PRINT_INTERVAL_MS 50

// 每帧最多缓存/打印的原始 CSI 字节数；CSI_PRINT_RAW_DATA=0 时不会输出原始数组。
#define CSI_RAW_MAX_LEN 1024

#endif // CSI_PRINT_CONFIG_H
