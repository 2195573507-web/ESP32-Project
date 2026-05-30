#include "mic_record_buffer.h"

#include <stddef.h>

/**
 * @brief 写入一个样本到正式录音缓存。
 *
 * 调用方法：mic_record_buffer_start() 复制预录音和 push_sample() 录音中追加样本时调用。
 *
 * @param buffer 录音缓存上下文，不能为空。
 * @param pcm_sample int16_t PCM 样本。
 * @return true 表示写入成功；容量满时返回 false 并标记 overflowed。
 */
static bool mic_record_buffer_append_record_sample(mic_record_buffer_t *buffer, int16_t pcm_sample)
{
    if (buffer->record_sample_count >= buffer->record_capacity_samples) {
        buffer->overflowed = true;
        return false;
    }

    buffer->record_samples[buffer->record_sample_count] = pcm_sample;
    buffer->record_sample_count++;
    return true;
}

/**
 * @brief 写入一个样本到预录音环形缓存。
 *
 * 调用方法：空闲或 READY 状态下由 mic_record_buffer_push_sample() 调用。
 *
 * @param buffer 录音缓存上下文，不能为空。
 * @param pcm_sample int16_t PCM 样本。
 */
static void mic_record_buffer_push_pre_roll_sample(mic_record_buffer_t *buffer, int16_t pcm_sample)
{
    if (buffer->pre_roll_samples == NULL || buffer->pre_roll_capacity_samples == 0) {
        return;
    }

    buffer->pre_roll_samples[buffer->pre_roll_write_index] = pcm_sample;
    buffer->pre_roll_write_index++;
    if (buffer->pre_roll_write_index >= buffer->pre_roll_capacity_samples) {
        buffer->pre_roll_write_index = 0;
    }
    if (buffer->pre_roll_sample_count < buffer->pre_roll_capacity_samples) {
        buffer->pre_roll_sample_count++;
    }
}

/**
 * @brief 把预录音环形缓存按时间顺序复制到正式录音缓存。
 *
 * 调用方法：mic_record_buffer_start() 清空正式录音后调用。
 *
 * @param buffer 录音缓存上下文，不能为空。
 */
static void mic_record_buffer_copy_pre_roll(mic_record_buffer_t *buffer)
{
    if (buffer->pre_roll_samples == NULL || buffer->pre_roll_sample_count == 0) {
        return;
    }

    size_t start_index = 0;
    if (buffer->pre_roll_sample_count == buffer->pre_roll_capacity_samples) {
        start_index = buffer->pre_roll_write_index;
    }

    for (size_t i = 0; i < buffer->pre_roll_sample_count; i++) {
        size_t read_index = start_index + i;
        if (read_index >= buffer->pre_roll_capacity_samples) {
            read_index -= buffer->pre_roll_capacity_samples;
        }
        mic_record_buffer_append_record_sample(buffer, buffer->pre_roll_samples[read_index]);
    }
}

void mic_record_buffer_init(mic_record_buffer_t *buffer,
                            int16_t *record_storage,
                            size_t record_capacity_samples,
                            int16_t *pre_roll_storage,
                            size_t pre_roll_capacity_samples)
{
    if (buffer == NULL) {
        return;
    }

    buffer->record_samples = record_storage;
    buffer->record_capacity_samples = record_capacity_samples;
    buffer->pre_roll_samples = pre_roll_storage;
    buffer->pre_roll_capacity_samples = pre_roll_capacity_samples;
    mic_record_buffer_reset(buffer);
}

void mic_record_buffer_reset(mic_record_buffer_t *buffer)
{
    if (buffer == NULL) {
        return;
    }

    buffer->record_sample_count = 0;
    buffer->pre_roll_sample_count = 0;
    buffer->pre_roll_write_index = 0;
    buffer->state = MIC_RECORD_BUFFER_STATE_IDLE;
    buffer->overflowed = false;
}

bool mic_record_buffer_start(mic_record_buffer_t *buffer)
{
    if (buffer == NULL ||
        buffer->record_samples == NULL ||
        buffer->record_capacity_samples == 0) {
        return false;
    }

    buffer->record_sample_count = 0;
    buffer->overflowed = false;
    mic_record_buffer_copy_pre_roll(buffer);
    buffer->state = MIC_RECORD_BUFFER_STATE_RECORDING;
    return true;
}

bool mic_record_buffer_push_sample(mic_record_buffer_t *buffer, int16_t pcm_sample)
{
    if (buffer == NULL) {
        return false;
    }

    if (buffer->state == MIC_RECORD_BUFFER_STATE_RECORDING) {
        return mic_record_buffer_append_record_sample(buffer, pcm_sample);
    }

    mic_record_buffer_push_pre_roll_sample(buffer, pcm_sample);
    return true;
}

size_t mic_record_buffer_push_samples(mic_record_buffer_t *buffer,
                                      const int16_t *pcm_samples,
                                      size_t sample_count)
{
    if (buffer == NULL || pcm_samples == NULL) {
        return 0;
    }

    size_t pushed = 0;
    for (size_t i = 0; i < sample_count; i++) {
        if (mic_record_buffer_push_sample(buffer, pcm_samples[i])) {
            pushed++;
        }
    }
    return pushed;
}

bool mic_record_buffer_stop(mic_record_buffer_t *buffer)
{
    if (buffer == NULL || buffer->state != MIC_RECORD_BUFFER_STATE_RECORDING) {
        return false;
    }

    buffer->state = MIC_RECORD_BUFFER_STATE_READY;
    buffer->pre_roll_sample_count = 0;
    buffer->pre_roll_write_index = 0;
    return true;
}

const int16_t *mic_record_buffer_get_pcm(const mic_record_buffer_t *buffer,
                                         size_t *out_sample_count)
{
    if (out_sample_count != NULL) {
        *out_sample_count = 0;
    }

    if (buffer == NULL ||
        buffer->record_samples == NULL ||
        buffer->record_sample_count == 0) {
        return NULL;
    }

    if (out_sample_count != NULL) {
        *out_sample_count = buffer->record_sample_count;
    }
    return buffer->record_samples;
}

size_t mic_record_buffer_get_byte_count(const mic_record_buffer_t *buffer)
{
    if (buffer == NULL) {
        return 0;
    }

    return buffer->record_sample_count * MIC_RECORD_BUFFER_BYTES_PER_SAMPLE;
}

uint32_t mic_record_buffer_get_duration_ms(const mic_record_buffer_t *buffer)
{
    if (buffer == NULL || MIC_RECORD_BUFFER_SAMPLE_RATE_HZ == 0) {
        return 0;
    }

    return (uint32_t)((buffer->record_sample_count * 1000U) / MIC_RECORD_BUFFER_SAMPLE_RATE_HZ);
}

bool mic_record_buffer_is_recording(const mic_record_buffer_t *buffer)
{
    return buffer != NULL && buffer->state == MIC_RECORD_BUFFER_STATE_RECORDING;
}

bool mic_record_buffer_is_ready(const mic_record_buffer_t *buffer)
{
    return buffer != NULL && buffer->state == MIC_RECORD_BUFFER_STATE_READY;
}

bool mic_record_buffer_overflowed(const mic_record_buffer_t *buffer)
{
    return buffer != NULL && buffer->overflowed;
}
