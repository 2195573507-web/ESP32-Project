#ifndef MIC_ADC_TEST_H
#define MIC_ADC_TEST_H

#include "esp_err.h"
#include "hal/adc_types.h"
#include "soc/soc_caps.h"
#include "app_debug_config.h"

/* 硬件连接：OPA_OUT -> ESP32-C5 GPIO6 / ADC1_CH5。 */
#define MIC_ADC_GPIO_NUM             6                         // Mic 输入 GPIO。
#define MIC_ADC_UNIT                 ADC_UNIT_1                // GPIO6 属于 ADC1。
#define MIC_ADC_CHANNEL              ADC_CHANNEL_5             // GPIO6 对应 ADC1_CH5。

/* ADC continuous 配置：只采一个 Mic 通道。 */
#define MIC_ADC_CONV_MODE            ADC_CONV_SINGLE_UNIT_1    // 只用 ADC1。
#define MIC_ADC_OUTPUT_FORMAT        ADC_DIGI_OUTPUT_FORMAT_TYPE2 // ESP32-C5 DMA 格式。
#define MIC_ADC_ATTEN                ADC_ATTEN_DB_12           // 输入衰减，量程更大。
#define MIC_ADC_BIT_WIDTH            SOC_ADC_DIGI_MAX_BITWIDTH // 使用芯片最大 ADC 位宽。
#define MIC_ADC_SAMPLE_FREQ_HZ       16000                     // 采样率，需等于 PCM 采样率。

/* ADC 读取和统计参数：调试刷新速度、缓冲和任务资源时改这里。 */
#define MIC_ADC_READ_BYTES            512  // 单次 ADC 读取字节数。
#define MIC_ADC_STORE_BYTES           4096 // ADC DMA 缓存大小。
#define MIC_ADC_REPORT_SAMPLES        (MIC_ADC_SAMPLE_FREQ_HZ / 5) // 约 200 ms 一帧 VAD。
#define MIC_ADC_READ_TIMEOUT_MS       1000 // ADC 读取超时。
#define MIC_ADC_ERROR_RETRY_DELAY_MS  100  // 异常后短暂退避。
#define MIC_ADC_TEST_TASK_STACK_SIZE  16384 // mic_adc_test 任务栈；ESP-IDF FreeRTOS 单位为字节。
#define MIC_ADC_TASK_PRIORITY         4    // ADC 任务优先级。
#define MIC_ADC_ENABLE_LOOP_DEBUG_LOG APP_DEBUG_MIC_ADC_LOOP_LOG   // 循环普通日志总开关，错误日志不受影响。
#define MIC_ADC_ENABLE_STACK_DEBUG_LOG APP_DEBUG_MIC_ADC_STACK_LOG  // 任务栈水位诊断开关，WebSocket 已稳定后默认关闭。

/* ASR 流式发送参数：只保留小预缓存和小实时块，避免 ASR 阶段占用整句 PCM 缓存。 */
#define MIC_ADC_ASR_PRE_ROLL_MS        (APP_ASR_AUDIO_PACKET_MS * APP_ASR_PRE_SPEECH_PACKETS) // VOICE_START 前保留 3~5 个 ASR 包的句首 PCM。
#define MIC_ADC_ASR_PRE_ROLL_MAX_MS    1000 // 预缓存上限，防止静态 RAM 误增。
#define MIC_ADC_ASR_PRE_ROLL_SAMPLES   ((MIC_ADC_SAMPLE_FREQ_HZ * MIC_ADC_ASR_PRE_ROLL_MS) / 1000) // 预缓存样本数。
#define MIC_ADC_ASR_POST_ROLL_MS       APP_ASR_POST_ROLL_MS // VOICE_END 后继续发送的尾部 PCM 时长。
#define MIC_ADC_ASR_POST_ROLL_SAMPLES  ((MIC_ADC_SAMPLE_FREQ_HZ * MIC_ADC_ASR_POST_ROLL_MS) / 1000) // 尾部补偿样本数。
#define MIC_ADC_ASR_LIVE_CHUNK_SAMPLES 160  // 实时发送块，160 samples = 10 ms PCM。
#define MIC_ADC_ASR_RETRY_DELAY_MS      2000 // ASR session 启动失败后等待 2 秒再允许下一次启动，避免断网时疯狂重连。

#if MIC_ADC_ASR_PRE_ROLL_MS <= 0
#error "MIC_ADC_ASR_PRE_ROLL_MS must be greater than 0"
#endif

#if MIC_ADC_ASR_PRE_ROLL_MS > MIC_ADC_ASR_PRE_ROLL_MAX_MS
#error "MIC_ADC_ASR_PRE_ROLL_MS must not exceed MIC_ADC_ASR_PRE_ROLL_MAX_MS"
#endif

#if MIC_ADC_ASR_POST_ROLL_MS < 0 || MIC_ADC_ASR_POST_ROLL_MS > 1000
#error "MIC_ADC_ASR_POST_ROLL_MS must be between 0 and 1000"
#endif

#if MIC_ADC_ASR_RETRY_DELAY_MS < 1000 || MIC_ADC_ASR_RETRY_DELAY_MS > 3000
#error "MIC_ADC_ASR_RETRY_DELAY_MS must be between 1000 and 3000"
#endif

/**
 * @brief 启动豆包 ASR WebSocket 和 Mic ADC continuous 采样任务。
 *
 * 硬件链路：外接模拟麦克风 -> 板上 Mic 前端/运放 -> OPA_OUT -> GPIO6/ADC1_CH5。
 * 调用方法：WiFi 已连接且稳定后调用一次；重复调用会直接返回 ESP_OK。
 *
 * 启动顺序必须保持为：
 * 1. WiFi 稳定后启动 ADC continuous。
 * 2. ADC 任务采到 PCM 后先进入 IDLE，只维护句首 pre-roll。
 * 3. VAD 触发 VOICE_START 时才启动一次豆包 ASR 会话并进入 STREAMING。
 * 4. 外层 VAD 触发 VOICE_END 后先发送 post-roll，再进入 FINISHING；
 *    finish/stop 完成本轮 session 后回到 IDLE，继续等待下一次说话。
 *
 * @return 成功返回 ESP_OK，失败返回 ESP-IDF 错误码。
 */
esp_err_t mic_adc_test_start(void);

#endif // MIC_ADC_TEST_H
