#ifndef LLM_GATEWAY_PROTOCOL_H
#define LLM_GATEWAY_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "llm_config.h"

/**
 * @file llm_gateway_protocol.h
 * @brief 火山引擎边缘网关 URL、鉴权 Header 和 JSON 协议封装。
 *
 * 调用方法：只允许 llm_client、llm_gateway_http 和 llm_gateway_ws 调用本文件接口。
 * 上层 bridge 不直接组装网关 JSON，也不直接读取 API Key。
 */

typedef struct {
    bool is_partial;                              // true 表示 ASR partial 文本。
    bool is_final;                                // true 表示 ASR final 文本。
    bool is_error;                                // true 表示服务端错误事件。
    bool has_audio;                               // true 表示事件带音频，当前仅占位。
    char type[96];                                // 服务端事件 type/event 名。
    char text[LLM_GATEWAY_ASR_TEXT_MAX_BYTES];    // ASR 文本。
    char message[160];                            // 服务端错误/状态说明。
    int code;                                     // 服务端状态码或本地错误码。
    size_t audio_len;                             // 音频字节数，当前 TTS 禁用时只记录长度。
} llm_gateway_asr_event_t;

typedef struct {
    bool is_session_updated;                      // true 表示 TTS session 配置已生效。
    bool is_audio_delta;                          // true 表示本事件携带一段 TTS 音频。
    bool is_audio_done;                           // true 表示本轮 TTS 音频输出完成。
    bool is_audio_dropped;                        // true 表示音频事件已识别但因本地上限被丢弃。
    bool is_error;                                // true 表示服务端错误事件。
    char type[96];                                // 服务端事件 type/event 名。
    char message[160];                            // 服务端错误/状态说明。
    int code;                                     // 服务端状态码或本地错误码。
    size_t audio_len;                             // 解码后的音频字节数。
    size_t audio_base64_len;                      // response.audio.delta 的 base64 字符串字节数。
    int64_t audio_decode_us;                      // base64 decode 耗时，非音频事件为 0。
} llm_gateway_tts_event_t;

/**
 * @brief 生成 Authorization Header 值。
 *
 * 调用方法：内部使用 LLM_GATEWAY_AUTH_BEARER_PREFIX 和 VOLC_GATEWAY_API_KEY；
 * 不打印明文 API Key。
 *
 * @param out 输出 Header value 缓冲区，不能为空。
 * @param out_size out 字节数，必须大于 0。
 * @return 成功返回 ESP_OK；参数错误或缓冲不足时返回错误码。
 */
esp_err_t llm_gateway_protocol_build_auth_header(char *out, size_t out_size);

/**
 * @brief 生成 API Key 脱敏摘要。
 *
 * @param out 输出摘要缓冲区，不能为空。
 * @param out_size out 字节数，必须大于 0。
 */
void llm_gateway_protocol_make_key_summary(char *out, size_t out_size);

/**
 * @brief 检查网关配置是否仍包含占位符。
 *
 * @return 任一必填项仍包含 LLM_GATEWAY_PLACEHOLDER_MARKER 时返回 true。
 */
bool llm_gateway_protocol_config_has_placeholders(void);

/**
 * @brief 组装 LLM chat completions 请求 JSON。
 *
 * 调用方法：返回的 out_json 由 llm_gateway_protocol_free() 释放。
 *
 * @param model LLM 模型名，不能为空。
 * @param system_prompt LLM system prompt；为空时使用默认配置。
 * @param user_text 用户文本，不能为空。
 * @param out_json 输出 JSON 字符串指针，不能为空。
 * @param out_len 输出 JSON 字节数，不能为空。
 * @return 成功返回 ESP_OK；参数错误或内存不足时返回错误码。
 */
esp_err_t llm_gateway_protocol_build_chat_request(const char *model,
                                                  const char *system_prompt,
                                                  const char *user_text,
                                                  char **out_json,
                                                  size_t *out_len);

/**
 * @brief 解析 LLM chat completions 响应文本。
 *
 * @param json 服务端 JSON 响应，不能为空。
 * @param out_text 输出文本缓冲区，不能为空。
 * @param out_size out_text 字节数，必须大于 0。
 * @return 成功返回 ESP_OK；JSON 无效或未找到文本时返回错误码。
 */
esp_err_t llm_gateway_protocol_parse_chat_response(const char *json,
                                                   char *out_text,
                                                   size_t out_size);

/**
 * @brief 组装 ASR Realtime transcription_session.update JSON。
 *
 * 调用方法：返回的 out_json 由 llm_gateway_protocol_free() 释放。
 *
 * @param model ASR 模型名，不能为空。
 * @param out_json 输出 JSON 字符串指针，不能为空。
 * @param out_len 输出 JSON 字节数，不能为空。
 * @return 成功返回 ESP_OK；参数错误或内存不足时返回错误码。
 */
esp_err_t llm_gateway_protocol_build_asr_ws_start_event(const char *model,
                                                        char **out_json,
                                                        size_t *out_len);

/**
 * @brief 组装 ASR Realtime input_audio_buffer.append JSON。
 *
 * 调用方法：把一段 PCM bytes base64 后放入 audio 字段；返回的 out_json 由
 * llm_gateway_protocol_free() 释放。
 *
 * @param audio PCM bytes 指针，不能为空。
 * @param audio_len PCM bytes 数，必须大于 0。
 * @param out_json 输出 JSON 字符串指针，不能为空。
 * @param out_len 输出 JSON 字节数，不能为空。
 * @return 成功返回 ESP_OK；参数错误或内存不足时返回错误码。
 */
esp_err_t llm_gateway_protocol_build_asr_ws_audio_append_event(const uint8_t *audio,
                                                               size_t audio_len,
                                                               char **out_json,
                                                               size_t *out_len);

/**
 * @brief 使用调用方复用缓冲区组装 ASR input_audio_buffer.append JSON。
 *
 * 调用方法：volc_gateway_asr 发送每个 100 ms PCM chunk 时调用，避免每包 malloc/free。
 *
 * @param audio PCM bytes 指针，不能为空。
 * @param audio_len PCM bytes 数，必须大于 0。
 * @param base64_buf base64 输出缓冲区，不能为空。
 * @param base64_buf_size base64_buf 字节数，必须足够容纳编码结果。
 * @param json_buf JSON 输出缓冲区，不能为空。
 * @param json_buf_size json_buf 字节数，必须足够容纳 append JSON。
 * @param out_len 输出 JSON 字节数，不能为空。
 * @return 成功返回 ESP_OK；参数错误、缓冲不足或编码失败时返回错误码。
 */
esp_err_t llm_gateway_protocol_build_asr_ws_audio_append_event_inplace(const uint8_t *audio,
                                                                       size_t audio_len,
                                                                       char *base64_buf,
                                                                       size_t base64_buf_size,
                                                                       char *json_buf,
                                                                       size_t json_buf_size,
                                                                       size_t *out_len);

/**
 * @brief 组装 ASR Realtime input_audio_buffer.commit JSON。
 *
 * 调用方法：返回的 out_json 由 llm_gateway_protocol_free() 释放。
 *
 * @param out_json 输出 JSON 字符串指针，不能为空。
 * @param out_len 输出 JSON 字节数，不能为空。
 * @return 成功返回 ESP_OK；参数错误或内存不足时返回错误码。
 */
esp_err_t llm_gateway_protocol_build_asr_ws_finish_event(char **out_json,
                                                         size_t *out_len);

/**
 * @brief 解析 ASR WebSocket 服务端事件。
 *
 * @param payload WebSocket payload，不能为空。
 * @param payload_len payload 字节数，必须大于 0。
 * @param out_event 输出解析结果，不能为空。
 * @return 成功返回 ESP_OK；payload 不是可识别 JSON 时返回错误码。
 */
esp_err_t llm_gateway_protocol_parse_asr_ws_event(const char *payload,
                                                  size_t payload_len,
                                                  llm_gateway_asr_event_t *out_event);

/**
 * @brief 组装 TTS Realtime tts_session.update JSON。
 *
 * 调用方法：TTS WebSocket 建连成功后立即发送，更新音色和输出音频参数。
 *
 * @param model TTS 模型名，不能为空。
 * @param out_json 输出 JSON 字符串指针，不能为空。
 * @param out_len 输出 JSON 字节数，不能为空。
 * @return 成功返回 ESP_OK；参数错误或内存不足时返回错误码。
 */
esp_err_t llm_gateway_protocol_build_tts_ws_session_update(const char *model,
                                                           char **out_json,
                                                           size_t *out_len);

/**
 * @brief 组装 TTS Realtime input_text.append JSON。
 *
 * @param text 待合成文本，不能为空。
 * @param out_json 输出 JSON 字符串指针，不能为空。
 * @param out_len 输出 JSON 字节数，不能为空。
 * @return 成功返回 ESP_OK；参数错误或内存不足时返回错误码。
 */
esp_err_t llm_gateway_protocol_build_tts_ws_text_append(const char *text,
                                                        char **out_json,
                                                        size_t *out_len);

/**
 * @brief 组装 TTS Realtime input_text.done JSON。
 *
 * @param out_json 输出 JSON 字符串指针，不能为空。
 * @param out_len 输出 JSON 字节数，不能为空。
 * @return 成功返回 ESP_OK；参数错误或内存不足时返回错误码。
 */
esp_err_t llm_gateway_protocol_build_tts_ws_text_done(char **out_json,
                                                      size_t *out_len);

/**
 * @brief 解析 TTS WebSocket 服务端事件，并解码 response.audio.delta。
 *
 * @param payload WebSocket payload，不能为空。
 * @param payload_len payload 字节数，必须大于 0。
 * @param audio_buf TTS 音频输出缓冲区，不能为空。
 * @param audio_buf_size audio_buf 字节数。
 * @param out_event 输出解析结果，不能为空。
 * @return 成功返回 ESP_OK；payload 不是可识别 JSON 或音频缓冲不足时返回错误码。
 */
esp_err_t llm_gateway_protocol_parse_tts_ws_event(const char *payload,
                                                  size_t payload_len,
                                                  uint8_t *audio_buf,
                                                  size_t audio_buf_size,
                                                  llm_gateway_tts_event_t *out_event);

/**
 * @brief 解析 TTS WebSocket 服务端事件，并把 response.audio.delta 解码到新分配 PCM chunk。
 *
 * 调用方法：llm_gateway_tts_ws 收到完整 JSON payload 后调用。若返回音频，
 * *out_audio 的 ownership 转移给调用方；调用方入队失败时必须 free。
 *
 * @param payload WebSocket payload，不能为空。
 * @param payload_len payload 字节数，必须大于 0。
 * @param out_event 输出解析结果，不能为空。
 * @param out_audio 输出 PCM 指针；仅 AUDIO_DELTA 且 audio_len > 0 时非空。
 * @param out_audio_len 输出 PCM 字节数。
 * @return 成功返回 ESP_OK；payload 不是可识别 JSON、音频过大或内存不足时返回错误码。
 */
esp_err_t llm_gateway_protocol_parse_tts_ws_event_owned_audio(const char *payload,
                                                              size_t payload_len,
                                                              llm_gateway_tts_event_t *out_event,
                                                              uint8_t **out_audio,
                                                              size_t *out_audio_len);

/**
 * @brief 释放本协议模块分配的 JSON 字符串。
 *
 * @param text 待释放字符串，可为空。
 */
void llm_gateway_protocol_free(char *text);

#endif // LLM_GATEWAY_PROTOCOL_H
