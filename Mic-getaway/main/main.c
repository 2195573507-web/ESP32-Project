#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "app_debug_config.h"
#include "app_main_config.h"
#include "mic_adc_test.h"
#include "mic_llm_bridge.h"
#include "speaker_llm_bridge.h"
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

    app_debug_apply_log_levels();

    // 初始化 WiFi 管理器：内部完成 NVS、网络接口、事件循环和 STA 模式初始化。
    ESP_ERROR_CHECK(wifi_manager_init());

    // 持续扫描并连接已保存列表中当前可用且信号最强的 WiFi。
    if (wifi_connect_to_ap() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (wifi_get_connected_ssid(connected_ssid, sizeof(connected_ssid))) {
        ESP_LOGI(TAG, "WiFi connected: %s", connected_ssid);
    } else {
        ESP_LOGI(TAG, "WiFi connected");
    }

    // 等待 WiFi 连续稳定后再启动 Mic/ASR，避免刚拿到 IP 时就分配 TLS/WebSocket 资源。
    while (!wifi_is_stable()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (MAIN_ENABLE_MIC_CHAIN) {
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
    } else {
        ESP_LOGI(TAG, "Mic chain disabled by MAIN_ENABLE_MIC_CHAIN");
    }

    if (MAIN_ENABLE_SPEAKER_CHAIN) {
        /*
         * speaker/TTS 链路当前默认开启。这里只初始化播放与 TTS 音频接收回调，
         * 是否开机测试播报由 MAIN_ENABLE_SPEAKER_BOOT_TTS_TEST 单独控制。
         */
        esp_err_t speaker_ret = speaker_llm_bridge_init();
        if (speaker_ret != ESP_OK) {
            ESP_LOGE(TAG, "Speaker bridge init failed: %s", esp_err_to_name(speaker_ret));
        }
    }

    if (MAIN_ENABLE_SPEAKER_CHAIN && MAIN_ENABLE_SPEAKER_BOOT_TTS_TEST) {
        /*
         * Speaker TTS 开机测试链路：
         * ESP 发送文字 -> 网关 TTS 合成 PCM -> TTS 音频回调 -> speaker_player 播放。
         */
        vTaskDelay(pdMS_TO_TICKS(MAIN_SPEAKER_BOOT_TTS_TEST_DELAY_MS));
        esp_err_t tts_ret = speaker_llm_bridge_speak_text(MAIN_SPEAKER_BOOT_TTS_TEST_TEXT);
        if (tts_ret != ESP_OK) {
            ESP_LOGE(TAG, "Speaker boot TTS test failed: %s", esp_err_to_name(tts_ret));
        }
    }

    // WiFi 重连、Mic ADC 采样和可选 speaker/TTS 链路都在后台任务中运行。
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MAIN_IDLE_DELAY_MS));
    }
}
