#ifndef MIC_SERIAL_OUTPUT_H
#define MIC_SERIAL_OUTPUT_H

#include <stdint.h>

/**
 * @file mic_serial_output.h
 * @brief Mic 串口输出模块。
 *
 * 本模块只负责把已经算好的 ADC、PCM、VAD 指标打印到串口，不采样、不转换、不判断 VAD。
 */

/* 串口输出前缀：上位机可按这些字符串过滤日志。 */
#define MIC_SERIAL_OUTPUT_ADC_PREFIX         "MIC_ADC"     // 每帧 ADC/PCM 统计。
#define MIC_SERIAL_OUTPUT_VOICE_START_PREFIX "VOICE_START" // VAD 检测到开始说话。
#define MIC_SERIAL_OUTPUT_VOICE_END_PREFIX   "VOICE_END"   // VAD 检测到说话结束。

/**
 * @brief 一帧 Mic 串口输出数据。
 *
 * 调用方法：ADC 统计窗口算完后填入本结构，再调用 mic_serial_output_print_*()。
 */
typedef struct {
    uint32_t gpio_num;       // Mic 输入 GPIO。
    uint32_t adc_unit;       // ADC 单元编号，从 1 开始打印。
    uint32_t adc_channel;    // ADC 通道编号。
    uint32_t samples;        // 本帧有效样本数。

    uint32_t adc_last;       // ADC 最近值。
    uint32_t adc_min;        // ADC 最小值。
    uint32_t adc_max;        // ADC 最大值。
    uint32_t adc_avg;        // ADC 平均值。
    uint32_t adc_rms;        // ADC 去直流 RMS。
    uint32_t adc_p2p;        // ADC 峰峰值。
    uint32_t adc_clip_low;   // ADC 低端削顶次数。
    uint32_t adc_clip_high;  // ADC 高端削顶次数。

    int32_t pcm_last;        // PCM 最近值。
    int32_t pcm_min;         // PCM 最小值。
    int32_t pcm_max;         // PCM 最大值。
    int32_t pcm_avg;         // PCM 平均值。
    uint32_t pcm_rms;        // PCM 去直流 RMS。
    uint32_t pcm_p2p;        // PCM 峰峰值。
    uint32_t pcm_clip_low;   // PCM 负向削顶次数。
    uint32_t pcm_clip_high;  // PCM 正向削顶次数。

    uint32_t clipped;        // 总削顶标记。
    uint32_t vad_state;      // VAD 状态码。
    uint32_t vad_event;      // VAD 事件码。
} mic_serial_output_frame_t;

/**
 * @brief 打印一帧 MIC_ADC 统计日志。
 *
 * 调用方法：每个 ADC 统计窗口结束后调用一次。
 *
 * @param frame 已填好的输出数据，不能为空。
 */
void mic_serial_output_print_adc_frame(const mic_serial_output_frame_t *frame);

/**
 * @brief 打印 VOICE_START 事件。
 *
 * 调用方法：VAD 返回 VOICE_START 事件时调用。
 *
 * @param frame 当前帧输出数据，不能为空。
 */
void mic_serial_output_print_voice_start(const mic_serial_output_frame_t *frame);

/**
 * @brief 打印 VOICE_END 事件。
 *
 * 调用方法：VAD 返回 VOICE_END 事件时调用。
 *
 * @param frame 当前帧输出数据，不能为空。
 */
void mic_serial_output_print_voice_end(const mic_serial_output_frame_t *frame);

#endif // MIC_SERIAL_OUTPUT_H
