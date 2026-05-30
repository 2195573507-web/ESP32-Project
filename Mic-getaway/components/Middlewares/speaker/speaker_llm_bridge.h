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

/* speaker 模型配置：当前 TTS 禁用，只保留后续接入时使用的模型名。 */
#define SPEAKER_LLM_BRIDGE_TTS_MODEL           "请填入Speaker_TTS模型名_当前不用"  // speaker 预留 TTS 模型，当前不调用。

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
 * 调用方法：当前阶段 TTS 禁用，本函数只做参数检查并返回 ESP_ERR_NOT_SUPPORTED。
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

#endif // SPEAKER_LLM_BRIDGE_H
