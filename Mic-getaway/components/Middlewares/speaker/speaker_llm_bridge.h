#ifndef SPEAKER_LLM_BRIDGE_H
#define SPEAKER_LLM_BRIDGE_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * @file speaker_llm_bridge.h
 * @brief speaker/TTS 到统一 llm_client 的桥接占位层。
 *
 * 调用方法：speaker 模块只调用本文件，不直接依赖 llm_client 或网关 TTS path。
 * 当前阶段 TTS 禁用，不实现播放链路。
 */

/**
 * @brief 初始化 speaker LLM bridge。
 *
 * 调用方法：系统初始化时可调用一次；当前只记录 TTS 是否启用。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t speaker_llm_bridge_init(void);

/**
 * @brief 请求播报文本。
 *
 * 调用方法：当前阶段 TTS 禁用，本函数转到 llm_client_tts_text() 后返回 ESP_ERR_NOT_SUPPORTED。
 *
 * @param text 待播报文本，不能为空。
 * @return 当前固定返回 ESP_ERR_NOT_SUPPORTED；参数为空时返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t speaker_llm_bridge_speak_text(const char *text);

/**
 * @brief 查询 speaker/TTS bridge 是否可用。
 *
 * @return 当前 TTS 禁用时返回 false。
 */
bool speaker_llm_bridge_is_enabled(void);

/* 当前阶段 Speaker/TTS 禁用，只保留 legacy bridge 兼容入口。 */

#endif // SPEAKER_LLM_BRIDGE_H
