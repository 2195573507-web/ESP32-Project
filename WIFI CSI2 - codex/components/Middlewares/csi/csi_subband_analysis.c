#include "csi_subband_analysis.h"

#include <math.h>
#include <string.h>

// 边缘跳过点数：与 csi_processor.c 的策略保持一致，避免边缘子载波噪声进入 band 判断。
#define CSI_SUBBAND_SKIP_EDGE 20

/**
 * @brief 计算两个归一化幅值区间的平均绝对差。
 *
 * 调用方法：仅供本文件内部的 csi_subband_analysis_process() 调用，用来计算
 * band_delta_norm 和 band_baseline_delta。
 *
 * @param a 第一组归一化幅值数组。
 * @param b 第二组归一化幅值数组。
 * @param start 起始索引，包含。
 * @param end 结束索引，不包含。
 * @return [start, end) 区间内的平均绝对差；如果区间为空则返回 0。
 */
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

/**
 * @brief 重置多频段算法跨帧状态。
 *
 * 调用方法：csi_processor.c 在 CSI 有效点数变化、算法重新初始化或需要清空 band 平滑
 * 记忆时调用。
 *
 * @param state 多频段算法跨帧状态，不能为空；NULL 时直接返回。
 */
void csi_subband_analysis_reset(csi_subband_analysis_state_t *state)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
}

/**
 * @brief 处理一帧归一化 CSI 幅值，输出 4 个 band 的局部状态和整体 subband 状态。
 *
 * 调用方法：csi_processor.c 每帧拿到 norm_values、last_norm_values 和 baseline_norm_values 后调用。
 * 本函数只读取这些输入并更新自己的 band 平滑状态，不修改主 baseline，也不解析原始 I/Q。
 *
 * @param state 多频段算法跨帧状态。
 * @param norm_values 当前帧归一化幅值数组。
 * @param norm_count 当前帧归一化幅值点数。
 * @param last_norm_values 上一帧归一化幅值数组。
 * @param last_norm_count 上一帧归一化幅值点数。
 * @param has_last_norm true 表示上一帧归一化幅值有效。
 * @param baseline_norm_values 静止 baseline 归一化幅值数组。
 * @param baseline_norm_count baseline 点数。
 * @param has_baseline_norm true 表示 baseline 有效。
 * @param config band 判定阈值配置。
 * @param result 输出：本帧多频段分析结果。
 * @return true 表示分析成功，false 表示输入无效、参考数据不足或 band 数不足。
 */
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

    // 先清空输出结果，再逐 band 填充。
    memset(result, 0, sizeof(*result));
    result->best_band = -1;
    result->subband_state = CSI_STATE_UNKNOWN;
    for (uint8_t i = 0; i < CSI_SUBBAND_COUNT; i++) {
        result->bands[i].state = CSI_STATE_UNKNOWN;
    }

    // 缺少上一帧或 baseline 时，不做 band 判断。
    if (!has_baseline_norm || baseline_norm_count != norm_count ||
        !has_last_norm || last_norm_values == NULL || last_norm_count != norm_count) {
        return false;
    }

    // 跳过两端边缘子载波，只分析中间有效区。
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

    // 把有效区平均切成 4 个 band，逐段计算局部变化和 baseline 偏移。
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

        // band_delta_norm：当前 band 与上一帧同一 band 的平均绝对差，表示短时变化强弱。
        float band_delta_norm = csi_subband_mean_abs_delta(norm_values,
                                                           last_norm_values,
                                                           band_start,
                                                           band_end);
        // band_baseline_delta：当前 band 与静止 baseline 的平均绝对差，表示相对静止背景的偏离程度。
        float band_baseline_delta = csi_subband_mean_abs_delta(norm_values,
                                                               baseline_norm_values,
                                                               band_start,
                                                               band_end);
        // band_baseline_delta_change：band 偏离 baseline 的程度相对上一帧的变化量。
        float band_baseline_delta_change = state->has_last_band_baseline_delta[band] ?
                                           fabsf(band_baseline_delta - state->last_band_baseline_delta[band]) :
                                           0.0f;
        // band_instant_score：短时变化分数，优先看相邻帧变化，其次看 baseline 偏移变化。
        float band_instant_score = 0.70f * band_delta_norm + 0.30f * band_baseline_delta_change;
        // smooth_band_score：对 band_instant_score 做 EMA 平滑，减少单帧尖峰误报。
        state->smooth_band_score[band] = 0.75f * state->smooth_band_score[band] +
                                         0.25f * band_instant_score;
        // band_activity：把瞬时变化和平滑变化再融合一次，作为 band 的活动强度。
        float band_activity = 0.65f * band_instant_score + 0.35f * state->smooth_band_score[band];
        // band_motion_score：最终 band 运动分数，短时变化为主，baseline 偏移为辅。
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

        // 记录本帧分数最高的两个 band，避免单个 band 尖峰直接决定整体判断。
        if (band_motion_score > top1) {
            top2 = top1;
            top1 = band_motion_score;
            result->best_band = (int8_t)band;
        } else if (band_motion_score > top2) {
            top2 = band_motion_score;
        }

        result->subband_count++;
    }

    // 没有任何有效 band 时，输出 unknown。
    if (result->subband_count == 0 || top1 < 0.0f) {
        result->subband_state = CSI_STATE_UNKNOWN;
        result->best_band = -1;
        return false;
    }

    // subband_score 取最高两个 band 的平均值，减少单点噪声尖峰的影响。
    result->subband_score = top2 >= 0.0f ? (top1 + top2) * 0.5f : top1;
    // 整体 subband 状态按整体分数优先，其次看是否存在 active / offset band。
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
