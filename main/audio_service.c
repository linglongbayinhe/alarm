#include "audio_service.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_log.h"

#define AUDIO_SERVICE_BCLK GPIO_NUM_14
#define AUDIO_SERVICE_WS   GPIO_NUM_13
#define AUDIO_SERVICE_DOUT GPIO_NUM_19
#define AUDIO_SERVICE_INPUT_BUFFER_BYTES 4096
#define AUDIO_SERVICE_DECODER_INPUT_BYTES 1024
#define AUDIO_SERVICE_DECODER_OUTPUT_BYTES 8192

static const char *TAG = "AUDIO_SERVICE";

typedef struct {
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t data_size;
    long data_offset;
} audio_service_wav_info_t;

static i2s_chan_handle_t s_tx_channel;
static bool s_initialized;
static bool s_i2s_ready;
static bool s_channel_enabled;
static uint32_t s_current_sample_rate;
static bool s_decoder_registered;

static uint16_t audio_service_read_u16_le(const uint8_t *buffer)
{
    return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

static uint32_t audio_service_read_u32_le(const uint8_t *buffer)
{
    return (uint32_t)buffer[0] |
           ((uint32_t)buffer[1] << 8) |
           ((uint32_t)buffer[2] << 16) |
           ((uint32_t)buffer[3] << 24);
}

static audio_service_format_t audio_service_detect_format(const char *path)
{
    const char *extension = strrchr(path, '.');

    if (extension == NULL) {
        return AUDIO_SERVICE_FORMAT_WAV;
    }
    if (strcasecmp(extension, ".wav") == 0) {
        return AUDIO_SERVICE_FORMAT_WAV;
    }
    if (strcasecmp(extension, ".mp3") == 0) {
        return AUDIO_SERVICE_FORMAT_MP3;
    }

    return AUDIO_SERVICE_FORMAT_WAV;
}

static const char *audio_service_format_name(audio_service_format_t format)
{
    switch (format) {
        case AUDIO_SERVICE_FORMAT_WAV:
            return "wav";
        case AUDIO_SERVICE_FORMAT_MP3:
            return "mp3";
        case AUDIO_SERVICE_FORMAT_AUTO:
        default:
            return "auto";
    }
}

static esp_err_t audio_service_ensure_i2s(uint32_t sample_rate)
{
    esp_err_t ret = ESP_OK;

    if (!s_i2s_ready) {
        i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
        i2s_std_config_t std_config = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
            .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = AUDIO_SERVICE_BCLK,
                .ws = AUDIO_SERVICE_WS,
                .dout = AUDIO_SERVICE_DOUT,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };

        ret = i2s_new_channel(&channel_config, &s_tx_channel, NULL);
        if (ret != ESP_OK) {
            return ret;
        }

        ret = i2s_channel_init_std_mode(s_tx_channel, &std_config);
        if (ret != ESP_OK) {
            return ret;
        }

        s_i2s_ready = true;
        s_current_sample_rate = sample_rate;
        ESP_LOGI(TAG, "I2S ready: sample_rate=%" PRIu32 " bclk=%d ws=%d dout=%d",
                 sample_rate,
                 AUDIO_SERVICE_BCLK,
                 AUDIO_SERVICE_WS,
                 AUDIO_SERVICE_DOUT);
    } else if (s_current_sample_rate != sample_rate) {
        i2s_std_clk_config_t clk_config = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);

        if (s_channel_enabled) {
            ret = i2s_channel_disable(s_tx_channel);
            if (ret != ESP_OK) {
                return ret;
            }
            s_channel_enabled = false;
        }

        ret = i2s_channel_reconfig_std_clock(s_tx_channel, &clk_config);
        if (ret != ESP_OK) {
            return ret;
        }
        s_current_sample_rate = sample_rate;
        ESP_LOGI(TAG, "I2S reconfigured: sample_rate=%" PRIu32, sample_rate);
    }

    if (!s_channel_enabled) {
        ret = i2s_channel_enable(s_tx_channel);
        if (ret != ESP_OK) {
            return ret;
        }
        s_channel_enabled = true;
    }

    return ESP_OK;
}

static esp_err_t audio_service_parse_wav(FILE *file, audio_service_wav_info_t *info)
{
    uint8_t header[12] = {0};
    bool fmt_found = false;
    bool data_found = false;

    if ((file == NULL) || (info == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        return ESP_FAIL;
    }
    if ((memcmp(header, "RIFF", 4) != 0) || (memcmp(header + 8, "WAVE", 4) != 0)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    memset(info, 0, sizeof(*info));

    while (!data_found) {
        uint8_t chunk_header[8] = {0};
        uint32_t chunk_size = 0;

        if (fread(chunk_header, 1, sizeof(chunk_header), file) != sizeof(chunk_header)) {
            break;
        }
        chunk_size = audio_service_read_u32_le(chunk_header + 4);

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            uint8_t fmt_buffer[32] = {0};
            size_t read_size = chunk_size > sizeof(fmt_buffer) ? sizeof(fmt_buffer) : chunk_size;

            if (read_size < 16) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (fread(fmt_buffer, 1, read_size, file) != read_size) {
                return ESP_FAIL;
            }
            if (chunk_size > read_size) {
                fseek(file, (long)(chunk_size - read_size), SEEK_CUR);
            }
            if (audio_service_read_u16_le(fmt_buffer) != 1) {
                return ESP_ERR_NOT_SUPPORTED;
            }

            info->channels = audio_service_read_u16_le(fmt_buffer + 2);
            info->sample_rate = audio_service_read_u32_le(fmt_buffer + 4);
            info->bits_per_sample = audio_service_read_u16_le(fmt_buffer + 14);
            fmt_found = true;
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            info->data_offset = ftell(file);
            info->data_size = chunk_size;
            data_found = true;
        } else {
            fseek(file, (long)chunk_size, SEEK_CUR);
        }

        if ((chunk_size & 1U) != 0U) {
            fseek(file, 1, SEEK_CUR);
        }
    }

    if (!fmt_found || !data_found) {
        return ESP_FAIL;
    }
    if ((info->bits_per_sample != 16) || ((info->channels != 1) && (info->channels != 2))) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

static void audio_service_scale_buffer(int16_t *samples, size_t sample_count, uint8_t volume_percent)
{
    size_t index = 0;

    if ((samples == NULL) || (volume_percent >= 100)) {
        return;
    }

    for (index = 0; index < sample_count; ++index) {
        int32_t scaled = ((int32_t)samples[index] * volume_percent) / 100;
        if (scaled > INT16_MAX) {
            scaled = INT16_MAX;
        } else if (scaled < INT16_MIN) {
            scaled = INT16_MIN;
        }
        samples[index] = (int16_t)scaled;
    }
}

static esp_err_t audio_service_write_pcm_16(int16_t *pcm_samples,
                                            size_t sample_count,
                                            uint8_t channels,
                                            uint8_t volume_percent,
                                            int16_t *stereo_buffer,
                                            size_t stereo_buffer_samples,
                                            uint64_t *frames_written,
                                            uint64_t max_frames)
{
    size_t write_size = 0;
    size_t written = 0;

    if ((pcm_samples == NULL) || ((channels != 1U) && (channels != 2U))) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_service_scale_buffer(pcm_samples, sample_count, volume_percent);

    if (channels == 1U) {
        size_t frame_count = sample_count;
        size_t frame_index = 0;

        if ((max_frames > 0U) && ((*frames_written + frame_count) > max_frames)) {
            frame_count = (size_t)(max_frames - *frames_written);
        }
        if ((stereo_buffer == NULL) || (stereo_buffer_samples < (frame_count * 2U))) {
            return ESP_ERR_INVALID_SIZE;
        }

        for (frame_index = 0; frame_index < frame_count; ++frame_index) {
            stereo_buffer[frame_index * 2U] = pcm_samples[frame_index];
            stereo_buffer[(frame_index * 2U) + 1U] = pcm_samples[frame_index];
        }

        write_size = frame_count * sizeof(int16_t) * 2U;
        *frames_written += frame_count;
        return i2s_channel_write(s_tx_channel, stereo_buffer, write_size, &written, portMAX_DELAY);
    }

    if ((max_frames > 0U) && ((*frames_written + (sample_count / 2U)) > max_frames)) {
        sample_count = (size_t)(max_frames - *frames_written) * 2U;
    }

    *frames_written += sample_count / 2U;
    write_size = sample_count * sizeof(int16_t);
    return i2s_channel_write(s_tx_channel, pcm_samples, write_size, &written, portMAX_DELAY);
}

static esp_err_t audio_service_play_wav(const char *path, uint8_t volume_percent, uint32_t max_duration_ms)
{
    FILE *file = NULL;
    audio_service_wav_info_t info = {0};
    uint8_t *input_buffer = NULL;
    int16_t *stereo_buffer = NULL;
    uint32_t bytes_remaining = 0;
    uint64_t max_frames = 0;
    uint64_t frames_written = 0;
    esp_err_t ret = ESP_OK;

    (void)max_duration_ms;

    file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    ret = audio_service_parse_wav(file, &info);
    if (ret != ESP_OK) {
        fclose(file);
        return ret;
    }

    ESP_LOGI(TAG,
             "Playback start: path=%s format=wav rate=%" PRIu32 "Hz channels=%u bits=%u volume=%u",
             path,
             info.sample_rate,
             info.channels,
             info.bits_per_sample,
             volume_percent);

    ret = audio_service_ensure_i2s(info.sample_rate);
    if (ret != ESP_OK) {
        fclose(file);
        return ret;
    }

    if (fseek(file, info.data_offset, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    input_buffer = calloc(1, AUDIO_SERVICE_INPUT_BUFFER_BYTES);
    if (input_buffer == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    if (info.channels == 1) {
        stereo_buffer = calloc(1, AUDIO_SERVICE_INPUT_BUFFER_BYTES * 2);
        if (stereo_buffer == NULL) {
            free(input_buffer);
            fclose(file);
            return ESP_ERR_NO_MEM;
        }
    }

    bytes_remaining = info.data_size;
    if ((max_duration_ms > 0) && (info.sample_rate > 0)) {
        max_frames = ((uint64_t)info.sample_rate * (uint64_t)max_duration_ms) / 1000ULL;
    }

    while (bytes_remaining > 0) {
        size_t chunk_size = bytes_remaining > AUDIO_SERVICE_INPUT_BUFFER_BYTES ?
                            AUDIO_SERVICE_INPUT_BUFFER_BYTES :
                            bytes_remaining;
        size_t bytes_read = fread(input_buffer, 1, chunk_size, file);
        size_t sample_count = 0;

        if (bytes_read == 0) {
            break;
        }

        if (info.channels == 1) {
            int16_t *mono_samples = (int16_t *)input_buffer;
            size_t mono_sample_count = bytes_read / sizeof(int16_t);
            uint64_t previous_frames_written = frames_written;

            ret = audio_service_write_pcm_16(mono_samples,
                                             mono_sample_count,
                                             1,
                                             volume_percent,
                                             stereo_buffer,
                                             AUDIO_SERVICE_INPUT_BUFFER_BYTES,
                                             &frames_written,
                                             max_frames);
            if ((max_frames > 0) && (frames_written >= max_frames)) {
                bytes_remaining = 0;
            } else {
                bytes_remaining -= (uint32_t)bytes_read;
            }
            if ((max_frames > 0) && (frames_written == previous_frames_written)) {
                break;
            }
        } else {
            int16_t *stereo_samples = (int16_t *)input_buffer;
            uint64_t previous_frames_written = frames_written;

            sample_count = bytes_read / sizeof(int16_t);
            bytes_remaining -= (uint32_t)bytes_read;
            ret = audio_service_write_pcm_16(stereo_samples,
                                             sample_count,
                                             2,
                                             volume_percent,
                                             NULL,
                                             0,
                                             &frames_written,
                                             max_frames);
            if ((max_frames > 0) && (frames_written == previous_frames_written)) {
                break;
            }
        }

        if (ret != ESP_OK) {
            break;
        }
        if ((max_frames > 0) && (frames_written >= max_frames)) {
            break;
        }
    }

    free(stereo_buffer);
    free(input_buffer);
    fclose(file);

    ESP_LOGI(TAG, "Playback end: path=%s format=wav result=%s", path, esp_err_to_name(ret));

    return ret;
}

static esp_err_t audio_service_play_mp3(const char *path, uint8_t volume_percent, uint32_t max_duration_ms)
{
    FILE *file = NULL;
    uint8_t *input_buffer = NULL;
    uint8_t *output_buffer = NULL;
    int16_t *stereo_buffer = NULL;
    esp_audio_simple_dec_handle_t decoder = NULL;
    esp_audio_simple_dec_cfg_t decoder_config = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .use_frame_dec = false,
    };
    esp_audio_err_t audio_ret = ESP_AUDIO_ERR_OK;
    esp_err_t ret = ESP_OK;
    bool info_ready = false;
    uint32_t output_buffer_size = AUDIO_SERVICE_DECODER_OUTPUT_BYTES;
    uint64_t max_frames = 0;
    uint64_t frames_written = 0;

    file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    input_buffer = calloc(1, AUDIO_SERVICE_DECODER_INPUT_BYTES);
    output_buffer = calloc(1, output_buffer_size);
    stereo_buffer = calloc(1, output_buffer_size * 2U);
    if ((input_buffer == NULL) || (output_buffer == NULL) || (stereo_buffer == NULL)) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    if ((max_duration_ms > 0U)) {
        max_frames = ((uint64_t)48000U * (uint64_t)max_duration_ms) / 1000ULL;
    }

    audio_ret = esp_audio_simple_dec_open(&decoder_config, &decoder);
    if (audio_ret != ESP_AUDIO_ERR_OK) {
        ret = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    while (true) {
        size_t read_size = fread(input_buffer, 1, AUDIO_SERVICE_DECODER_INPUT_BYTES, file);
        esp_audio_simple_dec_raw_t raw = {
            .buffer = input_buffer,
            .len = (uint32_t)read_size,
            .eos = (read_size < AUDIO_SERVICE_DECODER_INPUT_BYTES),
        };

        if (read_size == 0U && feof(file) == 0) {
            ret = ESP_FAIL;
            goto cleanup;
        }
        if ((read_size == 0U) && !raw.eos) {
            break;
        }

        while ((raw.len > 0U) || raw.eos) {
            esp_audio_simple_dec_out_t frame = {
                .buffer = output_buffer,
                .len = output_buffer_size,
            };

            audio_ret = esp_audio_simple_dec_process(decoder, &raw, &frame);
            if (audio_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint8_t *new_output = realloc(output_buffer, frame.needed_size);
                int16_t *new_stereo = realloc(stereo_buffer, frame.needed_size * 2U);

                if ((new_output == NULL) || (new_stereo == NULL)) {
                    free(new_output);
                    free(new_stereo);
                    ret = ESP_ERR_NO_MEM;
                    goto cleanup;
                }
                output_buffer = new_output;
                stereo_buffer = new_stereo;
                output_buffer_size = frame.needed_size;
                continue;
            }
            if ((audio_ret != ESP_AUDIO_ERR_OK) && (audio_ret != ESP_AUDIO_ERR_CONTINUE) &&
                !(raw.eos && (audio_ret == ESP_AUDIO_ERR_DATA_LACK))) {
                ret = ESP_FAIL;
                goto cleanup;
            }

            if (frame.decoded_size > 0U) {
                esp_audio_simple_dec_info_t decoder_info = {0};
                uint8_t channels = 2U;
                uint8_t bits_per_sample = 16U;

                audio_ret = esp_audio_simple_dec_get_info(decoder, &decoder_info);
                if (audio_ret == ESP_AUDIO_ERR_OK) {
                    channels = decoder_info.channel;
                    bits_per_sample = decoder_info.bits_per_sample;
                    if (!info_ready) {
                        ESP_LOGI(TAG,
                                 "Playback start: path=%s format=mp3 rate=%" PRIu32 "Hz channels=%u bits=%u volume=%u",
                                 path,
                                 decoder_info.sample_rate,
                                 channels,
                                 bits_per_sample,
                                 volume_percent);
                        ret = audio_service_ensure_i2s(decoder_info.sample_rate);
                        if (ret != ESP_OK) {
                            goto cleanup;
                        }
                        if ((max_duration_ms > 0U) && (decoder_info.sample_rate > 0U)) {
                            max_frames = ((uint64_t)decoder_info.sample_rate * (uint64_t)max_duration_ms) / 1000ULL;
                        }
                        info_ready = true;
                    }
                }

                if (bits_per_sample != 16U) {
                    ret = ESP_ERR_NOT_SUPPORTED;
                    goto cleanup;
                }
                if (!info_ready) {
                    ret = ESP_ERR_INVALID_STATE;
                    goto cleanup;
                }

                ret = audio_service_write_pcm_16((int16_t *)output_buffer,
                                                 frame.decoded_size / sizeof(int16_t),
                                                 channels,
                                                 volume_percent,
                                                 stereo_buffer,
                                                 output_buffer_size,
                                                 &frames_written,
                                                 max_frames);
                if (ret != ESP_OK) {
                    goto cleanup;
                }
                if ((max_frames > 0U) && (frames_written >= max_frames)) {
                    goto cleanup;
                }
            }

            if (raw.consumed > raw.len) {
                raw.len = 0;
            } else {
                raw.len -= raw.consumed;
                raw.buffer += raw.consumed;
            }

            if ((raw.len == 0U) && raw.eos) {
                goto cleanup;
            }
            if ((audio_ret == ESP_AUDIO_ERR_CONTINUE) && (frame.decoded_size == 0U) && (raw.len == 0U)) {
                break;
            }
            if ((audio_ret == ESP_AUDIO_ERR_OK) && (frame.decoded_size == 0U) && (raw.len == 0U)) {
                break;
            }
        }

        if (read_size == 0U) {
            break;
        }
    }

cleanup:
    if (decoder != NULL) {
        esp_audio_simple_dec_close(decoder);
    }
    free(stereo_buffer);
    free(output_buffer);
    free(input_buffer);
    if (file != NULL) {
        fclose(file);
    }

    ESP_LOGI(TAG, "Playback end: path=%s format=mp3 result=%s", path, esp_err_to_name(ret));

    return ret;
}

esp_err_t audio_service_init(void)
{
    if (!s_decoder_registered) {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        s_decoder_registered = true;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t audio_service_play(const char *path,
                             audio_service_format_t format,
                             uint8_t volume_percent,
                             uint32_t max_duration_ms)
{
    if ((path == NULL) || (path[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (volume_percent == 0) {
        volume_percent = 100;
    }

    if (format == AUDIO_SERVICE_FORMAT_AUTO) {
        format = audio_service_detect_format(path);
    }

    ESP_LOGI(TAG,
             "Playback request: path=%s format=%s volume=%u max_duration_ms=%" PRIu32,
             path,
             audio_service_format_name(format),
             volume_percent,
             max_duration_ms);

    switch (format) {
        case AUDIO_SERVICE_FORMAT_WAV:
            return audio_service_play_wav(path, volume_percent, max_duration_ms);
        case AUDIO_SERVICE_FORMAT_MP3:
            return audio_service_play_mp3(path, volume_percent, max_duration_ms);
        case AUDIO_SERVICE_FORMAT_AUTO:
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

bool audio_service_is_ready(void)
{
    return s_initialized;
}
