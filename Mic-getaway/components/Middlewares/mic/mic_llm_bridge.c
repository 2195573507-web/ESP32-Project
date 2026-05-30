#include "mic_llm_bridge.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "llm_client.h"

static const char *TAG = "mic_llm_bridge";

static bool s_mic_llm_bridge_initialized;

static const char *mic_llm_bridge_event_name(llm_client_event_type_t type)
{
    switch (type) {
    case LLM_CLIENT_EVENT_CONNECTED:
        return "connected";
    case LLM_CLIENT_EVENT_DISCONNECTED:
        return "disconnected";
    case LLM_CLIENT_EVENT_ASR_PARTIAL_TEXT:
        return "asr_partial";
    case LLM_CLIENT_EVENT_ASR_FINAL_TEXT:
        return "asr_final";
    case LLM_CLIENT_EVENT_LLM_DELTA_TEXT:
        return "llm_delta";
    case LLM_CLIENT_EVENT_LLM_FINAL_TEXT:
        return "llm_final";
    case LLM_CLIENT_EVENT_COMMAND_RESULT:
        return "command_result";
    case LLM_CLIENT_EVENT_TTS_AUDIO:
        return "tts_audio";
    case LLM_CLIENT_EVENT_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

static void mic_llm_bridge_event_cb(const llm_client_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event == NULL) {
        return;
    }

    if (event->type == LLM_CLIENT_EVENT_TTS_AUDIO) {
        ESP_LOGI(TAG, "LLM event %s: audio_len=%u ignored", mic_llm_bridge_event_name(event->type), (unsigned int)event->audio_len);
        return;
    }
    if (event->type == LLM_CLIENT_EVENT_ERROR) {
        ESP_LOGE(TAG,
                 "LLM event error: code=%d message=%s",
                 event->code,
                 event->message != NULL ? event->message : "<none>");
        return;
    }

    if (event->text != NULL && event->text[0] != '\0') {
        ESP_LOGI(TAG, "LLM event %s: %s", mic_llm_bridge_event_name(event->type), event->text);
    } else if (APP_DEBUG_MIC_LLM_BRIDGE) {
        ESP_LOGI(TAG, "LLM event %s", mic_llm_bridge_event_name(event->type));
    }
}

esp_err_t mic_llm_bridge_init(void)
{
    if (s_mic_llm_bridge_initialized) {
        return ESP_OK;
    }

    llm_client_config_t config = {
        .asr_model = MIC_LLM_BRIDGE_ASR_MODEL,
        .llm_model = MIC_LLM_BRIDGE_LLM_MODEL,
        .tts_model = MIC_LLM_BRIDGE_TTS_MODEL,
        .system_prompt = LLM_GATEWAY_SYSTEM_PROMPT,
        .event_cb = mic_llm_bridge_event_cb,
        .user_ctx = NULL,
    };
    esp_err_t ret = llm_client_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "llm_client init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_mic_llm_bridge_initialized = true;
    ESP_LOGI(TAG, "Mic LLM bridge initialized");
    return ESP_OK;
}

esp_err_t mic_llm_bridge_on_voice_start(void)
{
    if (!s_mic_llm_bridge_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "voice_start");
    return llm_client_start_voice_session();
}

esp_err_t mic_llm_bridge_on_pcm_chunk(const int16_t *pcm, size_t samples, uint32_t sample_rate_hz)
{
    if (!s_mic_llm_bridge_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pcm == NULL || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = llm_client_send_audio_pcm16(pcm, samples, sample_rate_hz);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "pcm_chunk failed, stop session: %s", esp_err_to_name(ret));
        (void)llm_client_stop_voice_session();
    } else if (APP_DEBUG_MIC_LLM_BRIDGE && APP_DEBUG_LLM_GATEWAY_AUDIO) {
        ESP_LOGI(TAG, "pcm_chunk samples=%u bytes=%u", (unsigned int)samples, (unsigned int)(samples * sizeof(int16_t)));
    }
    return ret;
}

esp_err_t mic_llm_bridge_on_voice_end(void)
{
    if (!s_mic_llm_bridge_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "voice_end");
    if (!llm_client_is_voice_session_active()) {
        return ESP_OK;
    }

    esp_err_t ret = llm_client_finish_voice_session();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "voice_end finish failed: %s", esp_err_to_name(ret));
        (void)llm_client_stop_voice_session();
    }
    return ret;
}

esp_err_t mic_llm_bridge_stop(void)
{
    if (!s_mic_llm_bridge_initialized) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "session stop");
    return llm_client_stop_voice_session();
}
