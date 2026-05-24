#ifndef CSI_STATE_H
#define CSI_STATE_H

/*
 * 公共 CSI 状态 ID。
 *
 * 注意：这些数字值已经作为串口协议的一部分，网页、Python 绘图、LCD 或云端都会按
 * 同一套数字解析。后续不要修改已有数字，只能在枚举末尾追加新状态。
 */
typedef enum {
    CSI_STATE_UNKNOWN = 0,     // 未知或无效状态。
    CSI_STATE_STATIC = 1,      // 稳定静止。
    CSI_STATE_ACTIVE = 2,      // 轻微扰动，主要用于频段状态。
    CSI_STATE_MOTION = 3,      // 检测到明显运动。
    CSI_STATE_OFFSET = 4,      // 相对 baseline 偏移较大，但相邻帧变化较小。
    CSI_STATE_RESERVED_5 = 5,   // 历史保留状态 ID，当前官方增益算法已移除，不再主动输出。
    CSI_STATE_DATA_FROZEN = 6, // CSI 数据疑似冻结。
    CSI_STATE_RECOVER = 7,     // baseline 正在恢复。
} csi_state_t;

#endif // CSI_STATE_H
