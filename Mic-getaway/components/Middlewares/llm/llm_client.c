#include "llm_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "llm_gateway_http.h"
#include "llm_gateway_protocol.h"
#include "llm_gateway_tts_ws.h"
#include "llm_gateway_ws.h"

static const char *TAG = "llm_client";

enum {
    LLM_CLIENT_ASR_FINAL_BIT = BIT0,
    LLM_CLIENT_ASR_DISCONNECTED_BIT = BIT1,
    LLM_CLIENT_ASR_ERROR_BIT = BIT2,
    LLM_CLIENT_CHAT_HTTP_403_ERROR_CODE = 264,
    LLM_CLIENT_ASR_FINISH_BITS = LLM_CLIENT_ASR_FINAL_BIT |
                                 LLM_CLIENT_ASR_DISCONNECTED_BIT |
                                 LLM_CLIENT_ASR_ERROR_BIT,
};

#define LLM_CLIENT_CHAT_HTTP_403_MESSAGE "ASR text to Chat failed: HTTP 403, check gateway Chat permission/model binding"
#define LLM_CLIENT_ASR_PCM_LOG_INTERVAL_PACKETS 100U
#define LLM_CLIENT_WEATHER_UNAVAILABLE_REPLY "我现在还没有接入实时天气接口。"

typedef struct {
    bool initialized;
    llm_client_state_t state;
    llm_client_event_cb_t event_cb;
    void *user_ctx;
    llm_client_tts_audio_sink_t tts_audio_sink;
    void *tts_audio_sink_ctx;
    llm_client_tts_done_sink_t tts_done_sink;
    void *tts_done_sink_ctx;
    const char *system_prompt;
    char asr_final_text[LLM_GATEWAY_ASR_TEXT_MAX_BYTES];
    char asr_last_partial_text[LLM_GATEWAY_ASR_TEXT_MAX_BYTES];
    char pending_chat_text[LLM_GATEWAY_ASR_TEXT_MAX_BYTES];
    char llm_final_text[LLM_GATEWAY_LLM_RESPONSE_MAX_BYTES];
    TaskHandle_t asr_final_task_handle;
    EventGroupHandle_t asr_event_group;
    TickType_t asr_last_event_tick;
    uint32_t asr_event_count;
    uint32_t pcm_packet_count;
    bool ws_started;
    bool asr_final_received;
    bool asr_partial_received;
} llm_client_context_t;

static llm_client_context_t s_client;

static void llm_client_asr_final_task(void *arg);

static bool llm_client_is_weather_query(const char *text)
{
    return text != NULL &&
           (strstr(text, "天气") != NULL ||
            strstr(text, "气温") != NULL ||
            strstr(text, "温度") != NULL ||
            strstr(text, "下雨") != NULL ||
            strstr(text, "降雨") != NULL);
}

static bool llm_client_make_weather_unavailable_reply(const char *user_text,
                                                      char *out_reply,
                                                      size_t out_reply_size)
{
    if (!llm_client_is_weather_query(user_text) ||
        out_reply == NULL || out_reply_size == 0) {
        return false;
    }

    strlcpy(out_reply, LLM_CLIENT_WEATHER_UNAVAILABLE_REPLY, out_reply_size);
    return true;
}

static const char *llm_client_get_model_name(llm_client_capability_t cap)
{
    switch (cap) {
    case LLM_CLIENT_CAP_ASR:
        return LLM_GATEWAY_ASR_MODEL;
    case LLM_CLIENT_CAP_TEXT:
        return LLM_CHAT_MODEL;
    case LLM_CLIENT_CAP_TTS:
        return LLM_GATEWAY_TTS_MODEL;
    default:
        return NULL;
    }
}

const char *llm_client_state_name(llm_client_state_t state)
{
    switch (state) {
    case LLM_CLIENT_STATE_IDLE:
        return "IDLE";
    case LLM_CLIENT_STATE_ASR_CONNECTING:
        return "ASR_CONNECTING";
    case LLM_CLIENT_STATE_ASR_STREAMING:
        return "ASR_STREAMING";
    case LLM_CLIENT_STATE_ASR_FINISHING:
        return "ASR_FINISHING";
    case LLM_CLIENT_STATE_CHAT_REQUESTING:
        return "CHAT_REQUESTING";
    case LLM_CLIENT_STATE_TTS_REQUESTING:
        return "TTS_REQUESTING";
    default:
        return "UNKNOWN";
    }
}

static void llm_client_set_state(llm_client_state_t state)
{
    if (APP_DEBUG_LLM_CLIENT && s_client.state != state) {
        ESP_LOGI(TAG, "state %s -> %s", llm_client_state_name(s_client.state), llm_client_state_name(state));
    }
    s_client.state = state;
}

static void llm_client_emit(llm_client_event_type_t type,
                            const char *text,
                            const uint8_t *audio,
                            size_t audio_len,
                            int code,
                            const char *message)
{
    if (s_client.event_cb == NULL) {
        return;
    }

    llm_client_event_t event = {
        .type = type,
        .text = text,
        .audio = audio,
        .audio_len = audio_len,
        .code = code,
        .message = message,
    };
    s_client.event_cb(&event, s_client.user_ctx);
}

static void llm_client_signal_asr_finish(EventBits_t bits)
{
    if (s_client.asr_event_group != NULL) {
        xEventGroupSetBits(s_client.asr_event_group, bits);
    }
}

static void llm_client_clear_asr_finish_bits(void)
{
    if (s_client.asr_event_group != NULL) {
        xEventGroupClearBits(s_client.asr_event_group, LLM_CLIENT_ASR_FINISH_BITS);
    }
}

static void llm_client_mark_asr_event(void)
{
    s_client.asr_event_count++;
    s_client.asr_last_event_tick = xTaskGetTickCount();
}

static void llm_client_reset_voice_session(void)
{
    if (s_client.ws_started) {
        (void)llm_gateway_ws_stop();
        s_client.ws_started = false;
    }
    llm_client_clear_asr_finish_bits();
    s_client.asr_final_text[0] = '\0';
    s_client.asr_last_partial_text[0] = '\0';
    s_client.pending_chat_text[0] = '\0';
    s_client.llm_final_text[0] = '\0';
    s_client.asr_event_count = 0;
    s_client.pcm_packet_count = 0;
    s_client.asr_final_received = false;
    s_client.asr_partial_received = false;
    llm_client_set_state(LLM_CLIENT_STATE_IDLE);
}

static esp_err_t llm_client_start_asr_final_task(const char *chat_text)
{
    if (s_client.asr_final_task_handle != NULL) {
        ESP_LOGW(TAG, "ASR final task already running");
        return ESP_OK;
    }

    if (chat_text != NULL && chat_text[0] != '\0') {
        strlcpy(s_client.pending_chat_text, chat_text, sizeof(s_client.pending_chat_text));
    } else {
        s_client.pending_chat_text[0] = '\0';
    }

    BaseType_t created = xTaskCreate(llm_client_asr_final_task,
                                     "llm_asr_final",
                                     LLM_GATEWAY_CHAT_TASK_STACK_SIZE,
                                     NULL,
                                     LLM_GATEWAY_CHAT_TASK_PRIORITY,
                                     &s_client.asr_final_task_handle);
    if (created != pdPASS) {
        s_client.asr_final_task_handle = NULL;
        s_client.pending_chat_text[0] = '\0';
        ESP_LOGE(TAG, "create ASR final task failed");
        if (s_client.ws_started) {
            (void)llm_gateway_ws_stop();
            s_client.ws_started = false;
        }
        llm_client_set_state(LLM_CLIENT_STATE_IDLE);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t llm_client_run_text_query(const char *system_prompt,
                                           const char *user_text,
                                           char *out_reply,
                                           size_t out_reply_size,
                                           bool require_idle)
{
    if (!s_client.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *model = llm_client_get_model_name(LLM_CLIENT_CAP_TEXT);
    if (model == NULL || model[0] == '\0' ||
        user_text == NULL || user_text[0] == '\0' ||
        out_reply == NULL || out_reply_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!LLM_GATEWAY_ENABLE_TEXT) {
        ESP_LOGW(TAG, "gateway chat reserved but not enabled");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (require_idle && s_client.state != LLM_CLIENT_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    if (llm_client_make_weather_unavailable_reply(user_text, out_reply, out_reply_size)) {
        ESP_LOGI(TAG, "LLM request");
        ESP_LOGI(TAG, "LLM final: %s", out_reply);
        llm_client_emit(LLM_CLIENT_EVENT_LLM_FINAL_TEXT,
                        out_reply,
                        NULL,
                        0,
                        0,
                        NULL);
        return ESP_OK;
    }

    llm_client_set_state(LLM_CLIENT_STATE_CHAT_REQUESTING);
    ESP_LOGI(TAG, "LLM request");
    out_reply[0] = '\0';
    int http_status_code = 0;
    esp_err_t ret = llm_gateway_http_chat_completion(model,
                                                     system_prompt != NULL ? system_prompt : s_client.system_prompt,
                                                     user_text,
                                                     out_reply,
                                                     out_reply_size,
                                                     &http_status_code);
    if (ret != ESP_OK) {
        int event_code = ret;
        const char *event_message = "LLM HTTP request failed";
        if (http_status_code == 403) {
            event_code = LLM_CLIENT_CHAT_HTTP_403_ERROR_CODE;
            event_message = LLM_CLIENT_CHAT_HTTP_403_MESSAGE;
            ESP_LOGW(TAG,
                     "Chat HTTP 403: state will return IDLE, model=%s",
                     model);
        }
        llm_client_emit(LLM_CLIENT_EVENT_ERROR, NULL, NULL, 0, event_code, event_message);
        llm_client_set_state(LLM_CLIENT_STATE_IDLE);
        return ret;
    }

    if (out_reply[0] == '\0') {
        llm_client_set_state(LLM_CLIENT_STATE_IDLE);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "LLM final: %s", out_reply);
    llm_client_emit(LLM_CLIENT_EVENT_LLM_FINAL_TEXT,
                    out_reply,
                    NULL,
                    0,
                    0,
                    NULL);

    llm_client_set_state(LLM_CLIENT_STATE_IDLE);
    return ESP_OK;
}

esp_err_t llm_client_text_query(const char *system_prompt,
                                const char *user_text,
                                char *out_reply,
                                size_t out_reply_size)
{
    return llm_client_run_text_query(system_prompt,
                                     user_text,
                                     out_reply,
                                     out_reply_size,
                                     true);
}

esp_err_t llm_client_text_request(const char *user_text)
{
    char reply[LLM_GATEWAY_LLM_RESPONSE_MAX_BYTES] = {0};
    return llm_client_text_query(s_client.system_prompt, user_text, reply, sizeof(reply));
}

static void llm_client_asr_final_task(void *arg)
{
    (void)arg;

    char chat_text[LLM_GATEWAY_ASR_TEXT_MAX_BYTES] = {0};
    strlcpy(chat_text, s_client.pending_chat_text, sizeof(chat_text));

    if (chat_text[0] == '\0') {
        if (APP_DEBUG_LLM_CLIENT) {
            ESP_LOGI(TAG, "ASR final text empty, skip Chat");
        }
    } else if (!LLM_GATEWAY_ENABLE_ASR_TO_CHAT) {
        if (APP_DEBUG_LLM_CLIENT) {
            ESP_LOGI(TAG, "ASR -> Chat disabled, keep transcript only");
        }
    } else {
        const char *model = llm_client_get_model_name(LLM_CLIENT_CAP_TEXT);
        if (!LLM_GATEWAY_ENABLE_TEXT || model == NULL || model[0] == '\0') {
            ESP_LOGW(TAG, "gateway chat reserved but not enabled");
        } else {
            ESP_LOGI(TAG, "LLM request");
            s_client.llm_final_text[0] = '\0';
            if (llm_client_make_weather_unavailable_reply(chat_text,
                                                          s_client.llm_final_text,
                                                          sizeof(s_client.llm_final_text))) {
                ESP_LOGI(TAG, "LLM final: %s", s_client.llm_final_text);
                llm_client_emit(LLM_CLIENT_EVENT_LLM_FINAL_TEXT,
                                s_client.llm_final_text,
                                NULL,
                                0,
                                0,
                                NULL);
            } else {
                llm_client_set_state(LLM_CLIENT_STATE_CHAT_REQUESTING);
                int http_status_code = 0;
                esp_err_t ret = llm_gateway_http_chat_completion(model,
                                                                 s_client.system_prompt,
                                                                 chat_text,
                                                                 s_client.llm_final_text,
                                                                 sizeof(s_client.llm_final_text),
                                                                 &http_status_code);
                if (ret != ESP_OK) {
                    int event_code = ret;
                    const char *event_message = "ASR text to Chat failed";
                    if (http_status_code == 403) {
                        event_code = LLM_CLIENT_CHAT_HTTP_403_ERROR_CODE;
                        event_message = LLM_CLIENT_CHAT_HTTP_403_MESSAGE;
                        ESP_LOGW(TAG,
                                 "ASR -> Chat failed: HTTP 403, check gateway Chat permission/model binding, model=%s",
                                 model);
                    } else {
                        ESP_LOGW(TAG,
                                 "ASR -> Chat failed: ret=%s http_status=%d",
                                 esp_err_to_name(ret),
                                 http_status_code);
                    }
                    llm_client_emit(LLM_CLIENT_EVENT_ERROR,
                                    NULL,
                                    NULL,
                                    0,
                                    event_code,
                                    event_message);
                } else if (s_client.llm_final_text[0] == '\0') {
                    ESP_LOGW(TAG, "ASR -> Chat completed without reply");
                } else {
                    ESP_LOGI(TAG, "LLM final: %s", s_client.llm_final_text);
                    llm_client_emit(LLM_CLIENT_EVENT_LLM_FINAL_TEXT,
                                    s_client.llm_final_text,
                                    NULL,
                                    0,
                                    0,
                                    NULL);
                }
            }
        }
    }

    if (s_client.state == LLM_CLIENT_STATE_CHAT_REQUESTING) {
        llm_client_set_state(LLM_CLIENT_STATE_IDLE);
    }
    s_client.pending_chat_text[0] = '\0';
    s_client.asr_final_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t llm_client_handle_asr_final(const char *text)
{
    s_client.asr_final_received = true;

    if (text == NULL || text[0] == '\0') {
        ESP_LOGW(TAG, "ASR completed without transcript");
        s_client.asr_final_text[0] = '\0';
        llm_client_signal_asr_finish(LLM_CLIENT_ASR_FINAL_BIT);
        return ESP_ERR_NOT_FOUND;
    }

    strlcpy(s_client.asr_final_text, text, sizeof(s_client.asr_final_text));
    ESP_LOGI(TAG, "ASR final: %s", s_client.asr_final_text);
    llm_client_emit(LLM_CLIENT_EVENT_ASR_FINAL_TEXT,
                    s_client.asr_final_text,
                    NULL,
                    0,
                    0,
                    NULL);

    llm_client_signal_asr_finish(LLM_CLIENT_ASR_FINAL_BIT);
    return ESP_OK;
}

static esp_err_t llm_client_start_asr_chat_if_needed(void)
{
    if (!s_client.asr_final_received || s_client.asr_final_text[0] == '\0') {
        if (APP_DEBUG_LLM_CLIENT) {
            ESP_LOGI(TAG, "ASR final text empty, skip Chat");
        }
        return ESP_OK;
    }
    if (!LLM_GATEWAY_ENABLE_ASR_TO_CHAT) {
        if (APP_DEBUG_LLM_CLIENT) {
            ESP_LOGI(TAG, "ASR -> Chat disabled, keep transcript only");
        }
        return ESP_OK;
    }

    return llm_client_start_asr_final_task(s_client.asr_final_text);
}

static EventBits_t llm_client_wait_asr_finish_events(void)
{
    if (s_client.asr_event_group == NULL) {
        return 0;
    }

    const TickType_t start_tick = xTaskGetTickCount();
    const TickType_t deadline_tick = start_tick + pdMS_TO_TICKS(LLM_GATEWAY_WS_FINAL_TIMEOUT_MS);
    const TickType_t quiet_ticks = pdMS_TO_TICKS(LLM_GATEWAY_WS_DRAIN_QUIET_MS);
    EventBits_t collected_bits = 0;
    bool final_seen = false;

    if (APP_DEBUG_LLM_CLIENT) {
        ESP_LOGI(TAG,
                 "ASR finish receive loop start: timeout_ms=%d drain_quiet_ms=%d",
                 LLM_GATEWAY_WS_FINAL_TIMEOUT_MS,
                 LLM_GATEWAY_WS_DRAIN_QUIET_MS);
    }

    while ((int32_t)(deadline_tick - xTaskGetTickCount()) > 0) {
        TickType_t now = xTaskGetTickCount();
        TickType_t wait_ticks = pdMS_TO_TICKS(100);
        TickType_t remaining_ticks = deadline_tick - now;
        if (remaining_ticks < wait_ticks) {
            wait_ticks = remaining_ticks;
        }

        EventBits_t bits = xEventGroupWaitBits(s_client.asr_event_group,
                                               LLM_CLIENT_ASR_FINISH_BITS,
                                               pdTRUE,
                                               pdFALSE,
                                               wait_ticks);
        collected_bits |= bits;
        if (bits & LLM_CLIENT_ASR_FINAL_BIT) {
            final_seen = true;
        }
        if (bits & (LLM_CLIENT_ASR_ERROR_BIT | LLM_CLIENT_ASR_DISCONNECTED_BIT)) {
            break;
        }

        now = xTaskGetTickCount();
        if (final_seen && (now - s_client.asr_last_event_tick) >= quiet_ticks) {
            if (APP_DEBUG_LLM_CLIENT) {
                ESP_LOGI(TAG, "ASR finish receive loop drained after final");
            }
            break;
        }
    }

    return collected_bits;
}

static void llm_client_ws_event_cb(const llm_gateway_ws_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case LLM_GATEWAY_WS_EVENT_CONNECTED:
        llm_client_mark_asr_event();
        llm_client_emit(LLM_CLIENT_EVENT_CONNECTED, NULL, NULL, 0, 0, NULL);
        break;
    case LLM_GATEWAY_WS_EVENT_DISCONNECTED:
        llm_client_mark_asr_event();
        llm_client_emit(LLM_CLIENT_EVENT_DISCONNECTED, NULL, NULL, 0, 0, NULL);
        if (s_client.state == LLM_CLIENT_STATE_ASR_FINISHING) {
            llm_client_signal_asr_finish(LLM_CLIENT_ASR_DISCONNECTED_BIT);
        }
        break;
    case LLM_GATEWAY_WS_EVENT_ASR_PARTIAL:
        llm_client_mark_asr_event();
        if (event->text != NULL && event->text[0] != '\0') {
            strlcpy(s_client.asr_last_partial_text, event->text, sizeof(s_client.asr_last_partial_text));
            s_client.asr_partial_received = true;
        }
        llm_client_emit(LLM_CLIENT_EVENT_ASR_PARTIAL_TEXT,
                        event->text,
                        NULL,
                        0,
                        event->code,
                        event->message);
        break;
    case LLM_GATEWAY_WS_EVENT_ASR_FINAL:
        llm_client_mark_asr_event();
        if (s_client.state == LLM_CLIENT_STATE_ASR_STREAMING ||
            s_client.state == LLM_CLIENT_STATE_ASR_FINISHING) {
            (void)llm_client_handle_asr_final(event->text);
        }
        break;
    case LLM_GATEWAY_WS_EVENT_ERROR:
    default:
        llm_client_mark_asr_event();
        llm_client_emit(LLM_CLIENT_EVENT_ERROR,
                        NULL,
                        NULL,
                        0,
                        event->code,
                        event->message != NULL ? event->message : "ASR WS error");
        if (s_client.state == LLM_CLIENT_STATE_ASR_FINISHING) {
            llm_client_signal_asr_finish(LLM_CLIENT_ASR_ERROR_BIT);
        } else {
            llm_client_reset_voice_session();
        }
        break;
    }
}

static void llm_client_tts_ws_event_cb(const llm_gateway_tts_ws_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case LLM_GATEWAY_TTS_WS_EVENT_CONNECTED:
        if (APP_DEBUG_LLM_CLIENT) {
            ESP_LOGI(TAG, "TTS connected");
        }
        break;
    case LLM_GATEWAY_TTS_WS_EVENT_SESSION_UPDATED:
        if (APP_DEBUG_LLM_CLIENT) {
            ESP_LOGI(TAG, "TTS session updated");
        }
        break;
    case LLM_GATEWAY_TTS_WS_EVENT_AUDIO_DELTA:
        if (event->audio != NULL && event->audio_len > 0) {
            if (APP_DEBUG_LLM_GATEWAY_AUDIO) {
                ESP_LOGI(TAG,
                         "TTS AUDIO chunk bytes=%u owned=%d",
                         (unsigned int)event->audio_len,
                         event->audio_owned ? 1 : 0);
            }
            llm_client_emit(LLM_CLIENT_EVENT_TTS_AUDIO,
                            NULL,
                            event->audio,
                            event->audio_len,
                            0,
                            NULL);

            bool ownership_transferred = false;
            if (s_client.tts_audio_sink != NULL) {
                esp_err_t sink_ret = s_client.tts_audio_sink(event->audio,
                                                             event->audio_len,
                                                             LLM_TTS_SAMPLE_RATE,
                                                             event->audio_owned,
                                                             s_client.tts_audio_sink_ctx);
                if (sink_ret != ESP_OK) {
                    ESP_LOGW(TAG, "TTS audio sink failed: %s", esp_err_to_name(sink_ret));
                } else if (event->audio_owned) {
                    ownership_transferred = true;
                }
            }
            if (event->audio_owned && !ownership_transferred) {
                free(event->audio);
            }
        }
        break;
    case LLM_GATEWAY_TTS_WS_EVENT_AUDIO_DONE:
        if (APP_DEBUG_LLM_CLIENT) {
            ESP_LOGI(TAG, "TTS audio done");
        }
        if (s_client.tts_done_sink != NULL) {
            esp_err_t done_ret = s_client.tts_done_sink(s_client.tts_done_sink_ctx);
            if (done_ret != ESP_OK) {
                ESP_LOGW(TAG, "TTS done sink failed: %s", esp_err_to_name(done_ret));
            }
        }
        llm_client_emit(LLM_CLIENT_EVENT_TTS_DONE, NULL, NULL, 0, 0, NULL);
        break;
    case LLM_GATEWAY_TTS_WS_EVENT_DISCONNECTED:
        if (APP_DEBUG_LLM_CLIENT) {
            ESP_LOGI(TAG, "TTS disconnected");
        }
        break;
    case LLM_GATEWAY_TTS_WS_EVENT_ERROR:
    default:
        llm_client_emit(LLM_CLIENT_EVENT_ERROR,
                        NULL,
                        NULL,
                        0,
                        event->code,
                        event->message != NULL ? event->message : "TTS WebSocket error");
        break;
    }
}

esp_err_t llm_client_init(const llm_client_config_t *config)
{
    if (s_client.initialized) {
        return ESP_OK;
    }
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_client, 0, sizeof(s_client));
    s_client.asr_event_group = xEventGroupCreate();
    if (s_client.asr_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_client.state = LLM_CLIENT_STATE_IDLE;
    s_client.system_prompt = (config->system_prompt != NULL) ?
        config->system_prompt : LLM_GATEWAY_SYSTEM_PROMPT;
    s_client.event_cb = config->event_cb;
    s_client.user_ctx = config->user_ctx;
    s_client.initialized = true;

    char key_summary[48] = {0};
    llm_gateway_protocol_make_key_summary(key_summary, sizeof(key_summary));
    if (APP_DEBUG_LLM_CLIENT) {
        ESP_LOGI(TAG, "llm_client initialized, key %s", key_summary);
    }
    if (llm_gateway_protocol_config_has_placeholders()) {
        ESP_LOGW(TAG, "llm_config.h still has placeholder gateway values");
    }
    return ESP_OK;
}

esp_err_t llm_client_deinit(void)
{
    if (!s_client.initialized) {
        return ESP_OK;
    }
    llm_client_reset_voice_session();
    if (s_client.asr_event_group != NULL) {
        vEventGroupDelete(s_client.asr_event_group);
        s_client.asr_event_group = NULL;
    }
    memset(&s_client, 0, sizeof(s_client));
    return ESP_OK;
}

esp_err_t llm_client_start_asr_session(void)
{
    if (!s_client.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_client.state != LLM_CLIENT_STATE_IDLE) {
        if (s_client.state == LLM_CLIENT_STATE_ASR_FINISHING) {
            if (APP_DEBUG_LLM_CLIENT) {
                ESP_LOGI(TAG, "ASR busy finishing previous session");
            }
        } else {
            ESP_LOGW(TAG, "ASR start rejected: state=%s", llm_client_state_name(s_client.state));
        }
        return ESP_ERR_INVALID_STATE;
    }
    const char *asr_model = llm_client_get_model_name(LLM_CLIENT_CAP_ASR);
    if (!LLM_GATEWAY_ENABLE_ASR || asr_model == NULL || asr_model[0] == '\0') {
        return ESP_ERR_NOT_SUPPORTED;
    }

    llm_client_clear_asr_finish_bits();
    s_client.asr_final_received = false;
    s_client.asr_partial_received = false;
    s_client.asr_final_text[0] = '\0';
    s_client.asr_last_partial_text[0] = '\0';
    s_client.asr_event_count = 0;
    s_client.pcm_packet_count = 0;
    s_client.asr_last_event_tick = xTaskGetTickCount();
    llm_client_set_state(LLM_CLIENT_STATE_ASR_CONNECTING);
    llm_gateway_ws_config_t ws_config = {
        .asr_model = asr_model,
        .event_cb = llm_client_ws_event_cb,
        .user_ctx = NULL,
    };
    esp_err_t ret = llm_gateway_ws_start(&ws_config);
    if (ret != ESP_OK) {
        llm_client_emit(LLM_CLIENT_EVENT_ERROR, NULL, NULL, 0, ret, "ASR WebSocket start failed");
        llm_client_reset_voice_session();
        return ret;
    } else {
        s_client.ws_started = true;
    }

    llm_client_set_state(LLM_CLIENT_STATE_ASR_STREAMING);
    return ESP_OK;
}

esp_err_t llm_client_send_asr_pcm(const int16_t *pcm, size_t samples)
{
    if (!s_client.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pcm == NULL || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_client.state != LLM_CLIENT_STATE_ASR_CONNECTING &&
        s_client.state != LLM_CLIENT_STATE_ASR_STREAMING) {
        ESP_LOGW(TAG,
                 "ASR PCM drop: pcm_bytes=%u sample_rate=%d channels=%d llm_client_state=%s",
                 (unsigned int)(samples * sizeof(int16_t)),
                 LLM_GATEWAY_AUDIO_SAMPLE_RATE,
                 LLM_GATEWAY_AUDIO_CHANNELS,
                 llm_client_state_name(s_client.state));
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_client.ws_started) {
        ESP_LOGW(TAG,
                 "ASR PCM drop: websocket not started, pcm_bytes=%u llm_client_state=%s",
                 (unsigned int)(samples * sizeof(int16_t)),
                 llm_client_state_name(s_client.state));
        return ESP_OK;
    }

    s_client.pcm_packet_count++;
    esp_err_t ret = llm_gateway_ws_send_pcm16(pcm, samples, LLM_GATEWAY_AUDIO_SAMPLE_RATE);
    if (APP_DEBUG_LLM_GATEWAY_AUDIO &&
        ((s_client.pcm_packet_count % LLM_CLIENT_ASR_PCM_LOG_INTERVAL_PACKETS) == 0U || ret != ESP_OK)) {
        ESP_LOGI(TAG,
                 "ASR PCM send: pcm_packet_count=%u pcm_bytes=%u sample_rate=%d channels=%d send_result=%s llm_client_state=%s",
                 (unsigned int)s_client.pcm_packet_count,
                 (unsigned int)(samples * sizeof(int16_t)),
                 LLM_GATEWAY_AUDIO_SAMPLE_RATE,
                 LLM_GATEWAY_AUDIO_CHANNELS,
                 esp_err_to_name(ret),
                 llm_client_state_name(s_client.state));
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ASR WS PCM send failed: %s", esp_err_to_name(ret));
        (void)llm_gateway_ws_stop();
        s_client.ws_started = false;
        llm_client_reset_voice_session();
    }
    return ret;
}

esp_err_t llm_client_finish_asr_session(void)
{
    if (!s_client.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_client.state != LLM_CLIENT_STATE_ASR_STREAMING &&
        s_client.state != LLM_CLIENT_STATE_ASR_FINISHING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_client.state == LLM_CLIENT_STATE_ASR_FINISHING) {
        return ESP_OK;
    }

    llm_client_clear_asr_finish_bits();
    s_client.asr_final_received = false;
    s_client.asr_final_text[0] = '\0';
    llm_client_set_state(LLM_CLIENT_STATE_ASR_FINISHING);
    esp_err_t finish_ret = ESP_OK;
    if (s_client.ws_started) {
        finish_ret = llm_gateway_ws_finish();
        if (APP_DEBUG_LLM_CLIENT) {
            ESP_LOGI(TAG, "ASR finalize send result: %s", esp_err_to_name(finish_ret));
        }
        if (finish_ret != ESP_OK) {
            ESP_LOGW(TAG, "ASR WS finish failed: %s", esp_err_to_name(finish_ret));
        }
    } else {
        ESP_LOGW(TAG, "ASR finalize skipped: websocket not started");
    }

    EventBits_t bits = 0;
    if (finish_ret == ESP_OK && s_client.asr_event_group != NULL) {
        s_client.asr_last_event_tick = xTaskGetTickCount();
        bits = llm_client_wait_asr_finish_events();
    }

    if (bits & LLM_CLIENT_ASR_FINAL_BIT) {
        if (APP_DEBUG_LLM_CLIENT) {
            ESP_LOGI(TAG, "ASR final transcript receive result: received");
        }
    } else if (bits & LLM_CLIENT_ASR_ERROR_BIT) {
        ESP_LOGW(TAG, "ASR final transcript receive result: websocket error");
    } else if (bits & LLM_CLIENT_ASR_DISCONNECTED_BIT) {
        ESP_LOGW(TAG, "ASR final transcript receive result: disconnected before final");
    } else if (finish_ret == ESP_OK) {
        ESP_LOGW(TAG,
                 "ASR final transcript receive result: timeout after %d ms",
                 LLM_GATEWAY_WS_FINAL_TIMEOUT_MS);
        if (s_client.asr_partial_received && s_client.asr_last_partial_text[0] != '\0') {
            strlcpy(s_client.asr_final_text,
                    s_client.asr_last_partial_text,
                    sizeof(s_client.asr_final_text));
            s_client.asr_final_received = true;
            ESP_LOGW(TAG, "ASR timeout fallback to latest partial transcript: %s", s_client.asr_final_text);
            llm_client_emit(LLM_CLIENT_EVENT_ASR_FINAL_TEXT,
                            s_client.asr_final_text,
                            NULL,
                            0,
                            0,
                            NULL);
        }
    }

    esp_err_t close_ret = ESP_OK;
    if (s_client.ws_started) {
        close_ret = llm_gateway_ws_stop();
        s_client.ws_started = false;
        if (APP_DEBUG_LLM_GATEWAY_WS) {
            ESP_LOGD(TAG,
                     "ASR WebSocket close result: %s",
                     esp_err_to_name(close_ret));
        }
    }

    llm_client_set_state(LLM_CLIENT_STATE_IDLE);

    if (s_client.asr_final_received && s_client.asr_final_text[0] != '\0') {
        esp_err_t chat_ret = llm_client_start_asr_chat_if_needed();
        if (chat_ret != ESP_OK) {
            ESP_LOGW(TAG, "start ASR -> Chat task failed: %s", esp_err_to_name(chat_ret));
            llm_client_emit(LLM_CLIENT_EVENT_ERROR,
                            NULL,
                            NULL,
                            0,
                            chat_ret,
                            "start ASR text to Chat failed");
        }
    }

    return ESP_OK;
}

esp_err_t llm_client_cancel_asr_session(void)
{
    if (!s_client.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    llm_client_reset_voice_session();
    llm_client_emit(LLM_CLIENT_EVENT_DISCONNECTED, NULL, NULL, 0, 0, "session stop");
    return ESP_OK;
}

bool llm_client_is_voice_session_active(void)
{
    return s_client.initialized &&
           (s_client.state == LLM_CLIENT_STATE_ASR_CONNECTING ||
            s_client.state == LLM_CLIENT_STATE_ASR_STREAMING ||
            s_client.state == LLM_CLIENT_STATE_ASR_FINISHING);
}

llm_client_state_t llm_client_get_state(void)
{
    return s_client.state;
}

esp_err_t llm_client_start_voice_session(void)
{
    return llm_client_start_asr_session();
}

esp_err_t llm_client_send_audio_pcm16(const int16_t *pcm, size_t samples, uint32_t sample_rate_hz)
{
    if (sample_rate_hz != LLM_GATEWAY_AUDIO_SAMPLE_RATE) {
        return ESP_ERR_INVALID_ARG;
    }
    return llm_client_send_asr_pcm(pcm, samples);
}

esp_err_t llm_client_finish_voice_session(void)
{
    return llm_client_finish_asr_session();
}

esp_err_t llm_client_stop_voice_session(void)
{
    return llm_client_cancel_asr_session();
}

esp_err_t llm_client_chat_json_context(const char *json_context)
{
    return llm_client_text_request(json_context);
}

esp_err_t llm_client_json_context_request(const char *source, const char *json_context)
{
    if (source == NULL || source[0] == '\0' || json_context == NULL || json_context[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char context[LLM_GATEWAY_SENSOR_CONTEXT_MAX_BYTES] = {0};
    int written = snprintf(context,
                           sizeof(context),
                           "{\"source\":\"%s\",\"data\":%s}",
                           source,
                           json_context);
    if (written < 0 || (size_t)written >= sizeof(context)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (APP_DEBUG_LLM_CLIENT) {
        ESP_LOGI(TAG, "JSON context request: source=%s len=%u", source, (unsigned int)strlen(json_context));
    }
    return llm_client_text_request(context);
}

esp_err_t llm_client_send_bme690_json(const char *json)
{
    return llm_client_json_context_request("bme690", json);
}

esp_err_t llm_client_send_csi_json(const char *json)
{
    return llm_client_json_context_request("csi", json);
}

esp_err_t llm_client_send_system_status_json(const char *json)
{
    return llm_client_json_context_request("system_status", json);
}

esp_err_t llm_client_tts_text(const char *text)
{
    if (!s_client.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const char *model = llm_client_get_model_name(LLM_CLIENT_CAP_TTS);
    if (model == NULL || model[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!LLM_GATEWAY_ENABLE_TTS) {
        ESP_LOGW(TAG, "gateway TTS reserved but not enabled");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_client.state != LLM_CLIENT_STATE_IDLE) {
        ESP_LOGW(TAG, "TTS request rejected: state=%s", llm_client_state_name(s_client.state));
        return ESP_ERR_INVALID_STATE;
    }

    llm_client_set_state(LLM_CLIENT_STATE_TTS_REQUESTING);
    if (APP_DEBUG_LLM_CLIENT) {
        ESP_LOGI(TAG, "TTS request start: model=%s text_len=%u", model, (unsigned int)strlen(text));
    }
    llm_gateway_tts_ws_config_t tts_config = {
        .tts_model = model,
        .event_cb = llm_client_tts_ws_event_cb,
        .user_ctx = NULL,
    };
    esp_err_t ret = llm_gateway_tts_ws_synthesize(&tts_config, text);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TTS request failed: %s", esp_err_to_name(ret));
        llm_client_emit(LLM_CLIENT_EVENT_ERROR,
                        NULL,
                        NULL,
                        0,
                        ret,
                        "TTS request failed");
    } else {
        if (APP_DEBUG_LLM_CLIENT) {
            ESP_LOGI(TAG, "TTS request done");
        }
    }
    llm_client_set_state(LLM_CLIENT_STATE_IDLE);
    return ret;
}

bool llm_client_is_tts_enabled(void)
{
    return LLM_GATEWAY_ENABLE_TTS != 0;
}

void llm_client_set_tts_audio_sink(llm_client_tts_audio_sink_t sink, void *user_ctx)
{
    s_client.tts_audio_sink = sink;
    s_client.tts_audio_sink_ctx = user_ctx;
}

void llm_client_set_tts_done_sink(llm_client_tts_done_sink_t sink, void *user_ctx)
{
    s_client.tts_done_sink = sink;
    s_client.tts_done_sink_ctx = user_ctx;
}
