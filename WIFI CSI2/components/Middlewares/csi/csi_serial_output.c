#include "csi_serial_output.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "wifi_manager.h"

/**
 * @brief 串口输出任务的运行上下文。
 *
 * 调用方法：csi_serial_output_start() 保存回调，wifi_csi_serial_output_task() 周期性读取。
 */
typedef struct {
    csi_serial_snapshot_fn snapshot_fn;      // 读取最新 CSI 特征快照的回调。
    csi_serial_link_info_fn link_info_fn;    // 读取链路信息的回调，用于打印头信息。
    void *ctx;                               // 传递给回调函数的用户上下文。
} csi_serial_output_ctx_t;

static csi_serial_output_ctx_t s_serial_output = {0};

/**
 * @brief 把 CSI 处理状态转换为串口文本。
 *
 * 调用方法：csi_print_feature_line() 输出 CSI_FEATURE_RAW 时调用。
 * 优先显示冻结保护状态，其次显示运动状态机结果。
 * @param processed 单帧 CSI 处理结果。
 * @return 状态文本：gain_freeze/data_frozen/motion/recover/static/unknown。
 */
/**
 * @brief 输出一行 CSI_FEATURE_RAW。
 *
 * 调用方法：wifi_csi_serial_output_task() 每收到新帧后调用一次。
 * 这一行给网页绘图使用，包含 RSSI、CSI 特征、增益状态和 motion_state。
 * @param label 串口行标签，当前固定为 CSI_FEATURE_RAW。
 * @param feature 单帧 CSI 特征快照。
 * @param processed feature 中的 RAW 主算法处理结果。
 */
static void csi_print_feature_line(const char *label,
                                   const csi_feature_t *feature,
                                   const csi_processor_result_t *processed)
{
    /*
     * CSI_FEATURE_RAW 兼容规则：
     * 前 20 个字段不插入新字段，只把第 20 字段从旧 motion_state 字符串改为 decision_state_id 整数。
     * 状态统一使用整数 ID，网页/Python/LCD 再负责映射成文字。
     * CSI_ENABLE_SUBBAND_ANALYSIS=1 且 CSI_PRINT_SUBBAND_DATA=1 时追加扩展字段，总字段数为 47。
     */
    printf("%s,%lu,%d,%u,%u,%u,%.2f,%.2f,%.4f,%.4f,%.2f,%.4f,%.4f,%.4f,%u,%u,%d,%.4f,%u,%u",
           label,
           (unsigned long)feature->frame_count,
           feature->rssi,
           feature->channel,
           feature->csi_len,
           processed->valid_points,
           processed->mean_amp,
           processed->var_amp,
           processed->delta_norm,
           processed->baseline_delta,
           processed->smooth_mean_amp,
           processed->motion_score,
           processed->smooth_motion_score,
           processed->window_motion_score,
           processed->freeze_count,
           feature->gain.agc_gain,
           feature->gain.fft_gain,
           feature->gain.compensate_gain,
           feature->gain.compensated ? 1U : 0U,
           (unsigned)processed->decision_state_id);
#if CSI_ENABLE_SUBBAND_ANALYSIS && CSI_PRINT_SUBBAND_DATA
    printf(",%u,%.4f,%u,%.4f,%u,%.4f,%u,%.4f,%u,%u,%d",
           processed->decision_mode,
           processed->global_norm_score,
           (unsigned)processed->global_state_id,
           processed->subband_norm_score,
           (unsigned)processed->subband_state_id,
           processed->fusion_score,
           (unsigned)processed->fusion_state_id,
           processed->decision_score,
           (unsigned)processed->decision_state_id,
           processed->subband.subband_count,
           processed->subband.best_band);
    for (uint8_t i = 0; i < CSI_SUBBAND_COUNT; i++) {
        const csi_subband_result_t *band = &processed->subband.bands[i];
        printf(",%.4f,%.4f,%.4f,%u",
               band->delta_norm,
               band->baseline_delta,
               band->motion_score,
               (unsigned)band->state);
    }
#endif
    printf("\n");
}

/**
 * @brief 输出一行 CSI_DBG。
 *
 * 调用方法：wifi_csi_serial_output_task() 每收到新帧后调用一次。
 * 这一行主要用于观察 AGC/FFT/compensate_gain 和运动评分之间的关系。
 * @param feature 单帧 CSI 特征快照。
 */
static void csi_print_debug_line(const csi_feature_t *feature)
{
    const csi_processor_result_t *processed = &feature->raw_processed;

    printf("CSI_DBG,%lu,%u,%d,%.3f,%.5f,%.5f,%.5f,%.5f\n",
           (unsigned long)feature->frame_count,
           feature->gain.agc_gain,
           feature->gain.fft_gain,
           feature->gain.compensate_gain,
           processed->delta_norm,
           processed->motion_score,
           processed->baseline_delta,
           processed->amp_motion_score);
}

/**
 * @brief 输出串口数据头和算法说明。
 *
 * 调用方法：串口输出任务启动时调用一次。
 * CSI_LINK 描述链路过滤条件，CSI_PROCESS_METHOD 描述当前算法公式，
 * 后续 HEADER 行描述每类串口数据的字段顺序。
 * @param link_info 当前 CSI 链路信息。
 */
static void csi_print_headers(const csi_link_info_t *link_info)
{
    char connected_ssid[33] = {0};
    const char *ssid_text = wifi_get_connected_ssid(connected_ssid, sizeof(connected_ssid)) ? connected_ssid : "unknown";

    printf("CSI_LINK,receiver=ESP32-C5,wifi_ssid=%s,link=router_to_esp32c5,filter=src_ap_bssid_and_dst_sta_mac,traffic=internal_ping_gateway_reply,ap_bssid=%02x:%02x:%02x:%02x:%02x:%02x,sta_mac=%02x:%02x:%02x:%02x:%02x:%02x,gateway=" IPSTR ",note=CSI_is_measured_by_ESP32-C5_receiver_not_exported_by_router\n",
           ssid_text,
           link_info->ap_bssid[0],
           link_info->ap_bssid[1],
           link_info->ap_bssid[2],
           link_info->ap_bssid[3],
           link_info->ap_bssid[4],
           link_info->ap_bssid[5],
           link_info->sta_mac[0],
           link_info->sta_mac[1],
           link_info->sta_mac[2],
           link_info->sta_mac[3],
           link_info->sta_mac[4],
           link_info->sta_mac[5],
           IP2STR(&link_info->gateway_ip));
    printf("CSI_PROCESS_METHOD,raw_data=ESP32-C5 RX CSI I/Q int8 byte stream used locally,raw_uart=%s,gain_ctrl=official_gain_used_as_freeze_only,no_int8_compensated_csi,feature_data=amp=sqrt(i*i+q*q),norm_amp=amp/mean_amp,delta_norm=mean_abs(norm_now-norm_last),baseline_delta=mean_abs(norm_now-static_baseline),baseline_delta_smoothing=off,instant=0.70*delta_norm+0.30*baseline_delta_change,activity=0.65*instant+0.35*smooth_motion,motion_score=0.82*activity+0.18*baseline_delta,state=numeric_id,subband_analysis=%s,decision_mode=%s,band_state=esp32c5_local_numeric_id,recover=baseline_adaptive_recovery,no_boot_calibration,baseline_update=blocked_by_update_blocked_or_fast_when_stable_offset,gain_freeze=agc_or_fft_large_jump_freezes_state_and_baseline_for_3_frames,compensate_gain_does_not_trigger_motion_score,feature_lines=CSI_FEATURE_RAW,each_frame=%s\n",
           CSI_PRINT_RAW_DATA ? "on" : "off",
           CSI_ENABLE_SUBBAND_ANALYSIS ? "on" : "off",
           CSI_DECISION_MODE == CSI_DECISION_SUBBAND_NORM ? "subband" :
           (CSI_DECISION_MODE == CSI_DECISION_FUSION ? "fusion" : "global"),
           CSI_PRINT_RAW_DATA ? "CSI_FRAME_BEGIN+CSI_DATA+CSI_FEATURE_RAW+CSI_DBG+CSI_FRAME_END" : "CSI_FRAME_BEGIN+CSI_FEATURE_RAW+CSI_DBG+CSI_FRAME_END");
    printf("CSI_FRAME_HEADER,type,frame\n");
#if CSI_PRINT_RAW_DATA
    printf("CSI_DATA_HEADER,type,frame,src_mac,dst_mac,link,rssi,rate,sig_mode,mcs,cwb,channel,len,first_word_invalid,data\n");
#endif
#if CSI_ENABLE_SUBBAND_ANALYSIS && CSI_PRINT_SUBBAND_DATA
    printf("CSI_FEATURE_RAW_HEADER,type,frame,rssi,channel,csi_len,valid_points,mean_amp,var_amp,delta_norm,baseline_delta,smooth_mean_amp,motion_score,smooth_motion_score,window_score,freeze_count,agc_gain,fft_gain,compensate_gain,gain_compensated,decision_state_id,decision_mode,global_norm_score,global_state_id,subband_norm_score,subband_state_id,fusion_score,fusion_state_id,decision_score,decision_state_id,subband_count,best_band,band0_delta,band0_base,band0_score,band0_state_id,band1_delta,band1_base,band1_score,band1_state_id,band2_delta,band2_base,band2_score,band2_state_id,band3_delta,band3_base,band3_score,band3_state_id\n");
#else
    printf("CSI_FEATURE_RAW_HEADER,type,frame,rssi,channel,csi_len,valid_points,mean_amp,var_amp,delta_norm,baseline_delta,smooth_mean_amp,motion_score,smooth_motion_score,window_score,freeze_count,agc_gain,fft_gain,compensate_gain,gain_compensated,decision_state_id\n");
#endif
    printf("CSI_DBG_HEADER,type,frame,agc_gain,fft_gain,compensate_gain,delta_norm,motion_score,baseline_delta,amp_motion\n");
}

/**
 * @brief CSI 串口输出 FreeRTOS 任务。
 *
 * 调用方法：csi_serial_output_start() 创建本任务。
 * 任务按 CSI_FEATURE_PRINT_INTERVAL_MS 周期读取最新快照，只在帧计数变化时输出。
 * @param arg FreeRTOS 任务参数，本模块未使用。
 */
static void wifi_csi_serial_output_task(void *arg)
{
    (void)arg;

    uint32_t last_printed_count = 0;
    csi_link_info_t link_info = {0};

    if (s_serial_output.link_info_fn != NULL &&
        s_serial_output.link_info_fn(&link_info, s_serial_output.ctx)) {
        csi_print_headers(&link_info);
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CSI_FEATURE_PRINT_INTERVAL_MS));

        csi_feature_t feature;
        uint32_t current_count;

        if (s_serial_output.snapshot_fn == NULL ||
            !s_serial_output.snapshot_fn(&feature, &current_count, s_serial_output.ctx)) {
            continue;
        }

        if (current_count == 0 || current_count == last_printed_count) {
            continue;
        }

        last_printed_count = current_count;

        printf("CSI_FRAME_BEGIN,%lu\n", (unsigned long)feature.frame_count);
#if CSI_PRINT_RAW_DATA
        printf("CSI_DATA,frame=%lu,src_mac=%02x:%02x:%02x:%02x:%02x:%02x,dst_mac=%02x:%02x:%02x:%02x:%02x:%02x,link=router_to_esp32c5,rssi=%d,rate=%u,sig_mode=%u,mcs=%u,cwb=%u,channel=%u,len=%u,first_word_invalid=%u,data=[",
               (unsigned long)feature.frame_count,
               feature.mac[0],
               feature.mac[1],
               feature.mac[2],
               feature.mac[3],
               feature.mac[4],
               feature.mac[5],
               feature.dmac[0],
               feature.dmac[1],
               feature.dmac[2],
               feature.dmac[3],
               feature.dmac[4],
               feature.dmac[5],
               feature.rssi,
               0U,
               feature.sig_mode,
               feature.mcs,
               feature.cwb,
               feature.channel,
               feature.raw_len,
               feature.first_word_invalid);

        for (uint16_t i = 0; i < feature.raw_len; i++) {
            printf("%s%d", i == 0 ? "" : " ", feature.raw_buf[i]);
        }
        printf("]\n");
#endif

#if CSI_PRINT_FEATURE_RAW
        csi_print_feature_line("CSI_FEATURE_RAW", &feature, &feature.raw_processed);
#endif
#if CSI_PRINT_DEBUG
        csi_print_debug_line(&feature);
#endif

        printf("CSI_FRAME_END,%lu\n", (unsigned long)feature.frame_count);
    }
}

/**
 * @brief 启动 CSI 串口输出任务。
 *
 * 调用方法：wifi_csi_monitor_start() 在需要串口输出时调用。
 * @param snapshot_fn 读取最新 CSI 特征快照的回调，不能为空。
 * @param link_info_fn 读取链路信息的回调，可为 NULL。
 * @param ctx 传给回调函数的用户上下文。
 * @return ESP_OK 表示启动成功，ESP_ERR_INVALID_ARG 表示参数错误，ESP_ERR_NO_MEM 表示任务创建失败。
 */
esp_err_t csi_serial_output_start(csi_serial_snapshot_fn snapshot_fn,
                                  csi_serial_link_info_fn link_info_fn,
                                  void *ctx)
{
    if (snapshot_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_serial_output.snapshot_fn = snapshot_fn;
    s_serial_output.link_info_fn = link_info_fn;
    s_serial_output.ctx = ctx;

    if (xTaskCreate(wifi_csi_serial_output_task, "wifi_csi_serial", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
