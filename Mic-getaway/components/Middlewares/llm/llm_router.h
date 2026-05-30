#ifndef LLM_ROUTER_H
#define LLM_ROUTER_H

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

/**
 * @file llm_router.h
 * @brief LLM final JSON 的 command/speech 路由层。
 *
 * 调用方法：llm_client 收到 LLM final 文本后调用本模块解析 JSON，并把 command
 * 输出到串口/日志或后续设备控制入口。当前阶段不触发 TTS 播放。
 */

typedef enum {
    LLM_COMMAND_NONE = 0,             // 非 command 或无命令。
    LLM_COMMAND_SCREEN_ON,            // 屏幕打开。
    LLM_COMMAND_SCREEN_OFF,           // 屏幕关闭。
    LLM_COMMAND_SCREEN_SET_BRIGHTNESS, // 设置屏幕亮度。
    LLM_COMMAND_SCREEN_SHOW_PAGE,     // 切换屏幕页面。
    LLM_COMMAND_SENSOR_READ_BME690,   // 查询 BME690。
    LLM_COMMAND_SENSOR_READ_CSI,      // 查询 CSI。
    LLM_COMMAND_AUDIO_STOP,           // 停止当前音频/语音会话。
    LLM_COMMAND_SYSTEM_STATUS,        // 查询系统状态。
    LLM_COMMAND_UNKNOWN,              // 未识别命令。
} llm_command_type_t;

typedef struct {
    bool valid;                         // JSON 和必需字段是否有效。
    bool speak;                         // 是否建议后续播报，当前 TTS 禁用。
    llm_command_type_t command_type;    // 归一化后的命令类型。
    char type[16];                      // 原始 type 字段：command 或 speech。
    char command[64];                   // 原始 command 字段。
    char reply[256];                    // 原始 reply 字段。
    cJSON *params;                      // params 深拷贝，使用后需 cleanup。
} llm_router_result_t;

/**
 * @brief 解析 LLM final 文本为 router 结果。
 *
 * @param text LLM final 文本，必须是 JSON。
 * @param out 输出 router 结果，不能为空。
 * @return 成功返回 ESP_OK；JSON 无效或必需字段缺失时返回错误码。
 */
esp_err_t llm_router_parse_final_text(const char *text, llm_router_result_t *out);

/**
 * @brief 处理已解析的 router 结果。
 *
 * 调用方法：当前阶段只打印 command/speech 和必要状态，不触发 TTS 播放。
 *
 * @param result 已解析结果，不能为空。
 * @return 成功返回 ESP_OK；参数错误或未知处理失败时返回错误码。
 */
esp_err_t llm_router_handle_result(const llm_router_result_t *result);

/**
 * @brief 清理 router 结果内部资源。
 *
 * @param result 待清理结果，可为空。
 */
void llm_router_result_cleanup(llm_router_result_t *result);

#endif // LLM_ROUTER_H
