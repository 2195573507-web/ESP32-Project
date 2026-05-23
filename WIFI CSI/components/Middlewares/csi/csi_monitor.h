#ifndef CSI_MONITOR_H
#define CSI_MONITOR_H

#include "esp_err.h"

/*
 * CSI 采集与串口输出调试参数
 *
 * 调参说明：
 * 1. CSI_FEATURE_PRINT_INTERVAL_MS 控制串口输出频率；越小曲线越密，但串口负载越高。
 * 2. CSI_INTERNAL_PING_INTERVAL_MS 控制内部 ping 网关的频率；越小 CSI 包流越密，但 WiFi 和 CPU 负载越高。
 * 3. CSI_INTERNAL_PING_SIZE 控制 ping 载荷大小；一般保持 32 字节即可。
 * 4. CSI_PRINT_RAW_DATA 控制是否把原始 CSI_DATA 打印到串口；0 只打印特征，1 同时打印原始数组。
 * 5. CSI_RAW_MAX_LEN 控制每帧最多缓存/打印多少原始 CSI 字节；越大串口输出越重。
 * 6. 当前默认 ping 为 50 ms 一次，约 20 次/秒；CSI_FEATURE 也为 50 ms 一次，约 20 帧/秒。
 */
#define CSI_FEATURE_PRINT_INTERVAL_MS 50  // 串口输出 CSI_FEATURE_RAW 的周期，单位 ms。
#define CSI_INTERNAL_PING_INTERVAL_MS 50  // 内部 ping 网关的周期，单位 ms。
#define CSI_INTERNAL_PING_SIZE 32         // 内部 ping 载荷大小，单位字节。
#define CSI_SERIAL_OUTPUT_ENABLE 1        // 1: 启动 CSI 串口输出任务；0: 只采集/处理，不输出串口。
#define CSI_PRINT_RAW_DATA 0              // 0: 不打印原始 CSI_DATA；1: 打印原始 CSI_DATA data=[...]。
#define CSI_RAW_MAX_LEN 1024              // 每帧最多缓存并打印的原始 CSI 字节数。

/**
 * @brief 启动 ESP32-C5 接收端 CSI 采集与人体运动特征输出。
 *
 * 调用前要求 WiFi STA 已经连接 AP。函数内部会：
 * 1. 记录当前 AP/路由器 BSSID 和本机 STA MAC，作为 router -> ESP32-C5 过滤条件；
 * 2. 打开混杂模式和 WiFi CSI；
 * 3. 注册 CSI 接收回调；
 * 4. 只保留源 MAC 为 AP/路由器、目的 MAC 为本机的下行 CSI；
 * 5. 启动网关 ping，用回包制造稳定 CSI 包流；
 * 6. 通过串口输出 CSI_FEATURE_RAW 和 CSI_DBG；CSI_PRINT_RAW_DATA 为 1 时额外输出 CSI_DATA 原始数据。
 *
 * @return ESP_OK 表示启动成功，其他值表示启动失败。
 */
esp_err_t wifi_csi_monitor_start(void);

#endif // CSI_MONITOR_H
