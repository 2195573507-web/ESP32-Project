#include "llm_gateway_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "llm_gateway_proto";

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
    }

    if (llm_gateway_protocol_copy_json_string(cJSON_GetObjectItemCaseSensitive(root, "text"), out_text, out_size) ||
        llm_gateway_protocol_copy_json_string(cJSON_GetObjectItemCaseSensitive(root, "output_text"), out_text, out_size) ||
        llm_gateway_protocol_copy_json_string(llm_gateway_protocol_get_path(root, "result", "text", NULL), out_text, out_size) ||
        llm_gateway_protocol_copy_json_string(llm_gateway_protocol_get_path(root, "data", "text", NULL), out_text, out_size)) {
        return true;
    }

    return false;
}

esp_err_t llm_gateway_protocol_build_url(const char *base_url,
                                         const char *path,
                                         char *out,
                                         size_t out_size)
{
    if (base_url == NULL || base_url[0] == '\0' ||
        path == NULL || path[0] == '\0' ||
        out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    bool base_slash = base_url[strlen(base_url) - 1] == '/';
    bool path_slash = path[0] == '/';
    int written = snprintf(out,
                           out_size,
                           "%s%s%s",
                           base_url,
                           (base_slash || path_slash) ? "" : "/",
                           (base_slash && path_slash) ? path + 1 : path);
    if (written < 0 || (size_t)written >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t llm_gateway_protocol_build_auth_header(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out,
                           out_size,
                           "%s%s",
                           LLM_GATEWAY_AUTH_BEARER_PREFIX,
                           LLM_GATEWAY_API_KEY);
    if (written < 0 || (size_t)written >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

void llm_gateway_protocol_make_key_summary(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    size_t key_len = strlen(LLM_GATEWAY_API_KEY);
    if (key_len < 7) {
        snprintf(out, out_size, "len=%u, masked=***", (unsigned int)key_len);
        return;
    }

    snprintf(out,
             out_size,
             "len=%u, masked=%.3s***%s",
             (unsigned int)key_len,
             LLM_GATEWAY_API_KEY,
             LLM_GATEWAY_API_KEY + key_len - 3);
}

bool llm_gateway_protocol_config_has_placeholders(void)
{
    return strstr(LLM_GATEWAY_HTTP_BASE_URL, LLM_GATEWAY_PLACEHOLDER_MARKER) != NULL ||
           strstr(LLM_GATEWAY_WS_BASE_URL, LLM_GATEWAY_PLACEHOLDER_MARKER) != NULL ||
           strstr(LLM_GATEWAY_API_KEY, LLM_GATEWAY_PLACEHOLDER_MARKER) != NULL;
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

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "LLM response is not valid JSON");
        return ESP_FAIL;
    }

    bool found = llm_gateway_protocol_extract_text_candidates(root, out_text, out_size);
    cJSON_Delete(root);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t llm_gateway_protocol_parse_asr_http_response(const char *json,
                                                       char *out_text,
                                                       size_t out_size)
{
    return llm_gateway_protocol_parse_chat_response(json, out_text, out_size);
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
    cJSON *audio = cJSON_CreateObject();
    if (root == NULL || audio == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(audio);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "session.start");
    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddStringToObject(audio, "format", LLM_GATEWAY_AUDIO_FORMAT);
    cJSON_AddNumberToObject(audio, "sample_rate", LLM_GATEWAY_AUDIO_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio, "bits", LLM_GATEWAY_AUDIO_BITS);
    cJSON_AddNumberToObject(audio, "channels", LLM_GATEWAY_AUDIO_CHANNELS);
    cJSON_AddItemToObject(root, "audio", audio);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_json = json;
    *out_len = strlen(json);
    return ESP_OK;
}

esp_err_t llm_gateway_protocol_build_asr_ws_finish_event(const char *model,
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
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "input_audio_buffer.commit");
    cJSON_AddStringToObject(root, "model", model);

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

    (void)llm_gateway_protocol_extract_text_candidates(root,
                                                       out_event->text,
                                                       sizeof(out_event->text));

    if (name != NULL) {
        out_event->is_final = strstr(name, "final") != NULL ||
                              strstr(name, "completed") != NULL ||
                              strstr(name, "transcription.done") != NULL;
        out_event->is_partial = strstr(name, "partial") != NULL ||
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

void llm_gateway_protocol_free(char *text)
{
    if (text != NULL) {
        cJSON_free(text);
    }
}
