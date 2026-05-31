#ifndef MIC_LLM_BRIDGE_H
#define MIC_LLM_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @file mic_llm_bridge.h
 * @brief Mic PCM 到统一 llm_client ASR 能力的桥接层。
 *
 * 调用方法：Mic ADC/VAD 模块只调用本文件，不直接依赖 llm_client、HTTP、
 * WebSocket 或网关协议细节。
 */

/**
 * @brief 初始化 Mic LLM bridge。
 *
 * 调用方法：WiFi 稳定后、启动 Mic ADC 前调用一次；重复调用直接返回 ESP_OK。
 *
 * @return 成功返回 ESP_OK；llm_client 初始化失败时返回错误码。
 */
esp_err_t ai_mic_bridge_init(void);

/**
 * @brief 通知 bridge 本地 VAD 检测到 VOICE_START。
 *
 * 调用方法：Mic ADC/VAD 进入说话状态时调用一次，用于启动网关语音会话。
 *
 * @return 成功返回 ESP_OK；未初始化或会话启动失败时返回错误码。
 */
esp_err_t ai_mic_bridge_voice_start(void);

/**
 * @brief 向当前语音会话发送 PCM16 音频块。
 *
 * 调用方法：Mic 采集任务在 STREAMING 状态下持续调用。pcm 必须是 16 kHz、
 * signed int16、单声道 PCM。
 *
 * @param pcm PCM16 样本指针，不能为空。
 * @param samples PCM 样本数，必须大于 0。
 * @param sample_rate_hz PCM 采样率。
 * @return 成功返回 ESP_OK；未初始化、参数错误或发送失败时返回错误码。
 */
esp_err_t ai_mic_bridge_pcm_append(const int16_t *pcm, size_t samples);

/**
 * @brief 通知 bridge 本地 VAD 检测到 VOICE_END。
 *
 * 调用方法：Mic ADC/VAD 结束本轮语音时调用一次，只触发 ASR commit。
 *
 * @return 成功返回 ESP_OK；finish 失败时返回错误码。
 */
esp_err_t ai_mic_bridge_voice_end(void);

/**
 * @brief 停止当前 Mic 语音会话。
 *
 * 调用方法：异常、超时或需要中断当前识别时调用。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t ai_mic_bridge_voice_cancel(void);

/**
 * @brief 查询底层 ASR 是否已回到空闲态。
 *
 * 调用方法：Mic ADC/VAD 在打印 session done 或启动下一轮 ASR 前调用。
 *
 * @return llm_client 处于 IDLE 返回 true，否则返回 false。
 */
bool ai_mic_bridge_is_idle(void);

/**
 * @brief 查询底层 ASR 是否仍在等待 final/关闭 WebSocket。
 *
 * 调用方法：VOICE_START 到来但底层仍在 ASR_FINISHING 时只打印 busy 并忽略本次启动。
 *
 * @return 处于 ASR_FINISHING 返回 true，否则返回 false。
 */
bool ai_mic_bridge_is_asr_finishing(void);

/**
 * @brief 获取底层 llm_client 当前状态名。
 *
 * @return 状态字符串，未初始化时返回 UNINITIALIZED。
 */
const char *ai_mic_bridge_state_name(void);

/**
 * @brief ASR partial 文本回调。
 *
 * @param text partial 文本，可为空。
 * @return 成功返回 ESP_OK。
 */
esp_err_t ai_mic_bridge_on_asr_partial(const char *text);

/**
 * @brief ASR final 文本回调。
 *
 * @param text final 文本，可为空。
 * @return 成功返回 ESP_OK。
 */
esp_err_t ai_mic_bridge_on_asr_final(const char *text);

/* 兼容旧文件名接口：内部转到 ai_mic_bridge_*，不暴露网关细节。 */
esp_err_t mic_llm_bridge_init(void);
esp_err_t mic_llm_bridge_on_voice_start(void);
esp_err_t mic_llm_bridge_on_pcm_chunk(const int16_t *pcm, size_t samples, uint32_t sample_rate_hz);
esp_err_t mic_llm_bridge_on_voice_end(void);
esp_err_t mic_llm_bridge_stop(void);

#endif // MIC_LLM_BRIDGE_H
