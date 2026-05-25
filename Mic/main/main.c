#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "mic_adc_test.h"
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

    // 启动 Mic ADC continuous 采样测试：OPA_OUT -> GPIO6 / ADC1_CH5。
    ESP_ERROR_CHECK(mic_adc_test_start());

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

    // WiFi 重连和 Mic ADC 采样都在后台任务中运行。
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
