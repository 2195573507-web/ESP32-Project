#ifndef SPEAKER_LLM_BRIDGE_H
#define SPEAKER_LLM_BRIDGE_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * @file speaker_llm_bridge.h
 * @brief speaker/TTS 到统一 llm_client 的桥接层。
 *
 * 调用方法：speaker 模块只调用本文件，不直接依赖 llm_client 或网关 TTS path。
 * 当前阶段已可发起 TTS 合成，但尚未接入实际扬声器播放底层。
 */

/**
 * @brief 初始化 speaker LLM bridge。
 *
 * 调用方法：系统初始化时可调用一次；当前记录 TTS 是否启用。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t speaker_llm_bridge_init(void);

/**
 * @brief 请求播报文本。
 *
 * 调用方法：speaker 需要播报文本时调用，内部转到 llm_client_tts_text()。
 *
 * @param text 待播报文本，不能为空。
 * @return 成功返回 ESP_OK；参数为空时返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t speaker_llm_bridge_speak_text(const char *text);

/**
 * @brief 查询 speaker/TTS bridge 是否可用。
 *
 * @return 当前 TTS 能力未启用时返回 false。
 */
bool speaker_llm_bridge_is_enabled(void);

/* 当前阶段保留兼容入口，实际合成由 llm_client_tts_text() 执行。 */

#endif // SPEAKER_LLM_BRIDGE_H
