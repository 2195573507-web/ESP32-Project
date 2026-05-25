#ifndef MIC_ADC_TEST_H
#define MIC_ADC_TEST_H

#include "esp_err.h"
#include "hal/adc_types.h"
#include "soc/soc_caps.h"

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
#define MIC_ADC_READ_BYTES           512                       // 单次读取字节数。
#define MIC_ADC_STORE_BYTES          4096                      // ADC DMA 缓存大小。
#define MIC_ADC_REPORT_SAMPLES       (MIC_ADC_SAMPLE_FREQ_HZ / 5) // 约 200 ms 输出一次。
#define MIC_ADC_READ_TIMEOUT_MS      1000                      // ADC 读取超时。
#define MIC_ADC_ERROR_RETRY_DELAY_MS 100                       // 异常后等待时间。
#define MIC_ADC_TASK_STACK           6144                      // ADC 任务栈大小。
#define MIC_ADC_TASK_PRIORITY        4                         // ADC 任务优先级。

/**
 * @brief 启动 Mic ADC continuous 采样测试任务。
 *
 * 硬件链路：外接模拟麦克风 -> 板上 Mic 前端/运放 -> OPA_OUT -> GPIO6/ADC1_CH5。
 * 任务会周期性通过串口输出 raw/min/max/avg/RMS/p2p，方便观察说话和安静时的幅度差异。
 * 调用方法：系统初始化后调用一次；重复调用会直接返回 ESP_OK。
 *
 * @return 成功返回 ESP_OK，失败返回 ESP-IDF 错误码。
 */
esp_err_t mic_adc_test_start(void);

#endif // MIC_ADC_TEST_H
