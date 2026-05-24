#ifndef CSI_ESPECTRE_LIKE_H
#define CSI_ESPECTRE_LIKE_H

#include <stdbool.h>
#include <stdint.h>

#include "csi_state.h"

#define CSI_ESPECTRE_SELECTED_MAX 12

/**
 * @brief ESPectre-like 幅值域算法结果。
 *
 * 本模块参考 ESPectre 的 NBVI / turbulence / MVS 思路，但不依赖官方增益算法，
 * 也不复制 ESPectre GPL 代码。算法只读取当前帧归一化幅值数组，内部维护独立
 * calibration、选中子载波和运动窗口状态。
 */
typedef struct {
    bool calibrated;                 // true 表示 NBVI 子载波选择已完成。
    bool calibrating;                // true 表示仍在静止校准阶段。
    uint16_t calibration_frames;     // 已累计的校准帧数。
    uint8_t selected_count;          // 当前选中的子载波数量。
    int16_t best_index;              // 当前帧变化最大的已选子载波索引；无效为 -1。
    float nbvi_score;                // best_index 对应的 NBVI 分数。
    float turbulence;                // 已选子载波相对独立 baseline 的平均扰动。
    float mvs_score;                 // turbulence 滑动窗口方差。
    float threshold;                 // 当前 motion 判定阈值。
    float motion_score;              // turbulence + sqrt(MVS) 的组合分数。
    csi_state_t state;               // 本算法独立判断出的状态。
    uint16_t selected_indices[CSI_ESPECTRE_SELECTED_MAX]; // NBVI 选出的子载波索引。
} csi_espectre_like_result_t;

/**
 * @brief 重置 ESPectre-like 独立算法状态。
 *
 * 调用方法：需要重新静止校准时调用；当前主流程会在输入点数变化时自动调用。
 */
void csi_espectre_like_reset(void);

/**
 * @brief 处理一帧归一化幅值，输出 ESPectre-like 独立算法结果。
 *
 * 调用方法：csi_processor.c 完成共享 I/Q -> amp -> norm_amp 预处理后调用。
 * @param norm_values 当前帧归一化幅值数组。
 * @param norm_count norm_values 中的有效点数。
 * @param result 输出：本帧 ESPectre-like 算法结果。
 * @return true 表示处理成功，false 表示输入无效。
 */
bool csi_espectre_like_process(const float *norm_values,
                               uint16_t norm_count,
                               csi_espectre_like_result_t *result);

#endif // CSI_ESPECTRE_LIKE_H
