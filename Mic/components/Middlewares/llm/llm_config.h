#ifndef LLM_CONFIG_H
#define LLM_CONFIG_H

/**
 * @file llm_config.h
 * @brief LLM OpenAI-compatible 云端地址、API Key 和模型等环境配置。
 *
 * 调用方法：
 * 1. 需要替换 LLM API Key、请求地址或模型时，只改本文件。
 * 2. llm_client.h 只保留通用客户端接口；mic_llm_doubao.h 只保留 Mic 适配接口。
 * 3. 本文件只属于 LLM；豆包 ASR 的 MIC_ASR_DOUBAO_* 配置仍独立保留在 ASR 模块。
 * 4. 默认 API Key 为占位配置，实机联网调用前必须替换。
 */

/* OpenAI-compatible LLM 配置：通用 llm_client 和 Mic 适配层统一使用这一组宏。 */
#define OPENAI_API_KEY              "apikey-20260414133629-p8mkl"              // OpenAI-compatible API Key，实机使用前替换。
#define OPENAI_BASE_URL             "https://ai-gateway.vei.volces.com/v1/"    // OpenAI-compatible Base URL，必须以 / 结尾。
#define OPENAI_MODEL_NAME           "doubao-seed-1.6-thinking"                 // Chat Completion 模型名。
#define OPENAI_SYSTEM_PROMPT        "You are a helpful assistant."             // 默认 system prompt。
#define OPENAI_USER_ID              "esp32_client"                             // user 字段，用于服务端侧区分客户端。

/* 派生请求地址：OPENAI_BASE_URL 保持和 OpenAI 库习惯一致，这里拼出实际 chat/completions URL。 */
#define OPENAI_CHAT_COMPLETIONS_PATH "chat/completions"
#define OPENAI_CHAT_COMPLETIONS_URL  OPENAI_BASE_URL OPENAI_CHAT_COMPLETIONS_PATH
#define OPENAI_PLACEHOLDER_MARKER    "REPLACE_WITH" // init 时识别未替换的占位配置。

#endif // LLM_CONFIG_H
