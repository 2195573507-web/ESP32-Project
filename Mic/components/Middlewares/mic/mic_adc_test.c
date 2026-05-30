#include "mic_adc_test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mic_adc_pcm.h"
#include "mic_asr_doubao.h"
#include "mic_vad.h"
#include "wifi_manager.h"
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

/* ASR 流式链路只保留这个小环形预缓存，不再分配整句 PCM 缓存，给 WebSocket/TLS 留堆内存。 */
static int16_t s_mic_asr_pre_roll_storage[MIC_ADC_ASR_PRE_ROLL_SAMPLES];
static int16_t s_mic_asr_live_chunk_storage[MIC_ADC_ASR_LIVE_CHUNK_SAMPLES];
static char s_mic_asr_result_text[MIC_ASR_DOUBAO_RESULT_TEXT_MAX_LEN];
static uint8_t s_mic_adc_raw_buffer[MIC_ADC_READ_BYTES];
static adc_continuous_data_t s_mic_adc_parsed_buffer[MIC_ADC_READ_BYTES / SOC_ADC_DIGI_RESULT_BYTES];

/**
 * @brief mic_adc_test 内部的 ASR 流式会话状态。
 *
 * 状态含义：
 * - IDLE：未建立 ASR 会话，只允许把当前安静期 PCM 写入 pre-roll 环形缓存。
 * - STREAMING：VOICE_START 后 ASR start 成功，只允许在这个状态发送 pre-roll 和实时 PCM。
 * - FINISHING：VOICE_END 后立即进入，正在调用 finish/stop 收尾，禁止继续发送 pre-roll 和 PCM。
 * - DEBUG_DONE：单次 ASR 调试模式下 finish 完成后进入，后续样本和 VAD 事件都忽略，
 *   方便串口只保留一次完整协议收发日志，避免自动开始下一轮把关键响应冲掉。
 */
typedef enum {
    MIC_ADC_ASR_STATE_IDLE = 0,    // 空闲态：只维护下一句话的句首预缓存。
    MIC_ADC_ASR_STATE_STREAMING,   // 流式态：当前 ASR 会话可接收 pre-roll 和 PCM。
    MIC_ADC_ASR_STATE_FINISHING,   // 收尾态：当前会话正在结束，新的音频样本全部丢弃。
    MIC_ADC_ASR_STATE_DEBUG_DONE,  // 单次调试完成态：finish 后保持静默，不再启动新的 ASR 会话。
} mic_adc_asr_state_t;

/**
 * @brief ASR 预缓存和流式状态。
 *
 * 调用方法：mic_adc_test_task() 创建后先调用 mic_adc_asr_stream_init()。
 * 每个 PCM 样本调用 mic_adc_asr_stream_push_sample()，VAD 事件由
 * mic_adc_window_report() 调用 start/finish 挂接。
 */
typedef struct {
    int16_t *pre_roll_samples;       // 句首预缓存环形数组，由调用方提供。
    size_t pre_roll_capacity_samples;// 预缓存最大样本数。
    size_t pre_roll_sample_count;    // 当前有效预缓存样本数。
    size_t pre_roll_write_index;     // 下一次写入位置。
    int16_t *live_chunk_samples;      // ASR 已启动后的实时小批量发送缓冲，由静态存储提供。
    size_t live_chunk_capacity_samples;// live_chunk_samples 可容纳的样本数。
    size_t live_chunk_sample_count;   // 当前 live_chunk 中已累计的样本数。
    mic_adc_asr_state_t state;        // ASR 会话三态；只有 STREAMING 允许向豆包发送音频。
} mic_adc_asr_stream_t;

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
 * @brief 初始化 ASR 流式状态。
 *
 * 调用方法：Mic ADC 任务启动时调用一次。预缓存默认 500 ms，最大由编译期检查
 * 限制为不超过 1000 ms，用于弥补 VAD 帧级判断导致的句首延迟。
 *
 * @param stream ASR 流式状态，不能为空。
 * @param pre_roll_storage 预缓存数组，不能为空。
 * @param pre_roll_capacity_samples 预缓存数组可容纳的样本数。
 */
static void mic_adc_asr_stream_init(mic_adc_asr_stream_t *stream,
                                    int16_t *pre_roll_storage,
                                    size_t pre_roll_capacity_samples,
                                    int16_t *live_chunk_storage,
                                    size_t live_chunk_capacity_samples)
{
    if (stream == NULL) {
        return;
    }

    stream->pre_roll_samples = pre_roll_storage;
    stream->pre_roll_capacity_samples = pre_roll_capacity_samples;
    stream->pre_roll_sample_count = 0;
    stream->pre_roll_write_index = 0;
    stream->live_chunk_samples = live_chunk_storage;
    stream->live_chunk_capacity_samples = live_chunk_capacity_samples;
    stream->live_chunk_sample_count = 0;
    stream->state = MIC_ADC_ASR_STATE_IDLE;
}

/**
 * @brief 写入一个样本到 ASR 句首预缓存。
 *
 * 调用方法：ASR 状态为 IDLE 时，mic_adc_asr_stream_push_sample() 每收到一个 PCM 样本调用。
 * STREAMING 和 FINISHING 状态禁止写 pre-roll，避免当前会话音频或收尾期音频污染下一句话。
 *
 * @param stream ASR 流式状态，不能为空。
 * @param pcm_sample int16_t PCM 样本。
 */
static void mic_adc_asr_stream_push_pre_roll(mic_adc_asr_stream_t *stream, int16_t pcm_sample)
{
    if (stream == NULL ||
        stream->pre_roll_samples == NULL ||
        stream->pre_roll_capacity_samples == 0) {
        return;
    }

    stream->pre_roll_samples[stream->pre_roll_write_index] = pcm_sample;
    stream->pre_roll_write_index++;
    if (stream->pre_roll_write_index >= stream->pre_roll_capacity_samples) {
        stream->pre_roll_write_index = 0;
    }
    if (stream->pre_roll_sample_count < stream->pre_roll_capacity_samples) {
        stream->pre_roll_sample_count++;
    }
}

/**
 * @brief 清空 ASR 句首预缓存。
 *
 * 调用方法：pre-roll 已经发送给 ASR、ASR start 失败或 finish 完成后调用，
 * 避免下一段语音开始太快时混入上一段缓存。
 *
 * @param stream ASR 流式状态，不能为空。
 */
static void mic_adc_asr_stream_clear_pre_roll(mic_adc_asr_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }

    stream->pre_roll_sample_count = 0;
    stream->pre_roll_write_index = 0;
}

/**
 * @brief 把 ASR 预缓存按时间顺序发送给豆包 ASR。
 *
 * 调用方法：mic_adc_asr_stream_activate() 在 VAD 检测到 VOICE_START 且 ASR start
 * 成功后调用。函数开头再次检查状态，只有 STREAMING 才允许调用
 * mic_asr_doubao_send_pcm()，确保 FINISHING 期间不会继续补发句首缓存。
 *
 * @param stream ASR 流式状态，不能为空。
 * @return 成功返回 ESP_OK；发送失败返回错误码。
 */
static esp_err_t mic_adc_asr_stream_send_pre_roll(const mic_adc_asr_stream_t *stream)
{
    if (stream == NULL || stream->state != MIC_ADC_ASR_STATE_STREAMING ||
        stream->pre_roll_samples == NULL ||
        stream->pre_roll_sample_count == 0) {
        return ESP_OK;
    }

    size_t start_index = 0;
    if (stream->pre_roll_sample_count == stream->pre_roll_capacity_samples) {
        start_index = stream->pre_roll_write_index;
    }

    for (size_t i = 0; i < stream->pre_roll_sample_count; i++) {
        size_t read_index = start_index + i;
        if (read_index >= stream->pre_roll_capacity_samples) {
            read_index -= stream->pre_roll_capacity_samples;
        }

        esp_err_t ret = mic_asr_doubao_send_pcm(&stream->pre_roll_samples[read_index],
                                                sizeof(int16_t));
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

/**
 * @brief 在 VAD VOICE_START 时启动 ASR 会话，并进入 STREAMING 后发送句首预缓存。
 *
 * 调用方法：mic_adc_window_report() 检测到 MIC_VAD_EVENT_VOICE_START 后调用。
 * 只有 IDLE 才会响应 VOICE_START；start 成功后进入 STREAMING，随后才允许发送
 * pre-roll。start 或 pre-roll 发送失败都直接回到 IDLE，并清空本轮 pre-roll 状态。
 *
 * @param stream ASR 流式状态，不能为空。
 * @return ASR 已进入 PCM 发送状态返回 true；失败返回 false。
 */
static bool mic_adc_asr_stream_activate(mic_adc_asr_stream_t *stream)
{
    if (stream == NULL) {
        return false;
    }
    if (stream->state != MIC_ADC_ASR_STATE_IDLE) {
        return false;
    }

    esp_err_t ret = mic_asr_doubao_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ASR start failed: %s", esp_err_to_name(ret));
        stream->state = MIC_ADC_ASR_STATE_IDLE;
        stream->live_chunk_sample_count = 0;
        mic_adc_asr_stream_clear_pre_roll(stream);
        return false;
    }

    stream->state = MIC_ADC_ASR_STATE_STREAMING;
    stream->live_chunk_sample_count = 0;
    ret = mic_adc_asr_stream_send_pre_roll(stream);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ASR send pre-roll failed: %s", esp_err_to_name(ret));
        mic_asr_doubao_stop();
        stream->state = MIC_ADC_ASR_STATE_IDLE;
        stream->live_chunk_sample_count = 0;
        mic_adc_asr_stream_clear_pre_roll(stream);
        return false;
    }

    mic_adc_asr_stream_clear_pre_roll(stream);
    return true;
}

/**
 * @brief 发送 ASR 实时小批量缓冲中的 PCM。
 *
 * 调用方法：STREAMING 状态下 live_chunk 凑满时调用。它只减少
 * send_pcm() 调用次数，豆包真正的 MIC_ASR_DOUBAO_PACKET_BYTES 分包仍由
 * mic_asr_doubao_send_pcm() 完成。
 *
 * @param stream ASR 流式状态，不能为空。
 * @return 成功返回 ESP_OK；发送失败返回错误码。
 */
static esp_err_t mic_adc_asr_stream_flush_live_chunk(mic_adc_asr_stream_t *stream)
{
    if (stream == NULL || stream->state != MIC_ADC_ASR_STATE_STREAMING ||
        stream->live_chunk_sample_count == 0) {
        return ESP_OK;
    }

    size_t bytes = stream->live_chunk_sample_count * sizeof(stream->live_chunk_samples[0]);
    esp_err_t ret = mic_asr_doubao_send_pcm(stream->live_chunk_samples, bytes);
    if (ret == ESP_OK) {
        stream->live_chunk_sample_count = 0;
    }
    return ret;
}

/**
 * @brief 向 ASR 流式链路追加一个 PCM 样本。
 *
 * 调用方法：每得到一个 int16_t PCM 样本就调用。
 * - IDLE：只写入 500 ms 小环形 pre-roll。
 * - STREAMING：累计 live_chunk，凑满后调用 mic_asr_doubao_send_pcm()。
 * - FINISHING：直接丢弃样本，禁止继续发送 pre-roll 和 PCM。
 *
 * @param stream ASR 流式状态，不能为空。
 * @param pcm_sample int16_t PCM 样本。
 */
static void mic_adc_asr_stream_push_sample(mic_adc_asr_stream_t *stream, int16_t pcm_sample)
{
    if (stream == NULL) {
        return;
    }

    if (stream->state == MIC_ADC_ASR_STATE_IDLE) {
        mic_adc_asr_stream_push_pre_roll(stream, pcm_sample);
        return;
    }
    if (stream->state != MIC_ADC_ASR_STATE_STREAMING) {
        return;
    }

    if (stream->live_chunk_samples == NULL ||
        stream->live_chunk_capacity_samples == 0) {
        return;
    }

    stream->live_chunk_samples[stream->live_chunk_sample_count] = pcm_sample;
    stream->live_chunk_sample_count++;
    if (stream->live_chunk_sample_count < stream->live_chunk_capacity_samples) {
        return;
    }

    esp_err_t ret = mic_adc_asr_stream_flush_live_chunk(stream);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ASR send PCM failed: %s", esp_err_to_name(ret));
        mic_asr_doubao_stop();
        stream->state = MIC_ADC_ASR_STATE_IDLE;
        stream->live_chunk_sample_count = 0;
        mic_adc_asr_stream_clear_pre_roll(stream);
    }
}

/**
 * @brief 在 VAD VOICE_END 时结束 ASR，并关闭 WebSocket。
 *
 * 调用方法：mic_adc_window_report() 检测到 MIC_VAD_EVENT_VOICE_END 后调用。
 * 函数先把 live_chunk 中不足 10 ms 的尾部 PCM 推入 mic_asr_doubao_send_pcm()；
 * 随后进入 FINISHING，因此 ADC 循环期间再来的样本不会继续发送 pre-roll 或 PCM。
 * finish() 会把 mic_asr_doubao 内部保留的最后一块真实 PCM 作为
 * LAST_AUDIO_ONLY_REQUEST 发送，不再额外发 payload_size=0 的空包。
 *
 * @param stream ASR 流式状态，不能为空。
 */
static void mic_adc_asr_stream_finish(mic_adc_asr_stream_t *stream)
{
    if (stream == NULL || stream->state != MIC_ADC_ASR_STATE_STREAMING) {
        return;
    }

    esp_err_t ret = mic_adc_asr_stream_flush_live_chunk(stream);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ASR flush tail PCM failed: %s", esp_err_to_name(ret));
    }
    stream->state = MIC_ADC_ASR_STATE_FINISHING;

    s_mic_asr_result_text[0] = '\0';
    ret = mic_asr_doubao_finish(s_mic_asr_result_text, sizeof(s_mic_asr_result_text));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ASR finish failed: %s", esp_err_to_name(ret));
    }

    mic_asr_doubao_stop();
#if MIC_ADC_ASR_SINGLE_SHOT_DEBUG
    ESP_LOGW(TAG, "ASR single-shot debug done, keep ASR idle until reboot");
    stream->state = MIC_ADC_ASR_STATE_DEBUG_DONE;
#else
    stream->state = MIC_ADC_ASR_STATE_IDLE;
#endif
    stream->live_chunk_sample_count = 0;
    mic_adc_asr_stream_clear_pre_roll(stream);
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
 * @brief 计算当前统计窗口的 Mic 指标，并驱动 VAD 和 ASR 流程。
 *
 * 调用方法：mic_adc_test_task() 累计到 MIC_ADC_REPORT_SAMPLES 个有效样本后调用。
 * 本函数只维护 VAD 和 ASR 流式发送，不再使用整句 PCM 缓存。
 *
 * @param window 已累计完成的统计窗口。
 * @param vad VAD 状态机，不能为空。
 * @param asr_stream ASR 流式状态，不能为空。
 */
static void mic_adc_window_report(const mic_adc_window_t *window,
                                  mic_vad_t *vad,
                                  mic_adc_asr_stream_t *asr_stream)
{
    if (window->count == 0 || vad == NULL || asr_stream == NULL) {
        return;
    }

    uint32_t adc_rms = mic_adc_calc_ac_rms(window->count,
                                           (int64_t)window->sum_raw,
                                           window->sum_square_raw);
    uint32_t adc_p2p = window->max_raw - window->min_raw;
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

    if (vad_event == MIC_VAD_EVENT_VOICE_START) {
        // ASR 阶段采用边采样边发送：这里只打开 PCM 发送闸门并发送句首小预缓存，不再启动整句 PCM 缓存。
        (void)mic_adc_asr_stream_activate(asr_stream);
    } else if (vad_event == MIC_VAD_EVENT_VOICE_END) {
        // 句尾只发送剩余 live_chunk 和 last audio packet，随后关闭 WebSocket。
        mic_adc_asr_stream_finish(asr_stream);
    }
}

/**
 * @brief 打印 mic_adc_test 任务栈剩余水位。
 *
 * 调用方法：只有 MIC_ADC_ENABLE_STACK_DEBUG_LOG 打开时才会在任务启动后和采集循环中调用，
 * 用于临时观察任务栈余量；正式 ASR 流程默认关闭，避免串口被诊断日志占用。
 */
#if MIC_ADC_ENABLE_STACK_DEBUG_LOG
static void mic_adc_log_stack_high_water_mark(void)
{
    ESP_LOGI(TAG,
             "mic_adc_test uxTaskGetStackHighWaterMark: %u",
             (unsigned int)uxTaskGetStackHighWaterMark(NULL));
}
#endif

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
 * 调用方法：由 mic_adc_test_start() 在 WiFi 稳定且 ADC continuous 启动后通过 xTaskCreate() 创建。
 * 不要在外部直接调用。
 * 任务流程：
 * 1. 从 ADC continuous 驱动读取 DMA 原始帧。
 * 2. 调用 adc_continuous_parse_data() 解析出 ADC 单元、通道和 raw 值。
 * 3. 只保留 GPIO6 对应的 ADC1_CH5 有效样本。
 * 4. 调用独立的 mic_adc_pcm 模块把 raw 转成 16000 Hz/mono/int16/little-endian PCM。
 * 5. 每个 PCM 样本实时送入 ASR 三态链路，不再写入整句 PCM 缓存。
 * 6. 累计一个统计窗口后，更新 VAD；VOICE_START 启动豆包 ASR，VOICE_END 进入 FINISHING。
 *
 * @param arg adc_continuous_handle_t 句柄，由 mic_adc_test_start() 传入。
 */
static void mic_adc_test_task(void *arg)
{
    adc_continuous_handle_t handle = (adc_continuous_handle_t)arg;
    mic_adc_window_t window;
    mic_adc_pcm_converter_t pcm_converter;
    mic_vad_t vad;
    mic_adc_asr_stream_t asr_stream;
#if MIC_ADC_ENABLE_STACK_DEBUG_LOG
    TickType_t last_stack_log_tick = 0;
#endif

    mic_adc_pcm_converter_init(&pcm_converter);
    mic_vad_init(&vad);
    mic_adc_asr_stream_init(&asr_stream,
                            s_mic_asr_pre_roll_storage,
                            MIC_ADC_ASR_PRE_ROLL_SAMPLES,
                            s_mic_asr_live_chunk_storage,
                            MIC_ADC_ASR_LIVE_CHUNK_SAMPLES);
    mic_adc_window_reset(&window);
    ESP_LOGI(TAG,
             "mic_adc_test task started, ASR input is signed int16 little-endian PCM converted by mic_adc_pcm_convert_sample(), not raw ADC values");
#if MIC_ADC_ENABLE_STACK_DEBUG_LOG
    mic_adc_log_stack_high_water_mark();
    last_stack_log_tick = xTaskGetTickCount();
#endif

    while (1) {
#if MIC_ADC_ENABLE_STACK_DEBUG_LOG
        TickType_t now = xTaskGetTickCount();
        if ((now - last_stack_log_tick) >= pdMS_TO_TICKS(1000)) {
            last_stack_log_tick = now;
            mic_adc_log_stack_high_water_mark();
        }
#endif

        uint32_t read_bytes = 0;
        esp_err_t ret = adc_continuous_read(handle,
                                            s_mic_adc_raw_buffer,
                                            sizeof(s_mic_adc_raw_buffer),
                                            &read_bytes,
                                            MIC_ADC_READ_TIMEOUT_MS);
        if (ret == ESP_ERR_TIMEOUT) {
#if MIC_ADC_ENABLE_LOOP_DEBUG_LOG
            ESP_LOGW(TAG, "ADC read timeout");
#endif
            continue;
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(MIC_ADC_ERROR_RETRY_DELAY_MS));
            continue;
        }

        uint32_t sample_count = 0;
        ret = adc_continuous_parse_data(handle,
                                        s_mic_adc_raw_buffer,
                                        read_bytes,
                                        s_mic_adc_parsed_buffer,
                                        &sample_count);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC parse failed: %s", esp_err_to_name(ret));
            continue;
        }

        for (uint32_t i = 0; i < sample_count; i++) {
            if (!s_mic_adc_parsed_buffer[i].valid ||
                s_mic_adc_parsed_buffer[i].unit != MIC_ADC_UNIT ||
                s_mic_adc_parsed_buffer[i].channel != MIC_ADC_CHANNEL) {
                continue;
            }

            int16_t pcm_sample = mic_adc_pcm_convert_sample(&pcm_converter,
                                                            s_mic_adc_parsed_buffer[i].raw_data);
            // ASR 采用流式发送：未触发 VAD 时写入小预缓存，触发后边采样边发送 PCM。
            mic_adc_asr_stream_push_sample(&asr_stream, pcm_sample);
            mic_adc_window_add(&window, s_mic_adc_parsed_buffer[i].raw_data, pcm_sample);
            if (window.count >= MIC_ADC_REPORT_SAMPLES) {
                mic_adc_window_report(&window, &vad, &asr_stream);
                mic_adc_window_reset(&window);
            }
        }
    }
}

/**
 * @brief 启动 Mic ADC continuous 采样测试。
 *
 * 调用方法：app_main() 在 WiFi 已连接且稳定后调用一次。
 * 正确启动顺序必须是：WiFi 稳定后先启动 Mic ADC continuous；ADC 任务在 IDLE
 * 状态只收集 pre-roll；VAD 触发 VOICE_START 时再调用 mic_asr_doubao_start() 建立
 * 本次 ASR 会话并进入 STREAMING。
 *
 * @return 成功返回 ESP_OK，失败返回 ESP-IDF 错误码。
 */
esp_err_t mic_adc_test_start(void)
{
    if (s_mic_adc_task_handle != NULL) {
        return ESP_OK;
    }

    if (!wifi_is_stable()) {
        ESP_LOGW(TAG, "WiFi is not stable yet, skip Mic ADC/ASR start");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = mic_adc_continuous_init(&s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC continuous init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = adc_continuous_start(s_adc_handle);
    if (ret != ESP_OK) {
        adc_continuous_deinit(s_adc_handle);
        s_adc_handle = NULL;
        ESP_LOGE(TAG, "ADC continuous start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t task_created = xTaskCreate(mic_adc_test_task,
                                          "mic_adc_test",
                                          MIC_ADC_TEST_TASK_STACK_SIZE,
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
