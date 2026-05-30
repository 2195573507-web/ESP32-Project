#ifndef MIC_LLM_BRIDGE_H
#define MIC_LLM_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @file mic_llm_bridge.h
 * @brief Mic PCM 到统一 llm_client 的桥接层。
 *
 * 调用方法：Mic ADC/VAD 模块只调用本文件，不直接依赖 llm_client、HTTP、
 * WebSocket 或网关协议细节。
 */

/* Mic 模型配置：模型名归属 Mic bridge，llm 层只接收调用方传入的模型名。 */
#define MIC_LLM_BRIDGE_ASR_MODEL               "请填入Mic_ASR模型名"              // Mic 语音识别模型。
#define MIC_LLM_BRIDGE_LLM_MODEL               "请填入Mic_LLM模型名"              // Mic ASR final 后使用的 LLM 模型。
#define MIC_LLM_BRIDGE_TTS_MODEL               "请填入Mic_TTS模型名_当前不用"       // Mic 预留 TTS 模型，当前不调用。

/**
 * @brief 初始化 Mic LLM bridge。
 *
 * 调用方法：WiFi 稳定后、启动 Mic ADC 前调用一次；重复调用直接返回 ESP_OK。
 *
 * @return 成功返回 ESP_OK；llm_client 初始化失败时返回错误码。
 */
esp_err_t mic_llm_bridge_init(void);

/**
 * @brief 通知 bridge 本地 VAD 检测到 VOICE_START。
 *
 * 调用方法：Mic ADC/VAD 进入说话状态时调用一次，用于启动网关语音会话。
 *
 * @return 成功返回 ESP_OK；未初始化或会话启动失败时返回错误码。
 */
esp_err_t mic_llm_bridge_on_voice_start(void);

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
esp_err_t mic_llm_bridge_on_pcm_chunk(const int16_t *pcm, size_t samples, uint32_t sample_rate_hz);

/**
 * @brief 通知 bridge 本地 VAD 检测到 VOICE_END。
 *
 * 调用方法：Mic ADC/VAD 结束本轮语音时调用一次，用于触发 ASR final、LLM 和 router。
 *
 * @return 成功返回 ESP_OK；finish 失败时返回错误码。
 */
esp_err_t mic_llm_bridge_on_voice_end(void);

/**
 * @brief 停止当前 Mic 语音会话。
 *
 * 调用方法：异常、超时或需要中断当前识别时调用。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t mic_llm_bridge_stop(void);

#endif // MIC_LLM_BRIDGE_H
