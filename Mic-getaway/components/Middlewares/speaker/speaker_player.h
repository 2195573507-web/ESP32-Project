#ifndef SPEAKER_PLAYER_H
#define SPEAKER_PLAYER_H

#include <stdint.h>

#include "esp_err.h"
#include "iis.h"

/**
 * @file speaker_player.h
 * @brief speaker PCM 播放器。
 *
 * 输入格式：
 * 1. audio_player_play_pcm() 接收 PCM16 单声道 24 kHz。
 * 2. audio_player_play_tts_pcm() 可接收 TTS 常见的 PCM16 单声道 16 kHz，
 *    内部会线性重采样到 24 kHz 后再播放。
 *
 * 职责边界：
 * 1. IIS/PDM GPIO、PA、DMA 底层归 BSP/IIS 管理。
 * 2. 本模块只负责把 PCM 切成固定音频块，经环形缓冲区交给写入任务写 IIS。
 */

/* 对外暴露的播放格式与 BSP/IIS 保持一致，避免上层重复包含 IIS 细节。 */
#define AUDIO_PLAYER_SAMPLE_RATE_HZ IIS_SAMPLE_RATE_HZ
#define AUDIO_PLAYER_BITS_PER_SAMPLE IIS_BITS_PER_SAMPLE
#define AUDIO_PLAYER_PDM_SLOT_MODE IIS_PDM_SLOT_MODE
#define AUDIO_PLAYER_PDM_SLOT_MASK IIS_PDM_SLOT_MASK
#define AUDIO_PLAYER_DMA_DESC_NUM IIS_DMA_DESC_NUM
#define AUDIO_PLAYER_DMA_FRAME_NUM IIS_DMA_FRAME_NUM
#define AUDIO_PLAYER_EFFECTIVE_DMA_DESC_NUM IIS_EFFECTIVE_DMA_DESC_NUM
#define AUDIO_PLAYER_EFFECTIVE_DMA_FRAME_NUM IIS_EFFECTIVE_DMA_FRAME_NUM

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 speaker 播放器和底层 IIS。
 *
 * 调用方法：系统启动或首次播放前调用；函数可重复调用。
 */
esp_err_t audio_player_init(void);

/**
 * @brief 播放 PCM16 单声道 24 kHz 数据。
 *
 * @param data PCM 采样数组。
 * @param samples int16_t 采样点数量，不是字节数。
 */
esp_err_t audio_player_play_pcm(const int16_t *data, uint32_t samples);

/**
 * @brief 播放 TTS PCM 数据。
 *
 * @param data PCM 采样数组。
 * @param samples int16_t 采样点数量，不是字节数。
 * @param sample_rate_hz 输入采样率；支持 24 kHz 直通和 16 kHz 重采样。
 */
esp_err_t audio_player_play_tts_pcm(const int16_t *data,
                                    uint32_t samples,
                                    int sample_rate_hz);

/**
 * @brief 打开一轮流式 PCM 播放。
 *
 * 调用方法：TTS 播放任务收到本轮第一个 audio.delta chunk 前调用一次。
 * 本函数会创建 ringbuffer、启动 IIS 和写入任务；同一轮后续 chunk 不会重复初始化。
 */
esp_err_t audio_player_stream_open(void);

/**
 * @brief 向已打开的流式播放器写入一个 PCM chunk。
 *
 * @param data PCM 采样数组。
 * @param samples int16_t 采样点数量，不是字节数。
 * @param sample_rate_hz 输入采样率；支持 24 kHz 直通和 16 kHz 重采样。
 */
esp_err_t audio_player_write_pcm_chunk(const int16_t *data,
                                       uint32_t samples,
                                       int sample_rate_hz);

/**
 * @brief 结束当前流式播放。
 *
 * 调用方法：TTS 播放任务收到 response.audio.done 对应 end marker 后调用；
 * 会等待 ringbuffer 内已入队 PCM 写完，再停止本轮 IIS 输出。
 */
esp_err_t audio_player_stream_finish(void);

#ifdef __cplusplus
}
#endif

#endif /* SPEAKER_PLAYER_H */
