#ifndef CSI_MONITOR_H
#define CSI_MONITOR_H

#include "csi_print_config.h"
#include "esp_err.h"

/*
 * CSI 采集参数。
 *
 * 打印相关开关集中放在 csi_print_config.h：
 * - CSI_SERIAL_OUTPUT_ENABLE
 * - CSI_PRINT_FEATURE_RAW
 * - CSI_PRINT_DEBUG
 * - CSI_PRINT_RAW_DATA
 * - CSI_FEATURE_PRINT_INTERVAL_MS
 * - CSI_RAW_MAX_LEN
 */
#define CSI_INTERNAL_PING_INTERVAL_MS 50  // 内部 ping 网关周期，单位 ms。
#define CSI_INTERNAL_PING_SIZE 32         // 内部 ping 载荷大小，单位字节。

/**
 * @brief 启动 ESP32-C5 接收端 CSI 采集与人体运动特征输出。
 *
 * 调用前要求 WiFi STA 已连接 AP。函数内部会：
 * 1. 记录当前 AP BSSID 和本机 STA MAC，作为 router -> ESP32-C5 过滤条件；
 * 2. 打开混杂模式和 WiFi CSI；
 * 3. 注册 CSI 接收回调；
 * 4. 只保留源 MAC 为 AP、目的 MAC 为本机的下行 CSI；
 * 5. 启动网关 ping，用回包制造稳定 CSI 包流；
 * 6. 按 csi_print_config.h 中的开关输出 CSI_FEATURE_RAW / CSI_DBG / CSI_DATA。
 *
 * @return ESP_OK 表示启动成功，其他值表示启动失败。
 */
esp_err_t wifi_csi_monitor_start(void);

#endif // CSI_MONITOR_H
