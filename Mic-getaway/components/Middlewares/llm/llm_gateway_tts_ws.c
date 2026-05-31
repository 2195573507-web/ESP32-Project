#include "llm_gateway_tts_ws.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "llm_config.h"
#include "llm_gateway_protocol.h"
#include "volc_gateway_auth.h"

static const char *TAG = "llm_gateway_tts_ws";

enum {
    LLM_GATEWAY_TTS_WS_CONNECTED_BIT = BIT0,
    LLM_GATEWAY_TTS_WS_SESSION_UPDATED_BIT = BIT1,
    LLM_GATEWAY_TTS_WS_AUDIO_DONE_BIT = BIT2,
    LLM_GATEWAY_TTS_WS_ERROR_BIT = BIT3,
};

typedef struct {
    esp_websocket_client_handle_t client;
    EventGroupHandle_t event_group;
    const char *tts_model;
    llm_gateway_tts_ws_event_cb_t event_cb;
    void *user_ctx;
    char *rx_buffer;
    size_t rx_buffer_len;
    size_t rx_buffer_expected;
    char headers[320];
    uint8_t audio_buffer[LLM_GATEWAY_TTS_AUDIO_CHUNK_MAX_BYTES];
    int last_status_code;
    esp_err_t last_transport_error;
    esp_err_t last_tls_error;
    int last_errno;
    bool connected;
    bool audio_received;
} llm_gateway_tts_ws_state_t;

static llm_gateway_tts_ws_state_t s_tts;

static void llm_gateway_tts_ws_emit(llm_gateway_tts_ws_event_type_t type,
                                    const uint8_t *audio,
                                    size_t audio_len,
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
        .code = code,
        .message = message,
    };
    s_tts.event_cb(&event, s_tts.user_ctx);
}

static void llm_gateway_tts_ws_clear_rx_buffer(void)
{
    free(s_tts.rx_buffer);
    s_tts.rx_buffer = NULL;
    s_tts.rx_buffer_len = 0;
    s_tts.rx_buffer_expected = 0;
}

static esp_err_t llm_gateway_tts_ws_send_json(char *json,
                                              size_t json_len,
                                              const char *event_name)
{
    if (json == NULL || json_len == 0 || event_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tts.client == NULL || !s_tts.connected ||
        !esp_websocket_client_is_connected(s_tts.client)) {
        return ESP_ERR_INVALID_STATE;
    }

    int sent = esp_websocket_client_send_text(s_tts.client,
                                              json,
                                              (int)json_len,
                                              pdMS_TO_TICKS(LLM_GATEWAY_WS_SEND_TIMEOUT_MS));
    esp_err_t ret = sent < 0 ? ESP_FAIL : ESP_OK;
    ESP_LOGI(TAG, "TTS %s send result: %s", event_name, esp_err_to_name(ret));
    return ret;
}

static void llm_gateway_tts_ws_log_reject_hint(void)
{
    if (s_tts.last_status_code == 401 || s_tts.last_status_code == 403) {
        ESP_LOGE(TAG,
                 "Gateway TTS rejected: check Authorization Bearer API key, "
                 "check whether API key is bound to model %s, check model query parameter, "
                 "check whether X-Api-Resource-Id should be omitted for preset models.",
                 VOLC_GATEWAY_TTS_MODEL);
    }
}

static void llm_gateway_tts_ws_log_error_diag(const char *reason)
{
    char key_summary[48] = {0};
    volc_gateway_auth_make_key_summary(key_summary, sizeof(key_summary));
    ESP_LOGE(TAG,
             "%s uri=%s model=%s gateway_mode=%d headers=[Authorization%s] key=%s status=%d transport_err=0x%x tls_err=0x%x errno=%d",
             reason != NULL ? reason : "TTS WS error",
             VOLC_GATEWAY_TTS_REALTIME_URI,
             VOLC_GATEWAY_TTS_MODEL,
             LLM_CLIENT_USE_VOLC_GATEWAY,
#if VOLC_GATEWAY_TTS_USE_RESOURCE_ID
             ",X-Api-Resource-Id",
#else
             "",
#endif
             key_summary,
             s_tts.last_status_code,
             (unsigned int)s_tts.last_transport_error,
             (unsigned int)s_tts.last_tls_error,
             s_tts.last_errno);
    llm_gateway_tts_ws_log_reject_hint();
}

static void llm_gateway_tts_ws_handle_payload(const char *payload, size_t payload_len)
{
    llm_gateway_tts_event_t parsed = {0};
    esp_err_t ret = llm_gateway_protocol_parse_tts_ws_event(payload,
                                                            payload_len,
                                                            s_tts.audio_buffer,
                                                            sizeof(s_tts.audio_buffer),
                                                            &parsed);
    if (ret != ESP_OK) {
        size_t preview_len = payload_len < 256U ? payload_len : 256U;
        ESP_LOGW(TAG,
                 "TTS WS JSON parse failed: ret=%s payload_len=%u preview=%.*s",
                 esp_err_to_name(ret),
                 (unsigned int)payload_len,
                 (int)preview_len,
                 payload);
        xEventGroupSetBits(s_tts.event_group, LLM_GATEWAY_TTS_WS_ERROR_BIT);
        llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_ERROR,
                                NULL,
                                0,
                                ret,
                                "TTS WS parse failed");
        return;
    }

    ESP_LOGI(TAG,
             "TTS WS event type=%s audio_len=%u done=%d error=%d",
             parsed.type[0] != '\0' ? parsed.type : "<unknown>",
             (unsigned int)parsed.audio_len,
             parsed.is_audio_done ? 1 : 0,
             parsed.is_error ? 1 : 0);

    if (parsed.is_error) {
        xEventGroupSetBits(s_tts.event_group, LLM_GATEWAY_TTS_WS_ERROR_BIT);
        llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_ERROR,
                                NULL,
                                0,
                                parsed.code,
                                parsed.message[0] != '\0' ? parsed.message : "TTS WebSocket error");
        return;
    }
    if (parsed.is_session_updated) {
        xEventGroupSetBits(s_tts.event_group, LLM_GATEWAY_TTS_WS_SESSION_UPDATED_BIT);
        llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_SESSION_UPDATED,
                                NULL,
                                0,
                                0,
                                NULL);
        return;
    }
    if (parsed.is_audio_delta && parsed.audio_len > 0) {
        s_tts.audio_received = true;
        llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_AUDIO_DELTA,
                                s_tts.audio_buffer,
                                parsed.audio_len,
                                0,
                                NULL);
        return;
    }
    if (parsed.is_audio_done) {
        xEventGroupSetBits(s_tts.event_group, LLM_GATEWAY_TTS_WS_AUDIO_DONE_BIT);
        llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_AUDIO_DONE,
                                NULL,
                                0,
                                0,
                                NULL);
    }
}

static void llm_gateway_tts_ws_handle_data(const esp_websocket_event_data_t *data)
{
    if (data == NULL || data->data_ptr == NULL || data->data_len <= 0) {
        return;
    }

    if (data->payload_len <= data->data_len && data->payload_offset == 0) {
        char *payload = (char *)malloc((size_t)data->data_len + 1U);
        if (payload == NULL) {
            xEventGroupSetBits(s_tts.event_group, LLM_GATEWAY_TTS_WS_ERROR_BIT);
            llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_ERROR,
                                    NULL,
                                    0,
                                    ESP_ERR_NO_MEM,
                                    "TTS WS RX alloc failed");
            return;
        }
        memcpy(payload, data->data_ptr, (size_t)data->data_len);
        payload[data->data_len] = '\0';
        llm_gateway_tts_ws_handle_payload(payload, (size_t)data->data_len);
        free(payload);
        return;
    }

    if (data->payload_offset == 0) {
        llm_gateway_tts_ws_clear_rx_buffer();
        if (data->payload_len <= 0 ||
            data->payload_len > LLM_GATEWAY_TTS_WS_PAYLOAD_MAX_BYTES) {
            xEventGroupSetBits(s_tts.event_group, LLM_GATEWAY_TTS_WS_ERROR_BIT);
            llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_ERROR,
                                    NULL,
                                    0,
                                    ESP_ERR_INVALID_SIZE,
                                    "TTS WS payload too large");
            return;
        }
        s_tts.rx_buffer = (char *)malloc((size_t)data->payload_len + 1U);
        if (s_tts.rx_buffer == NULL) {
            xEventGroupSetBits(s_tts.event_group, LLM_GATEWAY_TTS_WS_ERROR_BIT);
            llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_ERROR,
                                    NULL,
                                    0,
                                    ESP_ERR_NO_MEM,
                                    "TTS WS fragmented RX alloc failed");
            return;
        }
        s_tts.rx_buffer_expected = (size_t)data->payload_len;
        s_tts.rx_buffer_len = 0;
    }

    if (s_tts.rx_buffer == NULL ||
        (size_t)data->payload_offset != s_tts.rx_buffer_len ||
        s_tts.rx_buffer_len + (size_t)data->data_len > s_tts.rx_buffer_expected) {
        llm_gateway_tts_ws_clear_rx_buffer();
        xEventGroupSetBits(s_tts.event_group, LLM_GATEWAY_TTS_WS_ERROR_BIT);
        llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_ERROR,
                                NULL,
                                0,
                                ESP_ERR_INVALID_STATE,
                                "TTS WS fragmented RX order invalid");
        return;
    }

    memcpy(&s_tts.rx_buffer[s_tts.rx_buffer_len], data->data_ptr, (size_t)data->data_len);
    s_tts.rx_buffer_len += (size_t)data->data_len;
    if (s_tts.rx_buffer_len == s_tts.rx_buffer_expected) {
        s_tts.rx_buffer[s_tts.rx_buffer_len] = '\0';
        llm_gateway_tts_ws_handle_payload(s_tts.rx_buffer, s_tts.rx_buffer_len);
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

    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_tts.connected = true;
        xEventGroupSetBits(s_tts.event_group, LLM_GATEWAY_TTS_WS_CONNECTED_BIT);
        llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_CONNECTED, NULL, 0, 0, NULL);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        s_tts.connected = false;
        if (s_tts.last_status_code != 0 ||
            s_tts.last_transport_error != ESP_OK ||
            s_tts.last_tls_error != ESP_OK ||
            s_tts.last_errno != 0) {
            llm_gateway_tts_ws_log_error_diag("TTS WS disconnected");
        }
        llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_DISCONNECTED, NULL, 0, 0, NULL);
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
        llm_gateway_tts_ws_log_error_diag("TTS WS transport error");
        xEventGroupSetBits(s_tts.event_group, LLM_GATEWAY_TTS_WS_ERROR_BIT);
        llm_gateway_tts_ws_emit(LLM_GATEWAY_TTS_WS_EVENT_ERROR,
                                NULL,
                                0,
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
    if (had_client) {
        if (close_ret == ESP_OK && stop_ret == ESP_OK && destroy_ret == ESP_OK) {
            ESP_LOGI(TAG,
                     "TTS websocket closed: connected=%d close=%s stop=%s destroy=%s",
                     was_connected ? 1 : 0,
                     esp_err_to_name(close_ret),
                     esp_err_to_name(stop_ret),
                     esp_err_to_name(destroy_ret));
        } else {
            ESP_LOGW(TAG,
                     "TTS websocket closed with warning: connected=%d close=%s stop=%s destroy=%s",
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
        ret = llm_gateway_tts_ws_send_json(json, json_len, "input_text.append");
    }
    llm_gateway_protocol_free(json);
    if (ret != ESP_OK) {
        return ret;
    }

    json = NULL;
    json_len = 0;
    ret = llm_gateway_protocol_build_tts_ws_text_done(&json, &json_len);
    if (ret == ESP_OK) {
        ret = llm_gateway_tts_ws_send_json(json, json_len, "input_text.done");
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
             "TTS WS connect uri=%s model=%s voice=%s format=%s sample_rate=%d key=%s headers=[Authorization%s] header_len=%u text_len=%u",
             VOLC_GATEWAY_TTS_REALTIME_URI,
             s_tts.tts_model,
             VOLC_GATEWAY_TTS_VOICE,
             VOLC_GATEWAY_TTS_OUTPUT_FORMAT,
             VOLC_GATEWAY_TTS_OUTPUT_SAMPLE_RATE,
             key_summary,
#if VOLC_GATEWAY_TTS_USE_RESOURCE_ID
             ",X-Api-Resource-Id",
#else
             "",
#endif
             (unsigned int)header_len,
             (unsigned int)strlen(text));

    esp_websocket_client_config_t ws_config = {
        .uri = VOLC_GATEWAY_TTS_REALTIME_URI,
        .headers = s_tts.headers,
        .disable_auto_reconnect = true,
        .reconnect_timeout_ms = 0,
        .task_name = "llm_tts_ws",
        .task_stack = 8192,
        .task_prio = 4,
        .buffer_size = LLM_GATEWAY_WS_BUFFER_BYTES,
        .network_timeout_ms = LLM_GATEWAY_WS_CONNECT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

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
    if ((bits & LLM_GATEWAY_TTS_WS_CONNECTED_BIT) == 0) {
        ret = ESP_ERR_TIMEOUT;
        if (bits & LLM_GATEWAY_TTS_WS_ERROR_BIT) {
            ret = (s_tts.last_status_code == 401 || s_tts.last_status_code == 403) ?
                ESP_ERR_INVALID_RESPONSE : ESP_FAIL;
        }
        (void)llm_gateway_tts_ws_stop();
        return ret;
    }

    ret = llm_gateway_tts_ws_send_session_update();
    if (ret != ESP_OK) {
        (void)llm_gateway_tts_ws_stop();
        return ret;
    }

    bits = xEventGroupWaitBits(s_tts.event_group,
                               LLM_GATEWAY_TTS_WS_SESSION_UPDATED_BIT | LLM_GATEWAY_TTS_WS_ERROR_BIT,
                               pdTRUE,
                               pdFALSE,
                               pdMS_TO_TICKS(LLM_GATEWAY_WS_CONNECT_TIMEOUT_MS));
    if (bits & LLM_GATEWAY_TTS_WS_ERROR_BIT) {
        (void)llm_gateway_tts_ws_stop();
        return ESP_FAIL;
    }
    if ((bits & LLM_GATEWAY_TTS_WS_SESSION_UPDATED_BIT) == 0) {
        ESP_LOGW(TAG, "TTS session.updated timeout after %d ms", LLM_GATEWAY_WS_CONNECT_TIMEOUT_MS);
        (void)llm_gateway_tts_ws_stop();
        return ESP_ERR_TIMEOUT;
    }

    ret = llm_gateway_tts_ws_send_text(text);
    if (ret != ESP_OK) {
        (void)llm_gateway_tts_ws_stop();
        return ret;
    }

    bits = xEventGroupWaitBits(s_tts.event_group,
                               LLM_GATEWAY_TTS_WS_AUDIO_DONE_BIT | LLM_GATEWAY_TTS_WS_ERROR_BIT,
                               pdTRUE,
                               pdFALSE,
                               pdMS_TO_TICKS(LLM_GATEWAY_TTS_FINAL_TIMEOUT_MS));
    if (bits & LLM_GATEWAY_TTS_WS_ERROR_BIT) {
        ret = ESP_FAIL;
    } else if ((bits & LLM_GATEWAY_TTS_WS_AUDIO_DONE_BIT) == 0) {
        ESP_LOGW(TAG, "TTS audio.done timeout after %d ms", LLM_GATEWAY_TTS_FINAL_TIMEOUT_MS);
        ret = ESP_ERR_TIMEOUT;
    } else if (!s_tts.audio_received) {
        ESP_LOGW(TAG, "TTS audio.done received without audio delta");
        ret = ESP_ERR_NOT_FOUND;
    } else {
        ESP_LOGI(TAG, "TTS synth finished");
        ret = ESP_OK;
    }

    esp_err_t close_ret = llm_gateway_tts_ws_stop();
    if (ret == ESP_OK && close_ret != ESP_OK) {
        ESP_LOGW(TAG, "TTS close warning ignored after synth complete: %s", esp_err_to_name(close_ret));
    }
    return ret;
}
