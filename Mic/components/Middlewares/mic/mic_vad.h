#ifndef MIC_VAD_H
#define MIC_VAD_H

#include <stdint.h>

/**
 * @file mic_vad.h
 * @brief 独立的最小语音活动检测模块。
 *
 * 本模块不读取 ADC、不转换 PCM、不依赖 FreeRTOS；只根据一帧统计值判断是否有人声活动。
 */

/* VAD 时间参数：当前 MIC_ADC 约 200 ms 输出一帧统计。 */
#define MIC_VAD_FRAME_MS           200 // 每帧统计对应的时间。
#define MIC_VAD_END_HOLD_MS        800 // 连续安静这么久后输出 VOICE_END。

/* VAD 启动阈值：任意一项达到，就认为有语音活动。 */
#define MIC_VAD_ADC_RMS_START      80   // ADC RMS 启动阈值。
#define MIC_VAD_ADC_P2P_START      200  // ADC 峰峰值启动阈值。
#define MIC_VAD_PCM_RMS_START      300  // PCM RMS 启动阈值。
#define MIC_VAD_PCM_P2P_START      2000 // PCM 峰峰值启动阈值。

/* VAD 安静阈值：全部低于这些值，才算当前帧安静。 */
#define MIC_VAD_ADC_RMS_STOP       40   // ADC RMS 安静阈值。
#define MIC_VAD_ADC_P2P_STOP       120  // ADC 峰峰值安静阈值。
#define MIC_VAD_PCM_RMS_STOP       250  // PCM RMS 安静阈值。
#define MIC_VAD_PCM_P2P_STOP       1500 // PCM 峰峰值安静阈值。

/**
 * @brief VAD 状态机状态。
 *
 * 调用方法：上层一般只读 state 做调试，不需要直接修改。
 * 串口状态码：0=IDLE，1=SPEAKING，2=SILENCE_HOLD。
 */
typedef enum {
    MIC_VAD_STATE_IDLE = 0,      // 空闲，当前没有语音。
    MIC_VAD_STATE_SPEAKING,      // 说话中。
    MIC_VAD_STATE_SILENCE_HOLD,  // 已变安静，等待结束保持时间。
} mic_vad_state_t;

/**
 * @brief VAD 输出事件。
 *
 * 调用方法：mic_vad_process() 每处理一帧返回一个事件，上层按事件打印日志。
 * 串口事件码：0=NONE，1=VOICE_START，2=VOICE_END。
 */
typedef enum {
    MIC_VAD_EVENT_NONE = 0,         // 没有新事件。
    MIC_VAD_EVENT_VOICE_START,      // 本帧检测到说话开始。
    MIC_VAD_EVENT_VOICE_END,        // 本帧检测到说话结束。
} mic_vad_event_t;

/**
 * @brief VAD 输入特征。
 *
 * 调用方法：ADC 统计窗口算出 adc_rms/adc_p2p/pcm_rms/pcm_p2p/clipped 后填入本结构。
 */
typedef struct {
    uint32_t adc_rms;  // ADC 去直流 RMS。
    uint32_t adc_p2p;  // ADC 峰峰值。
    uint32_t pcm_rms;  // PCM 去直流 RMS。
    uint32_t pcm_p2p;  // PCM 峰峰值。
    uint32_t clipped;  // 1 表示 ADC 或 PCM 有削顶。
} mic_vad_features_t;

/**
 * @brief VAD 状态机上下文。
 *
 * 调用方法：定义一个 mic_vad_t 变量，先调用 mic_vad_init()，再逐帧调用 mic_vad_process()。
 */
typedef struct {
    mic_vad_state_t state;      // 当前状态。
    uint32_t silence_hold_ms;   // 已连续安静的时间。
} mic_vad_t;

/**
 * @brief 初始化 VAD 状态机。
 *
 * 调用方法：Mic ADC 任务启动时调用一次。
 *
 * @param vad VAD 状态机，不能为空。
 */
void mic_vad_init(mic_vad_t *vad);

/**
 * @brief 处理一帧统计值并更新 VAD 状态。
 *
 * 调用方法：每输出一帧 MIC_ADC 统计时调用一次。
 *
 * @param vad VAD 状态机，不能为空。
 * @param features 当前帧统计值，不能为空。
 * @return 本帧产生的 VAD 事件。
 */
mic_vad_event_t mic_vad_process(mic_vad_t *vad, const mic_vad_features_t *features);

#endif // MIC_VAD_H
