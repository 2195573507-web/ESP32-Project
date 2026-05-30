#include "llm_wav.h"

#include <stdlib.h>
#include <string.h>

static void llm_wav_write_u16_le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void llm_wav_write_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8) & 0xFFU);
    out[2] = (uint8_t)((value >> 16) & 0xFFU);
    out[3] = (uint8_t)((value >> 24) & 0xFFU);
}

esp_err_t llm_wav_build_from_pcm16(const int16_t *pcm,
                                   size_t samples,
                                   uint32_t sample_rate_hz,
                                   uint16_t channels,
                                   uint8_t **out_wav,
                                   size_t *out_wav_bytes)
{
    if (out_wav == NULL || out_wav_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_wav = NULL;
    *out_wav_bytes = 0;

    if (pcm == NULL || samples == 0 || sample_rate_hz == 0 || channels == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t pcm_bytes = samples * sizeof(int16_t);
    if (pcm_bytes / sizeof(int16_t) != samples ||
        pcm_bytes > UINT32_MAX - LLM_WAV_HEADER_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t wav_bytes = LLM_WAV_HEADER_BYTES + pcm_bytes;
    uint8_t *wav = (uint8_t *)malloc(wav_bytes);
    if (wav == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(&wav[0], "RIFF", 4);
    llm_wav_write_u32_le(&wav[4], (uint32_t)(wav_bytes - 8));
    memcpy(&wav[8], "WAVE", 4);
    memcpy(&wav[12], "fmt ", 4);
    llm_wav_write_u32_le(&wav[16], 16);
    llm_wav_write_u16_le(&wav[20], 1);
    llm_wav_write_u16_le(&wav[22], channels);
    llm_wav_write_u32_le(&wav[24], sample_rate_hz);
    uint32_t byte_rate = sample_rate_hz * channels * (uint32_t)sizeof(int16_t);
    llm_wav_write_u32_le(&wav[28], byte_rate);
    llm_wav_write_u16_le(&wav[32], (uint16_t)(channels * sizeof(int16_t)));
    llm_wav_write_u16_le(&wav[34], 16);
    memcpy(&wav[36], "data", 4);
    llm_wav_write_u32_le(&wav[40], (uint32_t)pcm_bytes);
    memcpy(&wav[LLM_WAV_HEADER_BYTES], pcm, pcm_bytes);

    *out_wav = wav;
    *out_wav_bytes = wav_bytes;
    return ESP_OK;
}
