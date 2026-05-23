#include "csi_processor.h"

#include "csi_print_config.h"
#include "esp_timer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// 原始 CSI 最大处理长度。和 monitor 中的原始打印缓存保持一致。
#define CSI_AMP_MAX_POINTS (CSI_RAW_MAX_LEN / 2)

// 去掉两端子载波，减少边缘噪声对运动能量的影响。
#define CSI_SUBCARRIER_SKIP_EDGE 20

/**
 * @brief CSI 运动检测状态机状态。
 *
 * 调用方法：仅由 csi_update_state() 在本文件内部切换，外部通过
 * csi_processor_result_t.motion / recovering / frozen 读取结果。
 */
typedef enum {
    CSI_FSM_STATIC = 0,  // 静止状态：允许慢速更新 baseline。
    CSI_FSM_MOTION,      // 明显运动状态：禁止更新 baseline，避免把运动学成背景。
    CSI_FSM_RECOVER,     // baseline 自适应恢复状态：运动结束或稳定偏移时快速更新 baseline。
} csi_motion_fsm_state_t;

/**
 * @brief CSI 算法内部跨帧状态。
 *
 * 调用方法：本文件只保存 RAW 主算法状态；官方补偿只用于 gain 冻结。
 */
typedef struct {
    float last_mean_amp;                         // 上一帧幅值均值，用于冻结判断。
    float last_var_amp;                          // 上一帧幅值方差，用于冻结判断。
    float last_norm_values[CSI_AMP_MAX_POINTS];  // 上一帧归一化子载波幅值。
    uint16_t last_norm_count;                    // 上一帧有效归一化点数。
    bool has_last_norm;                          // 是否已经保存上一帧归一化数据。
    float baseline_norm_values[CSI_AMP_MAX_POINTS]; // 静止 baseline 的归一化子载波幅值。
    uint16_t baseline_norm_count;                // baseline 中的有效点数。
    bool has_baseline_norm;                      // 是否已经初始化 baseline。
    float last_baseline_delta;                   // 上一帧 baseline_delta，用于计算偏移变化量。
    bool has_last_baseline_delta;                // 是否已有上一帧 baseline_delta。
    float smooth_mean_amp;                       // 幅值均值平滑值。
    float smooth_motion_score;                   // 短时运动分数平滑值。
    uint8_t freeze_count;                        // 数据冻结连续帧计数。
    uint16_t motion_count;                       // 连续超过进入 motion 阈值的帧数。
    uint16_t still_count;                        // motion 中连续低活动帧数。
    uint16_t stable_offset_count;                // 稳定但偏离旧 baseline 的连续帧数。
    int64_t last_motion_time_ms;                 // 最近一次仍认为有运动的时间，单位 ms。
    int64_t recover_start_time_ms;               // 进入 recover 的时间，单位 ms。
    uint8_t gain_freeze_count;                   // 增益跳变后的冻结帧计数。
    uint8_t last_agc_gain;                       // 上一次 AGC 增益。
    int8_t last_fft_gain;                        // 上一次 FFT 增益。
    bool has_last_gain;                          // 是否已有上一帧增益状态。
    csi_motion_fsm_state_t state;                // 当前内部 static/motion/recover 状态机状态。
    csi_subband_analysis_state_t subband_state;  // 子频段跨帧平滑状态，不修改主 baseline。
    uint32_t frame_count;                        // 已处理帧计数。
} csi_processor_state_t;

static csi_processor_state_t s_raw_processor = {0};

/**
 * @brief 判断单个 I/Q 点是否为明显无效点。
 *
 * 调用方法：仅由 csi_iq_near_invalid_pair() 内部调用。
 * @param i_value I 分量。
 * @param q_value Q 分量。
 * @return true 表示该 I/Q 点应跳过，false 表示可以继续使用。
 */
static bool csi_iq_is_invalid(int i_value, int q_value)
{
    // 明显无效点：全 0，或当前采样中反复出现的固定异常片段。
    return (i_value == 0 && q_value == 0) ||
           (i_value == -27 && q_value == 21) ||
           (i_value == -13 && q_value == 3);
}

/**
 * @brief 判断某个 I/Q 点附近是否存在无效点。
 *
 * 调用方法：csi_extract_normalized_amp() 遍历每个 I/Q 点时调用，用于连带跳过
 * 固定异常片段附近的点。
 * @param data 原始 CSI I/Q 字节流。
 * @param pair_count data 中的 I/Q 点对数量。
 * @param pair_index 当前检查的 I/Q 点对索引。
 * @return true 表示当前点附近有无效片段，应跳过；false 表示可参与计算。
 */
static bool csi_iq_near_invalid_pair(const int8_t *data, int pair_count, int pair_index)
{
    // 对异常点前后 2 个 I/Q 点一起跳过，降低固定异常片段对均值和方差的影响。
    for (int offset = -2; offset <= 2; offset++) {
        int index = pair_index + offset;
        if (index < 0 || index >= pair_count) {
            continue;
        }

        int i_value = data[2 * index];
        int q_value = data[2 * index + 1];
        if (csi_iq_is_invalid(i_value, q_value)) {
            return true;
        }
    }

    return false;
}

/**
 * @brief 从原始 CSI I/Q 数据中提取归一化幅值序列。
 *
 * 调用方法：csi_processor_process() 每帧先调用本函数，得到 mean/var、
 * valid_points 和 norm_amp_values，再进入差分与状态机计算。
 * @param data 原始 CSI I/Q 字节流。
 * @param len data 字节长度。
 * @param first_word_invalid true 时跳过前两个 I/Q 点。
 * @param mean_amp 输出：有效点幅值均值。
 * @param var_amp 输出：有效点幅值方差。
 * @param valid_points 输出：有效 I/Q 点数量。
 * @param norm_amp_values 输出：归一化后的幅值数组。
 * @param max_amp_points norm_amp_values 可写入的最大点数。
 * @return true 表示提取成功，false 表示输入无效或没有有效点。
 */
static bool csi_extract_normalized_amp(const int8_t *data,
                                       uint16_t len,
                                       bool first_word_invalid,
                                       float *mean_amp,
                                       float *var_amp,
                                       uint16_t *valid_points,
                                       float *norm_amp_values,
                                       uint16_t max_amp_points)
{
    if (data == NULL || len < 2 || mean_amp == NULL || var_amp == NULL ||
        valid_points == NULL || norm_amp_values == NULL || max_amp_points == 0) {
        return false;
    }

    float amp_values[CSI_AMP_MAX_POINTS] = {0};
    float sum = 0.0f;
    float sum_square = 0.0f;
    int pair_count = len / 2;
    int start_pair = first_word_invalid ? 2 : 0;
    int count = 0;

    if (pair_count > max_amp_points) {
        pair_count = max_amp_points;
    }

    for (int i = start_pair; i < pair_count; i++) {
        if (csi_iq_near_invalid_pair(data, pair_count, i)) {
            continue;
        }

        int i_value = data[2 * i];
        int q_value = data[2 * i + 1];
        float amp = sqrtf((float)(i_value * i_value + q_value * q_value));
        amp_values[count] = amp;
        sum += amp;
        sum_square += amp * amp;
        count++;
    }

    if (count == 0) {
        return false;
    }

    *mean_amp = sum / count;
    if (*mean_amp < 0.001f) {
        return false;
    }

    *var_amp = (sum_square / count) - (*mean_amp * *mean_amp);
    if (*var_amp < 0.0f) {
        *var_amp = 0.0f;
    }

    // 归一化后再做差分，避免 RSSI/AGC 造成整体幅值变化时误判成运动。
    for (int i = 0; i < count; i++) {
        norm_amp_values[i] = amp_values[i] / *mean_amp;
    }

    *valid_points = (uint16_t)count;
    return true;
}

/**
 * @brief 计算两个归一化子载波幅值序列的平均差异。
 *
 * 调用方法：用于计算 delta_norm 和 baseline_delta；函数内部会跳过两端边缘子载波。
 * @param current_values 当前帧归一化幅值。
 * @param reference_values 参考帧或 baseline 归一化幅值。
 * @param current_count 当前帧有效点数。
 * @param reference_count 参考序列有效点数，必须和 current_count 相同。
 * @return 平均绝对差异；输入无效或长度不一致时返回 0。
 */
static float csi_calculate_profile_delta(const float *current_values,
                                         const float *reference_values,
                                         uint16_t current_count,
                                         uint16_t reference_count)
{
    if (current_values == NULL || reference_values == NULL || current_count == 0 || current_count != reference_count) {
        return 0.0f;
    }

    uint16_t start = current_count > (CSI_SUBCARRIER_SKIP_EDGE * 2) ? CSI_SUBCARRIER_SKIP_EDGE : 0;
    uint16_t end = current_count > (CSI_SUBCARRIER_SKIP_EDGE * 2) ? (current_count - CSI_SUBCARRIER_SKIP_EDGE) : current_count;
    float diff_sum = 0.0f;
    uint16_t count = 0;

    for (uint16_t i = start; i < end; i++) {
        diff_sum += fabsf(current_values[i] - reference_values[i]);
        count++;
    }

    return count > 0 ? diff_sum / count : 0.0f;
}

/**
 * @brief 按 EMA 方式更新 baseline。
 *
 * 调用方法：只在 static 或 recover 状态满足条件时由 csi_processor_process() 调用。
 * @param baseline_values 输入/输出：待更新的 baseline 幅值序列。
 * @param current_values 当前稳定帧的归一化幅值序列。
 * @param count 参与更新的点数。
 * @param alpha 更新系数，越大 baseline 追当前环境越快。
 */
static void csi_update_baseline(float *baseline_values,
                                const float *current_values,
                                uint16_t count,
                                float alpha)
{
    if (baseline_values == NULL || current_values == NULL || count == 0) {
        return;
    }

    for (uint16_t i = 0; i < count; i++) {
        baseline_values[i] = baseline_values[i] * (1.0f - alpha) + current_values[i] * alpha;
    }
}

/**
 * @brief 计算单帧瞬时运动分数。
 *
 * 调用方法：csi_processor_process() 在得到 delta_norm 和 baseline_delta_change 后调用。
 * @param delta_norm 当前帧与上一帧的归一化形状差异。
 * @param baseline_delta_change baseline_delta 相对上一帧的变化量。
 * @return 单帧瞬时运动分数。
 */
static float csi_calculate_instant_score(float delta_norm,
                                         float baseline_delta_change)
{
    // 只看“变化速度”：相邻帧形状变化和 baseline_delta 的变化量。
    // 当前 baseline_delta 可能只是新静止环境和旧 baseline 的偏移，不直接代表还在运动。
    return delta_norm * 0.70f +
           baseline_delta_change * 0.30f;
}

/**
 * @brief 计算短时活动分数。
 *
 * 调用方法：csi_processor_process() 在得到 instant/smooth 两个分数后调用。
 * @param instant_motion_score 当前帧瞬时运动分数。
 * @param smooth_motion_score EMA 平滑后的运动分数。
 * @return 短时活动分数，用于判断 motion 是否应退出和 recover 是否可更新 baseline。
 */
static float csi_calculate_activity_score(float instant_motion_score,
                                          float smooth_motion_score)
{
    // activity_score 只描述短时变化，用来判断是否还在动。
    return instant_motion_score * 0.65f +
           smooth_motion_score * 0.35f;
}

/**
 * @brief 计算最终 motion_score。
 *
 * 调用方法：csi_processor_process() 在得到 activity_score 和 baseline_delta 后调用，
 * 输出到串口/网页，并用于 static 进入 motion 的状态判断。
 * @param activity_score 短时活动分数。
 * @param baseline_delta 当前帧与 baseline 的差异。
 * @return 最终运动评分。
 */
static float csi_calculate_final_score(float activity_score, float baseline_delta)
{
    // motion_score 用固定权重融合“短时变化”和“偏离 baseline”。
    // baseline 自纠偏会处理稳定高 baseline_delta，避免长期误抬 motion_score。
    return activity_score * (1.0f - CSI_BASELINE_MOTION_WEIGHT) +
           baseline_delta * CSI_BASELINE_MOTION_WEIGHT;
}

static csi_state_t csi_fsm_to_public_state(csi_motion_fsm_state_t state)
{
    switch (state) {
    case CSI_FSM_STATIC:
        return CSI_STATE_STATIC;
    case CSI_FSM_MOTION:
        return CSI_STATE_MOTION;
    case CSI_FSM_RECOVER:
        return CSI_STATE_RECOVER;
    default:
        return CSI_STATE_UNKNOWN;
    }
}

static csi_state_t csi_score_to_simple_state(float score)
{
    return score >= CSI_MOTION_ON_THRESHOLD ? CSI_STATE_MOTION : CSI_STATE_STATIC;
}

static void csi_subband_override_bands(csi_subband_analysis_result_t *subband, csi_state_t state_id)
{
    if (subband == NULL) {
        return;
    }

    subband->subband_state = state_id;
    for (uint8_t i = 0; i < CSI_SUBBAND_COUNT; i++) {
        subband->bands[i].state = state_id;
    }
}

/**
 * @brief 检查增益是否突变，并更新冻结帧计数。
 *
 * 调用方法：RAW 主算法每帧传入当前增益状态，用于冻结 motion 和 baseline 更新。
 */
static bool csi_update_gain_freeze(csi_processor_state_t *state, const csi_processor_gain_t *gain)
{
    if (state == NULL || gain == NULL) {
        return false;
    }

    bool gain_changed = false;
    if (state->has_last_gain) {
        int agc_diff = (int)gain->agc_gain - (int)state->last_agc_gain;
        int fft_diff = (int)gain->fft_gain - (int)state->last_fft_gain;
        gain_changed = (abs(agc_diff) >= CSI_GAIN_AGC_CHANGE_TH) ||
                       (abs(fft_diff) >= CSI_GAIN_FFT_CHANGE_TH);
    }

    if (gain_changed && state->gain_freeze_count == 0) {
        state->gain_freeze_count = CSI_GAIN_FREEZE_FRAMES;
    }

    bool gain_freezing = state->gain_freeze_count > 0;
    if (state->gain_freeze_count > 0) {
        state->gain_freeze_count--;
    }

    state->last_agc_gain = gain->agc_gain;
    state->last_fft_gain = gain->fft_gain;
    state->has_last_gain = true;

    return gain_freezing;
}

/**
 * @brief 清理运动状态退出时的平滑尾巴。
 *
 * 调用方法：状态机从 motion/recover 回到 static 或进入 recover 时调用，
 * 避免旧平滑分数导致状态拖尾。
 * @param state 算法内部状态。
 * @param activity_score 当前活动分数，用来重置平滑值。
 */
static void csi_reset_motion_tail(csi_processor_state_t *state, float activity_score)
{
    if (state == NULL) {
        return;
    }

    state->smooth_motion_score = activity_score;
}

/**
 * @brief 更新 static/motion/recover 三态状态机。
 *
 * 调用方法：csi_processor_process() 每帧计算完 activity_score、motion_score、
 * baseline_delta 后调用；update_blocked 时调用方不调用本函数。
 * @param state 算法内部状态。
 * @param activity_score 短时活动分数，用于判断 motion 退出和 recover 中断。
 * @param motion_score 最终运动评分，用于 static 进入 motion。
 * @param baseline_delta 当前 baseline 差异，用于 recover 完成判断。
 * @param motion_entry_blocked true 表示本帧禁止 STATIC 累计进入 MOTION。
 */
static void csi_update_state(csi_processor_state_t *state,
                             float activity_score,
                             float motion_score,
                             float baseline_delta,
                             bool motion_entry_blocked)
{
    if (state == NULL) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    if (state->state == CSI_FSM_STATIC) {
        if (motion_entry_blocked) {
            state->motion_count = 0;
        } else if (motion_score > CSI_MOTION_ON_THRESHOLD) {
            if (state->motion_count < UINT16_MAX) {
                state->motion_count++;
            }
        } else {
            state->motion_count = 0;
        }

        if (state->motion_count >= CSI_MOTION_ON_FRAMES) {
            state->state = CSI_FSM_MOTION;
            state->still_count = 0;
            state->last_motion_time_ms = now_ms;
        }
    } else if (state->state == CSI_FSM_MOTION) {
        if (activity_score > CSI_MOTION_OFF_THRESHOLD) {
            state->still_count = 0;
            state->last_motion_time_ms = now_ms;
        } else if (state->still_count < UINT16_MAX) {
            state->still_count++;
        }

        if (state->still_count >= CSI_STILL_FRAMES &&
            (now_ms - state->last_motion_time_ms) > CSI_MOTION_HOLD_MS) {
            state->state = CSI_FSM_RECOVER;
            state->motion_count = 0;
            state->still_count = 0;
            state->recover_start_time_ms = now_ms;
            csi_reset_motion_tail(state, activity_score);
        }
    } else {
        if (activity_score >= CSI_RECOVER_ACTIVITY_THRESHOLD) {
            state->state = CSI_FSM_MOTION;
            state->still_count = 0;
            state->last_motion_time_ms = now_ms;
            return;
        }

        if (baseline_delta < CSI_RECOVER_TARGET_DELTA ||
            (now_ms - state->recover_start_time_ms) > CSI_RECOVER_MAX_MS) {
            state->state = CSI_FSM_STATIC;
            state->motion_count = 0;
            state->still_count = 0;
            csi_reset_motion_tail(state, activity_score);
        }
    }
}

/**
 * @brief 处理一帧 CSI 原始 I/Q 数据。
 *
 * 参数：
 *   data: 原始 CSI I/Q 字节流。
 *   len: data 字节长度。
 *   first_word_invalid: ESP-IDF 标记的首字无效标志。
 *   result: 输出参数，保存处理后的特征和状态。
 * 调用方法：wifi_csi_rx_cb() 收到一帧 CSI 后调用。
 * @return 处理成功返回 true，输入无效或有效点不足返回 false。
 */
static bool csi_processor_process_with_state(csi_processor_state_t *state,
                                             const int8_t *data,
                                             uint16_t len,
                                             bool first_word_invalid,
                                             const csi_processor_gain_t *gain,
                                             csi_processor_result_t *result)
{
    if (state == NULL || result == NULL) {
        return false;
    }

    memset(result, 0, sizeof(*result));

    float norm_values[CSI_AMP_MAX_POINTS] = {0};
    uint16_t valid_points = 0;
    float mean_amp = 0.0f;
    float var_amp = 0.0f;

    if (!csi_extract_normalized_amp(data,
                                    len,
                                    first_word_invalid,
                                    &mean_amp,
                                    &var_amp,
                                    &valid_points,
                                    norm_values,
                                    CSI_AMP_MAX_POINTS)) {
        return false;
    }

    float mean_diff_ratio = state->last_mean_amp > 0.001f ?
                            fabsf(mean_amp - state->last_mean_amp) / state->last_mean_amp : 0.0f;
    float var_diff_ratio = state->last_var_amp > 0.001f ?
                           fabsf(var_amp - state->last_var_amp) / state->last_var_amp : 0.0f;
    float delta_norm = state->has_last_norm ?
                       csi_calculate_profile_delta(norm_values,
                                                   state->last_norm_values,
                                                   valid_points,
                                                   state->last_norm_count) :
                       0.0f;

    if (!state->has_baseline_norm || state->baseline_norm_count != valid_points) {
        memcpy(state->baseline_norm_values, norm_values, sizeof(float) * valid_points);
        state->baseline_norm_count = valid_points;
        state->has_baseline_norm = true;
    }

    float baseline_delta = csi_calculate_profile_delta(norm_values,
                                                       state->baseline_norm_values,
                                                       valid_points,
                                                       state->baseline_norm_count);
    bool gain_freeze = csi_update_gain_freeze(state, gain);
    float baseline_delta_change = state->has_last_baseline_delta ?
                                  fabsf(baseline_delta - state->last_baseline_delta) :
                                  0.0f;
    state->last_baseline_delta = baseline_delta;
    state->has_last_baseline_delta = true;
    float instant_motion_score = csi_calculate_instant_score(delta_norm,
                                                             baseline_delta_change);

    if (state->has_last_norm && delta_norm < CSI_FREEZE_EPSILON &&
        mean_diff_ratio < CSI_FREEZE_EPSILON && var_diff_ratio < CSI_FREEZE_EPSILON) {
        if (state->freeze_count < UINT8_MAX) {
            state->freeze_count++;
        }
    } else {
        state->freeze_count = 0;
    }

    state->last_mean_amp = mean_amp;
    state->last_var_amp = var_amp;

    bool frozen = state->freeze_count >= CSI_FREEZE_FRAME_THRESHOLD;
    bool update_blocked = frozen || gain_freeze;
    if (!update_blocked) {
        if (state->frame_count == 0) {
            state->smooth_mean_amp = mean_amp;
            state->smooth_motion_score = 0.0f;
        } else {
            state->smooth_mean_amp = state->smooth_mean_amp * 0.8f + mean_amp * 0.2f;
            state->smooth_motion_score = state->smooth_motion_score * (1.0f - CSI_MOTION_EMA_ALPHA) +
                                         instant_motion_score * CSI_MOTION_EMA_ALPHA;
        }
    }

    float activity_score = csi_calculate_activity_score(instant_motion_score,
                                                       state->smooth_motion_score);
    float motion_score = csi_calculate_final_score(activity_score, baseline_delta);
    float global_norm_score = motion_score;
    float subband_norm_score = 0.0f;
    csi_state_t subband_state_id = CSI_STATE_UNKNOWN;
#if CSI_ENABLE_SUBBAND_ANALYSIS
    csi_subband_config_t subband_config = {
        .motion_threshold = CSI_MOTION_ON_THRESHOLD * 0.85f,
        .offset_delta_max = CSI_STABLE_OFFSET_DELTA_NORM_MAX,
        .offset_base_min = CSI_STABLE_OFFSET_DELTA_MIN,
        .active_delta_min = CSI_STATIC_UPDATE_ACTIVITY_MAX,
    };
    /*
     * subband 只做局部频段分析、辅助融合和算法对比，不重写 static/motion/recover 主状态机。
     * 输入使用本文件已经计算好的归一化幅值数组，模块不会解析原始 I/Q，也不会更新全局 baseline。
     */
    bool has_subband = csi_subband_analysis_process(&state->subband_state,
                                                    norm_values,
                                                    valid_points,
                                                    state->last_norm_values,
                                                    state->last_norm_count,
                                                    state->has_last_norm,
                                                    state->baseline_norm_values,
                                                    state->baseline_norm_count,
                                                    state->has_baseline_norm,
                                                    &subband_config,
                                                    &result->subband);
    if (has_subband) {
        subband_norm_score = result->subband.subband_score;
        subband_state_id = result->subband.subband_state;
    }
#endif
    float fusion_score = 0.75f * global_norm_score + 0.25f * subband_norm_score;
    csi_state_t fusion_state_id = csi_score_to_simple_state(fusion_score);
    float decision_score = global_norm_score;
    csi_state_t decision_base_state_id = csi_score_to_simple_state(global_norm_score);
    /*
     * decision_mode 控制最终采用 global/subband/fusion 哪一套分数和状态。
     * 默认 GLOBAL_NORM，保证新增多频段分析后初始行为尽量接近旧算法。
     */
    if (CSI_DECISION_MODE == CSI_DECISION_SUBBAND_NORM) {
        decision_score = subband_norm_score;
        decision_base_state_id = subband_state_id;
    } else if (CSI_DECISION_MODE == CSI_DECISION_FUSION) {
        decision_score = fusion_score;
        decision_base_state_id = fusion_state_id;
    }
    bool stable_offset = activity_score < CSI_STABLE_OFFSET_ACTIVITY_MAX &&
                         delta_norm < CSI_STABLE_OFFSET_DELTA_NORM_MAX &&
                         baseline_delta > CSI_STABLE_OFFSET_DELTA_MIN;

    if (update_blocked) {
        state->stable_offset_count = 0;
    } else if (stable_offset) {
        if (state->stable_offset_count < UINT16_MAX) {
            state->stable_offset_count++;
        }
    } else {
        state->stable_offset_count = 0;
    }

    bool stable_offset_ready = state->stable_offset_count >= CSI_STABLE_OFFSET_FRAMES;

    if (update_blocked) {
        state->motion_count = 0;
    } else {
        csi_update_state(state, activity_score, decision_score, baseline_delta, stable_offset);

        if (stable_offset_ready) {
            if (state->state != CSI_FSM_RECOVER) {
                state->state = CSI_FSM_RECOVER;
                state->motion_count = 0;
                state->still_count = 0;
                state->recover_start_time_ms = esp_timer_get_time() / 1000;
                csi_reset_motion_tail(state, activity_score);
            }
        }

        if (state->state == CSI_FSM_RECOVER && activity_score < CSI_RECOVER_ACTIVITY_THRESHOLD) {
            float alpha = stable_offset_ready ? CSI_BASELINE_ALPHA_STABLE_OFFSET : CSI_BASELINE_ALPHA_RECOVER;
            csi_update_baseline(state->baseline_norm_values,
                                norm_values,
                                valid_points,
                                alpha);
        } else if (state->state == CSI_FSM_STATIC &&
                   activity_score < CSI_STATIC_UPDATE_ACTIVITY_MAX &&
                   baseline_delta < CSI_STATIC_UPDATE_DELTA_MAX &&
                   delta_norm < CSI_MOTION_OFF_THRESHOLD) {
            csi_update_baseline(state->baseline_norm_values,
                                norm_values,
                                valid_points,
                                CSI_BASELINE_ALPHA_STATIC);
        }
    }

    bool moving_now = !gain_freeze && state->state == CSI_FSM_MOTION;
    bool recovering = !gain_freeze && state->state == CSI_FSM_RECOVER;
    /*
     * global_state_id 表示旧全局归一化主状态机结果；
     * subband_state_id 表示本地频段分离算法整体结果；
     * fusion_state_id 表示 global/subband 分数融合后的简单状态；
     * decision_state_id 表示当前 CSI_DECISION_MODE 最终对外采用的状态。
     */
    csi_state_t global_state_id = csi_fsm_to_public_state(state->state);
    if (gain_freeze) {
        global_state_id = CSI_STATE_GAIN_FREEZE;
    } else if (frozen) {
        global_state_id = CSI_STATE_DATA_FROZEN;
    }

    csi_state_t decision_state_id = decision_base_state_id;
    if (CSI_DECISION_MODE == CSI_DECISION_GLOBAL_NORM) {
        decision_state_id = global_state_id;
    }

    if (gain_freeze) {
        decision_state_id = CSI_STATE_GAIN_FREEZE;
        subband_state_id = CSI_STATE_GAIN_FREEZE;
        fusion_state_id = CSI_STATE_GAIN_FREEZE;
#if CSI_ENABLE_SUBBAND_ANALYSIS
        /*
         * gain_freeze 是全局异常状态，此时局部 band 判断不可靠，统一覆盖 band state。
         */
        csi_subband_override_bands(&result->subband, CSI_STATE_GAIN_FREEZE);
#endif
    } else if (frozen) {
        decision_state_id = CSI_STATE_DATA_FROZEN;
        subband_state_id = CSI_STATE_DATA_FROZEN;
        fusion_state_id = CSI_STATE_DATA_FROZEN;
#if CSI_ENABLE_SUBBAND_ANALYSIS
        /*
         * data_frozen 表示 CSI 数据疑似未刷新，此时局部 band 判断不可靠，统一覆盖 band state。
         */
        csi_subband_override_bands(&result->subband, CSI_STATE_DATA_FROZEN);
#endif
    }

    memcpy(state->last_norm_values, norm_values, sizeof(float) * valid_points);
    state->last_norm_count = valid_points;
    state->has_last_norm = true;
    state->frame_count++;

    result->valid_points = valid_points;
    result->mean_amp = mean_amp;
    result->var_amp = var_amp;
    result->delta_norm = delta_norm;
    result->baseline_delta = baseline_delta;
    result->smooth_mean_amp = state->smooth_mean_amp;
    result->motion_score = motion_score;
    result->smooth_motion_score = state->smooth_motion_score;
    result->window_motion_score = 0.0f;
    result->amp_motion_score = 0.0f;
    result->freeze_count = state->freeze_count;
    result->frozen = frozen;
    result->gain_frozen = gain_freeze;
    result->recovering = recovering;
    result->motion = moving_now;
    result->state = decision_state_id;
    result->global_norm_score = global_norm_score;
    result->subband_norm_score = subband_norm_score;
    result->fusion_score = fusion_score;
    result->decision_score = decision_score;
    result->decision_mode = (uint8_t)CSI_DECISION_MODE;
    result->global_state_id = global_state_id;
    result->subband_state_id = subband_state_id;
    result->fusion_state_id = fusion_state_id;
    result->decision_state_id = decision_state_id;

    return true;
}

bool csi_processor_process(const int8_t *data,
                           uint16_t len,
                           bool first_word_invalid,
                           const csi_processor_gain_t *gain,
                           csi_processor_result_t *result)
{
    return csi_processor_process_with_state(&s_raw_processor, data, len, first_word_invalid, gain, result);
}
