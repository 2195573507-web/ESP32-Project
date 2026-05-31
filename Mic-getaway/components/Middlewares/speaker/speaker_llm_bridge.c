#include "speaker_llm_bridge.h"

/**
 * @file speaker_llm_bridge.c
 * @brief speaker/TTS 到 llm_client 的桥接实现。
 *
 * 本层不直接组装火山网关协议，也不持有 WebSocket/HTTP 细节；它只注册 TTS
 * 音频接收回调，把 llm_client 回调到的 PCM 指针快速入队，再由独立播放任务
 * 调用 speaker_player。这样 WebSocket event task 不会被 IIS/播放阻塞。
 */

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "llm_client.h"
#include "speaker_player.h"

static const char *TAG = "speaker_llm_bridge";

enum {
    SPEAKER_TTS_QUEUE_DEPTH = 4,
    SPEAKER_TTS_QUEUE_SEND_TIMEOUT_MS = 20,
    SPEAKER_TTS_PREBUFFER_CHUNKS = 2,
    SPEAKER_TTS_PREBUFFER_TIMEOUT_MS = 300,
    SPEAKER_TTS_PLAY_TASK_STACK = 4096,
    SPEAKER_TTS_PLAY_TASK_PRIORITY = 4,
};

typedef struct {
    uint8_t *pcm;
    size_t pcm_len;
    int sample_rate_hz;
    bool end_marker;
} speaker_tts_pcm_item_t;

static QueueHandle_t s_tts_pcm_queue;
static TaskHandle_t s_tts_play_task;
static bool s_speaker_llm_bridge_initialized;
static bool s_tts_done_marker_queued;
static bool s_speaker_tts_round_active;
static uint32_t s_speaker_tts_audio_delta_count;
static size_t s_speaker_tts_pcm_bytes_total;
static size_t s_speaker_playback_bytes_total;
static uint32_t s_speaker_tts_dropped_chunk_count;
static UBaseType_t s_speaker_tts_stack_high_water_bytes;

static UBaseType_t speaker_llm_bridge_queue_depth(void)
{
    return s_tts_pcm_queue != NULL ? uxQueueMessagesWaiting(s_tts_pcm_queue) : 0;
}

static size_t speaker_llm_bridge_free_heap(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

static size_t speaker_llm_bridge_min_free_heap(void)
{
    return heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
}

static size_t speaker_llm_bridge_largest_free_block(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

static UBaseType_t speaker_llm_bridge_current_stack_high_water(void)
{
#if (INCLUDE_uxTaskGetStackHighWaterMark == 1)
    return uxTaskGetStackHighWaterMark(NULL);
#else
    return 0;
#endif
}

static UBaseType_t speaker_llm_bridge_note_stack_high_water(void)
{
    UBaseType_t current = speaker_llm_bridge_current_stack_high_water();
    if (current != 0 &&
        (s_speaker_tts_stack_high_water_bytes == 0 ||
         current < s_speaker_tts_stack_high_water_bytes)) {
        s_speaker_tts_stack_high_water_bytes = current;
    }
    return current;
}

static void speaker_llm_bridge_begin_round(void)
{
    s_speaker_tts_round_active = true;
    s_tts_done_marker_queued = false;
    s_speaker_tts_audio_delta_count = 0;
    s_speaker_tts_pcm_bytes_total = 0;
    s_speaker_playback_bytes_total = 0;
    s_speaker_tts_dropped_chunk_count = 0;
    s_speaker_tts_stack_high_water_bytes = 0;
}

static void speaker_llm_bridge_log_round_summary(const char *reason)
{
    (void)speaker_llm_bridge_note_stack_high_water();
    ESP_LOGI(TAG,
             "speaker TTS summary reason=%s free_heap=%u min_free_heap=%u "
             "largest_free_block=%u audio_delta_count=%u pcm_bytes_total=%u "
             "playback_bytes_total=%u dropped_chunk_count=%u speaker_stack_hwm=%u "
             "queue_depth=%u queue_depth_max=%u chunk_max=%u",
             reason != NULL ? reason : "tts_done",
             (unsigned int)speaker_llm_bridge_free_heap(),
             (unsigned int)speaker_llm_bridge_min_free_heap(),
             (unsigned int)speaker_llm_bridge_largest_free_block(),
             (unsigned int)s_speaker_tts_audio_delta_count,
             (unsigned int)s_speaker_tts_pcm_bytes_total,
             (unsigned int)s_speaker_playback_bytes_total,
             (unsigned int)s_speaker_tts_dropped_chunk_count,
             (unsigned int)s_speaker_tts_stack_high_water_bytes,
             (unsigned int)speaker_llm_bridge_queue_depth(),
             (unsigned int)SPEAKER_TTS_QUEUE_DEPTH,
             (unsigned int)SPEAKER_TTS_MAX_CHUNK_BYTES);
}

static esp_err_t speaker_llm_bridge_enqueue_end_marker(void)
{
    if (s_tts_pcm_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_tts_done_marker_queued) {
        return ESP_OK;
    }

    speaker_tts_pcm_item_t item = {
        .pcm = NULL,
        .pcm_len = 0,
        .sample_rate_hz = 0,
        .end_marker = true,
    };
    int64_t enqueue_start_us = esp_timer_get_time();
    BaseType_t sent = xQueueSend(s_tts_pcm_queue,
                                 &item,
                                 pdMS_TO_TICKS(SPEAKER_TTS_QUEUE_SEND_TIMEOUT_MS));
    int64_t enqueue_us = esp_timer_get_time() - enqueue_start_us;
    ESP_LOGI(TAG,
             "speaker TTS enqueue end_marker ret=%d enqueue_us=%lld queue_depth=%u playback_bytes_total=%u",
             sent == pdTRUE ? 1 : 0,
             (long long)enqueue_us,
             (unsigned int)speaker_llm_bridge_queue_depth(),
             (unsigned int)s_speaker_playback_bytes_total);
    if (sent != pdTRUE) {
        speaker_tts_pcm_item_t dropped = {0};
        BaseType_t got_drop = xQueueReceive(s_tts_pcm_queue, &dropped, 0);
        if (got_drop == pdTRUE && dropped.pcm != NULL) {
            s_speaker_tts_dropped_chunk_count++;
            ESP_LOGW(TAG,
                     "speaker TTS queue full, drop queued chunk for end_marker bytes=%u dropped_chunk_count=%u",
                     (unsigned int)dropped.pcm_len,
                     (unsigned int)s_speaker_tts_dropped_chunk_count);
            free(dropped.pcm);
        }
        sent = xQueueSend(s_tts_pcm_queue, &item, 0);
        if (sent != pdTRUE) {
            ESP_LOGW(TAG, "speaker TTS queue full, drop end marker after retry");
            return ESP_ERR_TIMEOUT;
        }
    }
    s_tts_done_marker_queued = true;
    return ESP_OK;
}

static esp_err_t speaker_llm_bridge_play_audio_item(speaker_tts_pcm_item_t *item,
                                                    bool *stream_open)
{
    if (item == NULL || stream_open == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (item->pcm == NULL || item->pcm_len == 0) {
        free(item->pcm);
        return ESP_OK;
    }

    int64_t play_start_us = esp_timer_get_time();
    if (!*stream_open) {
        esp_err_t open_ret = audio_player_stream_open();
        if (open_ret != ESP_OK) {
            ESP_LOGW(TAG, "speaker TTS stream open failed: %s", esp_err_to_name(open_ret));
            free(item->pcm);
            return open_ret;
        }
        *stream_open = true;
    }

    esp_err_t ret = audio_player_write_pcm_chunk((const int16_t *)item->pcm,
                                                 (uint32_t)(item->pcm_len / sizeof(int16_t)),
                                                 item->sample_rate_hz);
    int64_t play_us = esp_timer_get_time() - play_start_us;
    if (ret == ESP_OK) {
        s_speaker_playback_bytes_total += item->pcm_len;
    } else {
        s_speaker_tts_dropped_chunk_count++;
        ESP_LOGW(TAG, "speaker TTS play failed: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG,
             "speaker TTS play chunk bytes=%u sample_rate=%d play_us=%lld queue_depth=%u playback_bytes_total=%u",
             (unsigned int)item->pcm_len,
             item->sample_rate_hz,
             (long long)play_us,
             (unsigned int)speaker_llm_bridge_queue_depth(),
             (unsigned int)s_speaker_playback_bytes_total);
    free(item->pcm);
    item->pcm = NULL;
    item->pcm_len = 0;
    return ret;
}

static void speaker_llm_bridge_finish_stream_if_open(bool *stream_open)
{
    if (stream_open == NULL || !*stream_open) {
        return;
    }
    esp_err_t finish_ret = audio_player_stream_finish();
    if (finish_ret != ESP_OK) {
        ESP_LOGW(TAG, "speaker TTS stream finish failed: %s", esp_err_to_name(finish_ret));
    } else {
        ESP_LOGI(TAG,
                 "speaker TTS playback done queue_depth=%u playback_bytes_total=%u",
                 (unsigned int)speaker_llm_bridge_queue_depth(),
                 (unsigned int)s_speaker_playback_bytes_total);
    }
    *stream_open = false;
}

static void speaker_llm_bridge_play_task(void *arg)
{
    (void)arg;
    bool stream_open = false;

    while (1) {
        (void)speaker_llm_bridge_note_stack_high_water();
        speaker_tts_pcm_item_t item = {0};
        if (xQueueReceive(s_tts_pcm_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (item.end_marker) {
            s_tts_done_marker_queued = false;
            speaker_llm_bridge_finish_stream_if_open(&stream_open);
            speaker_llm_bridge_log_round_summary("tts_done");
            s_speaker_tts_round_active = false;
            if (APP_DEBUG_SPEAKER_LLM_BRIDGE) {
                ESP_LOGI(TAG,
                         "speaker TTS stream end queue_depth=%u playback_bytes_total=%u",
                         (unsigned int)speaker_llm_bridge_queue_depth(),
                         (unsigned int)s_speaker_playback_bytes_total);
            }
            continue;
        }

        if (!stream_open) {
            speaker_tts_pcm_item_t buffered[SPEAKER_TTS_PREBUFFER_CHUNKS] = {0};
            size_t buffered_count = 1;
            bool got_end_marker = false;
            buffered[0] = item;

            while (buffered_count < SPEAKER_TTS_PREBUFFER_CHUNKS) {
                speaker_tts_pcm_item_t next = {0};
                BaseType_t got_next = xQueueReceive(s_tts_pcm_queue,
                                                    &next,
                                                    pdMS_TO_TICKS(SPEAKER_TTS_PREBUFFER_TIMEOUT_MS));
                if (got_next != pdTRUE) {
                    break;
                }
                if (next.end_marker) {
                    got_end_marker = true;
                    break;
                }
                if (next.pcm == NULL || next.pcm_len == 0) {
                    free(next.pcm);
                    continue;
                }
                buffered[buffered_count++] = next;
            }

            ESP_LOGI(TAG,
                     "speaker TTS prebuffer chunks=%u got_end_marker=%d queue_depth=%u",
                     (unsigned int)buffered_count,
                     got_end_marker ? 1 : 0,
                     (unsigned int)speaker_llm_bridge_queue_depth());

            for (size_t i = 0; i < buffered_count; i++) {
                if (speaker_llm_bridge_play_audio_item(&buffered[i], &stream_open) != ESP_OK) {
                    speaker_llm_bridge_finish_stream_if_open(&stream_open);
                    break;
                }
            }
            if (got_end_marker) {
                speaker_llm_bridge_finish_stream_if_open(&stream_open);
            }
            continue;
        }

        if (speaker_llm_bridge_play_audio_item(&item, &stream_open) != ESP_OK) {
            speaker_llm_bridge_finish_stream_if_open(&stream_open);
        }
    }
}

static void speaker_llm_bridge_event_cb(const llm_client_event_t *event, void *user_ctx)
{
    /* 只保留错误常显；普通 TTS 音频事件受 APP_DEBUG_SPEAKER_LLM_BRIDGE 控制。 */
    (void)user_ctx;
    if (event == NULL) {
        return;
    }

    if (event->type == LLM_CLIENT_EVENT_ERROR) {
        ESP_LOGE(TAG,
                 "speaker LLM event error: code=%d message=%s",
                 event->code,
                 event->message != NULL ? event->message : "<none>");
    } else if (APP_DEBUG_SPEAKER_LLM_BRIDGE &&
               event->type == LLM_CLIENT_EVENT_TTS_AUDIO) {
        ESP_LOGI(TAG, "speaker TTS event audio bytes=%u", (unsigned int)event->audio_len);
    } else if (event->type == LLM_CLIENT_EVENT_TTS_DONE) {
        if (APP_DEBUG_SPEAKER_LLM_BRIDGE) {
            ESP_LOGI(TAG, "speaker TTS done");
        }
        (void)speaker_llm_bridge_enqueue_end_marker();
    }
}

static esp_err_t speaker_llm_bridge_enqueue_tts_audio(uint8_t *audio,
                                                      size_t audio_len,
                                                      int sample_rate_hz,
                                                      bool take_ownership,
                                                      void *user_ctx)
{
    /*
     * 这里运行在 llm_client/TTS WebSocket 回调路径上，只能做快速校验和入队。
     * take_ownership=true 时 PCM chunk 入队成功后 ownership 转移给播放任务；
     * 入队失败则返回错误，让 llm_client 释放原始 PCM。
     */
    (void)user_ctx;
    if (audio == NULL || audio_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((audio_len % sizeof(int16_t)) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_tts_pcm_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_speaker_tts_round_active) {
        speaker_llm_bridge_begin_round();
    }
    s_speaker_tts_audio_delta_count++;
    s_speaker_tts_pcm_bytes_total += audio_len;
    s_tts_done_marker_queued = false;

    if (!take_ownership) {
        s_speaker_tts_dropped_chunk_count++;
        ESP_LOGW(TAG,
                 "speaker TTS drop non-owned PCM chunk bytes=%u dropped_chunk_count=%u",
                 (unsigned int)audio_len,
                 (unsigned int)s_speaker_tts_dropped_chunk_count);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (audio_len > SPEAKER_TTS_MAX_CHUNK_BYTES) {
        s_speaker_tts_dropped_chunk_count++;
        ESP_LOGW(TAG,
                 "speaker TTS drop oversized PCM chunk bytes=%u max=%u dropped_chunk_count=%u",
                 (unsigned int)audio_len,
                 (unsigned int)SPEAKER_TTS_MAX_CHUNK_BYTES,
                 (unsigned int)s_speaker_tts_dropped_chunk_count);
        return ESP_ERR_INVALID_SIZE;
    }

    speaker_tts_pcm_item_t item = {
        .pcm = audio,
        .pcm_len = audio_len,
        .sample_rate_hz = sample_rate_hz,
        .end_marker = false,
    };
    int64_t enqueue_start_us = esp_timer_get_time();
    BaseType_t sent = xQueueSend(s_tts_pcm_queue,
                                 &item,
                                 pdMS_TO_TICKS(SPEAKER_TTS_QUEUE_SEND_TIMEOUT_MS));
    int64_t enqueue_us = esp_timer_get_time() - enqueue_start_us;
    if (sent != pdTRUE) {
        s_speaker_tts_dropped_chunk_count++;
        ESP_LOGW(TAG,
                 "speaker TTS queue full, drop chunk bytes=%u enqueue_us=%lld queue_depth=%u dropped_chunk_count=%u",
                 (unsigned int)audio_len,
                 (long long)enqueue_us,
                 (unsigned int)speaker_llm_bridge_queue_depth(),
                 (unsigned int)s_speaker_tts_dropped_chunk_count);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG,
             "speaker TTS enqueue chunk bytes=%u sample_rate=%d owned=%d enqueue_us=%lld queue_depth=%u",
             (unsigned int)audio_len,
             sample_rate_hz,
             take_ownership ? 1 : 0,
             (long long)enqueue_us,
             (unsigned int)speaker_llm_bridge_queue_depth());
    return ESP_OK;
}

static esp_err_t speaker_llm_bridge_tts_done_sink(void *user_ctx)
{
    (void)user_ctx;
    return speaker_llm_bridge_enqueue_end_marker();
}

esp_err_t speaker_llm_bridge_init(void)
{
    if (s_speaker_llm_bridge_initialized) {
        return ESP_OK;
    }
    /*
     * 初始化顺序：
     * 1. llm_client 准备 TTS/网关能力。
     * 2. speaker_player 准备 IIS/PDM 输出。
     * 3. 注册 TTS 音频接收回调，让后续 TTS 音频块自动进入播放链路。
     */
    llm_client_config_t config = {
        .system_prompt = LLM_GATEWAY_SYSTEM_PROMPT,
        .event_cb = speaker_llm_bridge_event_cb,
        .user_ctx = NULL,
    };
    esp_err_t ret = llm_client_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "llm_client init for speaker failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = audio_player_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "speaker player init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_tts_pcm_queue = xQueueCreate(SPEAKER_TTS_QUEUE_DEPTH, sizeof(speaker_tts_pcm_item_t));
    if (s_tts_pcm_queue == NULL) {
        ESP_LOGW(TAG, "speaker TTS queue create failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_created = xTaskCreate(speaker_llm_bridge_play_task,
                                          "speaker_tts_play",
                                          SPEAKER_TTS_PLAY_TASK_STACK,
                                          NULL,
                                          SPEAKER_TTS_PLAY_TASK_PRIORITY,
                                          &s_tts_play_task);
    if (task_created != pdPASS) {
        ESP_LOGW(TAG, "speaker TTS play task create failed");
        vQueueDelete(s_tts_pcm_queue);
        s_tts_pcm_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    llm_client_set_tts_audio_sink(speaker_llm_bridge_enqueue_tts_audio, NULL);
    llm_client_set_tts_done_sink(speaker_llm_bridge_tts_done_sink, NULL);
    s_speaker_llm_bridge_initialized = true;
    if (APP_DEBUG_SPEAKER_LLM_BRIDGE) {
        ESP_LOGI(TAG, "speaker bridge initialized, tts_enabled=%d", llm_client_is_tts_enabled());
    }
    return ESP_OK;
}

esp_err_t speaker_llm_bridge_speak_text(const char *text)
{
    /* 文本合成仍由 llm_client_tts_text() 负责，本层不关心网关 payload 细节。 */
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (APP_DEBUG_SPEAKER_LLM_BRIDGE) {
        ESP_LOGI(TAG, "speaker speak_text request len=%u", (unsigned int)strlen(text));
    }
    speaker_llm_bridge_begin_round();
    return llm_client_tts_text(text);
}

bool speaker_llm_bridge_is_enabled(void)
{
    return llm_client_is_tts_enabled();
}
