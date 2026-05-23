#include "csi_gain_compensation.h"

#include "esp_csi_gain_ctrl.h"

/**
 * @brief 读取官方 esp_csi_gain_ctrl 的接收增益和补偿系数。
 *
 * 调用方法：wifi_csi_rx_cb() 对每帧通过过滤的 CSI 调用本函数，随后把
 * compensate_gain 交给补偿算法在 float 幅值计算阶段使用。
 */
esp_err_t csi_gain_compensation_get_state(const wifi_pkt_rx_ctrl_t *rx_ctrl,
                                          csi_gain_compensation_result_t *result)
{
    if (rx_ctrl == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *result = (csi_gain_compensation_result_t){0};
    result->compensate_gain = 1.0f;

    esp_csi_gain_ctrl_get_rx_gain(rx_ctrl, &result->agc_gain, &result->fft_gain);
    esp_csi_gain_ctrl_record_rx_gain(result->agc_gain, result->fft_gain);

    esp_err_t ret = esp_csi_gain_ctrl_get_gain_compensation(&result->compensate_gain,
                                                            result->agc_gain,
                                                            result->fft_gain);
    if (ret == ESP_OK) {
        result->compensated = true;
    } else {
        result->compensate_gain = 1.0f;
        result->compensated = false;
    }

    return ESP_OK;
}
