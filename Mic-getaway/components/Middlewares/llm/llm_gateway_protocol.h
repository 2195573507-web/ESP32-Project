#ifndef LLM_GATEWAY_PROTOCOL_H
#define LLM_GATEWAY_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "llm_config.h"

/**
 * @file llm_gateway_protocol.h
 * @brief 豆包边缘网关 URL、鉴权 Header 和 JSON 协议封装。
 *
 * 调用方法：只允许 llm_client、llm_gateway_http 和 llm_gateway_ws 调用本文件接口。
 * 上层 bridge 不直接组装网关 JSON，也不直接读取 API Key。
 */

typedef struct {
    bool is_partial;                              // true 表示 ASR partial 文本。
    bool is_final;                                // true 表示 ASR final 文本。
    bool is_error;                                // true 表示服务端错误事件。
    bool has_audio;                               // true 表示事件带音频，当前仅占位。
    char text[LLM_GATEWAY_ASR_TEXT_MAX_BYTES];    // ASR 文本。
    char message[160];                            // 服务端错误/状态说明。
    int code;                                     // 服务端状态码或本地错误码。
    size_t audio_len;                             // 音频字节数，当前 TTS 禁用时只记录长度。
} llm_gateway_asr_event_t;

/**
 * @brief 拼接网关 base URL 和 API path。
 *
 * @param base_url 网关根地址，不能为空。
 * @param path API path，不能为空。
 * @param out 输出 URL 缓冲区，不能为空。
 * @param out_size out 字节数，必须大于 0。
 * @return 成功返回 ESP_OK；参数错误或缓冲不足时返回错误码。
 */
esp_err_t llm_gateway_protocol_build_url(const char *base_url,
                                         const char *path,
                                         char *out,
                                         size_t out_size);

/**
 * @brief 生成 Authorization Header 值。
 *
 * 调用方法：内部使用 LLM_GATEWAY_AUTH_BEARER_PREFIX 和 LLM_GATEWAY_API_KEY；
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
 * @brief 解析 HTTP ASR 响应文本。
 *
 * @param json 服务端 JSON 响应，不能为空。
 * @param out_text 输出 ASR 文本缓冲区，不能为空。
 * @param out_size out_text 字节数，必须大于 0。
 * @return 成功返回 ESP_OK；JSON 无效或未找到文本时返回错误码。
 */
esp_err_t llm_gateway_protocol_parse_asr_http_response(const char *json,
                                                       char *out_text,
                                                       size_t out_size);

/**
 * @brief 组装 ASR WebSocket session.start JSON。
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
 * @brief 组装 ASR WebSocket session.finish JSON。
 *
 * 调用方法：返回的 out_json 由 llm_gateway_protocol_free() 释放。
 *
 * @param model ASR 模型名，不能为空。
 * @param out_json 输出 JSON 字符串指针，不能为空。
 * @param out_len 输出 JSON 字节数，不能为空。
 * @return 成功返回 ESP_OK；参数错误或内存不足时返回错误码。
 */
esp_err_t llm_gateway_protocol_build_asr_ws_finish_event(const char *model,
                                                         char **out_json,
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
 * @brief 释放本协议模块分配的 JSON 字符串。
 *
 * @param text 待释放字符串，可为空。
 */
void llm_gateway_protocol_free(char *text);

#endif // LLM_GATEWAY_PROTOCOL_H
