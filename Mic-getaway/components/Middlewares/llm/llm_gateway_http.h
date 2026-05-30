#ifndef LLM_GATEWAY_HTTP_H
#define LLM_GATEWAY_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @file llm_gateway_http.h
 * @brief 豆包边缘网关 HTTP transport 封装。
 *
 * 调用方法：只允许 llm_client 调用本文件接口。上层 bridge 不直接依赖 HTTP path、
 * Authorization Header、multipart 或响应 JSON 解析细节。
 */

/**
 * @brief 发送一次 LLM chat completions 请求。
 *
 * 调用方法：ASR final 文本或结构化上下文准备好后由 llm_client 调用。
 *
 * @param model LLM 模型名，不能为空。
 * @param system_prompt LLM system prompt；为空时使用默认配置。
 * @param user_text 用户文本，不能为空。
 * @param out_text 输出回复文本缓冲区，不能为空。
 * @param out_text_size out_text 字节数，必须大于 0。
 * @return 成功返回 ESP_OK；HTTP 请求、JSON 解析或参数错误时返回错误码。
 */
esp_err_t llm_gateway_http_chat_completion(const char *model,
                                           const char *system_prompt,
                                           const char *user_text,
                                           char *out_text,
                                           size_t out_text_size);

/**
 * @brief 发送一次 HTTP ASR fallback 转写请求。
 *
 * 调用方法：WebSocket streaming 未拿到 final 且本地已有 PCM 缓存时由 llm_client 调用。
 *
 * @param model ASR 模型名，不能为空。
 * @param pcm PCM16 样本指针，不能为空。
 * @param samples PCM 样本数，必须大于 0。
 * @param sample_rate_hz PCM 采样率。
 * @param out_text 输出 ASR 文本缓冲区，不能为空。
 * @param out_text_size out_text 字节数，必须大于 0。
 * @return 成功返回 ESP_OK；HTTP 请求、JSON 解析或参数错误时返回错误码。
 */
esp_err_t llm_gateway_http_asr_transcription(const char *model,
                                             const int16_t *pcm,
                                             size_t samples,
                                             uint32_t sample_rate_hz,
                                             char *out_text,
                                             size_t out_text_size);

#endif // LLM_GATEWAY_HTTP_H
