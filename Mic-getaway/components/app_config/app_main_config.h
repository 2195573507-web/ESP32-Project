#ifndef APP_MAIN_CONFIG_H
#define APP_MAIN_CONFIG_H

/**
 * @file app_main_config.h
 * @brief app_main 运行链路开关。
 *
 * 调用方法：
 * 1. 默认关闭 Mic/ASR 链路，打开 speaker/TTS bridge，用于优先调试扬声器路径。
 * 2. 需要恢复 Mic 时，把 MAIN_ENABLE_MIC_CHAIN 设为 1。
 * 3. Speaker TTS 开机测试默认关闭；需要测试 ESP 发文字 -> TTS -> ESP 播放声音时手动打开。
 */

#ifndef MAIN_ENABLE_MIC_CHAIN
/* Mic/ASR 主链路开关：当前默认关闭，避免占用录音/ASR 资源。 */
#define MAIN_ENABLE_MIC_CHAIN 0
#endif

#ifndef MAIN_ENABLE_SPEAKER_CHAIN
/* speaker/TTS 桥接开关：当前默认开启，用于初始化扬声器播放链路。 */
#define MAIN_ENABLE_SPEAKER_CHAIN 1
#endif

#ifndef MAIN_ENABLE_SPEAKER_BOOT_TTS_TEST
/* Speaker TTS 开机测试开关：需要同时打开 MAIN_ENABLE_SPEAKER_CHAIN。 */
#define MAIN_ENABLE_SPEAKER_BOOT_TTS_TEST 1
#endif

#ifndef MAIN_SPEAKER_BOOT_TTS_TEST_DELAY_MS
/* Speaker TTS 开机测试延迟，给 WiFi/LLM client 和 speaker bridge 初始化留出时间。 */
#define MAIN_SPEAKER_BOOT_TTS_TEST_DELAY_MS 1000
#endif

#ifndef MAIN_SPEAKER_BOOT_TTS_TEST_TEXT
/* ESP 主动发送给 TTS 的测试文本。 */
#define MAIN_SPEAKER_BOOT_TTS_TEST_TEXT "你好，我是豆包"
#endif

#ifndef MAIN_IDLE_DELAY_MS
/* app_main 空闲循环延迟，后台任务继续运行。 */
#define MAIN_IDLE_DELAY_MS 1000
#endif

#if MAIN_ENABLE_MIC_CHAIN != 0 && MAIN_ENABLE_MIC_CHAIN != 1
#error "MAIN_ENABLE_MIC_CHAIN must be 0 or 1"
#endif

#if MAIN_ENABLE_SPEAKER_CHAIN != 0 && MAIN_ENABLE_SPEAKER_CHAIN != 1
#error "MAIN_ENABLE_SPEAKER_CHAIN must be 0 or 1"
#endif

#if MAIN_ENABLE_SPEAKER_BOOT_TTS_TEST != 0 && MAIN_ENABLE_SPEAKER_BOOT_TTS_TEST != 1
#error "MAIN_ENABLE_SPEAKER_BOOT_TTS_TEST must be 0 or 1"
#endif

#endif /* APP_MAIN_CONFIG_H */
