#ifndef CSI_GAIN_COMPENSATION_H
#define CSI_GAIN_COMPENSATION_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

/**
 * @brief 官方 CSI 增益补偿输出状态。
 *
 * 调用方法：csi_gain_compensation_get_state() 填充本结构，monitor 再把状态输出到串口/网页。
 */
typedef struct {
    uint8_t agc_gain;          // 当前 CSI 包的 AGC 接收增益。
    int8_t fft_gain;           // 当前 CSI 包的 FFT 接收增益。
    float compensate_gain;     // 官方补偿系数；1.0 表示本帧未补偿。
    bool compensated;          // true 表示 compensate_gain 已可用。
} csi_gain_compensation_result_t;

/**
 * @brief 读取官方 esp_csi_gain_ctrl 的接收增益和补偿系数。
 *
 * 本函数只读取 agc_gain / fft_gain / compensate_gain，不再生成 int8_t 补偿数据。
 * 补偿算法需要使用该系数时，应在 float 幅值计算阶段处理，避免 int8 截断、
 * 溢出或饱和破坏 CSI 子载波形状。
 *
 * @param rx_ctrl ESP-IDF CSI 回调中的 RX 控制信息。
 * @param result 输出：增益和补偿状态。
 * @return ESP_OK 表示已成功取得当前增益信息；result->compensated 表示补偿系数是否可用。
 */
esp_err_t csi_gain_compensation_get_state(const wifi_pkt_rx_ctrl_t *rx_ctrl,
                                          csi_gain_compensation_result_t *result);

#endif // CSI_GAIN_COMPENSATION_H
