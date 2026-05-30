#include "speaker_llm_bridge.h"

#include <string.h>

#include "esp_log.h"
#include "llm_client.h"

static const char *TAG = "speaker_llm_bridge";

esp_err_t speaker_llm_bridge_init(void)
{
    ESP_LOGI(TAG, "speaker bridge reserved, tts_enabled=%d", llm_client_is_tts_enabled());
    return ESP_OK;
}

esp_err_t speaker_llm_bridge_speak_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "TTS disabled, skip speak_text len=%u", (unsigned int)strlen(text));
    return ESP_ERR_NOT_SUPPORTED;
}

bool speaker_llm_bridge_is_enabled(void)
{
    return llm_client_is_tts_enabled();
}
