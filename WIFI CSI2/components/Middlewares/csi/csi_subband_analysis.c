#include "csi_subband_analysis.h"

#include <math.h>
#include <string.h>

// 与 csi_processor.c 的边缘跳过策略保持一致，避免边缘子载波噪声进入 band 判断。
#define CSI_SUBBAND_SKIP_EDGE 20

static float csi_subband_mean_abs_delta(const float *a,
                                        const float *b,
                                        uint16_t start,
                                        uint16_t end)
{
    float sum = 0.0f;
    uint16_t count = 0;

    for (uint16_t i = start; i < end; i++) {
        sum += fabsf(a[i] - b[i]);
        count++;
    }

    return count > 0 ? sum / count : 0.0f;
}

void csi_subband_analysis_reset(csi_subband_analysis_state_t *state)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
}

bool csi_subband_analysis_process(
    csi_subband_analysis_state_t *state,
    const float *norm_values,
    uint16_t norm_count,
    const float *last_norm_values,
    uint16_t last_norm_count,
    bool has_last_norm,
    const float *baseline_norm_values,
    uint16_t baseline_norm_count,
    bool has_baseline_norm,
    const csi_subband_config_t *config,
    csi_subband_analysis_result_t *result)
{
    if (state == NULL || norm_values == NULL || baseline_norm_values == NULL ||
        config == NULL || result == NULL) {
        return false;
    }

    memset(result, 0, sizeof(*result));
    result->best_band = -1;
    result->subband_state = CSI_STATE_UNKNOWN;
    for (uint8_t i = 0; i < CSI_SUBBAND_COUNT; i++) {
        result->bands[i].state = CSI_STATE_UNKNOWN;
    }

    if (!has_baseline_norm || baseline_norm_count != norm_count ||
        !has_last_norm || last_norm_values == NULL || last_norm_count != norm_count) {
        return false;
    }

    uint16_t start = norm_count > (CSI_SUBBAND_SKIP_EDGE * 2) ? CSI_SUBBAND_SKIP_EDGE : 0;
    uint16_t end = norm_count > (CSI_SUBBAND_SKIP_EDGE * 2) ? (norm_count - CSI_SUBBAND_SKIP_EDGE) : norm_count;
    uint16_t usable_count = end > start ? (uint16_t)(end - start) : 0;
    if (usable_count < CSI_SUBBAND_COUNT) {
        return false;
    }

    float top1 = -1.0f;
    float top2 = -1.0f;
    bool any_active = false;
    bool any_offset = false;

    for (uint8_t band = 0; band < CSI_SUBBAND_COUNT; band++) {
        uint16_t band_start = start + (uint16_t)((uint32_t)usable_count * band / CSI_SUBBAND_COUNT);
        uint16_t band_end = start + (uint16_t)((uint32_t)usable_count * (band + 1U) / CSI_SUBBAND_COUNT);
        csi_subband_result_t *band_result = &result->bands[band];

        band_result->start_index = band_start;
        band_result->end_index = band_end;
        band_result->valid_points = band_end > band_start ? (uint16_t)(band_end - band_start) : 0;
        if (band_result->valid_points == 0) {
            continue;
        }

        // band_delta_norm：当前归一化 CSI 频段形状与上一帧频段形状的平均绝对差。
        float band_delta_norm = csi_subband_mean_abs_delta(norm_values,
                                                           last_norm_values,
                                                           band_start,
                                                           band_end);
        // band_baseline_delta：当前归一化 CSI 频段形状与静止 baseline 频段形状的平均绝对差。
        float band_baseline_delta = csi_subband_mean_abs_delta(norm_values,
                                                               baseline_norm_values,
                                                               band_start,
                                                               band_end);
        // band_baseline_delta_change：当前 band 偏离 baseline 的程度相对上一帧的变化量。
        float band_baseline_delta_change = state->has_last_band_baseline_delta[band] ?
                                           fabsf(band_baseline_delta - state->last_band_baseline_delta[band]) :
                                           0.0f;
        float band_instant_score = 0.70f * band_delta_norm + 0.30f * band_baseline_delta_change;
        state->smooth_band_score[band] = 0.75f * state->smooth_band_score[band] +
                                         0.25f * band_instant_score;
        float band_activity = 0.65f * band_instant_score + 0.35f * state->smooth_band_score[band];
        // band_motion_score：以短时变化为主，少量融合 baseline 偏移，和全局算法权重保持一致。
        float band_motion_score = 0.82f * band_activity + 0.18f * band_baseline_delta;

        state->last_band_baseline_delta[band] = band_baseline_delta;
        state->has_last_band_baseline_delta[band] = true;

        band_result->delta_norm = band_delta_norm;
        band_result->baseline_delta = band_baseline_delta;
        band_result->baseline_delta_change = band_baseline_delta_change;
        band_result->instant_score = band_instant_score;
        band_result->smooth_motion_score = state->smooth_band_score[band];
        band_result->motion_score = band_motion_score;

        if (band_motion_score >= config->motion_threshold) {
            band_result->state = CSI_STATE_MOTION;
        } else if (band_delta_norm < config->offset_delta_max &&
                   band_baseline_delta > config->offset_base_min) {
            band_result->state = CSI_STATE_OFFSET;
            any_offset = true;
        } else if (band_delta_norm >= config->active_delta_min) {
            band_result->state = CSI_STATE_ACTIVE;
            any_active = true;
        } else {
            band_result->state = CSI_STATE_STATIC;
        }

        if (band_motion_score > top1) {
            top2 = top1;
            top1 = band_motion_score;
            result->best_band = (int8_t)band;
        } else if (band_motion_score > top2) {
            top2 = band_motion_score;
        }

        result->subband_count++;
    }

    if (result->subband_count == 0 || top1 < 0.0f) {
        result->subband_state = CSI_STATE_UNKNOWN;
        result->best_band = -1;
        return false;
    }

    // subband_score 取最高两个 band 的平均值，避免单个 band 噪声尖峰直接造成误报。
    result->subband_score = top2 >= 0.0f ? (top1 + top2) * 0.5f : top1;
    if (result->subband_score >= config->motion_threshold) {
        result->subband_state = CSI_STATE_MOTION;
    } else if (any_active) {
        result->subband_state = CSI_STATE_ACTIVE;
    } else if (any_offset) {
        result->subband_state = CSI_STATE_OFFSET;
    } else {
        result->subband_state = CSI_STATE_STATIC;
    }

    return true;
}
