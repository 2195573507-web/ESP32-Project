#ifndef LLM_WAV_H
#define LLM_WAV_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @file llm_wav.h
 * @brief PCM16 到 WAV 内存块的轻量封装。
 *
 * 调用方法：当前只给 HTTP ASR fallback 使用。WebSocket streaming 仍发送裸 PCM16，
 * 不走 WAV 包装。
 */

#define LLM_WAV_HEADER_BYTES 44 // 标准 PCM WAV header 长度。

/**
 * @brief 把 PCM16 样本包装成 WAV 内存块。
 *
 * 调用方法：返回的 out_wav 使用 malloc 分配，调用方负责 free()。
 *
 * @param pcm PCM16 样本指针，不能为空。
 * @param samples PCM 样本数，必须大于 0。
 * @param sample_rate_hz PCM 采样率。
 * @param channels 声道数。
 * @param out_wav 输出 WAV 内存块指针，不能为空。
 * @param out_wav_bytes 输出 WAV 总字节数，不能为空。
 * @return 成功返回 ESP_OK；参数错误、大小溢出或内存不足时返回错误码。
 */
esp_err_t llm_wav_build_from_pcm16(const int16_t *pcm,
                                   size_t samples,
                                   uint32_t sample_rate_hz,
                                   uint16_t channels,
                                   uint8_t **out_wav,
                                   size_t *out_wav_bytes);

#endif // LLM_WAV_H
