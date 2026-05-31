#include "csi_llm_bridge.h"

#include <stdio.h>
#include <string.h>

#include "ai_bridge_common.h"
#include "esp_log.h"
#include "llm_client.h"

static const char *TAG = "csi_llm_bridge";

esp_err_t csi_llm_bridge_init(void)
{
    return ai_csi_bridge_init();
}

esp_err_t ai_csi_bridge_init(void)
{
    if (APP_DEBUG_CSI_LLM_BRIDGE) {
        ESP_LOGI(TAG, "CSI bridge reserved");
    }
    return ESP_OK;
}

esp_err_t ai_csi_bridge_build_context(const ai_csi_summary_t *summary,
                                      char *out_context,
                                      size_t out_context_size)
{
    if (summary == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ai_bridge_check_out_buf(out_context, out_context_size, "csi_context");
    if (ret != ESP_OK) {
        return ret;
    }

    int written = snprintf(out_context,
                           out_context_size,
                           "模块：CSI 感知模块\n\n"
                           "当前 CSI 摘要：\n"
                           "- RSSI：%.2f dBm\n"
                           "- 噪声底：%.2f dBm\n"
                           "- 活动分数：%.2f\n"
                           "- 是否检测到人体存在：%s\n"
                           "- 是否检测到运动：%s",
                           summary->rssi,
                           summary->noise_floor,
                           summary->activity_score,
                           summary->presence_detected ? "是" : "否",
                           summary->motion_detected ? "是" : "否");
    return (written < 0 || (size_t)written >= out_context_size) ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t ai_csi_bridge_report(const ai_csi_summary_t *summary)
{
    char context[256] = {0};
    esp_err_t ret = ai_csi_bridge_build_context(summary, context, sizeof(context));
    if (ret != ESP_OK) {
        return ret;
    }
    if (APP_DEBUG_CSI_LLM_BRIDGE) {
        ESP_LOGI(TAG, "%s", context);
    }
    return ESP_OK;
}

esp_err_t ai_csi_bridge_ask(const ai_csi_summary_t *summary,
                            const char *question,
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

    char context[384] = {0};
    ret = ai_csi_bridge_build_context(summary, context, sizeof(context));
    if (ret != ESP_OK) {
        return ret;
    }

    size_t offset = strlen(context);
    ret = ai_bridge_appendf(context, sizeof(context), &offset, "\n\n用户问题：\n%s", question);
    if (ret != ESP_OK) {
        return ret;
    }
    return llm_client_text_query("你是 ESP32 设备上的 CSI 感知解释模型。只基于输入数据回答，不要编造额外传感器信息。",
                                 context,
                                 out_reply,
                                 out_reply_size);
}

esp_err_t csi_llm_bridge_send_json(const char *json)
{
    if (json == NULL || json[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (APP_DEBUG_CSI_LLM_BRIDGE) {
        ESP_LOGI(TAG, "CSI JSON reserved");
    }
    return llm_client_send_csi_json(json);
}

esp_err_t csi_llm_bridge_send_summary(const char *summary_json)
{
    return csi_llm_bridge_send_json(summary_json);
}
