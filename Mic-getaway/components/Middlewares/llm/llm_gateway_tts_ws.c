#include "llm_gateway_tts_ws.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "llm_config.h"
#include "llm_gateway_protocol.h"
#include "volc_gateway_auth.h"

static const char *TAG = "llm_gateway_tts_ws";

#define TTS_WS_FIRST_RESPONSE_TIMEOUT_MS 10000
#define TTS_WS_AUDIO_DONE_TIMEOUT_MS 30000
#define TTS_WS_APPEND_DONE_GAP_MS 30

#ifndef CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN
#define CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN 0
#endif

#ifndef CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN
#define CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN 0
#endif

#ifndef CONFIG_WS_BUFFER_SIZE
#define CONFIG_WS_BUFFER_SIZE 0
#endif

enum {
    LLM_GATEWAY_TTS_WS_CONNECTED_BIT = BIT0,
    LLM_GATEWAY_TTS_WS_SESSION_UPDATED_BIT = BIT1,
    LLM_GATEWAY_TTS_WS_AUDIO_DONE_BIT = BIT2,
    LLM_GATEWAY_TTS_WS_ERROR_BIT = BIT3,
};

typedef enum {
    TTS_WS_CONNECTING = 0,
    TTS_WS_SESSION_UPDATE_SENT,
    TTS_WS_SESSION_READY,
    TTS_WS_TEXT_SENT,
    TTS_WS_DONE_SENT,
    TTS_WS_RECEIVING_AUDIO,
    TTS_WS_DONE,
    TTS_WS_ERROR,
} tts_ws_fsm_state_t;

typedef struct {
    esp_websocket_client_handle_t client;
    EventGroupHandle_t event_group;
    const char *tts_model;
    const char *pending_text;
    llm_gateway_tts_ws_event_cb_t event_cb;
    void *user_ctx;
    char *rx_buffer;
    size_t rx_buffer_received;
    size_t rx_buffer_expected;
    bool rx_drop_current;
    size_t rx_drop_expected;
    size_t rx_drop_received;
    char headers[320];
    size_t config_buffer_size;
    int last_payload_len;
    int last_payload_offset;
    int last_data_len;
    uint8_t last_opcode;
    int last_status_code;
    esp_err_t last_transport_error;
    esp_err_t last_tls_error;
    int last_errno;
    tts_ws_fsm_state_t state;
    bool connected;
    bool audio_received;
    bool session_update_sent;
    bool session_updated;
    bool text_append_sent;
    bool input_done_sent;
    uint32_t audio_delta_count;
    uint32_t dropped_chunk_count;
    size_t pcm_bytes_total;
    UBaseType_t ws_task_stack_high_water_bytes;
} llm_gateway_tts_ws_state_t;

static llm_gateway_tts_ws_state_t s_tts;

static esp_err_t llm_gateway_tts_ws_send_session_update(void);
static esp_err_t llm_gateway_tts_ws_send_text(const char *text);

static const char *llm_gateway_tts_ws_state_name(tts_ws_fsm_state_t state)
{
    switch (state) {
    case TTS_WS_CONNECTING:
        return "TTS_WS_CONNECTING";
    case TTS_WS_SESSION_UPDATE_SENT:
        return "TTS_WS_SESSION_UPDATE_SENT";
    case TTS_WS_SESSION_READY:
        return "TTS_WS_SESSION_READY";
    case TTS_WS_TEXT_SENT:
        return "TTS_WS_TEXT_SENT";
    case TTS_WS_DONE_SENT:
        return "TTS_WS_DONE_SENT";
    case TTS_WS_RECEIVING_AUDIO:
        return "TTS_WS_RECEIVING_AUDIO";
    case TTS_WS_DONE:
        return "TTS_WS_DONE";
    case TTS_WS_ERROR:
        return "TTS_WS_ERROR";
    default:
        return "TTS_WS_UNKNOWN";
    }
}

static void llm_gateway_tts_ws_set_state(tts_ws_fsm_state_t state, const char *reason)
{
    if (s_tts.state == state) {
        return;
    }
    ESP_LOGI(TAG,
             "TTS WS state %s -> %s reason=%s",
             llm_gateway_tts_ws_state_name(s_tts.state),
             llm_gateway_tts_ws_state_name(state),
             reason != NULL ? reason : "<none>");
    s_tts.state = state;
}

static void llm_gateway_tts_ws_emit(llm_gateway_tts_ws_event_type_t type,
                                    uint8_t *audio,
                                    size_t audio_len,
                                    bool audio_owned,
                                    int code,
                                    const char *message)
{
    if (s_tts.event_cb == NULL) {
        return;
    }

    llm_gateway_tts_ws_event_t event = {
        .type = type,
        .audio = audio,
        .audio_len = audio_len,
        .audio_owned = audio_owned,
        .code = code,
        .message = message,
    };
    s_tts.event_cb(&event, s_tts.user_ctx);
}

static void llm_gateway_tts_ws_emit_owned_audio(uint8_t *audio, size_t audio_len)
{
    if (audio == NULL || audio_len == 0) {
        free(audio);
        return;
    }
    if (s_tts.event_cb == NULL) {
        ESP_LOGW(TAG,
                 "TTS WS drop owned audio without event callback bytes=%u",
                 (unsigned int)audio_len);
        free(audio);
        return;
    }

    llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_AUDIO_DELTA,
                            audio,
                            audio_len,
                            true,
                            0,
                            NULL);
}

static void llm_gateway_tts_ws_clear_rx_buffer(void)
{
    free(s_tts.rx_buffer);
    s_tts.rx_buffer = NULL;
    s_tts.rx_buffer_received = 0;
    s_tts.rx_buffer_expected = 0;
    s_tts.rx_drop_current = false;
    s_tts.rx_drop_expected = 0;
    s_tts.rx_drop_received = 0;
}

static size_t llm_gateway_tts_ws_free_heap(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

static size_t llm_gateway_tts_ws_min_free_heap(void)
{
    return heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
}

static size_t llm_gateway_tts_ws_largest_free_block(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

static UBaseType_t llm_gateway_tts_ws_current_stack_high_water(void)
{
#if (INCLUDE_uxTaskGetStackHighWaterMark == 1)
    return uxTaskGetStackHighWaterMark(NULL);
#else
    return 0;
#endif
}

static UBaseType_t llm_gateway_tts_ws_note_stack_high_water(void)
{
    UBaseType_t current = llm_gateway_tts_ws_current_stack_high_water();
    if (current != 0 &&
        (s_tts.ws_task_stack_high_water_bytes == 0 ||
         current < s_tts.ws_task_stack_high_water_bytes)) {
        s_tts.ws_task_stack_high_water_bytes = current;
    }
    return current;
}

static UBaseType_t llm_gateway_tts_ws_stack_high_water(void)
{
    return s_tts.ws_task_stack_high_water_bytes != 0 ?
        s_tts.ws_task_stack_high_water_bytes :
        llm_gateway_tts_ws_current_stack_high_water();
}

static size_t llm_gateway_tts_ws_effective_buffer_size(void)
{
    return s_tts.config_buffer_size != 0 ? s_tts.config_buffer_size : LLM_TTS_WS_RX_BUFFER_SIZE;
}

static bool llm_gateway_tts_ws_rx_in_progress(void)
{
    if (s_tts.rx_drop_current) {
        return true;
    }
    return s_tts.rx_buffer != NULL && s_tts.rx_buffer_expected > s_tts.rx_buffer_received;
}

static size_t llm_gateway_tts_ws_partial_payload_len(void)
{
    return s_tts.rx_drop_current ? s_tts.rx_drop_expected : s_tts.rx_buffer_expected;
}

static size_t llm_gateway_tts_ws_partial_received_len(void)
{
    return s_tts.rx_drop_current ? s_tts.rx_drop_received : s_tts.rx_buffer_received;
}

static bool llm_gateway_tts_ws_fragment_complete(const esp_websocket_event_data_t *data)
{
    if (data == NULL || data->payload_len <= 0 || data->data_len <= 0) {
        return false;
    }
    return data->payload_offset + data->data_len >= data->payload_len;
}

static void llm_gateway_tts_ws_log_data_fragment(const esp_websocket_event_data_t *data,
                                                 bool received_complete,
                                                 size_t accumulated_len)
{
    if (data == NULL) {
        return;
    }
    ESP_LOGI(TAG,
             "TTS WS DATA buffer_size=%u opcode=%u payload_len=%d payload_offset=%d "
             "data_len=%d accumulated_len=%u is_final_fragment=%d received_complete=%d "
             "free_heap=%u min_free_heap=%u largest_free_block=%u ws_stack_hwm=%u state=%s",
             (unsigned int)llm_gateway_tts_ws_effective_buffer_size(),
             (unsigned int)data->op_code,
             data->payload_len,
             data->payload_offset,
             data->data_len,
             (unsigned int)accumulated_len,
             data->fin ? 1 : 0,
             received_complete ? 1 : 0,
             (unsigned int)llm_gateway_tts_ws_free_heap(),
             (unsigned int)llm_gateway_tts_ws_min_free_heap(),
             (unsigned int)llm_gateway_tts_ws_largest_free_block(),
             (unsigned int)s_tts.ws_task_stack_high_water_bytes,
             llm_gateway_tts_ws_state_name(s_tts.state));
}

static void llm_gateway_tts_ws_log_parse_failed(const char *payload,
                                                size_t payload_len,
                                                const char *event_type,
                                                const char *state_at_receive,
                                                size_t base64_len,
                                                esp_err_t ret)
{
    char prefix[81] = {0};
    size_t prefix_len = payload_len < 80U ? payload_len : 80U;
    for (size_t i = 0; i < prefix_len; i++) {
        unsigned char ch = (unsigned char)payload[i];
        prefix[i] = (ch >= 0x20U && ch <= 0x7EU) ? (char)ch : '.';
    }

    char first_char = payload_len > 0 ? payload[0] : '\0';
    char last_char = payload_len > 0 ? payload[payload_len - 1U] : '\0';
    ESP_LOGW(TAG,
             "TTS WS recv parse_failed type=%s payload_len=%u state=%s base64_len=%u "
             "ret=%s first_char=0x%02x last_char=0x%02x starts_with_brace=%d "
             "ends_with_brace=%d prefix80=%s",
             event_type != NULL ? event_type : "<unknown>",
             (unsigned int)payload_len,
             state_at_receive != NULL ? state_at_receive : "<unknown>",
             (unsigned int)base64_len,
             esp_err_to_name(ret),
             (unsigned int)(unsigned char)first_char,
             (unsigned int)(unsigned char)last_char,
             first_char == '{' ? 1 : 0,
             last_char == '}' ? 1 : 0,
             prefix);
}

static void llm_gateway_tts_ws_set_bits(EventBits_t bits)
{
    if (s_tts.event_group != NULL) {
        xEventGroupSetBits(s_tts.event_group, bits);
    }
}

static void llm_gateway_tts_ws_fail(esp_err_t code, const char *message)
{
    llm_gateway_tts_ws_set_state(TTS_WS_ERROR, message);
    llm_gateway_tts_ws_set_bits(LLM_GATEWAY_TTS_WS_ERROR_BIT);
    llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_ERROR,
                            NULL,
                            0,
                            false,
                            code,
                            message);
}

static bool llm_gateway_tts_ws_json_ends_with_object(const char *json, size_t json_len)
{
    return json != NULL && json_len > 0 && json[json_len - 1] == '}';
}

static void llm_gateway_tts_ws_log_text_payload_check(const char *event_name,
                                                      const char *json,
                                                      size_t json_len,
                                                      const char *text)
{
    size_t text_bytes = text != NULL ? strlen(text) : 0;
    ESP_LOGI(TAG,
             "TTS WS payload check event=%s payload_len=%u strlen=%u text_bytes=%u "
             "delta_non_empty=%d json_ends_with_brace=%d send_len_uses_strlen=1 json_escape=cJSON_utf8",
             event_name != NULL ? event_name : "<unknown>",
             (unsigned int)json_len,
             (unsigned int)(json != NULL ? strlen(json) : 0U),
             (unsigned int)text_bytes,
             text_bytes > 0 ? 1 : 0,
             llm_gateway_tts_ws_json_ends_with_object(json, json_len) ? 1 : 0);
}

static void llm_gateway_tts_ws_log_done_payload_check(const char *json, size_t json_len)
{
    ESP_LOGI(TAG,
             "TTS WS payload check event=input_text.done payload_len=%u strlen=%u "
             "json_ends_with_brace=%d send_len_uses_strlen=1",
             (unsigned int)json_len,
             (unsigned int)(json != NULL ? strlen(json) : 0U),
             llm_gateway_tts_ws_json_ends_with_object(json, json_len) ? 1 : 0);
}

static esp_err_t llm_gateway_tts_ws_send_json(char *json,
                                              size_t json_len,
                                              const char *event_name)
{
    if (json == NULL || json_len == 0 || event_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t send_len = strlen(json);
    if (send_len == 0 || send_len != json_len) {
        ESP_LOGW(TAG,
                 "TTS WS send length mismatch event=%s payload_len=%u strlen=%u state=%s",
                 event_name,
                 (unsigned int)json_len,
                 (unsigned int)send_len,
                 llm_gateway_tts_ws_state_name(s_tts.state));
        if (send_len == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    if (s_tts.client == NULL || !s_tts.connected ||
        !esp_websocket_client_is_connected(s_tts.client)) {
        ESP_LOGW(TAG,
                 "TTS WS send event=%s payload_len=%u ret=-1 state=%s reason=not_connected",
                 event_name,
                 (unsigned int)send_len,
                 llm_gateway_tts_ws_state_name(s_tts.state));
        return ESP_ERR_INVALID_STATE;
    }

    int sent = esp_websocket_client_send_text(s_tts.client,
                                              json,
                                              (int)send_len,
                                              pdMS_TO_TICKS(LLM_GATEWAY_WS_SEND_TIMEOUT_MS));
    ESP_LOGI(TAG,
             "TTS WS send event=%s payload_len=%u strlen=%u ret=%d ret_eq_strlen=%d state=%s",
             event_name,
             (unsigned int)json_len,
             (unsigned int)send_len,
             sent,
             sent == (int)send_len ? 1 : 0,
             llm_gateway_tts_ws_state_name(s_tts.state));
    esp_err_t ret = (sent == (int)send_len) ? ESP_OK : ESP_FAIL;
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "TTS WS send failed event=%s expected_strlen=%u payload_len=%u ret=%d state=%s",
                 event_name,
                 (unsigned int)send_len,
                 (unsigned int)json_len,
                 sent,
                 llm_gateway_tts_ws_state_name(s_tts.state));
    }
    return ret;
}

static void llm_gateway_tts_ws_log_reject_hint(void)
{
    if (s_tts.last_status_code == 401 || s_tts.last_status_code == 403) {
        ESP_LOGE(TAG,
                 "Gateway TTS rejected: check Authorization Bearer API key, "
                 "check whether API key is bound to model %s, check model query parameter, "
                 "check whether X-Api-Resource-Id should be omitted for preset models.",
                 LLM_MODEL_TTS);
    }
}

static void llm_gateway_tts_ws_log_error_diag(const char *reason)
{
    char key_summary[48] = {0};
    volc_gateway_auth_make_key_summary(key_summary, sizeof(key_summary));
    ESP_LOGE(TAG,
             "%s uri=%s model=%s voice=%s output_audio_format=%s output_audio_sample_rate=%d "
             "send_resource_id=%d auth_present=%d key=%s status=%d transport_err=0x%x tls_err=0x%x errno=%d "
             "state=%s session_update_sent=%d session_updated=%d input_text_done_sent=%d "
             "audio_delta_count=%u pcm_bytes_total=%u ws_buffer_size=%u rx_in_progress=%d "
             "partial_payload_len=%u partial_received_len=%u last_payload_len=%d "
             "last_payload_offset=%d last_data_len=%d last_opcode=%u free_heap=%u "
             "min_free_heap=%u largest_free_block=%u dropped_chunk_count=%u ws_stack_hwm=%u",
             reason != NULL ? reason : "TTS WS error",
             VOLC_GATEWAY_TTS_REALTIME_URI,
             LLM_MODEL_TTS,
             LLM_TTS_VOICE,
             LLM_TTS_OUTPUT_FORMAT,
             LLM_TTS_SAMPLE_RATE,
             LLM_TTS_USE_RESOURCE_ID ? 1 : 0,
             VOLC_GATEWAY_API_KEY[0] != '\0' ? 1 : 0,
             key_summary,
             s_tts.last_status_code,
             (unsigned int)s_tts.last_transport_error,
             (unsigned int)s_tts.last_tls_error,
             s_tts.last_errno,
             llm_gateway_tts_ws_state_name(s_tts.state),
             s_tts.session_update_sent ? 1 : 0,
             s_tts.session_updated ? 1 : 0,
             s_tts.input_done_sent ? 1 : 0,
             (unsigned int)s_tts.audio_delta_count,
             (unsigned int)s_tts.pcm_bytes_total,
             (unsigned int)llm_gateway_tts_ws_effective_buffer_size(),
             llm_gateway_tts_ws_rx_in_progress() ? 1 : 0,
             (unsigned int)llm_gateway_tts_ws_partial_payload_len(),
             (unsigned int)llm_gateway_tts_ws_partial_received_len(),
             s_tts.last_payload_len,
             s_tts.last_payload_offset,
             s_tts.last_data_len,
             (unsigned int)s_tts.last_opcode,
             (unsigned int)llm_gateway_tts_ws_free_heap(),
             (unsigned int)llm_gateway_tts_ws_min_free_heap(),
             (unsigned int)llm_gateway_tts_ws_largest_free_block(),
             (unsigned int)s_tts.dropped_chunk_count,
             (unsigned int)llm_gateway_tts_ws_stack_high_water());
    llm_gateway_tts_ws_log_reject_hint();
}

static void llm_gateway_tts_ws_log_disconnect_diag(const char *reason)
{
    ESP_LOGW(TAG,
             "%s state=%s session_update_sent=%d session_updated=%d input_text_done_sent=%d "
             "audio_delta_count=%u pcm_bytes_total=%u status=%d transport_err=0x%x tls_err=0x%x errno=%d "
             "ws_buffer_size=%u rx_in_progress=%d partial_payload_len=%u partial_received_len=%u "
             "last_payload_len=%d last_payload_offset=%d last_data_len=%d last_opcode=%u "
             "free_heap=%u min_free_heap=%u largest_free_block=%u dropped_chunk_count=%u "
             "ws_stack_hwm=%u",
             reason != NULL ? reason : "TTS WS disconnected",
             llm_gateway_tts_ws_state_name(s_tts.state),
             s_tts.session_update_sent ? 1 : 0,
             s_tts.session_updated ? 1 : 0,
             s_tts.input_done_sent ? 1 : 0,
             (unsigned int)s_tts.audio_delta_count,
             (unsigned int)s_tts.pcm_bytes_total,
             s_tts.last_status_code,
             (unsigned int)s_tts.last_transport_error,
             (unsigned int)s_tts.last_tls_error,
             s_tts.last_errno,
             (unsigned int)llm_gateway_tts_ws_effective_buffer_size(),
             llm_gateway_tts_ws_rx_in_progress() ? 1 : 0,
             (unsigned int)llm_gateway_tts_ws_partial_payload_len(),
             (unsigned int)llm_gateway_tts_ws_partial_received_len(),
             s_tts.last_payload_len,
             s_tts.last_payload_offset,
             s_tts.last_data_len,
             (unsigned int)s_tts.last_opcode,
             (unsigned int)llm_gateway_tts_ws_free_heap(),
             (unsigned int)llm_gateway_tts_ws_min_free_heap(),
             (unsigned int)llm_gateway_tts_ws_largest_free_block(),
             (unsigned int)s_tts.dropped_chunk_count,
             (unsigned int)llm_gateway_tts_ws_stack_high_water());
}

static void llm_gateway_tts_ws_log_round_summary(const char *reason, esp_err_t result)
{
    ESP_LOGI(TAG,
             "TTS round summary reason=%s result=%s free_heap=%u min_free_heap=%u "
             "largest_free_block=%u audio_delta_count=%u pcm_bytes_total=%u "
             "dropped_chunk_count=%u ws_stack_hwm=%u payload_max=%u chunk_max=%u",
             reason != NULL ? reason : "done",
             esp_err_to_name(result),
             (unsigned int)llm_gateway_tts_ws_free_heap(),
             (unsigned int)llm_gateway_tts_ws_min_free_heap(),
             (unsigned int)llm_gateway_tts_ws_largest_free_block(),
             (unsigned int)s_tts.audio_delta_count,
             (unsigned int)s_tts.pcm_bytes_total,
             (unsigned int)s_tts.dropped_chunk_count,
             (unsigned int)llm_gateway_tts_ws_stack_high_water(),
             (unsigned int)LLM_TTS_WS_PAYLOAD_MAX,
             (unsigned int)SPEAKER_TTS_MAX_CHUNK_BYTES);
}

static void llm_gateway_tts_ws_handle_payload(const char *payload, size_t payload_len)
{
    int64_t handle_start_us = esp_timer_get_time();
    const char *state_at_receive = llm_gateway_tts_ws_state_name(s_tts.state);

    llm_gateway_tts_event_t parsed = {0};
    uint8_t *owned_audio = NULL;
    size_t owned_audio_len = 0;
    esp_err_t ret = llm_gateway_protocol_parse_tts_ws_event_owned_audio(payload,
                                                                        payload_len,
                                                                        &parsed,
                                                                        &owned_audio,
                                                                        &owned_audio_len);
    if (ret != ESP_OK) {
        const char *fail_event_type = parsed.type[0] != '\0' ? parsed.type : "<unknown>";
        llm_gateway_tts_ws_log_parse_failed(payload,
                                            payload_len,
                                            fail_event_type,
                                            state_at_receive,
                                            parsed.audio_base64_len,
                                            ret);
        llm_gateway_tts_ws_fail(ret, "TTS WS parse failed");
        return;
    }

    const char *event_type = parsed.type[0] != '\0' ? parsed.type : "<unknown>";
    if (parsed.is_audio_delta) {
        ESP_LOGI(TAG,
                 "TTS WS recv type=%s full_payload_len=%u state=%s base64_len=%u pcm_bytes=%u dropped=%d",
                 event_type,
                 (unsigned int)payload_len,
                 state_at_receive,
                 (unsigned int)parsed.audio_base64_len,
                 (unsigned int)parsed.audio_len,
                 parsed.is_audio_dropped ? 1 : 0);
    } else {
        ESP_LOGI(TAG,
                 "TTS WS recv type=%s full_payload_len=%u state=%s",
                 event_type,
                 (unsigned int)payload_len,
                 state_at_receive);
    }

    if (parsed.is_error) {
        free(owned_audio);
        ESP_LOGE(TAG,
                 "TTS WS server error type=%s code=%d message=%s state=%s",
                 event_type,
                 parsed.code,
                 parsed.message[0] != '\0' ? parsed.message : "<none>",
                 llm_gateway_tts_ws_state_name(s_tts.state));
        llm_gateway_tts_ws_fail(parsed.code,
                                parsed.message[0] != '\0' ?
                                parsed.message : "TTS WebSocket error");
        return;
    }
    if (parsed.is_session_updated) {
        free(owned_audio);
        s_tts.session_updated = true;
        llm_gateway_tts_ws_set_state(TTS_WS_SESSION_READY, "tts_session.updated");
        llm_gateway_tts_ws_set_bits(LLM_GATEWAY_TTS_WS_SESSION_UPDATED_BIT);
        llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_SESSION_UPDATED,
                                NULL,
                                0,
                                false,
                                0,
                                NULL);
        ret = llm_gateway_tts_ws_send_text(s_tts.pending_text);
        if (ret != ESP_OK) {
            llm_gateway_tts_ws_fail(ret, "TTS input_text send failed");
        }
        return;
    }
    if (parsed.is_audio_delta && parsed.is_audio_dropped) {
        s_tts.dropped_chunk_count++;
        llm_gateway_tts_ws_set_state(TTS_WS_RECEIVING_AUDIO, "response.audio.delta oversized");
        ESP_LOGW(TAG,
                 "TTS WS drop oversized PCM chunk decoded_bytes=%u max=%u base64_len=%u "
                 "dropped_chunk_count=%u free_heap=%u min_free_heap=%u largest_free_block=%u",
                 (unsigned int)parsed.audio_len,
                 (unsigned int)SPEAKER_TTS_MAX_CHUNK_BYTES,
                 (unsigned int)parsed.audio_base64_len,
                 (unsigned int)s_tts.dropped_chunk_count,
                 (unsigned int)llm_gateway_tts_ws_free_heap(),
                 (unsigned int)llm_gateway_tts_ws_min_free_heap(),
                 (unsigned int)llm_gateway_tts_ws_largest_free_block());
        return;
    }
    if (parsed.is_audio_delta && owned_audio != NULL && owned_audio_len > 0) {
        s_tts.audio_received = true;
        s_tts.audio_delta_count++;
        s_tts.pcm_bytes_total += owned_audio_len;
        llm_gateway_tts_ws_set_state(TTS_WS_RECEIVING_AUDIO, "response.audio.delta");
        llm_gateway_tts_ws_emit_owned_audio(owned_audio, owned_audio_len);
        owned_audio = NULL;
        int64_t handle_us = esp_timer_get_time() - handle_start_us;
        ESP_LOGI(TAG,
                 "TTS WS audio.delta handled audio_delta_count=%u base64_len=%u pcm_bytes=%u "
                 "pcm_bytes_total=%u full_payload_len=%u decode_us=%lld event_us=%lld",
                 (unsigned int)s_tts.audio_delta_count,
                 (unsigned int)parsed.audio_base64_len,
                 (unsigned int)owned_audio_len,
                 (unsigned int)s_tts.pcm_bytes_total,
                 (unsigned int)payload_len,
                 (long long)parsed.audio_decode_us,
                 (long long)handle_us);
        return;
    }
    if (parsed.is_audio_done) {
        free(owned_audio);
        llm_gateway_tts_ws_set_state(TTS_WS_DONE, "response.audio.done");
        llm_gateway_tts_ws_set_bits(LLM_GATEWAY_TTS_WS_AUDIO_DONE_BIT);
        llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_AUDIO_DONE,
                                NULL,
                                0,
                                false,
                                0,
                                NULL);
        return;
    }

    if (strcmp(event_type, "response.audio_subtitle.delta") == 0) {
        free(owned_audio);
        ESP_LOGI(TAG,
                 "TTS WS ignore subtitle event type=%s payload_len=%u state=%s",
                 event_type,
                 (unsigned int)payload_len,
                 llm_gateway_tts_ws_state_name(s_tts.state));
        return;
    }

    ESP_LOGW(TAG,
             "TTS WS unknown event type=%s payload_len=%u state=%s",
             event_type,
             (unsigned int)payload_len,
             llm_gateway_tts_ws_state_name(s_tts.state));
    free(owned_audio);
}

static void llm_gateway_tts_ws_handle_data(const esp_websocket_event_data_t *data)
{
    if (data == NULL) {
        return;
    }

    s_tts.last_payload_len = data->payload_len;
    s_tts.last_payload_offset = data->payload_offset;
    s_tts.last_data_len = data->data_len;
    s_tts.last_opcode = data->op_code;

    bool fragment_complete = llm_gateway_tts_ws_fragment_complete(data);
    if (data->data_ptr == NULL || data->data_len <= 0) {
        llm_gateway_tts_ws_log_data_fragment(data, fragment_complete, s_tts.rx_buffer_received);
        ESP_LOGI(TAG,
                 "TTS WS ignore empty DATA payload_len=%d payload_offset=%d data_len=%d opcode=%u state=%s",
                 data->payload_len,
                 data->payload_offset,
                 data->data_len,
                 (unsigned int)data->op_code,
                 llm_gateway_tts_ws_state_name(s_tts.state));
        return;
    }

    if (data->payload_len <= 0 || data->payload_offset < 0 || data->data_len < 0) {
        llm_gateway_tts_ws_log_data_fragment(data, fragment_complete, s_tts.rx_buffer_received);
        ESP_LOGW(TAG,
                 "TTS WS drop invalid DATA payload_len=%d payload_offset=%d data_len=%d",
                 data->payload_len,
                 data->payload_offset,
                 data->data_len);
        return;
    }

    if (data->payload_offset == 0) {
        llm_gateway_tts_ws_clear_rx_buffer();
        if (data->payload_len > LLM_GATEWAY_TTS_WS_PAYLOAD_MAX_BYTES) {
            s_tts.dropped_chunk_count++;
            s_tts.rx_drop_current = true;
            s_tts.rx_drop_expected = (size_t)data->payload_len;
            s_tts.rx_drop_received = (size_t)data->data_len;
            llm_gateway_tts_ws_log_data_fragment(data,
                                                 fragment_complete,
                                                 s_tts.rx_drop_received);
            ESP_LOGW(TAG,
                     "TTS WS drop oversized payload payload_len=%d max=%u dropped_chunk_count=%u state=%s",
                     data->payload_len,
                     (unsigned int)LLM_GATEWAY_TTS_WS_PAYLOAD_MAX_BYTES,
                     (unsigned int)s_tts.dropped_chunk_count,
                     llm_gateway_tts_ws_state_name(s_tts.state));
            return;
        }
        s_tts.rx_buffer = (char *)malloc((size_t)data->payload_len + 1U);
        if (s_tts.rx_buffer == NULL) {
            llm_gateway_tts_ws_fail(ESP_ERR_NO_MEM, "TTS WS fragmented RX alloc failed");
            return;
        }
        s_tts.rx_buffer_expected = (size_t)data->payload_len;
        s_tts.rx_buffer_received = 0;
    }

    if (s_tts.rx_drop_current) {
        size_t dropped_received = (size_t)(data->payload_offset + data->data_len);
        if (dropped_received > s_tts.rx_drop_received) {
            s_tts.rx_drop_received = dropped_received;
        }
        llm_gateway_tts_ws_log_data_fragment(data,
                                             fragment_complete,
                                             s_tts.rx_drop_received);
        if (s_tts.rx_drop_received >= s_tts.rx_drop_expected) {
            s_tts.rx_drop_current = false;
            s_tts.rx_drop_expected = 0;
            s_tts.rx_drop_received = 0;
        }
        return;
    }

    if (s_tts.rx_buffer == NULL ||
        s_tts.rx_buffer_expected != (size_t)data->payload_len ||
        (size_t)data->payload_offset != s_tts.rx_buffer_received ||
        (size_t)data->payload_offset + (size_t)data->data_len > s_tts.rx_buffer_expected) {
        llm_gateway_tts_ws_log_data_fragment(data, fragment_complete, s_tts.rx_buffer_received);
        ESP_LOGW(TAG,
                 "TTS WS drop fragmented payload order invalid payload_len=%d payload_offset=%d data_len=%d rx_received=%u rx_expected=%u state=%s",
                 data->payload_len,
                 data->payload_offset,
                 data->data_len,
                 (unsigned int)s_tts.rx_buffer_received,
                 (unsigned int)s_tts.rx_buffer_expected,
                 llm_gateway_tts_ws_state_name(s_tts.state));
        llm_gateway_tts_ws_clear_rx_buffer();
        return;
    }

    memcpy(&s_tts.rx_buffer[data->payload_offset], data->data_ptr, (size_t)data->data_len);
    s_tts.rx_buffer_received += (size_t)data->data_len;
    fragment_complete = s_tts.rx_buffer_received >= s_tts.rx_buffer_expected;
    llm_gateway_tts_ws_log_data_fragment(data, fragment_complete, s_tts.rx_buffer_received);
    if (s_tts.rx_buffer_received >= s_tts.rx_buffer_expected) {
        s_tts.rx_buffer[s_tts.rx_buffer_expected] = '\0';
        ESP_LOGI(TAG,
                 "TTS WS full payload ready full_payload_len=%u state=%s",
                 (unsigned int)s_tts.rx_buffer_expected,
                 llm_gateway_tts_ws_state_name(s_tts.state));
        llm_gateway_tts_ws_handle_payload(s_tts.rx_buffer, s_tts.rx_buffer_expected);
        llm_gateway_tts_ws_clear_rx_buffer();
    }
}

static void llm_gateway_tts_ws_event_handler(void *handler_args,
                                             esp_event_base_t base,
                                             int32_t event_id,
                                             void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)llm_gateway_tts_ws_note_stack_high_water();

    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        s_tts.connected = true;
        llm_gateway_tts_ws_set_bits(LLM_GATEWAY_TTS_WS_CONNECTED_BIT);
        llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_CONNECTED, NULL, 0, false, 0, NULL);
        esp_err_t send_ret = llm_gateway_tts_ws_send_session_update();
        if (send_ret != ESP_OK) {
            llm_gateway_tts_ws_fail(send_ret, "TTS tts_session.update send failed");
        }
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        s_tts.connected = false;
        llm_gateway_tts_ws_log_disconnect_diag(event_id == WEBSOCKET_EVENT_CLOSED ?
                                               "TTS WS closed" : "TTS WS disconnected");
        if (s_tts.last_status_code != 0 ||
            s_tts.last_transport_error != ESP_OK ||
            s_tts.last_tls_error != ESP_OK ||
            s_tts.last_errno != 0) {
            llm_gateway_tts_ws_log_error_diag("TTS WS disconnected");
        }
        if (s_tts.state != TTS_WS_DONE && s_tts.state != TTS_WS_ERROR) {
            llm_gateway_tts_ws_set_state(TTS_WS_ERROR, "websocket disconnected before audio.done");
            llm_gateway_tts_ws_set_bits(LLM_GATEWAY_TTS_WS_ERROR_BIT);
        }
        llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_DISCONNECTED, NULL, 0, false, 0, NULL);
        break;
    case WEBSOCKET_EVENT_DATA:
        llm_gateway_tts_ws_handle_data(data);
        break;
    case WEBSOCKET_EVENT_ERROR:
        s_tts.connected = false;
        if (data != NULL) {
            s_tts.last_status_code = data->error_handle.esp_ws_handshake_status_code;
            s_tts.last_transport_error = data->error_handle.esp_tls_last_esp_err;
            s_tts.last_tls_error = data->error_handle.esp_tls_stack_err;
            s_tts.last_errno = data->error_handle.esp_transport_sock_errno;
        }
        llm_gateway_tts_ws_set_state(TTS_WS_ERROR, "websocket transport error");
        llm_gateway_tts_ws_log_error_diag("TTS WS transport error");
        llm_gateway_tts_ws_set_bits(LLM_GATEWAY_TTS_WS_ERROR_BIT);
        llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_ERROR,
                                NULL,
                                0,
                                false,
                                data != NULL ? data->error_handle.esp_tls_last_esp_err : ESP_FAIL,
                                "TTS WebSocket transport error");
        break;
    default:
        break;
    }
}

static esp_err_t llm_gateway_tts_ws_stop(void)
{
    esp_err_t close_ret = ESP_OK;
    esp_err_t stop_ret = ESP_OK;
    esp_err_t destroy_ret = ESP_OK;
    bool had_client = s_tts.client != NULL;
    bool was_connected = false;

    if (s_tts.client != NULL) {
        if (esp_websocket_client_is_connected(s_tts.client)) {
            was_connected = true;
            close_ret = esp_websocket_client_close(s_tts.client, pdMS_TO_TICKS(1000));
            stop_ret = esp_websocket_client_stop(s_tts.client);
        }
        destroy_ret = esp_websocket_client_destroy(s_tts.client);
    }
    llm_gateway_tts_ws_clear_rx_buffer();
    if (s_tts.event_group != NULL) {
        vEventGroupDelete(s_tts.event_group);
    }
    if (had_client && APP_DEBUG_LLM_GATEWAY_WS) {
        if (close_ret == ESP_OK && stop_ret == ESP_OK && destroy_ret == ESP_OK) {
            ESP_LOGD(TAG,
                     "TTS websocket closed: connected=%d close=%s stop=%s destroy=%s",
                     was_connected ? 1 : 0,
                     esp_err_to_name(close_ret),
                     esp_err_to_name(stop_ret),
                     esp_err_to_name(destroy_ret));
        } else {
            ESP_LOGD(TAG,
                     "TTS websocket closed with cleanup result: connected=%d close=%s stop=%s destroy=%s",
                     was_connected ? 1 : 0,
                     esp_err_to_name(close_ret),
                     esp_err_to_name(stop_ret),
                     esp_err_to_name(destroy_ret));
        }
    }
    memset(&s_tts, 0, sizeof(s_tts));
    if (close_ret != ESP_OK) {
        return close_ret;
    }
    if (stop_ret != ESP_OK) {
        return stop_ret;
    }
    return destroy_ret;
}

static esp_err_t llm_gateway_tts_ws_send_session_update(void)
{
    char *json = NULL;
    size_t json_len = 0;
    esp_err_t ret = llm_gateway_protocol_build_tts_ws_session_update(s_tts.tts_model,
                                                                     &json,
                                                                     &json_len);
    if (ret == ESP_OK) {
        ret = llm_gateway_tts_ws_send_json(json, json_len, "tts_session.update");
    }
    if (ret == ESP_OK) {
        s_tts.session_update_sent = true;
        llm_gateway_tts_ws_set_state(TTS_WS_SESSION_UPDATE_SENT, "tts_session.update sent");
    }
    llm_gateway_protocol_free(json);
    return ret;
}

static esp_err_t llm_gateway_tts_ws_send_text(const char *text)
{
    char *json = NULL;
    size_t json_len = 0;
    esp_err_t ret = llm_gateway_protocol_build_tts_ws_text_append(text,
                                                                  &json,
                                                                  &json_len);
    if (ret == ESP_OK) {
        llm_gateway_tts_ws_log_text_payload_check("input_text.append",
                                                  json,
                                                  json_len,
                                                  text);
    }
    if (ret == ESP_OK) {
        ret = llm_gateway_tts_ws_send_json(json, json_len, "input_text.append");
    }
    llm_gateway_protocol_free(json);
    if (ret != ESP_OK) {
        return ret;
    }
    s_tts.text_append_sent = true;
    llm_gateway_tts_ws_set_state(TTS_WS_TEXT_SENT, "input_text.append sent");

    vTaskDelay(pdMS_TO_TICKS(TTS_WS_APPEND_DONE_GAP_MS));

    json = NULL;
    json_len = 0;
    ret = llm_gateway_protocol_build_tts_ws_text_done(&json, &json_len);
    if (ret == ESP_OK) {
        llm_gateway_tts_ws_log_done_payload_check(json, json_len);
    }
    if (ret == ESP_OK) {
        ret = llm_gateway_tts_ws_send_json(json, json_len, "input_text.done");
    }
    if (ret == ESP_OK) {
        s_tts.input_done_sent = true;
        llm_gateway_tts_ws_set_state(TTS_WS_DONE_SENT, "input_text.done sent");
    }
    llm_gateway_protocol_free(json);
    return ret;
}

esp_err_t llm_gateway_tts_ws_synthesize(const llm_gateway_tts_ws_config_t *config,
                                        const char *text)
{
    if (config == NULL || config->tts_model == NULL || config->tts_model[0] == '\0' ||
        text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tts.client != NULL) {
        (void)llm_gateway_tts_ws_stop();
    }
    memset(&s_tts, 0, sizeof(s_tts));
    s_tts.tts_model = config->tts_model;
    s_tts.pending_text = text;
    s_tts.state = TTS_WS_CONNECTING;
    s_tts.config_buffer_size = LLM_TTS_WS_RX_BUFFER_SIZE;
    s_tts.event_cb = config->event_cb;
    s_tts.user_ctx = config->user_ctx;
    s_tts.event_group = xEventGroupCreate();
    if (s_tts.event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = volc_gateway_auth_build_tts_ws_headers(s_tts.headers, sizeof(s_tts.headers));
    if (ret != ESP_OK) {
        vEventGroupDelete(s_tts.event_group);
        memset(&s_tts, 0, sizeof(s_tts));
        return ret;
    }

    size_t header_len = strlen(s_tts.headers);
    char key_summary[48] = {0};
    volc_gateway_auth_make_key_summary(key_summary, sizeof(key_summary));
    ESP_LOGI(TAG,
             "TTS WS connect uri=%s model=%s voice=%s output_audio_format=%s output_audio_sample_rate=%d "
             "send_resource_id=%d auth_present=%d key=%s header_len=%u text_len=%u "
             "buffer_size=%u payload_max=%u tls_in=%d tls_out=%d ws_component_buffer=%d "
             "free_heap=%u min_free_heap=%u largest_free_block=%u state=%s",
             VOLC_GATEWAY_TTS_REALTIME_URI,
             s_tts.tts_model,
             LLM_TTS_VOICE,
             LLM_TTS_OUTPUT_FORMAT,
             LLM_TTS_SAMPLE_RATE,
             LLM_TTS_USE_RESOURCE_ID ? 1 : 0,
             VOLC_GATEWAY_API_KEY[0] != '\0' ? 1 : 0,
             key_summary,
             (unsigned int)header_len,
             (unsigned int)strlen(text),
             (unsigned int)s_tts.config_buffer_size,
             (unsigned int)LLM_GATEWAY_TTS_WS_PAYLOAD_MAX_BYTES,
             CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN,
             CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN,
             CONFIG_WS_BUFFER_SIZE,
             (unsigned int)llm_gateway_tts_ws_free_heap(),
             (unsigned int)llm_gateway_tts_ws_min_free_heap(),
             (unsigned int)llm_gateway_tts_ws_largest_free_block(),
             llm_gateway_tts_ws_state_name(s_tts.state));

    esp_websocket_client_config_t ws_config = {
        .uri = VOLC_GATEWAY_TTS_REALTIME_URI,
        .headers = s_tts.headers,
        .disable_auto_reconnect = true,
        .reconnect_timeout_ms = 0,
        .task_name = "llm_tts_ws",
        .task_stack = 8192,
        .task_prio = 4,
        .buffer_size = LLM_TTS_WS_RX_BUFFER_SIZE,
        .network_timeout_ms = LLM_GATEWAY_WS_CONNECT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    s_tts.config_buffer_size = (size_t)ws_config.buffer_size;
    ESP_LOGI(TAG,
             "TTS WS config actual buffer_size=%u sdk_ws_buffer_size=%d tls_in=%d tls_out=%d",
             (unsigned int)s_tts.config_buffer_size,
             CONFIG_WS_BUFFER_SIZE,
             CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN,
             CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN);

    s_tts.client = esp_websocket_client_init(&ws_config);
    if (s_tts.client == NULL) {
        vEventGroupDelete(s_tts.event_group);
        memset(&s_tts, 0, sizeof(s_tts));
        return ESP_ERR_NO_MEM;
    }

    ret = esp_websocket_register_events(s_tts.client,
                                        WEBSOCKET_EVENT_ANY,
                                        llm_gateway_tts_ws_event_handler,
                                        NULL);
    if (ret != ESP_OK) {
        (void)llm_gateway_tts_ws_stop();
        return ret;
    }

    ret = esp_websocket_client_start(s_tts.client);
    if (ret != ESP_OK) {
        (void)llm_gateway_tts_ws_stop();
        return ret;
    }

    EventBits_t bits = xEventGroupWaitBits(s_tts.event_group,
                                           LLM_GATEWAY_TTS_WS_CONNECTED_BIT | LLM_GATEWAY_TTS_WS_ERROR_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(LLM_GATEWAY_WS_CONNECT_TIMEOUT_MS));
    if (bits & LLM_GATEWAY_TTS_WS_ERROR_BIT) {
        ret = (s_tts.last_status_code == 401 || s_tts.last_status_code == 403) ?
            ESP_ERR_INVALID_RESPONSE : ESP_FAIL;
        (void)llm_gateway_tts_ws_stop();
        return ret;
    }
    if ((bits & LLM_GATEWAY_TTS_WS_CONNECTED_BIT) == 0) {
        ret = ESP_ERR_TIMEOUT;
        llm_gateway_tts_ws_set_state(TTS_WS_ERROR, "connect timeout");
        llm_gateway_tts_ws_log_disconnect_diag("TTS WS connect timeout");
        (void)llm_gateway_tts_ws_stop();
        return ret;
    }

    bits = xEventGroupWaitBits(s_tts.event_group,
                               LLM_GATEWAY_TTS_WS_SESSION_UPDATED_BIT | LLM_GATEWAY_TTS_WS_ERROR_BIT,
                               pdTRUE,
                               pdFALSE,
                               pdMS_TO_TICKS(TTS_WS_FIRST_RESPONSE_TIMEOUT_MS));
    if (bits & LLM_GATEWAY_TTS_WS_ERROR_BIT) {
        (void)llm_gateway_tts_ws_stop();
        return ESP_FAIL;
    }
    if ((bits & LLM_GATEWAY_TTS_WS_SESSION_UPDATED_BIT) == 0) {
        ESP_LOGW(TAG, "TTS first response/session.updated timeout after %d ms", TTS_WS_FIRST_RESPONSE_TIMEOUT_MS);
        llm_gateway_tts_ws_set_state(TTS_WS_ERROR, "session.updated timeout");
        llm_gateway_tts_ws_log_disconnect_diag("TTS WS first response timeout");
        (void)llm_gateway_tts_ws_stop();
        return ESP_ERR_TIMEOUT;
    }

    bits = xEventGroupWaitBits(s_tts.event_group,
                               LLM_GATEWAY_TTS_WS_AUDIO_DONE_BIT | LLM_GATEWAY_TTS_WS_ERROR_BIT,
                               pdTRUE,
                               pdFALSE,
                               pdMS_TO_TICKS(TTS_WS_AUDIO_DONE_TIMEOUT_MS));
    if (bits & LLM_GATEWAY_TTS_WS_ERROR_BIT) {
        ret = ESP_FAIL;
    } else if ((bits & LLM_GATEWAY_TTS_WS_AUDIO_DONE_BIT) == 0) {
        ESP_LOGW(TAG, "TTS audio.done timeout after %d ms", TTS_WS_AUDIO_DONE_TIMEOUT_MS);
        llm_gateway_tts_ws_set_state(TTS_WS_ERROR, "audio.done timeout");
        llm_gateway_tts_ws_log_disconnect_diag("TTS WS audio.done timeout");
        ret = ESP_ERR_TIMEOUT;
    } else if (!s_tts.audio_received) {
        ESP_LOGW(TAG, "TTS audio.done received without audio delta");
        ret = ESP_ERR_NOT_FOUND;
    } else {
        ESP_LOGI(TAG,
                 "TTS synth finished audio_delta_count=%u pcm_bytes_total=%u dropped_chunk_count=%u",
                 (unsigned int)s_tts.audio_delta_count,
                 (unsigned int)s_tts.pcm_bytes_total,
                 (unsigned int)s_tts.dropped_chunk_count);
        ret = ESP_OK;
    }
    llm_gateway_tts_ws_log_round_summary("synthesize_complete", ret);

    esp_err_t close_ret = llm_gateway_tts_ws_stop();
    if (ret == ESP_OK && close_ret != ESP_OK && APP_DEBUG_LLM_GATEWAY_WS) {
        ESP_LOGD(TAG, "TTS close cleanup result after synth complete: %s", esp_err_to_name(close_ret));
    }
    return ret;
}
