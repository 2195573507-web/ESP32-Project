#include "mic_vad.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief 判断当前帧是否达到语音启动条件。
 *
 * 调用方法：mic_vad_process() 在 IDLE 和 SILENCE_HOLD 状态下调用。
 *
 * @param features 当前帧统计值，不能为空。
 * @return true 表示当前帧有语音活动。
 */
static bool mic_vad_is_active_frame(const mic_vad_features_t *features)
{
    return features->clipped > 0 ||
           features->adc_rms >= MIC_VAD_ADC_RMS_START ||
           features->adc_p2p >= MIC_VAD_ADC_P2P_START ||
           features->pcm_rms >= MIC_VAD_PCM_RMS_START ||
           features->pcm_p2p >= MIC_VAD_PCM_P2P_START;
}

/**
 * @brief 判断当前帧是否足够安静。
 *
 * 调用方法：mic_vad_process() 在 SPEAKING 和 SILENCE_HOLD 状态下调用。
 *
 * @param features 当前帧统计值，不能为空。
 * @return true 表示当前帧满足安静条件。
 */
static bool mic_vad_is_quiet_frame(const mic_vad_features_t *features)
{
    return features->clipped == 0 &&
           features->adc_rms <= MIC_VAD_ADC_RMS_STOP &&
           features->adc_p2p <= MIC_VAD_ADC_P2P_STOP &&
           features->pcm_rms <= MIC_VAD_PCM_RMS_STOP &&
           features->pcm_p2p <= MIC_VAD_PCM_P2P_STOP;
}

void mic_vad_init(mic_vad_t *vad)
{
    if (vad == NULL) {
        return;
    }

    vad->state = MIC_VAD_STATE_IDLE;
    vad->silence_hold_ms = 0;
}

mic_vad_event_t mic_vad_process(mic_vad_t *vad, const mic_vad_features_t *features)
{
    if (vad == NULL || features == NULL) {
        return MIC_VAD_EVENT_NONE;
    }

    bool active = mic_vad_is_active_frame(features);
    bool quiet = mic_vad_is_quiet_frame(features);

    switch (vad->state) {
    case MIC_VAD_STATE_IDLE:
        if (active) {
            vad->state = MIC_VAD_STATE_SPEAKING;
            vad->silence_hold_ms = 0;
            return MIC_VAD_EVENT_VOICE_START;
        }
        break;

    case MIC_VAD_STATE_SPEAKING:
        if (quiet) {
            vad->state = MIC_VAD_STATE_SILENCE_HOLD;
            vad->silence_hold_ms = MIC_VAD_FRAME_MS;
        } else {
            vad->silence_hold_ms = 0;
        }
        break;

    case MIC_VAD_STATE_SILENCE_HOLD:
        if (active) {
            vad->state = MIC_VAD_STATE_SPEAKING;
            vad->silence_hold_ms = 0;
        } else if (quiet) {
            vad->silence_hold_ms += MIC_VAD_FRAME_MS;
            if (vad->silence_hold_ms >= MIC_VAD_END_HOLD_MS) {
                vad->state = MIC_VAD_STATE_IDLE;
                vad->silence_hold_ms = 0;
                return MIC_VAD_EVENT_VOICE_END;
            }
        } else {
            vad->state = MIC_VAD_STATE_SPEAKING;
            vad->silence_hold_ms = 0;
        }
        break;

    default:
        mic_vad_init(vad);
        break;
    }

    return MIC_VAD_EVENT_NONE;
}
