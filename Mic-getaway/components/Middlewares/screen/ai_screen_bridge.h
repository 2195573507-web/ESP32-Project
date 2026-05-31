#ifndef AI_SCREEN_BRIDGE_H
#define AI_SCREEN_BRIDGE_H

#include "esp_err.h"

/**
 * @file ai_screen_bridge.h
 * @brief Screen command 执行桥接层。
 *
 * 调用方法：Screen 不直接请求模型，只执行上层传来的显示命令。当前底层屏幕未接入时
 * 先打印日志，保证后续 LLM command 能有稳定接口。
 */

typedef enum {
    AI_SCREEN_CMD_CLEAR = 0,      // 清屏。
    AI_SCREEN_CMD_SHOW_TEXT,      // 显示普通文本。
    AI_SCREEN_CMD_SHOW_STATUS,    // 显示系统状态。
    AI_SCREEN_CMD_SHOW_SENSOR,    // 显示传感器摘要。
    AI_SCREEN_CMD_SHOW_ASR_TEXT,  // 显示 ASR 文本。
} ai_screen_cmd_type_t;

typedef struct {
    ai_screen_cmd_type_t type; // 命令类型。
    const char *title;         // 标题，可为空。
    const char *text;          // 文本，可为空。
    int timeout_ms;            // 显示超时，<=0 表示由底层决定。
} ai_screen_command_t;

esp_err_t ai_screen_bridge_init(void);
esp_err_t ai_screen_bridge_execute(const ai_screen_command_t *cmd);
esp_err_t ai_screen_bridge_show_text(const char *title,
                                     const char *text,
                                     int timeout_ms);
esp_err_t ai_screen_bridge_show_asr_text(const char *text);
esp_err_t ai_screen_bridge_clear(void);

#endif // AI_SCREEN_BRIDGE_H
