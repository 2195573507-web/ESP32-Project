#ifndef MIC_ASR_LLM_BRIDGE_H
#define MIC_ASR_LLM_BRIDGE_H

#include "esp_err.h"
#include "mic_asr_doubao.h"
#include "mic_llm_doubao.h"

/**
 * @file mic_asr_llm_bridge.h
 * @brief ASR final 文本到 LLM 阻塞请求任务的轻量桥接层。
 *
 * 本模块只做 ASR final -> FreeRTOS queue -> LLM task 的连接：
 * 1. ASR 模块仍然只负责录音、VAD、WebSocket 和 final 文本产出。
 * 2. LLM 模块仍然只负责 OpenAI-compatible chat/completions 请求。
 * 3. 桥接层用独立 task 调用阻塞式 mic_llm_doubao_chat_text()，避免阻塞音频采集、
 *    VAD 和 ASR WebSocket 发包路径。
 */

/* 桥接队列参数：深度保持很小，LLM 忙时直接丢弃新 final，避免语音文本堆积。 */
#define MIC_ASR_LLM_BRIDGE_QUEUE_DEPTH       1    // 最小闭环只保留 1 条待处理 ASR final。
#define MIC_ASR_LLM_BRIDGE_TASK_STACK_SIZE   12288 // LLM HTTPS 阻塞请求任务栈，ESP-IDF 单位为字节。
#define MIC_ASR_LLM_BRIDGE_TASK_PRIORITY     3    // 低于 Mic ADC 采集任务，避免抢占音频路径。

/* 文本缓冲参数：prompt 使用 ASR result.text 长度，response 使用 Mic LLM 建议回复长度。 */
#define MIC_ASR_LLM_BRIDGE_PROMPT_MAX_BYTES  MIC_ASR_DOUBAO_RESULT_TEXT_MAX_LEN
#define MIC_ASR_LLM_BRIDGE_RESPONSE_BYTES    MIC_LLM_DOUBAO_RESPONSE_MAX_BYTES

#if MIC_ASR_LLM_BRIDGE_QUEUE_DEPTH <= 0
#error "MIC_ASR_LLM_BRIDGE_QUEUE_DEPTH must be greater than 0"
#endif

#if MIC_ASR_LLM_BRIDGE_TASK_STACK_SIZE < 4096
#error "MIC_ASR_LLM_BRIDGE_TASK_STACK_SIZE must be at least 4096"
#endif

#if MIC_ASR_LLM_BRIDGE_TASK_PRIORITY <= 0
#error "MIC_ASR_LLM_BRIDGE_TASK_PRIORITY must be greater than 0"
#endif

#if MIC_ASR_LLM_BRIDGE_PROMPT_MAX_BYTES <= 1
#error "MIC_ASR_LLM_BRIDGE_PROMPT_MAX_BYTES must be greater than 1"
#endif

#if MIC_ASR_LLM_BRIDGE_RESPONSE_BYTES <= 1
#error "MIC_ASR_LLM_BRIDGE_RESPONSE_BYTES must be greater than 1"
#endif

/**
 * @brief 启动 ASR -> LLM 桥接任务。
 *
 * 调用方法：WiFi 稳定且 mic_llm_doubao_init() 成功后调用一次。重复调用会直接返回
 * ESP_OK。函数只创建 queue/task，不初始化 WiFi，不启动 ASR，也不接入 TTS/audio_player。
 *
 * @return 成功返回 ESP_OK；队列或任务创建失败时返回错误码。
 */
esp_err_t mic_asr_llm_bridge_start(void);

#endif // MIC_ASR_LLM_BRIDGE_H
