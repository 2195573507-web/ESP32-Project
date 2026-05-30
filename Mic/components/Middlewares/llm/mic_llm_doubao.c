#include "mic_llm_doubao.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "llm_client.h"

static const char *TAG = "mic_llm_doubao";

/**
 * @brief Mic 侧初始化状态。
 *
 * 调用方法：只用于避免业务代码在没有配置 URL/API Key 前直接发起 LLM 请求。
 * 通用 llm_client 的内部状态仍由 llm_client.c 自己维护。
 */
static bool s_mic_llm_doubao_initialized;

/**
 * @brief 判断配置值是否仍是占位符。
 *
 * 调用方法：init 时检查 API Key，避免把占位符写入 llm_client 后发到云端。
 */
static bool mic_llm_doubao_is_placeholder(const char *value)
{
    return value == NULL || value[0] == '\0' || strstr(value, OPENAI_PLACEHOLDER_MARKER) != NULL;
}

esp_err_t mic_llm_doubao_init(void)
{
    if (mic_llm_doubao_is_placeholder(OPENAI_API_KEY)) {
        ESP_LOGE(TAG, "Mic LLM API Key 未配置，请先修改 llm_config.h");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = llm_client_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "llm_client 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    s_mic_llm_doubao_initialized = true;
    ESP_LOGI(TAG, "Mic LLM interface initialized");
    return ESP_OK;
}

esp_err_t mic_llm_doubao_chat_text(const char *prompt,
                                   char *response_buffer,
                                   size_t response_buffer_size,
                                   size_t *response_len)
{
    if (response_len != NULL) {
        *response_len = 0;
    }
    if (response_buffer != NULL && response_buffer_size > 0) {
        response_buffer[0] = '\0';
    }

    if (!s_mic_llm_doubao_initialized) {
        ESP_LOGE(TAG, "Mic LLM interface 未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    if (prompt == NULL || prompt[0] == '\0' ||
        response_buffer == NULL || response_buffer_size <= 1) {
        ESP_LOGE(TAG, "Mic LLM chat_text 参数错误");
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 通用 llm_client 要求 response_len 非空；Mic 对外接口允许调用者不关心长度，
     * 因此这里用局部变量兜底，保持适配层接口更宽松。
     */
    size_t local_response_len = 0;
    return llm_send_message(prompt,
                            response_buffer,
                            response_buffer_size,
                            response_len != NULL ? response_len : &local_response_len);
}

void mic_llm_doubao_deinit(void)
{
    s_mic_llm_doubao_initialized = false;
    ESP_LOGI(TAG, "Mic LLM interface deinitialized");
}
