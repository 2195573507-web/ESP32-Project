#include "llm_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "llm_gateway_http.h"
#include "llm_gateway_protocol.h"
#include "llm_gateway_ws.h"
#include "llm_router.h"

static const char *TAG = "llm_client";

typedef enum {
    LLM_CLIENT_STATE_IDLE = 0,
    LLM_CLIENT_STATE_ASR_CONNECTING,
    LLM_CLIENT_STATE_ASR_STREAMING,
    LLM_CLIENT_STATE_ASR_WAIT_FINAL,
    LLM_CLIENT_STATE_LLM_REQUESTING,
    LLM_CLIENT_STATE_ROUTING,
    LLM_CLIENT_STATE_ERROR,
} llm_client_state_t;

typedef struct {
    bool initialized;
    llm_client_state_t state;
    llm_client_event_cb_t event_cb;
    void *user_ctx;
    const char *asr_model;
    const char *llm_model;
    const char *tts_model;
    const char *system_prompt;
    char asr_final_text[LLM_GATEWAY_ASR_TEXT_MAX_BYTES];
    char llm_final_text[LLM_GATEWAY_LLM_RESPONSE_MAX_BYTES];
    int16_t *record_buffer;
    size_t record_sample_count;
    size_t record_capacity_samples;
    bool record_overflowed;
    bool ws_started;
} llm_client_context_t;

static llm_client_context_t s_client;

static const char *llm_client_state_name(llm_client_state_t state)
{
    switch (state) {
    case LLM_CLIENT_STATE_IDLE:
        return "IDLE";
    case LLM_CLIENT_STATE_ASR_CONNECTING:
        return "ASR_CONNECTING";
    case LLM_CLIENT_STATE_ASR_STREAMING:
        return "ASR_STREAMING";
    case LLM_CLIENT_STATE_ASR_WAIT_FINAL:
        return "ASR_WAIT_FINAL";
    case LLM_CLIENT_STATE_LLM_REQUESTING:
        return "LLM_REQUESTING";
    case LLM_CLIENT_STATE_ROUTING:
        return "ROUTING";
    case LLM_CLIENT_STATE_ERROR:
        return "ERROR";
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

static void llm_client_free_record_buffer(void)
{
    free(s_client.record_buffer);
    s_client.record_buffer = NULL;
    s_client.record_sample_count = 0;
    s_client.record_capacity_samples = 0;
    s_client.record_overflowed = false;
}

static esp_err_t llm_client_alloc_record_buffer(void)
{
    llm_client_free_record_buffer();

    size_t capacity_samples = LLM_GATEWAY_RECORD_MAX_BYTES / sizeof(int16_t);
    size_t bytes = capacity_samples * sizeof(int16_t);
#if LLM_GATEWAY_USE_PSRAM_RECORD_BUFFER
    s_client.record_buffer = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_client.record_buffer == NULL) {
        ESP_LOGW(TAG, "PSRAM record buffer alloc failed, fallback to internal heap");
        s_client.record_buffer = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
#else
    s_client.record_buffer = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
#endif
    if (s_client.record_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_client.record_capacity_samples = capacity_samples;
    s_client.record_sample_count = 0;
    s_client.record_overflowed = false;
    return ESP_OK;
}

static void llm_client_cache_pcm(const int16_t *pcm, size_t samples)
{
    if (s_client.record_buffer == NULL || pcm == NULL || samples == 0) {
        return;
    }

    size_t available = s_client.record_capacity_samples - s_client.record_sample_count;
    size_t copy_samples = samples < available ? samples : available;
    if (copy_samples > 0) {
        memcpy(&s_client.record_buffer[s_client.record_sample_count],
               pcm,
               copy_samples * sizeof(int16_t));
        s_client.record_sample_count += copy_samples;
    }
    if (copy_samples < samples && !s_client.record_overflowed) {
        s_client.record_overflowed = true;
        ESP_LOGW(TAG,
                 "ASR fallback record buffer full: capacity_bytes=%u",
                 (unsigned int)(s_client.record_capacity_samples * sizeof(int16_t)));
    }
}

static void llm_client_reset_voice_session(void)
{
    if (s_client.ws_started) {
        (void)llm_gateway_ws_stop();
        s_client.ws_started = false;
    }
    s_client.asr_final_text[0] = '\0';
    s_client.llm_final_text[0] = '\0';
    llm_client_free_record_buffer();
    llm_client_set_state(LLM_CLIENT_STATE_IDLE);
}

static esp_err_t llm_client_route_final_text(void)
{
    if (s_client.llm_final_text[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    llm_client_set_state(LLM_CLIENT_STATE_ROUTING);
    llm_router_result_t result = {0};
    esp_err_t parse_ret = llm_router_parse_final_text(s_client.llm_final_text, &result);
    if (parse_ret != ESP_OK) {
        ESP_LOGW(TAG, "LLM final router parse failed, print raw text only");
        ESP_LOGI(TAG, "LLM FINAL RAW: %s", s_client.llm_final_text);
        return parse_ret;
    }

    esp_err_t handle_ret = llm_router_handle_result(&result);
    if (handle_ret == ESP_OK) {
        llm_client_emit(LLM_CLIENT_EVENT_COMMAND_RESULT,
                        result.reply[0] != '\0' ? result.reply : result.command,
                        NULL,
                        0,
                        0,
                        "router handled");
    } else {
        llm_client_emit(LLM_CLIENT_EVENT_ERROR,
                        NULL,
                        NULL,
                        0,
                        handle_ret,
                        "router handle failed");
    }
    llm_router_result_cleanup(&result);
    return handle_ret;
}

esp_err_t llm_client_chat_text_with_model(const char *llm_model, const char *user_text)
{
    if (!s_client.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (llm_model == NULL || llm_model[0] == '\0' ||
        user_text == NULL || user_text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    llm_client_set_state(LLM_CLIENT_STATE_LLM_REQUESTING);
    s_client.llm_final_text[0] = '\0';
    esp_err_t ret = llm_gateway_http_chat_completion(llm_model,
                                                     s_client.system_prompt,
                                                     user_text,
                                                     s_client.llm_final_text,
                                                     sizeof(s_client.llm_final_text));
    if (ret != ESP_OK) {
        llm_client_emit(LLM_CLIENT_EVENT_ERROR, NULL, NULL, 0, ret, "LLM HTTP request failed");
        llm_client_set_state(LLM_CLIENT_STATE_IDLE);
        return ret;
    }

    if (s_client.llm_final_text[0] == '\0') {
        llm_client_set_state(LLM_CLIENT_STATE_IDLE);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "LLM FINAL: %s", s_client.llm_final_text);
    llm_client_emit(LLM_CLIENT_EVENT_LLM_FINAL_TEXT,
                    s_client.llm_final_text,
                    NULL,
                    0,
                    0,
                    NULL);

    ret = llm_client_route_final_text();
    llm_client_set_state(LLM_CLIENT_STATE_IDLE);
    return ret == ESP_ERR_INVALID_RESPONSE ? ESP_OK : ret;
}

esp_err_t llm_client_chat_text(const char *user_text)
{
    return llm_client_chat_text_with_model(s_client.llm_model, user_text);
}

static esp_err_t llm_client_handle_asr_final(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        llm_client_set_state(LLM_CLIENT_STATE_IDLE);
        return ESP_ERR_NOT_FOUND;
    }

    strlcpy(s_client.asr_final_text, text, sizeof(s_client.asr_final_text));
    ESP_LOGI(TAG, "ASR FINAL: %s", s_client.asr_final_text);
    llm_client_emit(LLM_CLIENT_EVENT_ASR_FINAL_TEXT,
                    s_client.asr_final_text,
                    NULL,
                    0,
                    0,
                    NULL);

    esp_err_t ret = llm_client_chat_text_with_model(s_client.llm_model, s_client.asr_final_text);
    llm_client_free_record_buffer();
    return ret;
}

static void llm_client_ws_event_cb(const llm_gateway_ws_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case LLM_GATEWAY_WS_EVENT_CONNECTED:
        llm_client_emit(LLM_CLIENT_EVENT_CONNECTED, NULL, NULL, 0, 0, NULL);
        break;
    case LLM_GATEWAY_WS_EVENT_DISCONNECTED:
        llm_client_emit(LLM_CLIENT_EVENT_DISCONNECTED, NULL, NULL, 0, 0, NULL);
        break;
    case LLM_GATEWAY_WS_EVENT_ASR_PARTIAL:
        llm_client_emit(LLM_CLIENT_EVENT_ASR_PARTIAL_TEXT,
                        event->text,
                        NULL,
                        0,
                        event->code,
                        event->message);
        break;
    case LLM_GATEWAY_WS_EVENT_ASR_FINAL:
        if (s_client.state == LLM_CLIENT_STATE_ASR_STREAMING ||
            s_client.state == LLM_CLIENT_STATE_ASR_WAIT_FINAL) {
            (void)llm_client_handle_asr_final(event->text);
        }
        break;
    case LLM_GATEWAY_WS_EVENT_TTS_AUDIO:
        llm_client_emit(LLM_CLIENT_EVENT_TTS_AUDIO,
                        NULL,
                        NULL,
                        event->audio_len,
                        event->code,
                        "audio delta ignored, TTS disabled");
        break;
    case LLM_GATEWAY_WS_EVENT_ERROR:
    default:
        llm_client_emit(LLM_CLIENT_EVENT_ERROR,
                        NULL,
                        NULL,
                        0,
                        event->code,
                        event->message != NULL ? event->message : "ASR WS error");
        llm_client_set_state(LLM_CLIENT_STATE_ERROR);
        break;
    }
}

esp_err_t llm_client_init(const llm_client_config_t *config)
{
    if (s_client.initialized) {
        return ESP_OK;
    }
    if (config == NULL ||
        config->asr_model == NULL || config->asr_model[0] == '\0' ||
        config->llm_model == NULL || config->llm_model[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_client, 0, sizeof(s_client));
    s_client.state = LLM_CLIENT_STATE_IDLE;
    s_client.asr_model = config->asr_model;
    s_client.llm_model = config->llm_model;
    s_client.tts_model = config->tts_model;
    s_client.system_prompt = (config->system_prompt != NULL) ?
        config->system_prompt : LLM_GATEWAY_SYSTEM_PROMPT;
    s_client.event_cb = config->event_cb;
    s_client.user_ctx = config->user_ctx;
    s_client.initialized = true;

    char key_summary[48] = {0};
    llm_gateway_protocol_make_key_summary(key_summary, sizeof(key_summary));
    ESP_LOGI(TAG, "llm_client initialized, key %s", key_summary);
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
    memset(&s_client, 0, sizeof(s_client));
    return ESP_OK;
}

esp_err_t llm_client_start_voice_session(void)
{
    if (!s_client.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_client.state != LLM_CLIENT_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = llm_client_alloc_record_buffer();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ASR fallback PCM cache unavailable: %s", esp_err_to_name(ret));
    }

    llm_client_set_state(LLM_CLIENT_STATE_ASR_CONNECTING);
    llm_gateway_ws_config_t ws_config = {
        .asr_model = s_client.asr_model,
        .event_cb = llm_client_ws_event_cb,
        .user_ctx = NULL,
    };
    ret = llm_gateway_ws_start(&ws_config);
    if (ret != ESP_OK) {
        llm_client_emit(LLM_CLIENT_EVENT_ERROR, NULL, NULL, 0, ret, "ASR WebSocket start failed");
        if (!LLM_GATEWAY_ASR_HTTP_FALLBACK || s_client.record_buffer == NULL) {
            llm_client_reset_voice_session();
            return ret;
        }
        ESP_LOGW(TAG, "ASR WebSocket failed, will use HTTP fallback at voice_end");
    } else {
        s_client.ws_started = true;
    }

    llm_client_set_state(LLM_CLIENT_STATE_ASR_STREAMING);
    return ESP_OK;
}

esp_err_t llm_client_send_audio_pcm16(const int16_t *pcm, size_t samples, uint32_t sample_rate_hz)
{
    if (!s_client.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pcm == NULL || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_rate_hz != LLM_GATEWAY_AUDIO_SAMPLE_RATE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_client.state != LLM_CLIENT_STATE_ASR_STREAMING) {
        return ESP_ERR_INVALID_STATE;
    }

    llm_client_cache_pcm(pcm, samples);

    if (!s_client.ws_started) {
        return ESP_OK;
    }

    esp_err_t ret = llm_gateway_ws_send_pcm16(pcm, samples, sample_rate_hz);
    if (ret != ESP_OK && LLM_GATEWAY_ASR_HTTP_FALLBACK) {
        ESP_LOGW(TAG, "ASR WS PCM send failed, keep recording for HTTP fallback: %s", esp_err_to_name(ret));
        (void)llm_gateway_ws_stop();
        s_client.ws_started = false;
        return ESP_OK;
    }
    return ret;
}

esp_err_t llm_client_finish_voice_session(void)
{
    if (!s_client.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_client.state != LLM_CLIENT_STATE_ASR_STREAMING &&
        s_client.state != LLM_CLIENT_STATE_ASR_WAIT_FINAL &&
        s_client.state != LLM_CLIENT_STATE_LLM_REQUESTING &&
        s_client.state != LLM_CLIENT_STATE_ROUTING) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_client.state == LLM_CLIENT_STATE_LLM_REQUESTING ||
        s_client.state == LLM_CLIENT_STATE_ROUTING) {
        return ESP_OK;
    }

    llm_client_set_state(LLM_CLIENT_STATE_ASR_WAIT_FINAL);
    esp_err_t ret = ESP_OK;
    if (s_client.ws_started) {
        ret = llm_gateway_ws_finish();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ASR WS finish failed: %s", esp_err_to_name(ret));
        }
    }

    if (s_client.asr_final_text[0] != '\0') {
        llm_client_reset_voice_session();
        return ESP_OK;
    }

    if (LLM_GATEWAY_ASR_HTTP_FALLBACK &&
        s_client.record_buffer != NULL &&
        s_client.record_sample_count > 0) {
        char fallback_text[LLM_GATEWAY_ASR_TEXT_MAX_BYTES] = {0};
        ESP_LOGI(TAG,
                 "ASR HTTP fallback: samples=%u overflow=%d",
                 (unsigned int)s_client.record_sample_count,
                 s_client.record_overflowed);
        ret = llm_gateway_http_asr_transcription(s_client.asr_model,
                                                 s_client.record_buffer,
                                                 s_client.record_sample_count,
                                                 LLM_GATEWAY_AUDIO_SAMPLE_RATE,
                                                 fallback_text,
                                                 sizeof(fallback_text));
        if (ret == ESP_OK && fallback_text[0] != '\0') {
            ret = llm_client_handle_asr_final(fallback_text);
        }
    }

    if (s_client.ws_started) {
        (void)llm_gateway_ws_stop();
        s_client.ws_started = false;
    }
    llm_client_free_record_buffer();
    llm_client_set_state(LLM_CLIENT_STATE_IDLE);
    return ret;
}

esp_err_t llm_client_stop_voice_session(void)
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
    return s_client.initialized && s_client.state != LLM_CLIENT_STATE_IDLE;
}

esp_err_t llm_client_chat_json_context(const char *json_context)
{
    return llm_client_chat_text(json_context);
}

esp_err_t llm_client_chat_json_context_with_model(const char *llm_model, const char *json_context)
{
    return llm_client_chat_text_with_model(llm_model, json_context);
}

esp_err_t llm_client_send_sensor_json_with_model(const char *source, const char *json, const char *llm_model)
{
    if (source == NULL || source[0] == '\0' || json == NULL || json[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char context[LLM_GATEWAY_SENSOR_CONTEXT_MAX_BYTES] = {0};
    int written = snprintf(context,
                           sizeof(context),
                           "{\"source\":\"%s\",\"data\":%s}",
                           source,
                           json);
    if (written < 0 || (size_t)written >= sizeof(context)) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "sensor JSON reserved: source=%s len=%u", source, (unsigned int)strlen(json));
    return llm_client_chat_json_context_with_model(llm_model, context);
}

esp_err_t llm_client_send_sensor_json(const char *source, const char *json)
{
    return llm_client_send_sensor_json_with_model(source, json, s_client.llm_model);
}

esp_err_t llm_client_send_bme690_json(const char *json)
{
    return llm_client_send_sensor_json("bme690", json);
}

esp_err_t llm_client_send_csi_json(const char *json)
{
    return llm_client_send_sensor_json("csi", json);
}

esp_err_t llm_client_send_system_status_json(const char *json)
{
    return llm_client_send_sensor_json("system_status", json);
}

esp_err_t llm_client_tts_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "TTS disabled, skip text_len=%u", (unsigned int)strlen(text));
    return ESP_ERR_NOT_SUPPORTED;
}

bool llm_client_is_tts_enabled(void)
{
    return LLM_GATEWAY_ENABLE_TTS != 0;
}
