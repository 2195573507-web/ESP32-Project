#include "mic_serial_output.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>

/**
 * @brief 打印一行 VAD 事件日志。
 *
 * 调用方法：本文件内部由 mic_serial_output_print_voice_start/end() 调用。
 *
 * @param prefix 事件前缀，例如 VOICE_START 或 VOICE_END。
 * @param frame 当前帧输出数据，不能为空。
 */
static void mic_serial_output_print_voice_event(const char *prefix,
                                                const mic_serial_output_frame_t *frame)
{
    if (prefix == NULL || frame == NULL) {
        return;
    }

    printf("%s,vad_state=%" PRIu32 ",vad_event=%" PRIu32
           ",adc_rms=%" PRIu32 ",adc_p2p=%" PRIu32
           ",pcm_rms=%" PRIu32 ",pcm_p2p=%" PRIu32 ",clipped=%" PRIu32 "\n",
           prefix,
           frame->vad_state,
           frame->vad_event,
           frame->adc_rms,
           frame->adc_p2p,
           frame->pcm_rms,
           frame->pcm_p2p,
           frame->clipped);
    fflush(stdout);
}

void mic_serial_output_print_adc_frame(const mic_serial_output_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }

    printf(MIC_SERIAL_OUTPUT_ADC_PREFIX ",gpio=%" PRIu32 ",unit=ADC%" PRIu32
           ",channel=%" PRIu32 ",samples=%" PRIu32
           ",adc_last=%" PRIu32 ",adc_min=%" PRIu32 ",adc_max=%" PRIu32
           ",adc_avg=%" PRIu32 ",adc_rms=%" PRIu32 ",adc_p2p=%" PRIu32
           ",adc_clip_low=%" PRIu32 ",adc_clip_high=%" PRIu32
           ",pcm_last=%" PRId32 ",pcm_min=%" PRId32 ",pcm_max=%" PRId32
           ",pcm_avg=%" PRId32 ",pcm_rms=%" PRIu32 ",pcm_p2p=%" PRIu32
           ",pcm_clip_low=%" PRIu32 ",pcm_clip_high=%" PRIu32
           ",clipped=%" PRIu32 ",vad_state=%" PRIu32 ",vad_event=%" PRIu32 "\n",
           frame->gpio_num,
           frame->adc_unit,
           frame->adc_channel,
           frame->samples,
           frame->adc_last,
           frame->adc_min,
           frame->adc_max,
           frame->adc_avg,
           frame->adc_rms,
           frame->adc_p2p,
           frame->adc_clip_low,
           frame->adc_clip_high,
           frame->pcm_last,
           frame->pcm_min,
           frame->pcm_max,
           frame->pcm_avg,
           frame->pcm_rms,
           frame->pcm_p2p,
           frame->pcm_clip_low,
           frame->pcm_clip_high,
           frame->clipped,
           frame->vad_state,
           frame->vad_event);
    fflush(stdout);
}

void mic_serial_output_print_voice_start(const mic_serial_output_frame_t *frame)
{
    mic_serial_output_print_voice_event(MIC_SERIAL_OUTPUT_VOICE_START_PREFIX, frame);
}

void mic_serial_output_print_voice_end(const mic_serial_output_frame_t *frame)
{
    mic_serial_output_print_voice_event(MIC_SERIAL_OUTPUT_VOICE_END_PREFIX, frame);
}
