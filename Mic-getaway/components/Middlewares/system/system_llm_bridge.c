#include "system_llm_bridge.h"

#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "llm_client.h"
#include "wifi_manager.h"

static const char *TAG = "system_llm_bridge";

esp_err_t system_llm_bridge_init(void)
{
    ESP_LOGI(TAG, "system bridge initialized");
    return ESP_OK;
}

esp_err_t system_llm_bridge_get_status_json(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out,
                           out_size,
                           "{\"free_heap\":%lu,\"min_free_heap\":%lu,\"largest_free_block\":%u,"
                           "\"wifi_connected\":%s,\"voice_session_active\":%s,\"tts_enabled\":false}",
                           (unsigned long)esp_get_free_heap_size(),
                           (unsigned long)esp_get_minimum_free_heap_size(),
                           (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                           wifi_is_connected() ? "true" : "false",
                           llm_client_is_voice_session_active() ? "true" : "false");
    if (written < 0 || (size_t)written >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (APP_DEBUG_SYSTEM_LLM_BRIDGE) {
        ESP_LOGI(TAG, "system_status: %s", out);
    }
    return ESP_OK;
}

esp_err_t system_llm_bridge_send_status_to_llm(void)
{
    char json[512] = {0};
    esp_err_t ret = system_llm_bridge_get_status_json(json, sizeof(json));
    if (ret != ESP_OK) {
        return ret;
    }
    return llm_client_send_sensor_json_with_model("system_status", json, SYSTEM_LLM_BRIDGE_LLM_MODEL);
}
