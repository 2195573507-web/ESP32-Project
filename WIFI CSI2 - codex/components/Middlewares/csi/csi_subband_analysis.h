#ifndef CSI_SUBBAND_ANALYSIS_H
#define CSI_SUBBAND_ANALYSIS_H

#include <stdbool.h>
#include <stdint.h>

#include "csi_state.h"

// 子频段数量：把去掉边缘后的归一化 CSI 子载波平均切成 4 个 band 分别判断。
#define CSI_SUBBAND_COUNT 4

/**
 * @brief CSI 子载波多频段分析模块。
 *
 * 本模块只做已经归一化后的 CSI 子载波分区分析，不解析原始 I/Q，不负责主状态机，
 * 也不更新全局 baseline。主流程仍由 csi_processor.c 维护。
 *
 * 调用方法：csi_processor.c 每处理一帧 CSI 后，把当前归一化幅值、上一帧归一化幅值、
 * 全局静止 baseline 和本结构配置传入 csi_subband_analysis_process()，本模块返回
 * 4 个 band 的局部判断结果和一个 subband 整体状态。
 */
typedef struct {
    float motion_threshold; // band_motion_score 达到该阈值时，该 band 或整体 subband 判定为 motion。
    float offset_delta_max; // 相邻帧变化低于该值，同时 baseline 偏移较大时，判定为稳定偏移 offset。
    float offset_base_min;  // offset 判定所需的 baseline 偏移下限，用于区分普通静止和环境形状偏移。
    float active_delta_min; // 轻微扰动 active 的相邻帧变化下限，低于 motion 但高于纯静止噪声。
} csi_subband_config_t;

/**
 * @brief 单个子频段 band 的分析结果。
 *
 * 调用方法：由 csi_subband_analysis_process() 填充，调用方只读这些字段用于串口输出、
 * 网页展示和辅助判断，不应在外部修改后再传回本模块。
 */
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

/**
 * @brief 一帧 CSI 的多频段整体分析结果。
 *
 * 调用方法：调用方在每帧传入一个 result 指针，csi_subband_analysis_process() 会先清零，
 * 再写入有效 band 数量、最强 band、整体 subband 分数和每个 band 的细节。
 */
typedef struct {
    uint8_t subband_count;         // 有效 band 数量。
    int8_t best_band;              // motion_score 最高的 band，范围 0~3，无效时为 -1。
    float subband_score;           // 最高两个 band_motion_score 的平均值。
    csi_state_t subband_state;     // 多频段整体状态。
    csi_subband_result_t bands[CSI_SUBBAND_COUNT];
} csi_subband_analysis_result_t;

/**
 * @brief 多频段算法跨帧状态。
 *
 * 调用方法：由 csi_processor.c 长期保存同一个实例，每帧传给
 * csi_subband_analysis_process()。当输入点数变化、主处理器重置或需要重新开始统计时，
 * 调用 csi_subband_analysis_reset() 清空。
 */
typedef struct {
    float last_band_baseline_delta[CSI_SUBBAND_COUNT]; // 上一帧每个 band 的 baseline_delta，用于计算偏移变化量。
    float smooth_band_score[CSI_SUBBAND_COUNT];        // 每个 band 的 EMA 平滑分数，用于抑制单帧噪声。
    bool has_last_band_baseline_delta[CSI_SUBBAND_COUNT]; // true 表示对应 band 已经有上一帧 baseline_delta。
} csi_subband_analysis_state_t;

/**
 * @brief 重置多频段算法跨帧状态。
 *
 * 调用方法：当 CSI 有效点数变化、主 CSI 处理器重置，或需要重新开始频段平滑统计时调用。
 * @param state 多频段算法跨帧状态，不能为空；为 NULL 时函数直接返回。
 */
void csi_subband_analysis_reset(csi_subband_analysis_state_t *state);

/**
 * @brief 处理一帧归一化 CSI 幅值，输出 4 个频段和整体 subband 判断结果。
 *
 * 调用方法：csi_processor.c 在得到当前帧 norm_values、上一帧 last_norm_values 和
 * 当前 global baseline 后调用。本函数不保存原始 CSI，不更新 global baseline，只更新
 * state 中的 band 级平滑值和上一帧 band baseline_delta。
 *
 * @param state 多频段算法跨帧状态，由调用方长期保存。
 * @param norm_values 当前帧归一化幅值数组。
 * @param norm_count 当前帧归一化幅值点数。
 * @param last_norm_values 上一帧归一化幅值数组，用于计算 band_delta_norm。
 * @param last_norm_count 上一帧归一化幅值点数，必须与 norm_count 一致。
 * @param has_last_norm true 表示 last_norm_values 有效。
 * @param baseline_norm_values 全局静止 baseline 归一化幅值数组，用于计算 band_baseline_delta。
 * @param baseline_norm_count baseline 点数，必须与 norm_count 一致。
 * @param has_baseline_norm true 表示 baseline_norm_values 有效。
 * @param config 多频段判断阈值配置。
 * @param result 输出：本帧多频段分析结果。
 * @return true 表示分析成功，false 表示输入无效或当前缺少上一帧/baseline 参考。
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
    csi_subband_analysis_result_t *result);

#endif // CSI_SUBBAND_ANALYSIS_H
