#ifndef CSI_PROCESSOR_H
#define CSI_PROCESSOR_H

#include <stdbool.h>
#include <stdint.h>

#include "csi_espectre_like.h"
#include "csi_state.h"
#include "csi_subband_analysis.h"

#define CSI_ENABLE_SUBBAND_ANALYSIS 1

typedef enum {
    CSI_DECISION_GLOBAL_NORM = 0,
    CSI_DECISION_SUBBAND_NORM = 1,
    CSI_DECISION_FUSION = 2,
} csi_decision_mode_t;

#ifndef CSI_DECISION_MODE
#define CSI_DECISION_MODE CSI_DECISION_GLOBAL_NORM
#endif

/*
 * CSI 运动检测调试参数
 *
 * 当前目标是判断“是否有明显运动”，不是稳定区分“无人”和“有人静坐”。
 * 调参建议：
 * 1. 先记录无人静止 3~5 分钟，观察 motion_score_max，ON 阈值应明显高于无人最大值。
 * 2. 再记录有人走动，观察 motion_score_min/max；如果弱运动漏检，优先降低 CSI_MOTION_ON_THRESHOLD。
 * 3. 如果 motion 区间断断续续，优先降低 CSI_MOTION_OFF_THRESHOLD 或增大 CSI_STILL_FRAMES / CSI_MOTION_HOLD_MS。
 * 4. 如果 motion 结束后 baseline_delta 长时间降不下来，优先调大 CSI_BASELINE_ALPHA_RECOVER。
 * 5. 如果静止时 baseline 把弱运动“学进去”，调低 CSI_STATIC_UPDATE_ACTIVITY_MAX 或 CSI_STATIC_UPDATE_DELTA_MAX。
 * 6. baseline_delta 不再做 EMA 平滑；baseline 只在 static/recover 且 update_blocked 为 false 时更新。
 */
// smooth_motion_score 算法：EMA(instant_motion_score)，alpha 越大越跟手，越小越稳。
#define CSI_MOTION_EMA_ALPHA 0.08f
// static baseline 更新算法：baseline = baseline * (1 - alpha) + current * alpha。
#define CSI_BASELINE_ALPHA_STATIC 0.003f
// recover baseline 更新算法：普通 RECOVER 用更大的 alpha 追上当前稳定环境。
#define CSI_BASELINE_ALPHA_RECOVER 0.020f
// motion 进入算法：motion_score 连续 CSI_MOTION_ON_FRAMES 帧超过该阈值，static -> motion。
#define CSI_MOTION_ON_THRESHOLD 0.055f
// motion 退出算法：activity_score 低于该阈值时累计 still_count，满足后 motion -> recover。
#define CSI_MOTION_OFF_THRESHOLD 0.025f
// 防抖算法：进入 motion 需要连续超过阈值，避免单帧尖峰误触发。
#define CSI_MOTION_ON_FRAMES 8
// 退出防抖算法：motion 中连续低活动帧达到该数量，才允许进入 recover。
#define CSI_STILL_FRAMES 80
// motion 保持算法：进入 motion 后至少保持一段时间，避免动作低谷把状态切碎。
#define CSI_MOTION_HOLD_MS 2000
// recover 更新算法：RECOVER 中 activity_score 低于该值时，认为相邻帧稳定，可以快速更新 baseline。
#define CSI_RECOVER_ACTIVITY_THRESHOLD 0.020f
// recover 完成算法：baseline_delta 降到该值以下，认为 baseline 已追上当前环境，RECOVER -> STATIC。
#define CSI_RECOVER_TARGET_DELTA 0.030f
// recover 兜底算法：即使 baseline_delta 没降到目标，超过最大时间也回 STATIC，避免卡住。
#define CSI_RECOVER_MAX_MS 10000
// static baseline 更新保护：static 中 activity_score 必须低于该值，才允许慢速学习 baseline。
#define CSI_STATIC_UPDATE_ACTIVITY_MAX 0.016f
// static baseline 更新保护：baseline_delta 太高时先不慢速学习，避免把明显偏移误学进去。
#define CSI_STATIC_UPDATE_DELTA_MAX 0.035f
// 环境漂移自纠偏算法：稳定偏移需要连续满足 N 帧，才认为高 baseline_delta 是新环境而不是运动。
#define CSI_STABLE_OFFSET_FRAMES 20
// 环境漂移自纠偏条件：短时活动必须低，说明当前没有持续运动。
#define CSI_STABLE_OFFSET_ACTIVITY_MAX 0.018f
// 环境漂移自纠偏条件：相邻帧 delta_norm 必须低，说明图形已经稳定。
#define CSI_STABLE_OFFSET_DELTA_NORM_MAX 0.018f
// 环境漂移自纠偏条件：baseline_delta 必须高，说明当前稳定形状明显偏离旧 baseline。
#define CSI_STABLE_OFFSET_DELTA_MIN 0.040f
// 环境漂移自纠偏更新算法：稳定高 baseline_delta 时，用该 alpha 快速把 baseline 拉向当前帧。
#define CSI_BASELINE_ALPHA_STABLE_OFFSET 0.050f
// motion_score 算法：固定权重融合 activity_score 和 baseline_delta。
#define CSI_BASELINE_MOTION_WEIGHT 0.18f

// 冻结判断参数：连续多帧特征几乎完全不变，说明 CSI 数据可能没有正常刷新。
#define CSI_FREEZE_FRAME_THRESHOLD 20
// data_frozen 触发条件：相邻帧各项变化都小于该 epsilon，才累计 freeze_count。
#define CSI_FREEZE_EPSILON 0.0002f

/**
 * @brief 单帧 CSI 算法处理结果。
 *
 * global_norm、subband_norm 和 ESPectre-like 三套算法相互独立，只共享本帧
 * 归一化幅值输入。fusion 仅保留为兼容旧字段的派生展示值，不作为第三套算法。
 */
typedef struct {
    uint16_t valid_points;          // 本帧参与计算的有效 I/Q 点数量。
    float mean_amp;                 // 有效子载波幅值均值，用于观察整体接收强度。
    float var_amp;                  // 有效子载波幅值方差，用于观察本帧幅值离散程度。
    float delta_norm;               // 当前归一化子载波形状与上一帧的平均差异。
    float baseline_delta;           // 当前归一化子载波形状与静止 baseline 的平均差异。
    float smooth_mean_amp;          // mean_amp 的 EMA 平滑值。
    float motion_score;             // global_norm 最终运动评分。
    float smooth_motion_score;      // global_norm 短时运动分数的 EMA 平滑值。
    float window_motion_score;      // 兼容旧输出字段，当前固定为 0。
    float amp_motion_score;         // 兼容旧输出字段，当前固定为 0。
    uint8_t freeze_count;           // 连续“几乎完全不变”的帧计数，用于判断数据冻结。
    bool frozen;                    // true 表示 CSI 特征疑似未刷新，当前判断不可信。
    bool recovering;                // true 表示 global_norm baseline 正在自适应恢复。
    bool motion;                    // true 表示 global_norm 判断为有明显运动。

    csi_state_t state;              // 对外最终状态，等同于 decision_state_id。
    float global_norm_score;        // 原有全局归一化算法得分。
    float subband_norm_score;       // 子载波多频段算法整体得分。
    float fusion_score;             // 兼容旧输出字段：global/subband 派生融合分。
    float decision_score;           // 当前 CSI_DECISION_MODE 实际采用的最终得分。
    uint8_t decision_mode;          // 当前决策模式：global/subband/fusion。
    csi_state_t global_state_id;    // 原有全局归一化算法状态。
    csi_state_t subband_state_id;   // 多频段算法整体状态。
    csi_state_t fusion_state_id;    // 兼容旧输出字段：fusion 派生状态。
    csi_state_t decision_state_id;  // 当前最终对外状态，网页顶部人体状态使用该字段。
    csi_espectre_like_result_t espectre; // ESPectre-like 独立算法结果。
#if CSI_ENABLE_SUBBAND_ANALYSIS
    csi_subband_analysis_result_t subband; // 多频段分析结果，仅用于本地判断后的展示和对比。
#endif
} csi_processor_result_t;

/**
 * @brief 处理一帧原始 CSI I/Q 数据，输出归一化后的运动特征。
 *
 * 输入数据来自 ESP32-C5 CSI 回调中的 info->buf。本处理器不依赖官方增益补偿；
 * global_norm、subband_norm 和 ESPectre-like 三套算法只共享归一化幅值输入。
 */
bool csi_processor_process(const int8_t *data,
                           uint16_t len,
                           bool first_word_invalid,
                           csi_processor_result_t *result);

#endif // CSI_PROCESSOR_H
