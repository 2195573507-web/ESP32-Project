#include "llm_gateway_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "volc_gateway_auth.h"

static const char *TAG = "llm_gateway_proto";

#define LLM_GATEWAY_CHAT_PARSE_PREVIEW_BYTES     1024U
#define LLM_GATEWAY_CHAT_REASONING_PREVIEW_BYTES 160U

static const cJSON *llm_gateway_protocol_get_path(const cJSON *root,
                                                  const char *first,
                                                  const char *second,
                                                  const char *third)
{
    const cJSON *item = root;
    if (first != NULL) {
        item = cJSON_GetObjectItemCaseSensitive(item, first);
    }
    if (item != NULL && second != NULL) {
        item = cJSON_GetObjectItemCaseSensitive(item, second);
    }
    if (item != NULL && third != NULL) {
        item = cJSON_GetObjectItemCaseSensitive(item, third);
    }
    return item;
}

static bool llm_gateway_protocol_copy_json_string(const cJSON *item,
                                                  char *out,
                                                  size_t out_size)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        item->valuestring[0] == '\0' || out == NULL || out_size == 0) {
        return false;
    }

    strlcpy(out, item->valuestring, out_size);
    return true;
}

static bool llm_gateway_protocol_append_json_string(const cJSON *item,
                                                    char *out,
                                                    size_t out_size)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        item->valuestring[0] == '\0' || out == NULL || out_size == 0) {
        return false;
    }

    size_t used = strlen(out);
    if (used + 1U >= out_size) {
        return false;
    }

    strlcpy(out + used, item->valuestring, out_size - used);
    return true;
}

static const char *llm_gateway_protocol_trim_leading_chat_body(const char *body)
{
    const unsigned char *cursor = (const unsigned char *)body;
    bool moved = true;
    while (moved) {
        moved = false;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
               *cursor == '\n' || *cursor == '\v' || *cursor == '\f') {
            cursor++;
            moved = true;
        }
        if (cursor[0] == 0xEF && cursor[1] == 0xBB && cursor[2] == 0xBF) {
            cursor += 3;
            moved = true;
        }
    }
    return (const char *)cursor;
}

static const char *llm_gateway_protocol_trim_line_left(const char *line, size_t *line_len)
{
    while (*line_len > 0 &&
           (line[0] == ' ' || line[0] == '\t' || line[0] == '\r')) {
        line++;
        (*line_len)--;
    }
    return line;
}

static void llm_gateway_protocol_trim_line_right(size_t *line_len, const char *line)
{
    while (*line_len > 0) {
        char last = line[*line_len - 1U];
        if (last != ' ' && last != '\t' && last != '\r') {
            break;
        }
        (*line_len)--;
    }
}

static void llm_gateway_protocol_log_parse_diagnostics(const char *body,
                                                       const char *parser_mode,
                                                       const char *payload,
                                                       const char *error_ptr)
{
    const char *safe_body = body != NULL ? body : "";
    const char *safe_mode = parser_mode != NULL ? parser_mode : "unknown";
    const bool log_body_preview = APP_DEBUG_LLM_GATEWAY_HTTP || APP_DEBUG_LLM_GATEWAY_PROTO;
    size_t body_len = strlen(safe_body);
    size_t preview_len = body_len;
    if (preview_len > LLM_GATEWAY_CHAT_PARSE_PREVIEW_BYTES) {
        preview_len = LLM_GATEWAY_CHAT_PARSE_PREVIEW_BYTES;
    }

    ESP_LOGW(TAG,
             "Chat response parse failed: body_len=%u parser_mode=%s",
             (unsigned int)body_len,
             safe_mode);
    if (log_body_preview) {
        ESP_LOGW(TAG, "Chat response body_preview: %.*s", (int)preview_len, safe_body);
    }

    if (error_ptr == NULL) {
        ESP_LOGW(TAG, "cJSON_Parse error_ptr=<null>");
        return;
    }

    const char *safe_payload = payload != NULL ? payload : safe_body;
    size_t payload_len = strlen(safe_payload);
    if (error_ptr >= safe_payload && error_ptr <= safe_payload + payload_len) {
        size_t error_offset = (size_t)(error_ptr - safe_payload);
        ESP_LOGW(TAG,
                 "cJSON_Parse error_offset=%u error_ptr=%p",
                 (unsigned int)error_offset,
                 (const void *)error_ptr);
        if (log_body_preview) {
            ESP_LOGW(TAG, "cJSON_Parse near: %.*s", 96, error_ptr);
        }
    } else {
        ESP_LOGW(TAG, "cJSON_Parse error_ptr=%p", (const void *)error_ptr);
        if (log_body_preview) {
            ESP_LOGW(TAG, "cJSON_Parse near: %.*s", 96, error_ptr);
        }
    }
}

static void llm_gateway_protocol_log_json_string_preview(const char *field,
                                                         const cJSON *item)
{
    if (!APP_DEBUG_LLM_GATEWAY_PROTO) {
        return;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL || item->valuestring[0] == '\0') {
        return;
    }

    size_t preview_len = strlen(item->valuestring);
    if (preview_len > LLM_GATEWAY_CHAT_REASONING_PREVIEW_BYTES) {
        preview_len = LLM_GATEWAY_CHAT_REASONING_PREVIEW_BYTES;
    }
    ESP_LOGI(TAG,
             "Chat %s ignored as final reply: %.*s",
             field != NULL ? field : "reasoning_content",
             (int)preview_len,
             item->valuestring);
}

static void llm_gateway_protocol_log_reasoning_candidates(const cJSON *choice)
{
    if (choice == NULL) {
        return;
    }

    llm_gateway_protocol_log_json_string_preview(
        "message.reasoning_content",
        llm_gateway_protocol_get_path(choice, "message", "reasoning_content", NULL));
    llm_gateway_protocol_log_json_string_preview(
        "message.thinking_content",
        llm_gateway_protocol_get_path(choice, "message", "thinking_content", NULL));
    llm_gateway_protocol_log_json_string_preview(
        "delta.reasoning_content",
        llm_gateway_protocol_get_path(choice, "delta", "reasoning_content", NULL));
    llm_gateway_protocol_log_json_string_preview(
        "delta.thinking_content",
        llm_gateway_protocol_get_path(choice, "delta", "thinking_content", NULL));
    llm_gateway_protocol_log_json_string_preview(
        "reasoning_content",
        cJSON_GetObjectItemCaseSensitive(choice, "reasoning_content"));
    llm_gateway_protocol_log_json_string_preview(
        "thinking_content",
        cJSON_GetObjectItemCaseSensitive(choice, "thinking_content"));
}

static bool llm_gateway_protocol_extract_text_candidates(const cJSON *root,
                                                         char *out_text,
                                                         size_t out_size)
{
    const cJSON *item = llm_gateway_protocol_get_path(root, "choices", NULL, NULL);
    if (cJSON_IsArray(item)) {
        const cJSON *choice = cJSON_GetArrayItem(item, 0);
        if (llm_gateway_protocol_copy_json_string(llm_gateway_protocol_get_path(choice, "message", "content", NULL), out_text, out_size) ||
            llm_gateway_protocol_copy_json_string(llm_gateway_protocol_get_path(choice, "delta", "content", NULL), out_text, out_size) ||
            llm_gateway_protocol_copy_json_string(cJSON_GetObjectItemCaseSensitive(choice, "text"), out_text, out_size)) {
            return true;
        }
        llm_gateway_protocol_log_reasoning_candidates(choice);
    }

    if (llm_gateway_protocol_copy_json_string(cJSON_GetObjectItemCaseSensitive(root, "text"), out_text, out_size) ||
        llm_gateway_protocol_copy_json_string(cJSON_GetObjectItemCaseSensitive(root, "transcript"), out_text, out_size) ||
        llm_gateway_protocol_copy_json_string(cJSON_GetObjectItemCaseSensitive(root, "delta"), out_text, out_size) ||
        llm_gateway_protocol_copy_json_string(cJSON_GetObjectItemCaseSensitive(root, "result"), out_text, out_size) ||
        llm_gateway_protocol_copy_json_string(cJSON_GetObjectItemCaseSensitive(root, "output_text"), out_text, out_size) ||
        llm_gateway_protocol_copy_json_string(llm_gateway_protocol_get_path(root, "result", "text", NULL), out_text, out_size) ||
        llm_gateway_protocol_copy_json_string(llm_gateway_protocol_get_path(root, "result", "transcript", NULL), out_text, out_size) ||
        llm_gateway_protocol_copy_json_string(llm_gateway_protocol_get_path(root, "data", "transcript", NULL), out_text, out_size) ||
        llm_gateway_protocol_copy_json_string(llm_gateway_protocol_get_path(root, "data", "text", NULL), out_text, out_size)) {
        return true;
    }

    return false;
}

static bool llm_gateway_protocol_append_sse_text_candidates(const cJSON *root,
                                                            char *out_text,
                                                            size_t out_size)
{
    const cJSON *item = llm_gateway_protocol_get_path(root, "choices", NULL, NULL);
    if (!cJSON_IsArray(item)) {
        return false;
    }

    const cJSON *choice = cJSON_GetArrayItem(item, 0);
    if (llm_gateway_protocol_append_json_string(llm_gateway_protocol_get_path(choice, "delta", "content", NULL), out_text, out_size) ||
        llm_gateway_protocol_append_json_string(llm_gateway_protocol_get_path(choice, "message", "content", NULL), out_text, out_size)) {
        return true;
    }

    llm_gateway_protocol_log_reasoning_candidates(choice);
    return false;
}

static esp_err_t llm_gateway_protocol_parse_chat_json(const char *body,
                                                      const char *json,
                                                      char *out_text,
                                                      size_t out_size)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        llm_gateway_protocol_log_parse_diagnostics(body, "json", json, cJSON_GetErrorPtr());
        return ESP_FAIL;
    }

    bool found = llm_gateway_protocol_extract_text_candidates(root, out_text, out_size);
    cJSON_Delete(root);
    if (!found) {
        llm_gateway_protocol_log_parse_diagnostics(body, "json", json, NULL);
    }
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t llm_gateway_protocol_parse_chat_sse(const char *body,
                                                     const char *sse,
                                                     char *out_text,
                                                     size_t out_size)
{
    bool parsed_payload = false;
    bool found_text = false;
    const char *line = sse;

    while (line != NULL && line[0] != '\0') {
        const char *line_end = strchr(line, '\n');
        size_t line_len = line_end != NULL ? (size_t)(line_end - line) : strlen(line);
        const char *line_start = llm_gateway_protocol_trim_line_left(line, &line_len);
        llm_gateway_protocol_trim_line_right(&line_len, line_start);

        if (line_len >= 5U && strncmp(line_start, "data:", 5) == 0) {
            const char *payload = line_start + 5;
            size_t payload_len = line_len - 5U;
            payload = llm_gateway_protocol_trim_line_left(payload, &payload_len);
            llm_gateway_protocol_trim_line_right(&payload_len, payload);

            if (payload_len > 0 &&
                !(payload_len == 6U && strncmp(payload, "[DONE]", 6) == 0)) {
                char *payload_json = (char *)malloc(payload_len + 1U);
                if (payload_json == NULL) {
                    return ESP_ERR_NO_MEM;
                }
                memcpy(payload_json, payload, payload_len);
                payload_json[payload_len] = '\0';

                cJSON *root = cJSON_Parse(payload_json);
                if (root == NULL) {
                    llm_gateway_protocol_log_parse_diagnostics(body, "sse", payload_json, cJSON_GetErrorPtr());
                    free(payload_json);
                    return ESP_FAIL;
                }

                parsed_payload = true;
                if (llm_gateway_protocol_append_sse_text_candidates(root, out_text, out_size)) {
                    found_text = true;
                }
                cJSON_Delete(root);
                free(payload_json);
            }
        }

        if (line_end == NULL) {
            break;
        }
        line = line_end + 1;
    }

    if (found_text) {
        return ESP_OK;
    }

    llm_gateway_protocol_log_parse_diagnostics(body, "sse", sse, NULL);
    return parsed_payload ? ESP_ERR_NOT_FOUND : ESP_FAIL;
}

esp_err_t llm_gateway_protocol_build_auth_header(char *out, size_t out_size)
{
    return volc_gateway_auth_build_authorization(out, out_size);
}

void llm_gateway_protocol_make_key_summary(char *out, size_t out_size)
{
    volc_gateway_auth_make_key_summary(out, out_size);
}

bool llm_gateway_protocol_config_has_placeholders(void)
{
    return volc_gateway_auth_has_placeholder();
}

esp_err_t llm_gateway_protocol_build_chat_request(const char *model,
                                                  const char *system_prompt,
                                                  const char *user_text,
                                                  char **out_json,
                                                  size_t *out_len)
{
    if (model == NULL || model[0] == '\0' ||
        user_text == NULL || user_text[0] == '\0' ||
        out_json == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    *out_len = 0;

    const char *prompt = system_prompt != NULL && system_prompt[0] != '\0' ?
        system_prompt : LLM_GATEWAY_SYSTEM_PROMPT;

    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    cJSON *system = cJSON_CreateObject();
    cJSON *user = cJSON_CreateObject();
    if (root == NULL || messages == NULL || system == NULL || user == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(messages);
        cJSON_Delete(system);
        cJSON_Delete(user);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddNumberToObject(root, "temperature", 0.2);
    cJSON_AddStringToObject(system, "role", "system");
    cJSON_AddStringToObject(system, "content", prompt);
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", user_text);
    cJSON_AddItemToArray(messages, system);
    cJSON_AddItemToArray(messages, user);
    cJSON_AddItemToObject(root, "messages", messages);
    cJSON_AddBoolToObject(root, "stream", false);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_json = json;
    *out_len = strlen(json);
    return ESP_OK;
}

esp_err_t llm_gateway_protocol_parse_chat_response(const char *json,
                                                   char *out_text,
                                                   size_t out_size)
{
    if (json == NULL || out_text == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_text[0] = '\0';

    const char *body = json;
    const char *trimmed = llm_gateway_protocol_trim_leading_chat_body(body);
    if (strncmp(trimmed, "data:", 5) == 0) {
        return llm_gateway_protocol_parse_chat_sse(body, trimmed, out_text, out_size);
    }

    if (trimmed[0] == '{') {
        return llm_gateway_protocol_parse_chat_json(body, trimmed, out_text, out_size);
    }

    llm_gateway_protocol_log_parse_diagnostics(body, "unknown", trimmed, NULL);
    return ESP_FAIL;
}

esp_err_t llm_gateway_protocol_build_asr_ws_start_event(const char *model,
                                                        char **out_json,
                                                        size_t *out_len)
{
    if (model == NULL || model[0] == '\0' ||
        out_json == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    *out_len = 0;

    cJSON *root = cJSON_CreateObject();
    cJSON *session = cJSON_CreateObject();
    cJSON *transcription = cJSON_CreateObject();
    if (root == NULL || session == NULL || transcription == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(session);
        cJSON_Delete(transcription);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "transcription_session.update");
    cJSON_AddItemToObject(root, "session", session);
    cJSON_AddStringToObject(session, "input_audio_format", LLM_GATEWAY_AUDIO_FORMAT);
    cJSON_AddStringToObject(session, "input_audio_codec", LLM_GATEWAY_AUDIO_CODEC);
    cJSON_AddNumberToObject(session, "input_audio_sample_rate", LLM_GATEWAY_AUDIO_SAMPLE_RATE);
    cJSON_AddNumberToObject(session, "input_audio_bits", LLM_GATEWAY_AUDIO_BITS);
    cJSON_AddNumberToObject(session, "input_audio_channel", LLM_GATEWAY_AUDIO_CHANNELS);
    cJSON_AddStringToObject(transcription, "model", model);
    cJSON_AddItemToObject(session, "input_audio_transcription", transcription);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_json = json;
    *out_len = strlen(json);
    return ESP_OK;
}

esp_err_t llm_gateway_protocol_build_asr_ws_audio_append_event(const uint8_t *audio,
                                                               size_t audio_len,
                                                               char **out_json,
                                                               size_t *out_len)
{
    if (audio == NULL || audio_len == 0 ||
        out_json == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    *out_len = 0;

    size_t encoded_size = 0;
    int encode_ret = mbedtls_base64_encode(NULL, 0, &encoded_size, audio, audio_len);
    if (encode_ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || encoded_size == 0) {
        return ESP_FAIL;
    }

    uint8_t *encoded = (uint8_t *)malloc(encoded_size + 1U);
    if (encoded == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t encoded_len = 0;
    encode_ret = mbedtls_base64_encode(encoded, encoded_size, &encoded_len, audio, audio_len);
    if (encode_ret != 0) {
        free(encoded);
        return ESP_FAIL;
    }
    encoded[encoded_len] = '\0';

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        free(encoded);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "input_audio_buffer.append");
    cJSON_AddStringToObject(root, "audio", (const char *)encoded);
    free(encoded);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_json = json;
    *out_len = strlen(json);
    return ESP_OK;
}

esp_err_t llm_gateway_protocol_build_asr_ws_audio_append_event_inplace(const uint8_t *audio,
                                                                       size_t audio_len,
                                                                       char *base64_buf,
                                                                       size_t base64_buf_size,
                                                                       char *json_buf,
                                                                       size_t json_buf_size,
                                                                       size_t *out_len)
{
    if (audio == NULL || audio_len == 0 ||
        base64_buf == NULL || base64_buf_size == 0 ||
        json_buf == NULL || json_buf_size == 0 ||
        out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0;

    size_t encoded_size = 0;
    int encode_ret = mbedtls_base64_encode(NULL, 0, &encoded_size, audio, audio_len);
    if (encode_ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || encoded_size == 0) {
        return ESP_FAIL;
    }
    if (encoded_size + 1U > base64_buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t encoded_len = 0;
    encode_ret = mbedtls_base64_encode((uint8_t *)base64_buf,
                                       base64_buf_size,
                                       &encoded_len,
                                       audio,
                                       audio_len);
    if (encode_ret != 0 || encoded_len >= base64_buf_size) {
        return ESP_FAIL;
    }
    base64_buf[encoded_len] = '\0';

    int written = snprintf(json_buf,
                           json_buf_size,
                           "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}",
                           base64_buf);
    if (written < 0 || (size_t)written >= json_buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_len = (size_t)written;
    return ESP_OK;
}

esp_err_t llm_gateway_protocol_build_asr_ws_finish_event(char **out_json,
                                                         size_t *out_len)
{
    if (out_json == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    *out_len = 0;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "input_audio_buffer.commit");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_json = json;
    *out_len = strlen(json);
    return ESP_OK;
}

esp_err_t llm_gateway_protocol_parse_asr_ws_event(const char *payload,
                                                  size_t payload_len,
                                                  llm_gateway_asr_event_t *out_event)
{
    if (payload == NULL || payload_len == 0 || out_event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_event, 0, sizeof(*out_event));

    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
    const char *type_text = cJSON_IsString(type) ? type->valuestring : NULL;
    const char *event_text = cJSON_IsString(event) ? event->valuestring : NULL;
    const char *name = type_text != NULL ? type_text : event_text;
    if (name != NULL && name[0] != '\0') {
        strlcpy(out_event->type, name, sizeof(out_event->type));
    }

    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsNumber(code)) {
        out_event->code = code->valueint;
    }
    if (llm_gateway_protocol_copy_json_string(message, out_event->message, sizeof(out_event->message)) ||
        llm_gateway_protocol_copy_json_string(error, out_event->message, sizeof(out_event->message))) {
        out_event->is_error = out_event->code != 0 ||
                              (name != NULL && strstr(name, "error") != NULL);
    }

    const cJSON *audio_len = cJSON_GetObjectItemCaseSensitive(root, "audio_len");
    if (cJSON_IsNumber(audio_len) && audio_len->valueint > 0) {
        out_event->has_audio = true;
        out_event->audio_len = (size_t)audio_len->valueint;
    }

    if (!llm_gateway_protocol_extract_text_candidates(root,
                                                      out_event->text,
                                                      sizeof(out_event->text))) {
        (void)llm_gateway_protocol_copy_json_string(llm_gateway_protocol_get_path(root, "transcript", "text", NULL),
                                                    out_event->text,
                                                    sizeof(out_event->text));
        (void)llm_gateway_protocol_copy_json_string(llm_gateway_protocol_get_path(root, "item", "transcript", NULL),
                                                    out_event->text,
                                                    sizeof(out_event->text));
        (void)llm_gateway_protocol_copy_json_string(llm_gateway_protocol_get_path(root, "item", "content", NULL),
                                                    out_event->text,
                                                    sizeof(out_event->text));
    }

    if (APP_DEBUG_LLM_GATEWAY_PROTO && name != NULL) {
        ESP_LOGI(TAG, "ASR WS event type=%s text_len=%u", name, (unsigned int)strlen(out_event->text));
    }

    if (name != NULL) {
        out_event->is_final = strstr(name, "final") != NULL ||
                              strstr(name, "completed") != NULL ||
                              strstr(name, "conversation.item.input_audio_transcription.completed") != NULL ||
                              strstr(name, "transcription.done") != NULL;
        out_event->is_partial = strstr(name, "partial") != NULL ||
                                strstr(name, "conversation.item.input_audio_transcription.result") != NULL ||
                                strstr(name, "delta") != NULL ||
                                strstr(name, "transcription.delta") != NULL;
        if (strstr(name, "error") != NULL) {
            out_event->is_error = true;
        }
    }

    const cJSON *final_bool = cJSON_GetObjectItemCaseSensitive(root, "final");
    if (cJSON_IsBool(final_bool) && cJSON_IsTrue(final_bool)) {
        out_event->is_final = true;
    }

    if (!out_event->is_final && !out_event->is_partial && out_event->text[0] != '\0') {
        out_event->is_partial = true;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t llm_gateway_protocol_build_tts_ws_session_update(const char *model,
                                                           char **out_json,
                                                           size_t *out_len)
{
    if (model == NULL || model[0] == '\0' ||
        out_json == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    *out_len = 0;

    cJSON *root = cJSON_CreateObject();
    cJSON *session = cJSON_CreateObject();
    cJSON *text_to_speech = cJSON_CreateObject();
    if (root == NULL || session == NULL || text_to_speech == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(session);
        cJSON_Delete(text_to_speech);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "tts_session.update");
    cJSON_AddItemToObject(root, "session", session);
    cJSON_AddStringToObject(session, "voice", VOLC_GATEWAY_TTS_VOICE);
    cJSON_AddStringToObject(session, "output_audio_format", VOLC_GATEWAY_TTS_OUTPUT_FORMAT);
    cJSON_AddNumberToObject(session, "output_audio_sample_rate", VOLC_GATEWAY_TTS_OUTPUT_SAMPLE_RATE);
    cJSON_AddStringToObject(text_to_speech, "model", model);
    cJSON_AddItemToObject(session, "text_to_speech", text_to_speech);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_json = json;
    *out_len = strlen(json);
    return ESP_OK;
}

esp_err_t llm_gateway_protocol_build_tts_ws_text_append(const char *text,
                                                        char **out_json,
                                                        size_t *out_len)
{
    if (text == NULL || text[0] == '\0' ||
        out_json == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    *out_len = 0;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "input_text.append");
    cJSON_AddStringToObject(root, "delta", text);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_json = json;
    *out_len = strlen(json);
    return ESP_OK;
}

esp_err_t llm_gateway_protocol_build_tts_ws_text_done(char **out_json,
                                                      size_t *out_len)
{
    if (out_json == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    *out_len = 0;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "input_text.done");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_json = json;
    *out_len = strlen(json);
    return ESP_OK;
}

esp_err_t llm_gateway_protocol_parse_tts_ws_event(const char *payload,
                                                  size_t payload_len,
                                                  uint8_t *audio_buf,
                                                  size_t audio_buf_size,
                                                  llm_gateway_tts_event_t *out_event)
{
    if (payload == NULL || payload_len == 0 ||
        audio_buf == NULL || audio_buf_size == 0 ||
        out_event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_event, 0, sizeof(*out_event));

    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
    const char *type_text = cJSON_IsString(type) ? type->valuestring : NULL;
    const char *event_text = cJSON_IsString(event) ? event->valuestring : NULL;
    const char *name = type_text != NULL ? type_text : event_text;
    if (name != NULL && name[0] != '\0') {
        strlcpy(out_event->type, name, sizeof(out_event->type));
    }

    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsNumber(code)) {
        out_event->code = code->valueint;
    }
    if (cJSON_IsObject(error)) {
        const cJSON *error_code = cJSON_GetObjectItemCaseSensitive(error, "code");
        const cJSON *error_message = cJSON_GetObjectItemCaseSensitive(error, "message");
        if (cJSON_IsNumber(error_code)) {
            out_event->code = error_code->valueint;
        }
        (void)llm_gateway_protocol_copy_json_string(error_message,
                                                    out_event->message,
                                                    sizeof(out_event->message));
    } else {
        (void)llm_gateway_protocol_copy_json_string(error,
                                                    out_event->message,
                                                    sizeof(out_event->message));
    }
    if (out_event->message[0] == '\0') {
        (void)llm_gateway_protocol_copy_json_string(message,
                                                    out_event->message,
                                                    sizeof(out_event->message));
    }

    if (name != NULL) {
        out_event->is_session_updated = strcmp(name, "tts_session.updated") == 0;
        out_event->is_audio_delta = strcmp(name, "response.audio.delta") == 0 ||
                                    strstr(name, "audio.delta") != NULL;
        out_event->is_audio_done = strcmp(name, "response.audio.done") == 0 ||
                                   strstr(name, "audio.done") != NULL ||
                                   strstr(name, "completed") != NULL;
        if (strstr(name, "error") != NULL) {
            out_event->is_error = true;
        }
    }
    if (out_event->code != 0 || out_event->message[0] != '\0') {
        out_event->is_error = out_event->is_error || (name != NULL && strstr(name, "error") != NULL);
    }

    if (out_event->is_audio_delta) {
        const cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
        if (!cJSON_IsString(delta) || delta->valuestring == NULL || delta->valuestring[0] == '\0') {
            cJSON_Delete(root);
            return ESP_ERR_NOT_FOUND;
        }

        const unsigned char *encoded = (const unsigned char *)delta->valuestring;
        size_t encoded_len = strlen(delta->valuestring);
        size_t decoded_need = 0;
        int decode_ret = mbedtls_base64_decode(NULL, 0, &decoded_need, encoded, encoded_len);
        if (decode_ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && decode_ret != 0) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (decoded_need > audio_buf_size) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_SIZE;
        }

        size_t decoded_len = 0;
        decode_ret = mbedtls_base64_decode(audio_buf,
                                           audio_buf_size,
                                           &decoded_len,
                                           encoded,
                                           encoded_len);
        if (decode_ret != 0) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
        out_event->audio_len = decoded_len;
    }

    if (APP_DEBUG_LLM_GATEWAY_PROTO && name != NULL) {
        ESP_LOGI(TAG,
                 "TTS WS event type=%s audio_len=%u done=%d error=%d",
                 name,
                 (unsigned int)out_event->audio_len,
                 out_event->is_audio_done ? 1 : 0,
                 out_event->is_error ? 1 : 0);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

void llm_gateway_protocol_free(char *text)
{
    if (text != NULL) {
        cJSON_free(text);
    }
}
