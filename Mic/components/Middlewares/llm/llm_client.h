#ifndef LLM_CLIENT_H
#define LLM_CLIENT_H

#include <stddef.h>

#include "esp_err.h"
#include "llm_config.h"

/**
 * @file llm_client.h
 * @brief 跨工程通用 LLM Chat Completion 客户端接口。
 *
 * 本模块只负责通用 LLM HTTP 调用：保存 API URL/API Key、组装
 * chat/completions JSON、发送 HTTPS POST，并从响应 JSON 中提取 assistant.content。
 * LLM 云端配置统一来自 llm_config.h 的 OPENAI_* 宏；ASR 配置不在这里维护。
 */

/* 请求组装参数：只影响通用 JSON body 预留空间和 Authorization Header 文字。 */
#define LLM_CLIENT_REQUEST_EXTRA_BYTES      512       // message/system/model/user 之外为 JSON 结构预留的字节数。
#define LLM_CLIENT_AUTH_BEARER_PREFIX       "Bearer " // Authorization Header 前缀。

#if LLM_CLIENT_REQUEST_EXTRA_BYTES <= 0
#error "LLM_CLIENT_REQUEST_EXTRA_BYTES must be greater than 0"
#endif

/**
 * @brief 初始化通用 LLM 客户端。
 *
 * 调用方法：业务层初始化时调用一次。函数会初始化底层 HTTP 客户端，并写入
 * OPENAI_CHAT_COMPLETIONS_URL / OPENAI_API_KEY。业务层可随后调用 setter 覆盖。
 *
 * @return 成功返回 ESP_OK；底层 HTTP 客户端初始化失败时返回错误码。
 */
esp_err_t llm_client_init(void);

/**
 * @brief 发送一段用户文本到 LLM 并获取 assistant 回复。
 *
 * 调用方法：API URL/API Key 已设置且网络可用后调用。response_buffer 会被原始
 * HTTP 响应复用为解析后的 assistant.content 输出缓冲。
 *
 * @param message 用户文本，不能为空。
 * @param response_buffer 输出回复文本的缓冲区，不能为空。
 * @param buffer_size response_buffer 字节数，必须大于 1。
 * @param response_len 输出回复文本长度，不含结尾 '\0'。
 * @return 成功返回 ESP_OK；参数、内存、HTTP 或响应解析失败时返回错误码。
 */
esp_err_t llm_send_message(const char *message, char *response_buffer, size_t buffer_size, size_t *response_len);

/**
 * @brief 设置通用 LLM API URL。
 *
 * 调用方法：llm_client_init() 后调用，用于覆盖默认 URL。
 *
 * @param url API URL，函数内部复制保存。
 */
void llm_set_api_url(const char *url);

/**
 * @brief 设置通用 LLM API Key。
 *
 * 调用方法：llm_client_init() 后调用，用于覆盖默认 API Key。
 *
 * @param api_key API Key，函数内部复制保存。
 */
void llm_set_api_key(const char *api_key);

#endif // LLM_CLIENT_H
