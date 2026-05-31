#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "mic_adc_test.h"
#include "mic_llm_bridge.h"
#include "wifi_manager.h"

/* 日志标签：只在本文件使用，不作为调试参数。 */
static const char *TAG = "APP_MAIN";

/**
 * @brief 应用入口函数。
 *
 * 参数：无，由 ESP-IDF 启动流程自动调用。
 * 调用方法：不要手动调用；ESP32-C5 启动后会进入本函数。
 */
void app_main(void)
{
    char connected_ssid[33] = {0};

    ESP_LOGI(TAG, "System start");

    // 初始化 WiFi 管理器：内部完成 NVS、网络接口、事件循环和 STA 模式初始化。
    ESP_ERROR_CHECK(wifi_manager_init());

    // 持续扫描并连接已保存列表中当前可用且信号最强的 WiFi。
    ESP_LOGI(TAG, "WiFi connect task start");
    if (wifi_connect_to_ap() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (wifi_get_connected_ssid(connected_ssid, sizeof(connected_ssid))) {
        ESP_LOGI(TAG, "WiFi connected, SSID: %s", connected_ssid);
    } else {
        ESP_LOGI(TAG, "WiFi connected");
    }

    // 等待 WiFi 连续稳定后再启动 Mic/ASR，避免刚拿到 IP 时就分配 TLS/WebSocket 资源。
    while (!wifi_is_stable()) {
        ESP_LOGI(TAG, "Waiting for stable WiFi before Mic/ASR start");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /*
     * WiFi 稳定后初始化 Mic bridge。Mic 只通过 bridge 调用 llm_client，不直接依赖
     * 网关 HTTP、WebSocket 或协议细节。
     */
    esp_err_t llm_ret = ai_mic_bridge_init();
    if (llm_ret != ESP_OK) {
        ESP_LOGE(TAG, "Mic LLM bridge init failed: %s", esp_err_to_name(llm_ret));
    }

    // WiFi 已连接且稳定后启动 Mic/ASR 链路，ASR 内部会自己建立 WebSocket TLS。
    // 这里不要使用 ESP_ERROR_CHECK：ASR/TLS 失败只打印错误，避免 abort 导致设备反复重启。
    esp_err_t mic_ret = mic_adc_test_start();
    if (mic_ret != ESP_OK) {
        ESP_LOGE(TAG, "Mic ADC/ASR start failed: %s", esp_err_to_name(mic_ret));
    }

    // WiFi 重连和 Mic ADC 采样都在后台任务中运行。
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
