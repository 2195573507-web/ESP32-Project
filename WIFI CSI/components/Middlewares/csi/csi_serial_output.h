#ifndef CSI_SERIAL_OUTPUT_H
#define CSI_SERIAL_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "csi_feature.h"
#include "esp_err.h"

/**
 * @brief 读取最新 CSI 特征快照的回调函数类型。
 *
 * 调用方法：串口输出任务每个打印周期调用一次，由 csi_monitor.c 提供实现。
 * @param feature 输出：最近一帧已经处理完成的 CSI 特征。
 * @param count 输出：已接收的有效 CSI 帧计数，用于判断是否有新帧。
 * @param ctx 用户上下文，来自 csi_serial_output_start() 的 ctx 参数。
 * @return true 表示读取成功，false 表示当前没有可用快照。
 */
typedef bool (*csi_serial_snapshot_fn)(csi_feature_t *feature, uint32_t *count, void *ctx);

/**
 * @brief 读取 CSI 链路信息的回调函数类型。
 *
 * 调用方法：串口输出任务启动时调用一次，用于打印 CSI_LINK 头信息。
 * @param link_info 输出：AP BSSID、本机 STA MAC 和网关 IP。
 * @param ctx 用户上下文，来自 csi_serial_output_start() 的 ctx 参数。
 * @return true 表示读取成功，false 表示链路信息尚未准备好。
 */
typedef bool (*csi_serial_link_info_fn)(csi_link_info_t *link_info, void *ctx);

/**
 * @brief 启动 CSI 串口输出任务。
 *
 * 调用方法：wifi_csi_monitor_start() 在 CSI 回调注册完成后调用。
 * 后续如果不需要串口输出，可在 csi_monitor.h 中关闭 CSI_SERIAL_OUTPUT_ENABLE。
 * @param snapshot_fn 读取最新 CSI 特征快照的回调，不能为空。
 * @param link_info_fn 读取链路信息的回调，可为 NULL；为 NULL 时不打印 CSI_LINK 头。
 * @param ctx 传给回调函数的用户上下文。
 * @return ESP_OK 表示任务创建成功，其他值表示参数错误或内存不足。
 */
esp_err_t csi_serial_output_start(csi_serial_snapshot_fn snapshot_fn,
                                  csi_serial_link_info_fn link_info_fn,
                                  void *ctx);

#endif // CSI_SERIAL_OUTPUT_H
