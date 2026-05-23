#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "csi_monitor.h"
#include "wifi_manager.h"

#define TAG "APP_MAIN"

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

    // 连接已保存列表中当前信号最强的 WiFi。
    ESP_LOGI(TAG, "WiFi connect start");
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

    // CSI 采集、原始数据输出、特征提取、运动判断和内部 ping 都封装在中间件中。
    ESP_ERROR_CHECK(wifi_csi_monitor_start());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
