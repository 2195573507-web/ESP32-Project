#include "speaker_player.h"

/**
 * @file speaker_player.c
 * @brief speaker PCM 播放器实现。
 *
 * 播放路径：
 * 1. 上层传入 PCM16 mono buffer。
 * 2. 本模块把 PCM 拆成固定 512 个采样点的音频块，放入 FreeRTOS 环形缓冲区。
 * 3. 写入任务独占调用 iis_write()，避免多个任务同时触碰 IIS/PDM DMA。
 * 4. 播放结束后发送 END 条目，统一停止 IIS 并释放本次播放资源。
 */

#include <stdbool.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_debug_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "iis.h"
#include "speaker_resample.h"

static const char *TAG = "speaker_player";

/* speaker 对外只接受和 IIS 一致的 24 kHz PCM；TTS 16 kHz 会先重采样。 */
#define AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ IIS_SAMPLE_RATE_HZ

/* DMA/heap 诊断阈值，只影响日志，不影响播放数据内容。 */
#define AUDIO_PLAYER_DMA_STARVATION_THRESHOLD_US 20000LL
#define AUDIO_PLAYER_HEAP_MONITOR_INTERVAL_US 500000LL

/* 每个环形缓冲区条目固定承载 512 个 int16_t 采样点，便于 IIS 写入端稳定写 DMA。 */
#define AUDIO_PLAYER_PCM_CHUNK_SAMPLES 512U
#define AUDIO_PLAYER_PCM_CHUNK_BYTES (AUDIO_PLAYER_PCM_CHUNK_SAMPLES * sizeof(int16_t))
#define AUDIO_PLAYER_RING_ITEM_TYPE_PCM 1U
#define AUDIO_PLAYER_RING_ITEM_TYPE_END 2U

#ifndef AUDIO_PLAYER_RING_BUFFER_CHUNKS
#define AUDIO_PLAYER_RING_BUFFER_CHUNKS 8U
#endif

#ifndef AUDIO_PLAYER_RING_BUFFER_SEND_TIMEOUT_MS
#define AUDIO_PLAYER_RING_BUFFER_SEND_TIMEOUT_MS 1000U
#endif

#ifndef AUDIO_PLAYER_I2S_WRITER_TASK_STACK_SIZE
#define AUDIO_PLAYER_I2S_WRITER_TASK_STACK_SIZE 4096U
#endif

#ifndef AUDIO_PLAYER_I2S_WRITER_TASK_PRIORITY
#define AUDIO_PLAYER_I2S_WRITER_TASK_PRIORITY 6U
#endif

#define AUDIO_PLAYER_I2S_NONBLOCK_TIMEOUT_MS 0U
#define AUDIO_PLAYER_I2S_DMA_BACKOFF_TICKS 1U
#define AUDIO_PLAYER_I2S_RETRY_LOG_INTERVAL 32U

/* 播放互斥锁：同一时刻只允许一个 PCM 流占用 IIS/PDM 输出。 */
static SemaphoreHandle_t s_play_mutex = NULL;

/* 播放期间可选 heap 周期诊断，默认由 APP_DEBUG_SPEAKER_PLAYER_LOG 关闭。 */
static esp_timer_handle_t s_heap_monitor_timer = NULL;

/* 记录每次写 DMA 的耗时，用于定位 IIS DMA 饥饿或阻塞。 */
typedef struct {
    uint32_t write_count;
    uint64_t total_block_us;
    int64_t max_block_us;
} audio_player_dma_diag_t;

static audio_player_dma_diag_t s_dma_diag = {0};

/* 环形缓冲区中传递的固定大小条目。最后一个不足 512 个采样点的音频块会补 0。 */
typedef struct {
    uint32_t type;
    uint32_t sequence;
    uint32_t valid_samples;
    int16_t samples[AUDIO_PLAYER_PCM_CHUNK_SAMPLES];
} audio_player_ring_item_t;

/* 单次播放流的上下文，只在 audio_player_play_pcm() 生命周期内有效。 */
typedef struct {
    RingbufHandle_t ringbuf;
    SemaphoreHandle_t done;
    volatile bool writer_done;
    esp_err_t result;
    UBaseType_t writer_stack_high_water_bytes;
} audio_player_stream_ctx_t;

static audio_player_stream_ctx_t s_tts_stream_ctx = {0};
static bool s_tts_stream_open;
static uint32_t s_tts_stream_sequence;

static void speaker_player_log_values(const char *message,
                                      int64_t value0,
                                      int64_t value1,
                                      int64_t value2,
                                      int64_t value3)
{
#if APP_DEBUG_SPEAKER_PLAYER_LOG
    ESP_LOGI(TAG, "%s: %lld, %lld, %lld, %lld",
             message,
             value0,
             value1,
             value2,
             value3);
#else
    (void)message;
    (void)value0;
    (void)value1;
    (void)value2;
    (void)value3;
#endif
}

static void speaker_player_log_heap_state(const char *stage)
{
    speaker_player_log_values(stage,
                              esp_get_free_heap_size(),
                              (int64_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                              0,
                              0);
}

static UBaseType_t speaker_player_current_stack_high_water(void)
{
#if (INCLUDE_uxTaskGetStackHighWaterMark == 1)
    return uxTaskGetStackHighWaterMark(NULL);
#else
    return 0;
#endif
}

static void speaker_player_heap_monitor_timer_cb(void *arg)
{
    (void)arg;
    speaker_player_log_heap_state("play_running");
}

static esp_err_t speaker_player_heap_monitor_ensure_timer(void)
{
    if (s_heap_monitor_timer != NULL) {
        return ESP_OK;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = speaker_player_heap_monitor_timer_cb,
        .name = "speaker_heap_monitor",
        .skip_unhandled_events = true,
    };
    return esp_timer_create(&timer_args, &s_heap_monitor_timer);
}

static void speaker_player_heap_monitor_start(void)
{
    speaker_player_log_heap_state("play_start");
    if (speaker_player_heap_monitor_ensure_timer() != ESP_OK) {
        return;
    }
    if (esp_timer_is_active(s_heap_monitor_timer)) {
        (void)esp_timer_stop(s_heap_monitor_timer);
    }
    (void)esp_timer_start_periodic(s_heap_monitor_timer,
                                   AUDIO_PLAYER_HEAP_MONITOR_INTERVAL_US);
}

static void speaker_player_heap_monitor_stop(void)
{
    if (s_heap_monitor_timer != NULL && esp_timer_is_active(s_heap_monitor_timer)) {
        (void)esp_timer_stop(s_heap_monitor_timer);
    }
}

static void speaker_player_dma_diag_reset(void)
{
    s_dma_diag.write_count = 0;
    s_dma_diag.total_block_us = 0;
    s_dma_diag.max_block_us = 0;
}

static void speaker_player_dma_diag_record(size_t request_bytes,
                                           size_t written_bytes,
                                           int64_t elapsed_us)
{
    s_dma_diag.write_count++;
    s_dma_diag.total_block_us += (uint64_t)elapsed_us;
    if (elapsed_us > s_dma_diag.max_block_us) {
        s_dma_diag.max_block_us = elapsed_us;
    }

    if (elapsed_us > AUDIO_PLAYER_DMA_STARVATION_THRESHOLD_US) {
        ESP_LOGW(TAG,
                 "DMA starvation: request=%zu written=%zu elapsed_us=%lld",
                 request_bytes,
                 written_bytes,
                 elapsed_us);
    }
}

static void speaker_player_dma_diag_log_summary(void)
{
    uint64_t avg_us = s_dma_diag.write_count == 0 ? 0 :
                      s_dma_diag.total_block_us / s_dma_diag.write_count;
    speaker_player_log_values("DMA write summary",
                              (int64_t)s_dma_diag.write_count,
                              s_dma_diag.max_block_us,
                              (int64_t)avg_us,
                              0);
}

static esp_err_t speaker_player_ensure_mutex(void)
{
    if (s_play_mutex != NULL) {
        return ESP_OK;
    }
    s_play_mutex = xSemaphoreCreateMutex();
    return s_play_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

static esp_err_t speaker_player_write_bytes_dma_streaming(const void *data,
                                                          size_t total_bytes)
{
    /*
     * 写入任务使用非阻塞 iis_write() 轮询 DMA 可用空间。
     * 这样可以及时记录 DMA not ready，同时避免长时间阻塞导致上层无法收尾。
     */
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (total_bytes != AUDIO_PLAYER_PCM_CHUNK_BYTES) {
        ESP_LOGE(TAG,
                 "reject non-fixed IIS write: bytes=%zu expected=%zu",
                 total_bytes,
                 (size_t)AUDIO_PLAYER_PCM_CHUNK_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    size_t offset = 0;
    uint32_t retry_count = 0;

    while (offset < total_bytes) {
        size_t bytes_written = 0;
        size_t bytes_left = total_bytes - offset;
        int64_t start_us = esp_timer_get_time();
        esp_err_t err = iis_write(bytes + offset,
                                  bytes_left,
                                  &bytes_written,
                                  AUDIO_PLAYER_I2S_NONBLOCK_TIMEOUT_MS);
        int64_t elapsed_us = esp_timer_get_time() - start_us;
        speaker_player_dma_diag_record(bytes_left, bytes_written, elapsed_us);

        if (bytes_written > 0) {
            offset += bytes_written;
        }

        if (err == ESP_OK && bytes_written > 0) {
            continue;
        }

        if (err == ESP_ERR_TIMEOUT || (err == ESP_OK && bytes_written == 0)) {
            retry_count++;
            if ((retry_count % AUDIO_PLAYER_I2S_RETRY_LOG_INTERVAL) == 0) {
                ESP_LOGW(TAG,
                         "IIS DMA not ready: retries=%lu offset=%zu total=%zu",
                         (unsigned long)retry_count,
                         offset,
                         total_bytes);
            }
            vTaskDelay(AUDIO_PLAYER_I2S_DMA_BACKOFF_TICKS);
            continue;
        }

        ESP_LOGE(TAG, "iis_write failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static TickType_t speaker_player_ms_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == 0xFFFFFFFFU) {
        return portMAX_DELAY;
    }

    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0 && ticks == 0) {
        ticks = 1;
    }
    return ticks;
}

static esp_err_t speaker_player_ring_send(const audio_player_stream_ctx_t *ctx,
                                          const audio_player_ring_item_t *item)
{
    /*
     * 生产侧向环形缓冲区投递 PCM 音频块。
     * 如果写入任务已经报错退出，这里直接把错误返回给播放主流程。
     */
    if (ctx == NULL || ctx->ringbuf == NULL || item == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (true) {
        if (ctx->writer_done && ctx->result != ESP_OK) {
            return ctx->result;
        }

        BaseType_t sent = xRingbufferSend(ctx->ringbuf,
                                          item,
                                          sizeof(*item),
                                          speaker_player_ms_to_ticks(AUDIO_PLAYER_RING_BUFFER_SEND_TIMEOUT_MS));
        if (sent == pdTRUE) {
            return ESP_OK;
        }

#if APP_DEBUG_SPEAKER_PLAYER_LOG
        ESP_LOGW(TAG,
                 "speaker ringbuffer waiting: type=%lu seq=%lu samples=%lu",
                 (unsigned long)item->type,
                 (unsigned long)item->sequence,
                 (unsigned long)item->valid_samples);
#endif
    }
}

static esp_err_t speaker_player_ring_send_end(const audio_player_stream_ctx_t *ctx,
                                              uint32_t sequence)
{
    audio_player_ring_item_t item = {
        .type = AUDIO_PLAYER_RING_ITEM_TYPE_END,
        .sequence = sequence,
        .valid_samples = 0,
    };
    return speaker_player_ring_send(ctx, &item);
}

static void speaker_player_iis_writer_task(void *arg)
{
    /*
     * 只有本任务会调用 iis_write()。
     * 这样播放主流程只负责投递 PCM，IIS/PDM DMA 写入集中在一个任务中完成。
     */
    audio_player_stream_ctx_t *ctx = (audio_player_stream_ctx_t *)arg;
    esp_err_t result = ESP_OK;

    if (ctx == NULL || ctx->ringbuf == NULL || ctx->done == NULL) {
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        size_t item_size = 0;
        audio_player_ring_item_t *item =
            (audio_player_ring_item_t *)xRingbufferReceive(ctx->ringbuf,
                                                           &item_size,
                                                           portMAX_DELAY);
        if (item == NULL) {
            result = result == ESP_OK ? ESP_ERR_TIMEOUT : result;
            break;
        }

        bool end_of_stream = false;
        esp_err_t item_result = ESP_OK;

        if (item_size != sizeof(*item)) {
            ESP_LOGE(TAG,
                     "speaker ringbuffer item size mismatch: got=%zu expected=%zu",
                     item_size,
                     sizeof(*item));
            item_result = ESP_ERR_INVALID_SIZE;
        } else if (item->type == AUDIO_PLAYER_RING_ITEM_TYPE_END) {
            end_of_stream = true;
            speaker_player_log_values("speaker writer end", item->sequence, 0, 0, 0);
        } else if (item->type != AUDIO_PLAYER_RING_ITEM_TYPE_PCM) {
            ESP_LOGE(TAG,
                     "speaker ringbuffer item type invalid: type=%lu seq=%lu",
                     (unsigned long)item->type,
                     (unsigned long)item->sequence);
            item_result = ESP_ERR_INVALID_ARG;
        } else if (item->valid_samples == 0 ||
                   item->valid_samples > AUDIO_PLAYER_PCM_CHUNK_SAMPLES) {
            ESP_LOGE(TAG,
                     "speaker PCM valid_samples invalid: seq=%lu samples=%lu",
                     (unsigned long)item->sequence,
                     (unsigned long)item->valid_samples);
            item_result = ESP_ERR_INVALID_SIZE;
        } else if (result == ESP_OK) {
            item_result = speaker_player_write_bytes_dma_streaming(item->samples,
                                                                   AUDIO_PLAYER_PCM_CHUNK_BYTES);
        }

        vRingbufferReturnItem(ctx->ringbuf, item);

        if (result == ESP_OK && item_result != ESP_OK) {
            result = item_result;
        }
        if (end_of_stream) {
            break;
        }
    }

    ctx->writer_stack_high_water_bytes = speaker_player_current_stack_high_water();
    ESP_LOGI(TAG,
             "speaker IIS writer task summary result=%s stack_hwm=%u free_heap=%u min_free_heap=%u largest_free_block=%u",
             esp_err_to_name(result),
             (unsigned int)ctx->writer_stack_high_water_bytes,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ctx->result = result;
    ctx->writer_done = true;
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

static esp_err_t speaker_player_write_mono_pcm_to_ring_ex(audio_player_stream_ctx_t *ctx,
                                                          const int16_t *data,
                                                          uint32_t samples,
                                                          uint32_t *sequence_io,
                                                          bool send_end)
{
    /*
     * 把连续 PCM 拆成固定长度音频块。最后一包如果不足 512 个采样点，补 0 后再发送，
     * 保证写入任务每次写 IIS 的字节数一致。
     */
    uint32_t offset_samples = 0;
    uint32_t sequence = sequence_io != NULL ? *sequence_io : 0;

    while (offset_samples < samples) {
        if (ctx->writer_done && ctx->result != ESP_OK) {
            if (sequence_io != NULL) {
                *sequence_io = sequence;
            }
            return ctx->result;
        }

        uint32_t valid_samples = samples - offset_samples;
        if (valid_samples > AUDIO_PLAYER_PCM_CHUNK_SAMPLES) {
            valid_samples = AUDIO_PLAYER_PCM_CHUNK_SAMPLES;
        }

        audio_player_ring_item_t item = {
            .type = AUDIO_PLAYER_RING_ITEM_TYPE_PCM,
            .sequence = sequence,
            .valid_samples = valid_samples,
        };

        memcpy(item.samples,
               &data[offset_samples],
               (size_t)valid_samples * sizeof(item.samples[0]));
        if (valid_samples < AUDIO_PLAYER_PCM_CHUNK_SAMPLES) {
            memset(&item.samples[valid_samples],
                   0,
                   (size_t)(AUDIO_PLAYER_PCM_CHUNK_SAMPLES - valid_samples) *
                   sizeof(item.samples[0]));
        }

        esp_err_t send_err = speaker_player_ring_send(ctx, &item);
        if (send_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "send PCM chunk to speaker ringbuffer failed: %s",
                     esp_err_to_name(send_err));
            return send_err;
        }

        offset_samples += valid_samples;
        sequence++;
    }

    if (sequence_io != NULL) {
        *sequence_io = sequence;
    }
    if (send_end) {
        esp_err_t end_err = speaker_player_ring_send_end(ctx, sequence);
        if (end_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "send speaker ringbuffer EOS failed: %s",
                     esp_err_to_name(end_err));
            return end_err;
        }
    }
    return ESP_OK;
}

static esp_err_t speaker_player_write_mono_pcm_to_ring(audio_player_stream_ctx_t *ctx,
                                                       const int16_t *data,
                                                       uint32_t samples)
{
    uint32_t sequence = 0;
    return speaker_player_write_mono_pcm_to_ring_ex(ctx, data, samples, &sequence, true);
}

static int16_t speaker_player_resample_16k_to_24k_sample(const int16_t *input,
                                                         uint32_t input_samples,
                                                         size_t out_index)
{
    const size_t pos_num = out_index * 2U;
    const size_t in_index = pos_num / 3U;
    const size_t frac = pos_num % 3U;
    const int32_t s0 = input[in_index];
    const int32_t s1 = (in_index + 1U < input_samples) ? input[in_index + 1U] : s0;
    int32_t sample = s0;

    if (frac == 1U) {
        sample = (2 * s0 + s1) / 3;
    } else if (frac == 2U) {
        sample = (s0 + 2 * s1) / 3;
    }

    if (sample > INT16_MAX) {
        sample = INT16_MAX;
    }
    if (sample < INT16_MIN) {
        sample = INT16_MIN;
    }
    return (int16_t)sample;
}

static esp_err_t speaker_player_write_16k_resampled_to_ring(audio_player_stream_ctx_t *ctx,
                                                            const int16_t *data,
                                                            uint32_t samples,
                                                            uint32_t *sequence_io)
{
    if (ctx == NULL || data == NULL || sequence_io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t output_samples = audio_resample_16k_to_24k_output_samples(samples);
    if (output_samples == 0 || output_samples > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t out_index = 0;
    uint32_t sequence = *sequence_io;
    while (out_index < output_samples) {
        if (ctx->writer_done && ctx->result != ESP_OK) {
            *sequence_io = sequence;
            return ctx->result;
        }

        uint32_t valid_samples = (uint32_t)(output_samples - out_index);
        if (valid_samples > AUDIO_PLAYER_PCM_CHUNK_SAMPLES) {
            valid_samples = AUDIO_PLAYER_PCM_CHUNK_SAMPLES;
        }

        audio_player_ring_item_t item = {
            .type = AUDIO_PLAYER_RING_ITEM_TYPE_PCM,
            .sequence = sequence,
            .valid_samples = valid_samples,
        };

        for (uint32_t i = 0; i < valid_samples; i++) {
            item.samples[i] = speaker_player_resample_16k_to_24k_sample(data,
                                                                         samples,
                                                                         out_index + i);
        }
        if (valid_samples < AUDIO_PLAYER_PCM_CHUNK_SAMPLES) {
            memset(&item.samples[valid_samples],
                   0,
                   (size_t)(AUDIO_PLAYER_PCM_CHUNK_SAMPLES - valid_samples) *
                   sizeof(item.samples[0]));
        }

        esp_err_t send_err = speaker_player_ring_send(ctx, &item);
        if (send_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "send resampled PCM chunk to speaker ringbuffer failed: %s",
                     esp_err_to_name(send_err));
            *sequence_io = sequence;
            return send_err;
        }

        out_index += valid_samples;
        sequence++;
    }

    *sequence_io = sequence;
    return ESP_OK;
}

esp_err_t audio_player_init(void)
{
    /* 初始化播放器互斥锁和 BSP/IIS；不启动播放，也不占用 IIS DMA。 */
    esp_err_t err = speaker_player_ensure_mutex();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create play mutex failed: %s", esp_err_to_name(err));
        return err;
    }

    return iis_init();
}

static void speaker_player_stream_cleanup_locked(audio_player_stream_ctx_t *ctx,
                                                 bool destroy_buffers)
{
    if (ctx == NULL) {
        return;
    }

    (void)iis_stop();

    if (destroy_buffers && ctx->ringbuf != NULL) {
        vRingbufferDelete(ctx->ringbuf);
        ctx->ringbuf = NULL;
    }
    if (destroy_buffers && ctx->done != NULL) {
        vSemaphoreDelete(ctx->done);
        ctx->done = NULL;
    }
    ctx->writer_done = false;
    ctx->result = ESP_OK;
    speaker_player_dma_diag_log_summary();
    speaker_player_heap_monitor_stop();
}

esp_err_t audio_player_stream_open(void)
{
    if (s_tts_stream_open) {
        return ESP_OK;
    }

    esp_err_t err = audio_player_init();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_play_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_tts_stream_ctx.writer_done = false;
    s_tts_stream_ctx.result = ESP_OK;
    s_tts_stream_ctx.writer_stack_high_water_bytes = 0;
    s_tts_stream_sequence = 0;

    speaker_player_dma_diag_reset();
    speaker_player_heap_monitor_start();

    i2s_chan_info_t play_chan_info = {};
    if (iis_get_info(&play_chan_info) == ESP_OK) {
        speaker_player_log_values("stream_open DMA diagnostic",
                                  IIS_EFFECTIVE_DMA_DESC_NUM,
                                  IIS_EFFECTIVE_DMA_FRAME_NUM,
                                  play_chan_info.total_dma_buf_size,
                                  0);
    }

    const size_t ring_item_size = sizeof(audio_player_ring_item_t);
    const size_t ring_item_count = AUDIO_PLAYER_RING_BUFFER_CHUNKS < 2U ?
                                   2U : AUDIO_PLAYER_RING_BUFFER_CHUNKS;

    if (s_tts_stream_ctx.ringbuf == NULL) {
        s_tts_stream_ctx.ringbuf = xRingbufferCreateNoSplit(ring_item_size, ring_item_count);
        if (s_tts_stream_ctx.ringbuf == NULL) {
            ESP_LOGE(TAG,
                     "create speaker ringbuffer failed: item_size=%zu item_count=%zu",
                     ring_item_size,
                     ring_item_count);
            err = ESP_ERR_NO_MEM;
            goto open_fail;
        }
    }

    if (s_tts_stream_ctx.done == NULL) {
        s_tts_stream_ctx.done = xSemaphoreCreateBinary();
        if (s_tts_stream_ctx.done == NULL) {
            ESP_LOGE(TAG, "create speaker writer done semaphore failed");
            err = ESP_ERR_NO_MEM;
            goto open_fail;
        }
    }

    err = iis_start();
    if (err != ESP_OK) {
        goto open_fail;
    }

    BaseType_t task_created = xTaskCreate(speaker_player_iis_writer_task,
                                          "speaker_iis_writer",
                                          AUDIO_PLAYER_I2S_WRITER_TASK_STACK_SIZE,
                                          &s_tts_stream_ctx,
                                          AUDIO_PLAYER_I2S_WRITER_TASK_PRIORITY,
                                          NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "create speaker_iis_writer task failed");
        err = ESP_ERR_NO_MEM;
        goto open_fail;
    }

    s_tts_stream_open = true;
    ESP_LOGI(TAG,
             "speaker_player init once ok ring_item_size=%u item_count=%u reuse_ringbuffer=%d free_heap=%u",
             (unsigned int)ring_item_size,
             (unsigned int)ring_item_count,
             s_tts_stream_ctx.ringbuf != NULL ? 1 : 0,
             (unsigned int)esp_get_free_heap_size());
    return ESP_OK;

open_fail:
    speaker_player_stream_cleanup_locked(&s_tts_stream_ctx, true);
    xSemaphoreGive(s_play_mutex);
    return err;
}

esp_err_t audio_player_write_pcm_chunk(const int16_t *data,
                                       uint32_t samples,
                                       int sample_rate_hz)
{
    if (!s_tts_stream_open || s_tts_stream_ctx.ringbuf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples == 0) {
        return ESP_OK;
    }
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_rate_hz == (int)AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ) {
        return speaker_player_write_mono_pcm_to_ring_ex(&s_tts_stream_ctx,
                                                        data,
                                                        samples,
                                                        &s_tts_stream_sequence,
                                                        false);
    }
    if (sample_rate_hz != AUDIO_RESAMPLE_16K_HZ) {
        ESP_LOGE(TAG,
                 "unsupported PCM sample rate: got=%d supported=%d,%u",
                 sample_rate_hz,
                 AUDIO_RESAMPLE_16K_HZ,
                 AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return speaker_player_write_16k_resampled_to_ring(&s_tts_stream_ctx,
                                                      data,
                                                      samples,
                                                      &s_tts_stream_sequence);
}

esp_err_t audio_player_stream_finish(void)
{
    if (!s_tts_stream_open) {
        return ESP_OK;
    }

    esp_err_t err = speaker_player_ring_send_end(&s_tts_stream_ctx, s_tts_stream_sequence);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "send speaker stream EOS failed: %s",
                 esp_err_to_name(err));
    }

    if (s_tts_stream_ctx.done == NULL ||
        xSemaphoreTake(s_tts_stream_ctx.done, portMAX_DELAY) != pdTRUE) {
        err = err == ESP_OK ? ESP_ERR_TIMEOUT : err;
    } else if (err == ESP_OK && s_tts_stream_ctx.result != ESP_OK) {
        err = s_tts_stream_ctx.result;
    }
    ESP_LOGI(TAG,
             "speaker stream summary free_heap=%u min_free_heap=%u largest_free_block=%u "
             "iis_writer_stack_hwm=%u caller_stack_hwm=%u",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned int)s_tts_stream_ctx.writer_stack_high_water_bytes,
             (unsigned int)speaker_player_current_stack_high_water());

    speaker_player_stream_cleanup_locked(&s_tts_stream_ctx, false);
    s_tts_stream_open = false;
    s_tts_stream_sequence = 0;
    xSemaphoreGive(s_play_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "speaker stream playback failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "speaker stream playback complete");
    return ESP_OK;
}

esp_err_t audio_player_play_pcm(const int16_t *data, uint32_t samples)
{
    /*
     * 一次播放的完整生命周期：
     * 建环形缓冲区 -> 启动 IIS -> 启动写入任务 -> 投递 PCM -> 等待写入任务收尾。
     */
    speaker_player_log_values("audio_player_play_pcm", samples, 0, 0, 0);

    if (samples == 0) {
        return ESP_OK;
    }
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((size_t)samples > SIZE_MAX / sizeof(data[0])) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t pcm_bytes = (size_t)samples * sizeof(data[0]);
    speaker_player_log_values("PDM TX write format",
                              AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ,
                              (int64_t)pcm_bytes,
                              ((int64_t)samples * 1000LL) / AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ,
                              0);

    esp_err_t err = audio_player_init();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_play_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    speaker_player_dma_diag_reset();
    speaker_player_heap_monitor_start();

    i2s_chan_info_t play_chan_info = {};
    if (iis_get_info(&play_chan_info) == ESP_OK) {
        speaker_player_log_values("play_start DMA diagnostic",
                                  IIS_EFFECTIVE_DMA_DESC_NUM,
                                  IIS_EFFECTIVE_DMA_FRAME_NUM,
                                  play_chan_info.total_dma_buf_size,
                                  0);
    }

    audio_player_stream_ctx_t stream_ctx = {
        .ringbuf = NULL,
        .done = NULL,
        .writer_done = false,
        .result = ESP_OK,
    };
    const size_t ring_item_size = sizeof(audio_player_ring_item_t);
    const size_t ring_item_count = AUDIO_PLAYER_RING_BUFFER_CHUNKS < 2U ?
                                   2U : AUDIO_PLAYER_RING_BUFFER_CHUNKS;

    stream_ctx.ringbuf = xRingbufferCreateNoSplit(ring_item_size, ring_item_count);
    if (stream_ctx.ringbuf == NULL) {
        ESP_LOGE(TAG,
                 "create speaker ringbuffer failed: item_size=%zu item_count=%zu",
                 ring_item_size,
                 ring_item_count);
        err = ESP_ERR_NO_MEM;
        goto play_cleanup;
    }

    stream_ctx.done = xSemaphoreCreateBinary();
    if (stream_ctx.done == NULL) {
        ESP_LOGE(TAG, "create speaker writer done semaphore failed");
        err = ESP_ERR_NO_MEM;
        goto play_cleanup;
    }

    speaker_player_log_values("speaker ringbuffer ready",
                              AUDIO_PLAYER_PCM_CHUNK_SAMPLES,
                              (int64_t)ring_item_size,
                              (int64_t)ring_item_count,
                              0);

    err = iis_start();
    if (err != ESP_OK) {
        goto play_cleanup;
    }

    BaseType_t task_created = xTaskCreate(speaker_player_iis_writer_task,
                                          "speaker_iis_writer",
                                          AUDIO_PLAYER_I2S_WRITER_TASK_STACK_SIZE,
                                          &stream_ctx,
                                          AUDIO_PLAYER_I2S_WRITER_TASK_PRIORITY,
                                          NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "create speaker_iis_writer task failed");
        err = ESP_ERR_NO_MEM;
        goto play_cleanup;
    }

    err = speaker_player_write_mono_pcm_to_ring(&stream_ctx, data, samples);
    if (err != ESP_OK) {
        (void)speaker_player_ring_send_end(&stream_ctx, 0);
    }

    if (xSemaphoreTake(stream_ctx.done, portMAX_DELAY) != pdTRUE) {
        err = err == ESP_OK ? ESP_ERR_TIMEOUT : err;
    } else if (err == ESP_OK && stream_ctx.result != ESP_OK) {
        err = stream_ctx.result;
    }

play_cleanup:
    esp_err_t stop_err = iis_stop();
    if (err == ESP_OK && stop_err != ESP_OK) {
        err = stop_err;
    }

    if (stream_ctx.ringbuf != NULL) {
        vRingbufferDelete(stream_ctx.ringbuf);
    }
    if (stream_ctx.done != NULL) {
        vSemaphoreDelete(stream_ctx.done);
    }

    speaker_player_dma_diag_log_summary();
    speaker_player_heap_monitor_stop();
    xSemaphoreGive(s_play_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCM playback failed: %s", esp_err_to_name(err));
        return err;
    }
    speaker_player_log_values("playback complete", samples, 0, 0, 0);
    return ESP_OK;
}

esp_err_t audio_player_play_tts_pcm(const int16_t *data,
                                    uint32_t samples,
                                    int sample_rate_hz)
{
    speaker_player_log_values("audio_player_play_tts_pcm",
                              sample_rate_hz,
                              samples,
                              AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ,
                              0);

    if (samples == 0) {
        return ESP_OK;
    }
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_rate_hz == (int)AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ) {
        /* 24 kHz 已经匹配 IIS 输入格式，直接走 PCM 播放路径。 */
        return audio_player_play_pcm(data, samples);
    }
    if (sample_rate_hz != AUDIO_RESAMPLE_16K_HZ) {
        ESP_LOGE(TAG,
                 "unsupported PCM sample rate: got=%d supported=%d,%u",
                 sample_rate_hz,
                 AUDIO_RESAMPLE_16K_HZ,
                 AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const size_t output_samples = audio_resample_16k_to_24k_output_samples(samples);
    if (output_samples == 0 ||
        output_samples > UINT32_MAX ||
        output_samples > SIZE_MAX / sizeof(int16_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int16_t *resampled = (int16_t *)heap_caps_malloc(output_samples * sizeof(int16_t),
                                                     MALLOC_CAP_8BIT);
    if (resampled == NULL) {
        ESP_LOGE(TAG,
                 "PCM resample alloc failed: in_samples=%lu out_samples=%zu",
                 (unsigned long)samples,
                 output_samples);
        return ESP_ERR_NO_MEM;
    }

    size_t produced_samples = 0;
    /* TTS 常见 16 kHz PCM 先转成 IIS 需要的 24 kHz，再复用普通 PCM 播放流程。 */
    esp_err_t err = audio_resample_16k_to_24k_linear(data,
                                                     samples,
                                                     resampled,
                                                     output_samples,
                                                     &produced_samples);
    if (err == ESP_OK) {
        speaker_player_log_values("PCM resample",
                                  sample_rate_hz,
                                  AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ,
                                  samples,
                                  (int64_t)produced_samples);
        err = audio_player_play_pcm(resampled, (uint32_t)produced_samples);
    } else {
        ESP_LOGE(TAG, "PCM resample failed: %s", esp_err_to_name(err));
    }

    heap_caps_free(resampled);
    return err;
}
