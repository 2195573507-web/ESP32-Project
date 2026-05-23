#ifndef CSI_SUBBAND_ANALYSIS_H
#define CSI_SUBBAND_ANALYSIS_H

#include <stdbool.h>
#include <stdint.h>

#include "csi_state.h"

#define CSI_SUBBAND_COUNT 4

/*
 * CSI 子载波多频段分析模块。
 *
 * 本模块只做已经归一化后的 CSI 子载波分区分析，不解析原始 I/Q，不负责主状态机，
 * 也不更新全局 baseline。主流程仍由 csi_processor.c 维护。
 */
typedef struct {
    float motion_threshold; // band_motion_score 达到该阈值时判定为 motion。
    float offset_delta_max; // 相邻帧变化低于该值且偏离 baseline 较大时判定为 offset。
    float offset_base_min;  // offset 判定所需的 baseline 偏移下限。
    float active_delta_min; // 轻微扰动 active 的相邻帧变化下限。
} csi_subband_config_t;

typedef struct {
    uint16_t start_index;   // 当前 band 在归一化数组中的起始索引，已跳过边缘子载波。
    uint16_t end_index;     // 当前 band 的结束索引，左闭右开。
    uint16_t valid_points;  // 当前 band 参与计算的有效点数。

    float delta_norm;              // 当前 band 与上一帧的平均绝对差。
    float baseline_delta;          // 当前 band 与 baseline 的平均绝对差。
    float baseline_delta_change;   // 当前 band baseline_delta 的帧间变化。
    float instant_score;           // 当前 band 的瞬时变化分数。
    float smooth_motion_score;     // 当前 band 的 EMA 平滑变化分数。
    float motion_score;            // 当前 band 的最终运动分数。

    csi_state_t state;             // 当前 band 在 ESP32-C5 本地判断出的状态。
} csi_subband_result_t;

typedef struct {
    uint8_t subband_count;         // 有效 band 数量。
    int8_t best_band;              // motion_score 最高的 band，范围 0~3，无效时为 -1。
    float subband_score;           // 最高两个 band_motion_score 的平均值。
    csi_state_t subband_state;     // 多频段整体状态。
    csi_subband_result_t bands[CSI_SUBBAND_COUNT];
} csi_subband_analysis_result_t;

typedef struct {
    float last_band_baseline_delta[CSI_SUBBAND_COUNT];
    float smooth_band_score[CSI_SUBBAND_COUNT];
    bool has_last_band_baseline_delta[CSI_SUBBAND_COUNT];
} csi_subband_analysis_state_t;

void csi_subband_analysis_reset(csi_subband_analysis_state_t *state);

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
    csi_subband_analysis_result_t *result);

#endif // CSI_SUBBAND_ANALYSIS_H
