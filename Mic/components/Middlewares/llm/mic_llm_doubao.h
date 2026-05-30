#ifndef MIC_LLM_DOUBAO_H
#define MIC_LLM_DOUBAO_H

#include <stddef.h>

#include "esp_err.h"
#include "llm_config.h"

/**
 * @file mic_llm_doubao.h
 * @brief Mic 工程使用豆包 LLM 的薄接口层。
 *
 * 本文件只保存 Mic 侧运行参数和对外接口。通用 LLM HTTP 客户端实现保留在
 * llm_client.c / llm_client.h；其它工程可以直接复用 llm_client。LLM 的 API Key、
 * Base URL、模型、system prompt 和 user id 统一放在 llm_config.h 的 OPENAI_* 宏中；
 * ASR 的豆包配置仍独立保留在 ASR 模块。
 */

/* Mic 侧运行参数：只影响独立 chat_text() 调用，不接入 ASR/TTS 主流程。 */
#define MIC_LLM_DOUBAO_RESPONSE_MAX_BYTES      1024           // 建议回复文本缓冲大小；调用者可传更大缓冲。

#if MIC_LLM_DOUBAO_RESPONSE_MAX_BYTES <= 1
#error "MIC_LLM_DOUBAO_RESPONSE_MAX_BYTES must be greater than 1"
#endif

/**
 * @brief 初始化 Mic 豆包 LLM 接口。
 *
 * 调用方法：WiFi 已准备好之前或之后均可调用。本函数只初始化通用 llm_client；
 * 不会启动 ASR/TTS，也不会发请求。
 *
 * @return 成功返回 ESP_OK；API Key 未配置或底层 llm_client 初始化失败时返回错误码。
 */
esp_err_t mic_llm_doubao_init(void);

/**
 * @brief 发送文本到豆包 LLM 并读取回复。
 *
 * 调用方法：WiFi 已联网后由业务代码主动调用。当前阶段不在 ASR final 回调中自动调用，
 * 也不触发 TTS。
 *
 * @param prompt 用户输入文本，不能为空。
 * @param response_buffer 输出回复文本的缓冲区，不能为空。
 * @param response_buffer_size response_buffer 字节数，必须大于 1。
 * @param response_len 可选输出，返回不含 '\0' 的回复文本长度。
 * @return 成功返回 ESP_OK；参数错误、未初始化或底层请求失败时返回错误码。
 */
esp_err_t mic_llm_doubao_chat_text(const char *prompt,
                                   char *response_buffer,
                                   size_t response_buffer_size,
                                   size_t *response_len);

/**
 * @brief 反初始化 Mic 豆包 LLM 接口。
 *
 * 调用方法：当前底层 llm_client 没有释放接口，本函数只清除 Mic 侧初始化标记。
 */
void mic_llm_doubao_deinit(void);

#endif // MIC_LLM_DOUBAO_H
