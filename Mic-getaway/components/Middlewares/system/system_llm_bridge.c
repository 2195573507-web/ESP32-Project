#include "system_llm_bridge.h"

#include <stdio.h>
#include <string.h>

#include "ai_bridge_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "llm_client.h"
#include "wifi_manager.h"

static const char *TAG = "system_llm_bridge";

esp_err_t system_llm_bridge_init(void)
{
    return ai_system_bridge_init();
}

esp_err_t ai_system_bridge_init(void)
{
    ESP_LOGI(TAG, "system bridge initialized");
    return ESP_OK;
}

esp_err_t ai_system_bridge_get_snapshot(ai_system_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    snapshot->free_heap = esp_get_free_heap_size();
    snapshot->min_free_heap = esp_get_minimum_free_heap_size();
    snapshot->free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snapshot->wifi_connected = wifi_is_connected() ? 1 : 0;
    snapshot->asr_active = llm_client_is_voice_session_active() ? 1 : 0;
    snapshot->llm_state = (int)llm_client_get_state();
    return ESP_OK;
}

esp_err_t ai_system_bridge_build_context(const ai_system_snapshot_t *snapshot,
                                         char *out_context,
                                         size_t out_context_size)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ai_bridge_check_out_buf(out_context, out_context_size, "system_context");
    if (ret != ESP_OK) {
        return ret;
    }

    int written = snprintf(out_context,
                           out_context_size,
                           "模块：System 状态模块\n\n"
                           "当前系统状态：\n"
                           "- WiFi：%s\n"
                           "- free_heap：%u bytes\n"
                           "- min_free_heap：%u bytes\n"
                           "- free_psram：%u bytes\n"
                           "- llm_state：%s\n"
                           "- asr_active：%s",
                           snapshot->wifi_connected ? "已连接" : "未连接",
                           (unsigned int)snapshot->free_heap,
                           (unsigned int)snapshot->min_free_heap,
                           (unsigned int)snapshot->free_psram,
                           llm_client_state_name((llm_client_state_t)snapshot->llm_state),
                           snapshot->asr_active ? "是" : "否");
    return (written < 0 || (size_t)written >= out_context_size) ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t ai_system_bridge_context_query(const char *question,
                                         char *out_reply,
                                         size_t out_reply_size)
{
    esp_err_t ret = ai_bridge_check_str(question, "question");
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ai_bridge_check_out_buf(out_reply, out_reply_size, "reply");
    if (ret != ESP_OK) {
        return ret;
    }

    ai_system_snapshot_t snapshot = {0};
    ret = ai_system_bridge_get_snapshot(&snapshot);
    if (ret != ESP_OK) {
        return ret;
    }
    char context[512] = {0};
    ret = ai_system_bridge_build_context(&snapshot, context, sizeof(context));
    if (ret != ESP_OK) {
        return ret;
    }
    size_t offset = strlen(context);
    ret = ai_bridge_appendf(context, sizeof(context), &offset, "\n\n用户问题：\n%s", question);
    if (ret != ESP_OK) {
        return ret;
    }
    return llm_client_text_query("你是 ESP32 设备上的系统状态分析模型。回答要简短、可执行。",
                                 context,
                                 out_reply,
                                 out_reply_size);
}

esp_err_t system_llm_bridge_get_status_json(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ai_system_snapshot_t snapshot = {0};
    esp_err_t ret = ai_system_bridge_get_snapshot(&snapshot);
    if (ret != ESP_OK) {
        return ret;
    }
    int written = snprintf(out,
                           out_size,
                           "{\"free_heap\":%u,\"min_free_heap\":%u,\"free_psram\":%u,"
                           "\"largest_free_block\":%u,\"wifi_connected\":%s,"
                           "\"asr_active\":%s,\"llm_state\":\"%s\",\"tts_enabled\":%s}",
                           (unsigned int)snapshot.free_heap,
                           (unsigned int)snapshot.min_free_heap,
                           (unsigned int)snapshot.free_psram,
                           (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                           snapshot.wifi_connected ? "true" : "false",
                           snapshot.asr_active ? "true" : "false",
                           llm_client_state_name((llm_client_state_t)snapshot.llm_state),
                           llm_client_is_tts_enabled() ? "true" : "false");
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
    return llm_client_send_system_status_json(json);
}
