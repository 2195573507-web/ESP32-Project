#include "csi_espectre_like.h"

#include <math.h>
#include <string.h>

// calibration 算法：启动后先累计若干帧静止 CSI，用于计算每个子载波的均值、方差和 NBVI 分数。
#define CSI_ESPECTRE_CALIBRATION_FRAMES 200
// MVS 算法：保存最近 N 帧 turbulence，计算 turbulence 的滑动方差。
#define CSI_ESPECTRE_WINDOW_SIZE 32
// 子载波选择算法：跳过两端边缘子载波，减少边缘噪声进入 NBVI 排名。
#define CSI_ESPECTRE_SUBCARRIER_SKIP_EDGE 20
// NBVI 有效性保护：均值太低的子载波信噪比差，不参与选择。
#define CSI_ESPECTRE_MIN_MEAN 0.15f
// NBVI 算法：alpha 越大越强调 std / mean^2，对低均值高波动子载波更敏感。
#define CSI_ESPECTRE_NBVI_ALPHA 0.75f
// baseline 更新算法：ESPectre-like 独立 baseline 只在本算法判断 static 时慢速更新。
#define CSI_ESPECTRE_BASELINE_ALPHA 0.004f
// motion 阈值保护：自适应阈值不会低于该下限，避免校准过静时轻微噪声误报。
#define CSI_ESPECTRE_THRESHOLD_FLOOR 0.012f
// motion 阈值算法：初始稳定 turbulence 均值乘以该系数，形成自适应阈值。
#define CSI_ESPECTRE_THRESHOLD_MULTIPLIER 3.0f
// motion_score 算法：turbulence 为主，少量融合 sqrt(MVS) 捕捉短时变化。
#define CSI_ESPECTRE_MOTION_WEIGHT 0.35f
// 子载波选择算法：两个被选子载波至少间隔该数量，避免选中一串相邻冗余点。
#define CSI_ESPECTRE_MIN_INDEX_GAP 3

// ESP32-C5 当前 RAW 最大 1024 字节，即最多 512 个 I/Q 点；本模块最多处理 512 个归一化幅值点。
#define CSI_ESPECTRE_MAX_POINTS 512

/**
 * @brief ESPectre-like 独立算法跨帧状态。
 *
 * 调用方法：只由本文件的 csi_espectre_like_process() 读写。主 global_norm
 * 和 subband_norm 算法不会读取或修改本结构，保证三套算法判断互不影响。
 */
typedef struct {
    uint16_t calibration_frames;                       // 已累计的静止校准帧数。
    float sum[CSI_ESPECTRE_MAX_POINTS];                // 每个子载波归一化幅值累计和。
    float sum_square[CSI_ESPECTRE_MAX_POINTS];         // 每个子载波归一化幅值平方累计和。
    float nbvi[CSI_ESPECTRE_MAX_POINTS];               // 每个子载波的 NBVI 排名分数。
    float baseline[CSI_ESPECTRE_MAX_POINTS];           // 本算法独立 baseline，不复用 global baseline。
    uint16_t norm_count;                               // 当前归一化幅值数组长度。
    bool calibrated;                                   // true 表示已完成 NBVI 子载波选择。
    uint8_t selected_count;                            // 已选子载波数量。
    uint16_t selected_indices[CSI_ESPECTRE_SELECTED_MAX]; // NBVI 选出的子载波索引。
    float turbulence_window[CSI_ESPECTRE_WINDOW_SIZE]; // 最近 N 帧 turbulence 环形窗口。
    uint8_t window_count;                              // 当前窗口有效样本数。
    uint8_t window_index;                              // 下一次写入窗口的位置。
    float calibration_turbulence_mean;                 // 校准后首个窗口的 turbulence 均值。
    float threshold;                                   // ESPectre-like 当前 motion 判定阈值。
} csi_espectre_like_state_t;

static csi_espectre_like_state_t s_espectre = {0};

/**
 * @brief 限制本模块处理的子载波数量。
 *
 * 调用方法：csi_espectre_like_process() 每帧入口处调用。
 * @param norm_count 上游共享预处理得到的归一化幅值点数。
 * @return 本模块实际处理点数，最大不超过 CSI_ESPECTRE_MAX_POINTS。
 */
static uint16_t csi_espectre_limit_count(uint16_t norm_count)
{
    return norm_count > CSI_ESPECTRE_MAX_POINTS ? CSI_ESPECTRE_MAX_POINTS : norm_count;
}

/**
 * @brief 判断候选子载波是否离已选子载波足够远。
 *
 * 调用方法：csi_espectre_select_subcarriers() 做 NBVI Top-K 选择时调用。
 * @param index 当前候选子载波索引。
 * @param selected 已选子载波索引数组。
 * @param selected_count 已选子载波数量。
 * @return true 表示可以选择该索引，false 表示和已有索引过近。
 */
static bool csi_espectre_index_far_enough(uint16_t index,
                                          const uint16_t *selected,
                                          uint8_t selected_count)
{
    for (uint8_t i = 0; i < selected_count; i++) {
        uint16_t other = selected[i];
        uint16_t diff = index > other ? (uint16_t)(index - other) : (uint16_t)(other - index);
        if (diff < CSI_ESPECTRE_MIN_INDEX_GAP) {
            return false;
        }
    }

    return true;
}

/**
 * @brief 判断候选子载波是否已经被选择。
 *
 * 调用方法：csi_espectre_select_subcarriers() 做 Top-K 选择时调用，避免重复选择
 * 同一个索引，同时保留原始 NBVI 分数用于后续调试输出。
 * @param index 当前候选子载波索引。
 * @param selected 已选子载波索引数组。
 * @param selected_count 已选子载波数量。
 * @return true 表示已经选择过该索引，false 表示尚未选择。
 */
static bool csi_espectre_index_selected(uint16_t index,
                                        const uint16_t *selected,
                                        uint8_t selected_count)
{
    for (uint8_t i = 0; i < selected_count; i++) {
        if (selected[i] == index) {
            return true;
        }
    }

    return false;
}

/**
 * @brief 用 NBVI 分数选择一组非连续子载波。
 *
 * 调用方法：校准帧数达到 CSI_ESPECTRE_CALIBRATION_FRAMES 后调用一次。
 * 算法先根据校准期 mean/std 计算 NBVI，再按分数从高到低选择最多
 * CSI_ESPECTRE_SELECTED_MAX 个相互间隔足够远的子载波。
 * @param state ESPectre-like 独立算法状态。
 */
static void csi_espectre_select_subcarriers(csi_espectre_like_state_t *state)
{
    if (state == NULL || state->norm_count == 0) {
        return;
    }

    uint16_t start = state->norm_count > (CSI_ESPECTRE_SUBCARRIER_SKIP_EDGE * 2) ?
                     CSI_ESPECTRE_SUBCARRIER_SKIP_EDGE : 0;
    uint16_t end = state->norm_count > (CSI_ESPECTRE_SUBCARRIER_SKIP_EDGE * 2) ?
                   (uint16_t)(state->norm_count - CSI_ESPECTRE_SUBCARRIER_SKIP_EDGE) :
                   state->norm_count;

    for (uint16_t i = start; i < end; i++) {
        float mean = state->sum[i] / (float)state->calibration_frames;
        float variance = (state->sum_square[i] / (float)state->calibration_frames) - (mean * mean);
        if (variance < 0.0f) {
            variance = 0.0f;
        }

        float stddev = sqrtf(variance);
        if (mean < CSI_ESPECTRE_MIN_MEAN) {
            state->nbvi[i] = 0.0f;
            continue;
        }

        // NBVI 只用于选择“校准期波动相对明显但均值不过低”的子载波。
        float classic = CSI_ESPECTRE_NBVI_ALPHA * (stddev / (mean * mean)) +
                        (1.0f - CSI_ESPECTRE_NBVI_ALPHA) * (stddev / mean);
        state->nbvi[i] = classic;
    }

    state->selected_count = 0;
    memset(state->selected_indices, 0, sizeof(state->selected_indices));

    while (state->selected_count < CSI_ESPECTRE_SELECTED_MAX) {
        float best_score = 0.0f;
        int best_index = -1;

        for (uint16_t i = start; i < end; i++) {
            if (csi_espectre_index_selected(i, state->selected_indices, state->selected_count)) {
                continue;
            }

            if (state->nbvi[i] <= best_score) {
                continue;
            }

            if (!csi_espectre_index_far_enough(i, state->selected_indices, state->selected_count)) {
                continue;
            }

            best_score = state->nbvi[i];
            best_index = (int)i;
        }

        if (best_index < 0) {
            break;
        }

        state->selected_indices[state->selected_count] = (uint16_t)best_index;
        state->selected_count++;
    }
}

/**
 * @brief 计算 turbulence 窗口的滑动方差。
 *
 * 调用方法：csi_espectre_like_process() 每帧更新 turbulence 窗口后调用。
 * @param state ESPectre-like 独立算法状态。
 * @return 当前窗口 turbulence 方差；窗口为空时返回 0。
 */
static float csi_espectre_window_variance(const csi_espectre_like_state_t *state)
{
    if (state == NULL || state->window_count == 0) {
        return 0.0f;
    }

    float sum = 0.0f;
    float sum_square = 0.0f;
    for (uint8_t i = 0; i < state->window_count; i++) {
        float value = state->turbulence_window[i];
        sum += value;
        sum_square += value * value;
    }

    float mean = sum / (float)state->window_count;
    float variance = (sum_square / (float)state->window_count) - (mean * mean);
    return variance > 0.0f ? variance : 0.0f;
}

/**
 * @brief 把当前帧 turbulence 写入环形窗口。
 *
 * 调用方法：csi_espectre_like_process() 每帧计算完 turbulence 后调用。
 * @param state ESPectre-like 独立算法状态。
 * @param turbulence 当前帧已选子载波的平均扰动。
 */
static void csi_espectre_push_turbulence(csi_espectre_like_state_t *state, float turbulence)
{
    if (state == NULL) {
        return;
    }

    state->turbulence_window[state->window_index] = turbulence;
    state->window_index = (uint8_t)((state->window_index + 1U) % CSI_ESPECTRE_WINDOW_SIZE);
    if (state->window_count < CSI_ESPECTRE_WINDOW_SIZE) {
        state->window_count++;
    }
}

/**
 * @brief 重置 ESPectre-like 独立算法状态。
 *
 * 调用方法：归一化幅值长度变化或外部需要重新校准时调用。
 */
void csi_espectre_like_reset(void)
{
    memset(&s_espectre, 0, sizeof(s_espectre));
}

/**
 * @brief 处理一帧归一化幅值，输出 ESPectre-like 独立算法结果。
 *
 * 调用方法：csi_processor.c 在共享 I/Q -> amp -> norm_amp 预处理完成后调用。
 * 本函数内部完成静止校准、NBVI 子载波选择、turbulence 计算、MVS 评分、
 * 自适应阈值和独立 motion/static 判断。
 *
 * @param norm_values 当前帧归一化幅值数组，来自 csi_processor.c 的共享预处理。
 * @param norm_count norm_values 中的有效点数。
 * @param result 输出：本帧 ESPectre-like 算法结果。
 * @return true 表示处理成功，false 表示输入无效。
 */
bool csi_espectre_like_process(const float *norm_values,
                               uint16_t norm_count,
                               csi_espectre_like_result_t *result)
{
    if (norm_values == NULL || result == NULL || norm_count == 0) {
        return false;
    }

    memset(result, 0, sizeof(*result));
    result->best_index = -1;
    result->state = CSI_STATE_UNKNOWN;

    uint16_t count = csi_espectre_limit_count(norm_count);
    if (count == 0) {
        return false;
    }

    if (s_espectre.norm_count != 0 && s_espectre.norm_count != count) {
        csi_espectre_like_reset();
    }
    s_espectre.norm_count = count;

    if (!s_espectre.calibrated) {
        // 校准阶段假设环境相对静止，只统计幅值分布，不输出 motion。
        for (uint16_t i = 0; i < count; i++) {
            float value = norm_values[i];
            s_espectre.sum[i] += value;
            s_espectre.sum_square[i] += value * value;
            s_espectre.baseline[i] = s_espectre.sum[i] / (float)(s_espectre.calibration_frames + 1U);
        }

        s_espectre.calibration_frames++;
        result->calibrating = true;
        result->calibrated = false;
        result->calibration_frames = s_espectre.calibration_frames;
        result->state = CSI_STATE_UNKNOWN;

        if (s_espectre.calibration_frames >= CSI_ESPECTRE_CALIBRATION_FRAMES) {
            csi_espectre_select_subcarriers(&s_espectre);
            s_espectre.calibrated = s_espectre.selected_count > 0;
            s_espectre.threshold = CSI_ESPECTRE_THRESHOLD_FLOOR;
            result->calibrating = false;
            result->calibrated = s_espectre.calibrated;
            result->selected_count = s_espectre.selected_count;
            memcpy(result->selected_indices,
                   s_espectre.selected_indices,
                   sizeof(result->selected_indices));
            result->threshold = s_espectre.threshold;
            result->state = s_espectre.calibrated ? CSI_STATE_STATIC : CSI_STATE_UNKNOWN;
        }

        return true;
    }

    // turbulence：选中子载波相对本算法独立 baseline 的平均绝对偏移。
    float turbulence_sum = 0.0f;
    float best_delta = 0.0f;
    int16_t best_index = -1;
    float best_nbvi = 0.0f;

    for (uint8_t i = 0; i < s_espectre.selected_count; i++) {
        uint16_t index = s_espectre.selected_indices[i];
        if (index >= count) {
            continue;
        }

        float delta = fabsf(norm_values[index] - s_espectre.baseline[index]);
        turbulence_sum += delta;
        if (delta > best_delta) {
            best_delta = delta;
            best_index = (int16_t)index;
            best_nbvi = s_espectre.nbvi[index];
        }
    }

    float turbulence = s_espectre.selected_count > 0 ?
                       turbulence_sum / (float)s_espectre.selected_count : 0.0f;
    csi_espectre_push_turbulence(&s_espectre, turbulence);
    float mvs_score = csi_espectre_window_variance(&s_espectre);
    float motion_score = turbulence + sqrtf(mvs_score) * CSI_ESPECTRE_MOTION_WEIGHT;

    // 校准完成后的第一个完整窗口用于自适应阈值；阈值不会低于固定下限。
    if (s_espectre.calibration_turbulence_mean <= 0.0f && s_espectre.window_count >= CSI_ESPECTRE_WINDOW_SIZE) {
        float sum = 0.0f;
        for (uint8_t i = 0; i < s_espectre.window_count; i++) {
            sum += s_espectre.turbulence_window[i];
        }
        s_espectre.calibration_turbulence_mean = sum / (float)s_espectre.window_count;
        s_espectre.threshold = fmaxf(CSI_ESPECTRE_THRESHOLD_FLOOR,
                                     s_espectre.calibration_turbulence_mean *
                                     CSI_ESPECTRE_THRESHOLD_MULTIPLIER);
    }

    csi_state_t state_id = motion_score >= s_espectre.threshold ?
                           CSI_STATE_MOTION : CSI_STATE_STATIC;

    // 只有本算法自己判断为 static 时，才慢速更新 ESPectre-like baseline。
    if (state_id == CSI_STATE_STATIC) {
        for (uint8_t i = 0; i < s_espectre.selected_count; i++) {
            uint16_t index = s_espectre.selected_indices[i];
            if (index < count) {
                s_espectre.baseline[index] =
                    s_espectre.baseline[index] * (1.0f - CSI_ESPECTRE_BASELINE_ALPHA) +
                    norm_values[index] * CSI_ESPECTRE_BASELINE_ALPHA;
            }
        }
    }

    result->calibrated = true;
    result->calibrating = false;
    result->calibration_frames = s_espectre.calibration_frames;
    result->selected_count = s_espectre.selected_count;
    result->best_index = best_index;
    result->nbvi_score = best_nbvi;
    result->turbulence = turbulence;
    result->mvs_score = mvs_score;
    result->threshold = s_espectre.threshold;
    result->motion_score = motion_score;
    result->state = state_id;
    memcpy(result->selected_indices,
           s_espectre.selected_indices,
           sizeof(result->selected_indices));

    return true;
}
