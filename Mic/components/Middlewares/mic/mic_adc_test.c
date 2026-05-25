#include "mic_adc_test.h"

#include <stdint.h>

#include "mic_adc_pcm.h"
#include "mic_serial_output.h"
#include "mic_vad.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_adc/adc_continuous.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if MIC_ADC_SAMPLE_FREQ_HZ != MIC_ADC_PCM_SAMPLE_RATE_HZ
#error "MIC_ADC_SAMPLE_FREQ_HZ must match MIC_ADC_PCM_SAMPLE_RATE_HZ"
#endif

/* 日志标签仅供本模块内部使用，不作为后期调试参数暴露。 */
static const char *TAG = "mic_adc_test";

/* ADC continuous 句柄：由 mic_adc_test_start() 初始化，采样任务只借用该句柄读取数据。 */
static adc_continuous_handle_t s_adc_handle;

/* 任务句柄：用于防止重复创建 Mic ADC 测试任务。 */
static TaskHandle_t s_mic_adc_task_handle;

/**
 * @brief 一个串口统计窗口内的 Mic ADC 原始数据累加状态。
 *
 * 调用方法：mic_adc_test_task() 创建局部变量，随后通过
 * mic_adc_window_reset()、mic_adc_window_add()、mic_adc_window_report() 维护。
 */
typedef struct {
    uint32_t count;          // 当前窗口已经累计的有效 ADC 样本数。
    uint32_t min_raw;        // 当前窗口的最小 raw 值，用于观察安静/说话时的摆幅下沿。
    uint32_t max_raw;        // 当前窗口的最大 raw 值，用于观察安静/说话时的摆幅上沿。
    uint32_t last_raw;       // 最近一个 raw 值，用于确认 ADC 数据正在刷新。
    uint64_t sum_raw;        // raw 值累加和，用于计算平均值，即前端直流偏置附近的中心点。
    uint64_t sum_square_raw; // raw 平方累加和，用于计算去直流后的 RMS。
    uint32_t adc_clip_low;   // ADC raw 等于 0 的次数，用于判断低端削顶。
    uint32_t adc_clip_high;  // ADC raw 等于最大值的次数，用于判断高端削顶。
    int16_t min_pcm;         // 当前窗口 PCM 最小值，用于观察转换后的负向摆幅。
    int16_t max_pcm;         // 当前窗口 PCM 最大值，用于观察转换后的正向摆幅。
    int16_t last_pcm;        // 最近一个 PCM 样本，用于确认 ADC -> PCM 链路正在刷新。
    int64_t sum_pcm;         // PCM 样本累加和，用于观察去直流后是否接近 0。
    uint64_t sum_square_pcm; // PCM 平方累加和，用于计算转换后的音频 RMS。
    uint32_t pcm_clip_low;   // PCM 等于 INT16_MIN 的次数，用于判断负向削顶。
    uint32_t pcm_clip_high;  // PCM 等于 INT16_MAX 的次数，用于判断正向削顶。
} mic_adc_window_t;

/**
 * @brief 对 64 位无符号整数做整数平方根。
 *
 * 调用方法：mic_adc_window_report() 计算 RMS 时调用，避免引入浮点 sqrt 依赖。
 *
 * @param value 要开平方的无符号整数。
 * @return floor(sqrt(value))。
 */
static uint32_t mic_adc_isqrt_u64(uint64_t value)
{
    uint64_t result = 0;
    uint64_t bit = (uint64_t)1 << 62;

    while (bit > value) {
        bit >>= 2;
    }

    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }

    return (uint32_t)result;
}

/**
 * @brief 计算一个窗口内的 AC RMS。
 *
 * 调用方法：mic_adc_window_report() 分别计算 ADC raw 和 PCM RMS 时调用。
 * 公式为 sqrt(E[x^2] - E[x]^2)，直接使用 sum 和 sum_square，避免旧算法因整数舍入得到 0。
 *
 * @param count 样本数量。
 * @param sum 样本累加和，可以为负数。
 * @param sum_square 样本平方累加和。
 * @return 去直流后的 RMS。
 */
static uint32_t mic_adc_calc_ac_rms(uint32_t count, int64_t sum, uint64_t sum_square)
{
    if (count == 0) {
        return 0;
    }

    uint64_t sample_count = count;
    uint64_t rms_denominator = sample_count * sample_count;
    uint64_t mean_square_scaled = sample_count * sum_square;
    uint64_t avg_square_scaled = (uint64_t)(sum * sum);

    if (mean_square_scaled <= avg_square_scaled) {
        return 0;
    }

    uint64_t variance = (mean_square_scaled - avg_square_scaled + rms_denominator / 2) /
                        rms_denominator;
    return mic_adc_isqrt_u64(variance);
}

/**
 * @brief 重置一个 Mic ADC 统计窗口。
 *
 * 调用方法：任务开始时调用一次；每次串口输出一行统计结果后再次调用。
 *
 * @param window 要重置的统计窗口，不能为空。
 */
static void mic_adc_window_reset(mic_adc_window_t *window)
{
    window->count = 0;
    window->min_raw = UINT32_MAX;
    window->max_raw = 0;
    window->last_raw = 0;
    window->sum_raw = 0;
    window->sum_square_raw = 0;
    window->adc_clip_low = 0;
    window->adc_clip_high = 0;
    window->min_pcm = INT16_MAX;
    window->max_pcm = INT16_MIN;
    window->last_pcm = 0;
    window->sum_pcm = 0;
    window->sum_square_pcm = 0;
    window->pcm_clip_low = 0;
    window->pcm_clip_high = 0;
}

/**
 * @brief 向当前统计窗口追加一个有效 ADC raw 样本。
 *
 * 调用方法：mic_adc_test_task() 解析到目标 ADC1_CH5 的有效样本，并完成
 * ADC -> PCM 转换后调用。
 *
 * @param window 当前统计窗口，不能为空。
 * @param raw ADC continuous 驱动解析出的原始采样值。
 * @param pcm 由 mic_adc_pcm_convert_sample() 转出的 int16_t PCM 样本。
 */
static void mic_adc_window_add(mic_adc_window_t *window, uint32_t raw, int16_t pcm)
{
    window->count++;
    window->last_raw = raw;
    window->sum_raw += raw;
    window->sum_square_raw += (uint64_t)raw * raw;
    window->last_pcm = pcm;
    window->sum_pcm += pcm;
    int32_t pcm_i32 = pcm;
    window->sum_square_pcm += (uint64_t)(pcm_i32 * pcm_i32);

    if (raw < window->min_raw) {
        window->min_raw = raw;
    }
    if (raw > window->max_raw) {
        window->max_raw = raw;
    }
    if (raw == 0) {
        window->adc_clip_low++;
    }
    if (raw >= MIC_ADC_PCM_ADC_RAW_MAX) {
        window->adc_clip_high++;
    }
    if (pcm < window->min_pcm) {
        window->min_pcm = pcm;
    }
    if (pcm > window->max_pcm) {
        window->max_pcm = pcm;
    }
    if (pcm == INT16_MIN) {
        window->pcm_clip_low++;
    }
    if (pcm == INT16_MAX) {
        window->pcm_clip_high++;
    }
}

/**
 * @brief 计算当前统计窗口的 Mic 指标，并交给串口输出模块打印。
 *
 * 调用方法：mic_adc_test_task() 累计到 MIC_ADC_REPORT_SAMPLES 个有效样本后调用。
 * 输出字段说明：
 * - adc_last/adc_min/adc_max/adc_avg: ADC raw 的最近值、上下沿和平均值。
 * - adc_rms/pcm_rms: 去直流后的 RMS，安静时小，说话时变大。
 * - adc_p2p/pcm_p2p: 峰峰值，用于观察波形摆幅。
 * - *_clip_*: 当前窗口削顶次数，非 0 表示输入或 PCM 已接近过载。
 * - vad_state/vad_event: VAD 当前状态码和本帧事件码。
 * - VOICE_START/VOICE_END: 串口输出模块根据 VAD 事件打印语音活动事件。
 *
 * @param window 已累计完成的统计窗口。
 * @param vad VAD 状态机，不能为空。
 */
static void mic_adc_window_report(const mic_adc_window_t *window, mic_vad_t *vad)
{
    if (window->count == 0) {
        return;
    }

    uint32_t avg_raw = (uint32_t)((window->sum_raw + window->count / 2) / window->count);
    uint32_t adc_rms = mic_adc_calc_ac_rms(window->count,
                                           (int64_t)window->sum_raw,
                                           window->sum_square_raw);
    uint32_t adc_p2p = window->max_raw - window->min_raw;
    int32_t avg_pcm = (int32_t)(window->sum_pcm / (int64_t)window->count);
    uint32_t pcm_rms = mic_adc_calc_ac_rms(window->count,
                                           window->sum_pcm,
                                           window->sum_square_pcm);
    uint32_t pcm_p2p = (uint32_t)((int32_t)window->max_pcm - (int32_t)window->min_pcm);
    // clipped 是总削顶标记：ADC 或 PCM 任意方向发生削顶，都输出 1。
    uint32_t clipped = (window->adc_clip_low > 0 ||
                        window->adc_clip_high > 0 ||
                        window->pcm_clip_low > 0 ||
                        window->pcm_clip_high > 0) ? 1 : 0;
    mic_vad_features_t vad_features = {
        .adc_rms = adc_rms,
        .adc_p2p = adc_p2p,
        .pcm_rms = pcm_rms,
        .pcm_p2p = pcm_p2p,
        .clipped = clipped,
    };
    mic_vad_event_t vad_event = mic_vad_process(vad, &vad_features);

    mic_serial_output_frame_t output_frame = {
        .gpio_num = MIC_ADC_GPIO_NUM,
        .adc_unit = (uint32_t)MIC_ADC_UNIT + 1,
        .adc_channel = MIC_ADC_CHANNEL,
        .samples = window->count,
        .adc_last = window->last_raw,
        .adc_min = window->min_raw,
        .adc_max = window->max_raw,
        .adc_avg = avg_raw,
        .adc_rms = adc_rms,
        .adc_p2p = adc_p2p,
        .adc_clip_low = window->adc_clip_low,
        .adc_clip_high = window->adc_clip_high,
        .pcm_last = window->last_pcm,
        .pcm_min = window->min_pcm,
        .pcm_max = window->max_pcm,
        .pcm_avg = avg_pcm,
        .pcm_rms = pcm_rms,
        .pcm_p2p = pcm_p2p,
        .pcm_clip_low = window->pcm_clip_low,
        .pcm_clip_high = window->pcm_clip_high,
        .clipped = clipped,
        .vad_state = vad->state,
        .vad_event = vad_event,
    };
    mic_serial_output_print_adc_frame(&output_frame);

    if (vad_event == MIC_VAD_EVENT_VOICE_START) {
        mic_serial_output_print_voice_start(&output_frame);
    } else if (vad_event == MIC_VAD_EVENT_VOICE_END) {
        mic_serial_output_print_voice_end(&output_frame);
    }
}

/**
 * @brief 初始化 ADC continuous 驱动并绑定 Mic 所在的 ADC1_CH5。
 *
 * 调用方法：仅由 mic_adc_test_start() 调用一次；成功后通过 out_handle 返回驱动句柄。
 *
 * @param out_handle 输出参数，用于保存初始化完成的 ADC continuous 句柄。
 * @return 成功返回 ESP_OK，失败返回 ESP-IDF 错误码。
 */
static esp_err_t mic_adc_continuous_init(adc_continuous_handle_t *out_handle)
{
    adc_continuous_handle_t handle = NULL;
    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = MIC_ADC_STORE_BYTES,
        .conv_frame_size = MIC_ADC_READ_BYTES,
        .flags.flush_pool = true,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_new_handle(&handle_cfg, &handle), TAG, "create ADC handle failed");

    adc_digi_pattern_config_t pattern = {
        .atten = MIC_ADC_ATTEN,
        // ESP-IDF 示例要求 pattern channel 只保留低 3 bit；GPIO6/ADC1_CH5 保持为 5。
        .channel = MIC_ADC_CHANNEL & 0x7,
        .unit = MIC_ADC_UNIT,
        .bit_width = MIC_ADC_BIT_WIDTH,
    };

    adc_continuous_config_t adc_cfg = {
        .pattern_num = 1,
        .adc_pattern = &pattern,
        .sample_freq_hz = MIC_ADC_SAMPLE_FREQ_HZ,
        .conv_mode = MIC_ADC_CONV_MODE,
        .format = MIC_ADC_OUTPUT_FORMAT,
    };

    esp_err_t ret = adc_continuous_config(handle, &adc_cfg);
    if (ret != ESP_OK) {
        adc_continuous_deinit(handle);
        return ret;
    }

    *out_handle = handle;
    return ESP_OK;
}

/**
 * @brief Mic ADC continuous 采样和串口统计任务。
 *
 * 调用方法：由 mic_adc_test_start() 通过 xTaskCreate() 创建，不要在外部直接调用。
 * 任务流程：
 * 1. 从 ADC continuous 驱动读取 DMA 原始帧。
 * 2. 调用 adc_continuous_parse_data() 解析出 ADC 单元、通道和 raw 值。
 * 3. 只保留 GPIO6 对应的 ADC1_CH5 有效样本。
 * 4. 调用独立的 mic_adc_pcm 模块把 raw 转成 16000 Hz/mono/int16/little-endian PCM。
 * 5. 累计一个统计窗口后，交给 mic_serial_output 模块输出 MIC_ADC CSV 行。
 *
 * @param arg adc_continuous_handle_t 句柄，由 mic_adc_test_start() 传入。
 */
static void mic_adc_test_task(void *arg)
{
    adc_continuous_handle_t handle = (adc_continuous_handle_t)arg;
    uint8_t raw_buffer[MIC_ADC_READ_BYTES] = {0};
    adc_continuous_data_t parsed[MIC_ADC_READ_BYTES / SOC_ADC_DIGI_RESULT_BYTES] = {0};
    mic_adc_window_t window;
    mic_adc_pcm_converter_t pcm_converter;
    mic_vad_t vad;

    mic_adc_pcm_converter_init(&pcm_converter);
    mic_vad_init(&vad);
    mic_adc_window_reset(&window);

    while (1) {
        uint32_t read_bytes = 0;
        esp_err_t ret = adc_continuous_read(handle,
                                            raw_buffer,
                                            sizeof(raw_buffer),
                                            &read_bytes,
                                            MIC_ADC_READ_TIMEOUT_MS);
        if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "ADC read timeout");
            continue;
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(MIC_ADC_ERROR_RETRY_DELAY_MS));
            continue;
        }

        uint32_t sample_count = 0;
        ret = adc_continuous_parse_data(handle, raw_buffer, read_bytes, parsed, &sample_count);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC parse failed: %s", esp_err_to_name(ret));
            continue;
        }

        for (uint32_t i = 0; i < sample_count; i++) {
            if (!parsed[i].valid ||
                parsed[i].unit != MIC_ADC_UNIT ||
                parsed[i].channel != MIC_ADC_CHANNEL) {
                continue;
            }

            int16_t pcm_sample = mic_adc_pcm_convert_sample(&pcm_converter, parsed[i].raw_data);
            mic_adc_window_add(&window, parsed[i].raw_data, pcm_sample);
            if (window.count >= MIC_ADC_REPORT_SAMPLES) {
                mic_adc_window_report(&window, &vad);
                mic_adc_window_reset(&window);
            }
        }
    }
}

/**
 * @brief 启动 Mic ADC continuous 采样测试。
 *
 * 调用方法：app_main() 或其他上层入口在系统启动后调用一次。
 * 本函数只依赖 ESP-IDF ADC/FreeRTOS/日志模块，不依赖 WiFi 管理器，方便后续单独移植。
 *
 * @return 成功返回 ESP_OK，失败返回 ESP-IDF 错误码。
 */
esp_err_t mic_adc_test_start(void)
{
    if (s_mic_adc_task_handle != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(mic_adc_continuous_init(&s_adc_handle), TAG, "ADC continuous init failed");

    esp_err_t ret = adc_continuous_start(s_adc_handle);
    if (ret != ESP_OK) {
        adc_continuous_deinit(s_adc_handle);
        s_adc_handle = NULL;
        ESP_LOGE(TAG, "ADC continuous start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t task_created = xTaskCreate(mic_adc_test_task,
                                          "mic_adc_test",
                                          MIC_ADC_TASK_STACK,
                                          s_adc_handle,
                                          MIC_ADC_TASK_PRIORITY,
                                          &s_mic_adc_task_handle);
    if (task_created != pdPASS) {
        s_mic_adc_task_handle = NULL;
        adc_continuous_stop(s_adc_handle);
        adc_continuous_deinit(s_adc_handle);
        s_adc_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "Mic ADC continuous started: OPA_OUT -> GPIO%d/ADC1_CH%d, sample_rate=%d Hz",
             MIC_ADC_GPIO_NUM,
             (int)MIC_ADC_CHANNEL,
             MIC_ADC_SAMPLE_FREQ_HZ);
    return ESP_OK;
}
