#ifndef CSI_FEATURE_H
#define CSI_FEATURE_H

#include <stdint.h>

#include "csi_gain_compensation.h"
#include "csi_monitor.h"
#include "csi_processor.h"
#include "esp_netif.h"

/**
 * @brief 单帧 CSI 特征快照。
 *
 * 调用方法：csi_monitor.c 在 CSI 回调中填充本结构，串口输出模块只读取并格式化输出。
 * 这个结构把“采集处理结果”和“输出方式”解耦，后续可以被串口、网页或其他上报通道复用。
 */
typedef struct {
    uint32_t frame_count;              // 本模块内部递增的 CSI 帧号。
    int8_t rssi;                       // 接收该帧 WiFi 包时的 RSSI。
    uint8_t channel;                   // 当前 WiFi 信道。
    uint16_t csi_len;                  // ESP-IDF 回调给出的原始 CSI 字节长度。
    csi_processor_result_t raw_processed; // 原始 CSI 主算法输出的运动特征。
    csi_gain_compensation_result_t gain;  // 官方增益补偿状态。
    uint16_t raw_len;                  // 实际缓存/输出的原始 CSI 字节长度。
    uint8_t mac[6];                    // 源 MAC，期望为 AP/路由器 BSSID。
    uint8_t dmac[6];                   // 目的 MAC，期望为本机 STA MAC。
    uint8_t sig_mode;                  // 预留字段，ESP32-C5 当前填 0。
    uint8_t mcs;                       // 预留字段，ESP32-C5 当前填 0。
    uint8_t cwb;                       // 预留字段，ESP32-C5 当前填 0。
    uint8_t first_word_invalid;        // ESP-IDF 标记的首字无效标志。
#if CSI_PRINT_RAW_DATA
    int8_t raw_buf[CSI_RAW_MAX_LEN];   // 原始 CSI I/Q 字节缓存，仅在开启原始数据输出时存在。
#endif
} csi_feature_t;

/**
 * @brief 当前 CSI 链路信息。
 *
 * 调用方法：csi_monitor.c 初始化链路过滤后填充，串口输出模块启动时打印到 CSI_LINK。
 */
typedef struct {
    uint8_t ap_bssid[6];        // 当前连接 AP/路由器的 BSSID，作为 CSI 源 MAC。
    uint8_t sta_mac[6];         // 本机 STA MAC，作为 CSI 目的 MAC。
    esp_ip4_addr_t gateway_ip;  // 当前网络网关 IP，内部 ping 的目标地址。
} csi_link_info_t;

#endif // CSI_FEATURE_H
