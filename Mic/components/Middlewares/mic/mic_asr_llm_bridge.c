#include "mic_asr_llm_bridge.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "mic_asr_llm_bridge";

/**
 * @brief 队列中保存的一条 ASR final 文本。
 *
 * 调用方法：mic_asr_on_final_text() 收到 ASR final 后把文本复制到该结构体，再投递
 * 到 s_asr_llm_queue。队列按值复制，避免向 LLM task 传递 ASR 内部临时指针。
 */
typedef struct {
    char final_text[MIC_ASR_LLM_BRIDGE_PROMPT_MAX_BYTES]; // 已复制的 ASR final 文本。
} mic_asr_llm_bridge_item_t;

static QueueHandle_t s_asr_llm_queue;
static TaskHandle_t s_asr_llm_task_handle;
static volatile bool s_asr_llm_busy;

/**
 * @brief LLM 阻塞请求任务。
 *
 * 调用方法：mic_asr_llm_bridge_start() 创建后常驻运行。任务只从队列取 ASR final 文本，
 * 调用 mic_llm_doubao_chat_text()，并把 LLM 回复打印到串口日志；不播放音频、不维护
 * 多轮历史、不参与 ASR/VAD 状态机。
 */
static void mic_asr_llm_bridge_task(void *arg)
{
    (void)arg;

    mic_asr_llm_bridge_item_t item = {0};
    char response[MIC_ASR_LLM_BRIDGE_RESPONSE_BYTES] = {0};

    while (1) {
        if (xQueueReceive(s_asr_llm_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        s_asr_llm_busy = true;
        size_t final_len = strlen(item.final_text);
        ESP_LOGI(TAG,
                 "LLM request start: asr_final_len=%u, text=\"%s\"",
                 (unsigned int)final_len,
                 item.final_text);

        response[0] = '\0';
        size_t response_len = 0;
        esp_err_t ret = mic_llm_doubao_chat_text(item.final_text,
                                                 response,
                                                 sizeof(response),
                                                 &response_len);
        s_asr_llm_busy = false;
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LLM request failed: %s", esp_err_to_name(ret));
            continue;
        }

        ESP_LOGI(TAG,
                 "LLM response: len=%u, text=\"%s\"",
                 (unsigned int)response_len,
                 response);
    }
}

esp_err_t mic_asr_llm_bridge_start(void)
{
    if (s_asr_llm_task_handle != NULL) {
        ESP_LOGI(TAG, "ASR->LLM bridge already started");
        return ESP_OK;
    }

    if (s_asr_llm_queue == NULL) {
        s_asr_llm_queue = xQueueCreate(MIC_ASR_LLM_BRIDGE_QUEUE_DEPTH,
                                       sizeof(mic_asr_llm_bridge_item_t));
        if (s_asr_llm_queue == NULL) {
            ESP_LOGE(TAG, "ASR->LLM queue create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t created = xTaskCreate(mic_asr_llm_bridge_task,
                                     "asr_llm_task",
                                     MIC_ASR_LLM_BRIDGE_TASK_STACK_SIZE,
                                     NULL,
                                     MIC_ASR_LLM_BRIDGE_TASK_PRIORITY,
                                     &s_asr_llm_task_handle);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "ASR->LLM task create failed");
        s_asr_llm_task_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "ASR->LLM bridge started, queue_depth=%d",
             MIC_ASR_LLM_BRIDGE_QUEUE_DEPTH);
    return ESP_OK;
}

void mic_asr_on_final_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        ESP_LOGW(TAG, "ASR final is empty, skip LLM queue");
        return;
    }

    size_t text_len = strlen(text);
    ESP_LOGI(TAG,
             "ASR final received for LLM: len=%u, text=\"%s\"",
             (unsigned int)text_len,
             text);

    if (s_asr_llm_queue == NULL) {
        ESP_LOGW(TAG, "ASR->LLM queue not ready, drop ASR final");
        return;
    }
    if (s_asr_llm_busy) {
        ESP_LOGW(TAG,
                 "LLM busy, drop new ASR final: len=%u",
                 (unsigned int)text_len);
        return;
    }

    mic_asr_llm_bridge_item_t item = {0};
    strlcpy(item.final_text, text, sizeof(item.final_text));

    size_t copied_len = strlen(item.final_text);
    if (copied_len < text_len) {
        ESP_LOGW(TAG,
                 "ASR final truncated before LLM queue: original_len=%u, copied_len=%u",
                 (unsigned int)text_len,
                 (unsigned int)copied_len);
    }

    if (xQueueSend(s_asr_llm_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG,
                 "ASR->LLM queue full or LLM busy, drop new final: len=%u",
                 (unsigned int)copied_len);
        return;
    }

    ESP_LOGI(TAG,
             "ASR final queued for LLM: len=%u",
             (unsigned int)copied_len);
}
