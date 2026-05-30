#ifndef MIC_RECORD_BUFFER_H
#define MIC_RECORD_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file mic_record_buffer.h
 * @brief 独立的 Mic PCM 句子录音缓存模块。
 *
 * 本模块不采样、不做 VAD、不依赖 FreeRTOS；只负责按开始/停止事件缓存 int16_t PCM。
 * 调用方提供固定存储数组，模块不会 malloc，避免录音过长时无限占用内存。
 */

/* PCM 格式需与 mic_adc_pcm 保持一致：16000 Hz、单声道、int16_t。 */
#define MIC_RECORD_BUFFER_SAMPLE_RATE_HZ 16000
#define MIC_RECORD_BUFFER_MAX_RECORD_MS  3000
#define MIC_RECORD_BUFFER_PRE_ROLL_MS    200

#define MIC_RECORD_BUFFER_MS_TO_SAMPLES(ms) \
    ((MIC_RECORD_BUFFER_SAMPLE_RATE_HZ * (ms)) / 1000)
#define MIC_RECORD_BUFFER_DEFAULT_CAPACITY_SAMPLES \
    MIC_RECORD_BUFFER_MS_TO_SAMPLES(MIC_RECORD_BUFFER_MAX_RECORD_MS)
#define MIC_RECORD_BUFFER_PRE_ROLL_SAMPLES \
    MIC_RECORD_BUFFER_MS_TO_SAMPLES(MIC_RECORD_BUFFER_PRE_ROLL_MS)
#define MIC_RECORD_BUFFER_BYTES_PER_SAMPLE ((size_t)sizeof(int16_t))

#if MIC_RECORD_BUFFER_MAX_RECORD_MS <= 0
#error "MIC_RECORD_BUFFER_MAX_RECORD_MS must be greater than 0"
#endif

#if MIC_RECORD_BUFFER_PRE_ROLL_MS < 0
#error "MIC_RECORD_BUFFER_PRE_ROLL_MS must be greater than or equal to 0"
#endif

#if MIC_RECORD_BUFFER_PRE_ROLL_MS > MIC_RECORD_BUFFER_MAX_RECORD_MS
#error "MIC_RECORD_BUFFER_PRE_ROLL_MS must not exceed MIC_RECORD_BUFFER_MAX_RECORD_MS"
#endif

/**
 * @brief 录音缓存状态。
 *
 * 调用方法：上层只读 state 做调试，不需要直接修改。
 */
typedef enum {
    MIC_RECORD_BUFFER_STATE_IDLE = 0,       // 空闲，只维护预录音缓存。
    MIC_RECORD_BUFFER_STATE_RECORDING,      // 正在缓存一句话的 PCM。
    MIC_RECORD_BUFFER_STATE_READY,          // 已停止，缓存里有一整句话 PCM。
} mic_record_buffer_state_t;

/**
 * @brief PCM 句子录音缓存上下文。
 *
 * 调用方法：定义变量后先调用 mic_record_buffer_init()，再持续喂入 PCM 样本；
 * VAD 检测到 VOICE_START 时调用 mic_record_buffer_start()，检测到 VOICE_END 时调用
 * mic_record_buffer_stop()。
 */
typedef struct {
    int16_t *record_samples;          // 一整句话 PCM 存储，由调用方提供。
    size_t record_capacity_samples;   // record_samples 最大样本数。
    size_t record_sample_count;       // 当前已经缓存的样本数。

    int16_t *pre_roll_samples;        // 说话开始前的短缓存，由调用方提供，可为空。
    size_t pre_roll_capacity_samples; // pre_roll_samples 最大样本数。
    size_t pre_roll_sample_count;     // 当前预录音样本数。
    size_t pre_roll_write_index;      // 预录音环形缓存下一次写入位置。

    mic_record_buffer_state_t state;  // 当前缓存状态。
    bool overflowed;                  // true 表示本句话超过容量，后续样本被丢弃。
} mic_record_buffer_t;

/**
 * @brief 初始化录音缓存。
 *
 * 调用方法：任务启动时调用一次。record_storage 建议使用 static 全局数组，避免占用任务栈。
 *
 * @param buffer 录音缓存上下文，不能为空。
 * @param record_storage 一整句话 PCM 存储，不能为空。
 * @param record_capacity_samples record_storage 可容纳的 int16_t 样本数。
 * @param pre_roll_storage 预录音存储；不需要预录音时可传 NULL。
 * @param pre_roll_capacity_samples pre_roll_storage 可容纳的 int16_t 样本数。
 */
void mic_record_buffer_init(mic_record_buffer_t *buffer,
                            int16_t *record_storage,
                            size_t record_capacity_samples,
                            int16_t *pre_roll_storage,
                            size_t pre_roll_capacity_samples);

/**
 * @brief 清空当前录音和预录音状态。
 *
 * 调用方法：需要丢弃当前缓存、回到初始空闲状态时调用。
 *
 * @param buffer 录音缓存上下文，不能为空。
 */
void mic_record_buffer_reset(mic_record_buffer_t *buffer);

/**
 * @brief 开始缓存一句话。
 *
 * 调用方法：VAD 输出 VOICE_START 时调用。函数会先把预录音缓存复制到正式录音缓存，
 * 减少 VAD 帧级判断导致的句首丢失。
 *
 * @param buffer 录音缓存上下文，不能为空。
 * @return true 表示已经进入录音状态；参数或存储无效时返回 false。
 */
bool mic_record_buffer_start(mic_record_buffer_t *buffer);

/**
 * @brief 追加一个 PCM 样本。
 *
 * 调用方法：每得到一个 int16_t PCM 样本就调用一次。录音中会写入正式缓存；
 * 空闲或已就绪时会写入预录音缓存。
 *
 * @param buffer 录音缓存上下文，不能为空。
 * @param pcm_sample int16_t PCM 样本。
 * @return true 表示样本已写入或已用于预录音；录音缓存满时返回 false。
 */
bool mic_record_buffer_push_sample(mic_record_buffer_t *buffer, int16_t pcm_sample);

/**
 * @brief 批量追加 PCM 样本。
 *
 * 调用方法：当上层已经有连续 PCM 数组时调用，可减少循环样板代码。
 *
 * @param buffer 录音缓存上下文，不能为空。
 * @param pcm_samples 输入 PCM 数组，不能为空。
 * @param sample_count 要追加的样本数。
 * @return 实际写入正式录音或预录音缓存的样本数；参数无效时返回 0。
 */
size_t mic_record_buffer_push_samples(mic_record_buffer_t *buffer,
                                      const int16_t *pcm_samples,
                                      size_t sample_count);

/**
 * @brief 停止缓存一句话。
 *
 * 调用方法：VAD 输出 VOICE_END 时调用。停止后可通过 mic_record_buffer_get_pcm()
 * 取得本句话 PCM。
 *
 * @param buffer 录音缓存上下文，不能为空。
 * @return true 表示已经停止并进入 READY；当前不在录音中时返回 false。
 */
bool mic_record_buffer_stop(mic_record_buffer_t *buffer);

/**
 * @brief 获取当前缓存的 PCM 指针。
 *
 * 调用方法：mic_record_buffer_stop() 返回 true 后调用，得到一整句话的 PCM 样本数组。
 *
 * @param buffer 录音缓存上下文，不能为空。
 * @param out_sample_count 输出样本数，可为 NULL。
 * @return PCM 样本指针；无有效缓存时返回 NULL。
 */
const int16_t *mic_record_buffer_get_pcm(const mic_record_buffer_t *buffer,
                                         size_t *out_sample_count);

/**
 * @brief 获取当前缓存的 PCM 字节数。
 *
 * 调用方法：上传、写文件或打包裸 PCM 时调用。
 *
 * @param buffer 录音缓存上下文，不能为空。
 * @return 当前缓存字节数。
 */
size_t mic_record_buffer_get_byte_count(const mic_record_buffer_t *buffer);

/**
 * @brief 获取当前缓存对应的时长。
 *
 * 调用方法：打印日志或判断录音长度时调用。
 *
 * @param buffer 录音缓存上下文，不能为空。
 * @return 当前缓存时长，单位 ms。
 */
uint32_t mic_record_buffer_get_duration_ms(const mic_record_buffer_t *buffer);

/**
 * @brief 判断是否正在录音。
 *
 * @param buffer 录音缓存上下文，可为 NULL。
 * @return true 表示正在缓存 PCM。
 */
bool mic_record_buffer_is_recording(const mic_record_buffer_t *buffer);

/**
 * @brief 判断是否已经得到一整句话 PCM。
 *
 * @param buffer 录音缓存上下文，可为 NULL。
 * @return true 表示已停止，PCM 可被上层读取。
 */
bool mic_record_buffer_is_ready(const mic_record_buffer_t *buffer);

/**
 * @brief 判断当前句子是否发生过容量溢出。
 *
 * @param buffer 录音缓存上下文，可为 NULL。
 * @return true 表示录音过长，超出容量的样本已被丢弃。
 */
bool mic_record_buffer_overflowed(const mic_record_buffer_t *buffer);

#endif // MIC_RECORD_BUFFER_H
