#include "speaker_llm_bridge.h"

#include <string.h>

#include "esp_log.h"
#include "llm_client.h"

static const char *TAG = "speaker_llm_bridge";

esp_err_t speaker_llm_bridge_init(void)
{
    if (APP_DEBUG_SPEAKER_LLM_BRIDGE) {
        ESP_LOGI(TAG, "speaker bridge reserved, tts_enabled=%d", llm_client_is_tts_enabled());
    }
    return ESP_OK;
}

esp_err_t speaker_llm_bridge_speak_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (APP_DEBUG_SPEAKER_LLM_BRIDGE) {
        ESP_LOGI(TAG, "speaker speak_text request len=%u", (unsigned int)strlen(text));
    }
    return llm_client_tts_text(text);
}

bool speaker_llm_bridge_is_enabled(void)
{
    return llm_client_is_tts_enabled();
}
