#include "audio_service.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_audio_simple_dec.h"
#include "esp_mp3_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define AUDIO_SERVICE_BCLK GPIO_NUM_14
#define AUDIO_SERVICE_WS   GPIO_NUM_12
#define AUDIO_SERVICE_DOUT GPIO_NUM_19
#define AUDIO_SERVICE_INPUT_BUFFER_BYTES 4096
#define AUDIO_SERVICE_DECODER_INPUT_BYTES 1024
#define AUDIO_SERVICE_DECODER_OUTPUT_BYTES 4096
#define AUDIO_SERVICE_STEREO_CHUNK_FRAMES 512
#define AUDIO_SERVICE_STEREO_CHUNK_SAMPLES (AUDIO_SERVICE_STEREO_CHUNK_FRAMES * 2U)
#define AUDIO_SERVICE_TAIL_SILENCE_SAMPLES 512
#define AUDIO_SERVICE_DEFAULT_SAMPLE_RATE 16000
#define AUDIO_SERVICE_I2S_WRITE_TIMEOUT_MS 1000

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
static volatile bool s_stop_requested;
static volatile bool s_playback_active;

static void audio_service_begin_playback(void)
{
    s_stop_requested = false;
    s_playback_active = true;
}

static void audio_service_end_playback(void)
{
    s_playback_active = false;
}

static bool audio_service_should_stop(void)
{
    return s_stop_requested;
}

static void audio_service_log_heap(const char *stage)
{
    ESP_LOGI(TAG,
             "Heap %s: free=%u largest=%u",
             stage == NULL ? "unknown" : stage,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void audio_service_log_mp3_state(const char *stage,
                                        uint32_t output_buffer_size,
                                        size_t stereo_buffer_samples)
{
    ESP_LOGW(TAG,
             "MP3 %s: free=%u largest=%u output=%u stereo_chunk=%u",
             stage == NULL ? "unknown" : stage,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned int)output_buffer_size,
             (unsigned int)(stereo_buffer_samples * sizeof(int16_t)));
}

static esp_err_t audio_service_write_i2s(const void *buffer, size_t write_size)
{
    size_t written = 0;
    esp_err_t ret = ESP_OK;

    if ((buffer == NULL) || (write_size == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = i2s_channel_write(s_tx_channel,
                            buffer,
                            write_size,
                            &written,
                            pdMS_TO_TICKS(AUDIO_SERVICE_I2S_WRITE_TIMEOUT_MS));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "I2S write failed: ret=%s requested=%u written=%u timeout_ms=%u enabled=%d rate=%" PRIu32,
                 esp_err_to_name(ret),
                 (unsigned int)write_size,
                 (unsigned int)written,
                 (unsigned int)AUDIO_SERVICE_I2S_WRITE_TIMEOUT_MS,
                 s_channel_enabled ? 1 : 0,
                 s_current_sample_rate);
        return ret;
    }
    if (written != write_size) {
        ESP_LOGW(TAG,
                 "I2S short write: requested=%u written=%u timeout_ms=%u enabled=%d rate=%" PRIu32,
                 (unsigned int)write_size,
                 (unsigned int)written,
                 (unsigned int)AUDIO_SERVICE_I2S_WRITE_TIMEOUT_MS,
                 s_channel_enabled ? 1 : 0,
                 s_current_sample_rate);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

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
            // Most plug-in I2S speaker amps expect the standard Philips/I2S slot timing.
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
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
            (void)i2s_del_channel(s_tx_channel);
            s_tx_channel = NULL;
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
        ESP_LOGI(TAG, "I2S enabling: sample_rate=%" PRIu32, s_current_sample_rate);
        ret = i2s_channel_enable(s_tx_channel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S enable failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_channel_enabled = true;
        ESP_LOGI(TAG, "I2S enabled: sample_rate=%" PRIu32, s_current_sample_rate);
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
    esp_err_t ret = ESP_OK;

    if ((pcm_samples == NULL) || ((channels != 1U) && (channels != 2U))) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_service_scale_buffer(pcm_samples, sample_count, volume_percent);

    if (channels == 1U) {
        size_t frame_count = sample_count;
        size_t frame_offset = 0;
        size_t stereo_chunk_frames = 0;

        if ((max_frames > 0U) && ((*frames_written + frame_count) > max_frames)) {
            frame_count = (size_t)(max_frames - *frames_written);
        }
        if ((stereo_buffer == NULL) || (stereo_buffer_samples < 2U)) {
            return ESP_ERR_INVALID_SIZE;
        }

        stereo_chunk_frames = stereo_buffer_samples / 2U;
        while (frame_offset < frame_count) {
            size_t chunk_frames = frame_count - frame_offset;
            size_t frame_index = 0;

            if (audio_service_should_stop()) {
                return ESP_OK;
            }
            if (chunk_frames > stereo_chunk_frames) {
                chunk_frames = stereo_chunk_frames;
            }

            for (frame_index = 0; frame_index < chunk_frames; ++frame_index) {
                int16_t sample = pcm_samples[frame_offset + frame_index];
                stereo_buffer[frame_index * 2U] = sample;
                stereo_buffer[(frame_index * 2U) + 1U] = sample;
            }

            write_size = chunk_frames * sizeof(int16_t) * 2U;
            ret = audio_service_write_i2s(stereo_buffer, write_size);
            if (ret != ESP_OK) {
                return ret;
            }
            *frames_written += chunk_frames;
            frame_offset += chunk_frames;
        }

        return ESP_OK;
    }

    if ((max_frames > 0U) && ((*frames_written + (sample_count / 2U)) > max_frames)) {
        sample_count = (size_t)(max_frames - *frames_written) * 2U;
    }

    if (audio_service_should_stop()) {
        return ESP_OK;
    }

    *frames_written += sample_count / 2U;
    write_size = sample_count * sizeof(int16_t);
    return audio_service_write_i2s(pcm_samples, write_size);
}

static void audio_service_finish_output(void)
{
    int16_t silence_buffer[AUDIO_SERVICE_TAIL_SILENCE_SAMPLES] = {0};

    if (!s_i2s_ready || (s_tx_channel == NULL) || !s_channel_enabled) {
        return;
    }

    // Push a short silence tail so the amp does not hold the last decoded sample.
    (void)audio_service_write_i2s(silence_buffer, sizeof(silence_buffer));
    if (i2s_channel_disable(s_tx_channel) == ESP_OK) {
        s_channel_enabled = false;
    }
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
        stereo_buffer = calloc(AUDIO_SERVICE_STEREO_CHUNK_SAMPLES, sizeof(*stereo_buffer));
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

    while ((bytes_remaining > 0) && !audio_service_should_stop()) {
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
                                             AUDIO_SERVICE_STEREO_CHUNK_SAMPLES,
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
    audio_service_finish_output();

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

    audio_service_log_heap("mp3_before_buffers");
    input_buffer = calloc(1, AUDIO_SERVICE_DECODER_INPUT_BYTES);
    output_buffer = calloc(1, output_buffer_size);
    stereo_buffer = calloc(AUDIO_SERVICE_STEREO_CHUNK_SAMPLES, sizeof(*stereo_buffer));
    if ((input_buffer == NULL) || (output_buffer == NULL) || (stereo_buffer == NULL)) {
        audio_service_log_heap("mp3_buffer_alloc_failed");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    ESP_LOGI(TAG,
             "MP3 buffers allocated: input=%u output=%u stereo=%u total=%u",
             (unsigned int)AUDIO_SERVICE_DECODER_INPUT_BYTES,
             (unsigned int)output_buffer_size,
             (unsigned int)(AUDIO_SERVICE_STEREO_CHUNK_SAMPLES * sizeof(*stereo_buffer)),
             (unsigned int)(AUDIO_SERVICE_DECODER_INPUT_BYTES +
                            output_buffer_size +
                            (AUDIO_SERVICE_STEREO_CHUNK_SAMPLES * sizeof(*stereo_buffer))));
    audio_service_log_heap("mp3_after_buffers");

    if ((max_duration_ms > 0U)) {
        max_frames = ((uint64_t)48000U * (uint64_t)max_duration_ms) / 1000ULL;
    }

    audio_ret = esp_audio_simple_dec_open(&decoder_config, &decoder);
    if (audio_ret != ESP_AUDIO_ERR_OK) {
        audio_service_log_mp3_state("decoder_open_failed",
                                    output_buffer_size,
                                    AUDIO_SERVICE_STEREO_CHUNK_SAMPLES);
        ret = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }
    audio_service_log_heap("mp3_after_decoder_open");

    while (!audio_service_should_stop()) {
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

        while (((raw.len > 0U) || raw.eos) && !audio_service_should_stop()) {
            esp_audio_simple_dec_out_t frame = {
                .buffer = output_buffer,
                .len = output_buffer_size,
            };

            audio_ret = esp_audio_simple_dec_process(decoder, &raw, &frame);
            if (audio_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint8_t *new_output = realloc(output_buffer, frame.needed_size);

                if (new_output == NULL) {
                    audio_service_log_mp3_state("buffer_realloc_failed",
                                                output_buffer_size,
                                                AUDIO_SERVICE_STEREO_CHUNK_SAMPLES);
                    ret = ESP_ERR_NO_MEM;
                    goto cleanup;
                }
                output_buffer = new_output;
                output_buffer_size = frame.needed_size;
                ESP_LOGI(TAG,
                         "MP3 buffers resized: output=%u stereo=%u total=%u",
                         (unsigned int)output_buffer_size,
                         (unsigned int)(AUDIO_SERVICE_STEREO_CHUNK_SAMPLES * sizeof(*stereo_buffer)),
                         (unsigned int)(AUDIO_SERVICE_DECODER_INPUT_BYTES +
                                        output_buffer_size +
                                        (AUDIO_SERVICE_STEREO_CHUNK_SAMPLES * sizeof(*stereo_buffer))));
                audio_service_log_heap("mp3_after_buffer_resize");
                continue;
            }
            if ((audio_ret != ESP_AUDIO_ERR_OK) && (audio_ret != ESP_AUDIO_ERR_CONTINUE) &&
                !(raw.eos && (audio_ret == ESP_AUDIO_ERR_DATA_LACK))) {
                audio_service_log_mp3_state("decode_failed",
                                            output_buffer_size,
                                            AUDIO_SERVICE_STEREO_CHUNK_SAMPLES);
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
                                                 AUDIO_SERVICE_STEREO_CHUNK_SAMPLES,
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
    audio_service_finish_output();
    audio_service_log_heap("mp3_after_cleanup");

    ESP_LOGI(TAG, "Playback end: path=%s format=mp3 result=%s", path, esp_err_to_name(ret));

    return ret;
}

typedef struct {
    const char *path;
    audio_service_format_t format;
    FILE *file;
    audio_service_wav_info_t wav_info;
    uint32_t wav_bytes_remaining;
    esp_audio_simple_dec_handle_t decoder;
    uint8_t *input_buffer;
    uint32_t mp3_pending_len;
    bool mp3_eos_pending;
    uint8_t *pcm_buffer;
    uint32_t pcm_buffer_size;
    uint32_t pcm_decoded_size;
    size_t pcm_sample_offset;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
    bool info_ready;
    bool exhausted;
    uint32_t output_sample_rate;
    uint64_t resample_step_q32;
    uint64_t resample_phase_q32;
    int16_t resample_prev_left;
    int16_t resample_prev_right;
    int16_t resample_next_left;
    int16_t resample_next_right;
    bool resample_initialized;
    bool resample_has_prev;
    bool resample_has_next;
} audio_service_mix_stream_t;

static void audio_service_mix_stream_close(audio_service_mix_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }

    if (stream->decoder != NULL) {
        esp_audio_simple_dec_close(stream->decoder);
        stream->decoder = NULL;
    }
    if (stream->file != NULL) {
        fclose(stream->file);
        stream->file = NULL;
    }
    free(stream->input_buffer);
    free(stream->pcm_buffer);
    memset(stream, 0, sizeof(*stream));
}

static esp_err_t audio_service_mix_stream_open(audio_service_mix_stream_t *stream, const char *path)
{
    esp_err_t ret = ESP_OK;

    if ((stream == NULL) || (path == NULL) || (path[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(stream, 0, sizeof(*stream));
    stream->path = path;
    stream->format = audio_service_detect_format(path);
    stream->file = fopen(path, "rb");
    if (stream->file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (stream->format == AUDIO_SERVICE_FORMAT_WAV) {
        ret = audio_service_parse_wav(stream->file, &stream->wav_info);
        if (ret != ESP_OK) {
            audio_service_mix_stream_close(stream);
            return ret;
        }
        if (fseek(stream->file, stream->wav_info.data_offset, SEEK_SET) != 0) {
            audio_service_mix_stream_close(stream);
            return ESP_FAIL;
        }

        stream->sample_rate = stream->wav_info.sample_rate;
        stream->channels = (uint8_t)stream->wav_info.channels;
        stream->bits_per_sample = (uint8_t)stream->wav_info.bits_per_sample;
        stream->wav_bytes_remaining = stream->wav_info.data_size;
        stream->info_ready = true;
        stream->pcm_buffer_size = AUDIO_SERVICE_DECODER_OUTPUT_BYTES;
        stream->pcm_buffer = calloc(1, stream->pcm_buffer_size);
    } else if (stream->format == AUDIO_SERVICE_FORMAT_MP3) {
        esp_audio_simple_dec_cfg_t decoder_config = {
            .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
            .use_frame_dec = false,
        };
        esp_audio_err_t audio_ret = ESP_AUDIO_ERR_OK;

        stream->input_buffer = calloc(1, AUDIO_SERVICE_DECODER_INPUT_BYTES);
        stream->pcm_buffer_size = AUDIO_SERVICE_DECODER_OUTPUT_BYTES;
        stream->pcm_buffer = calloc(1, stream->pcm_buffer_size);
        if ((stream->input_buffer == NULL) || (stream->pcm_buffer == NULL)) {
            audio_service_mix_stream_close(stream);
            return ESP_ERR_NO_MEM;
        }

        audio_ret = esp_audio_simple_dec_open(&decoder_config, &stream->decoder);
        if (audio_ret != ESP_AUDIO_ERR_OK) {
            audio_service_mix_stream_close(stream);
            return ESP_ERR_NOT_SUPPORTED;
        }
    } else {
        audio_service_mix_stream_close(stream);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (stream->pcm_buffer == NULL) {
        audio_service_mix_stream_close(stream);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t audio_service_mix_stream_refill_wav(audio_service_mix_stream_t *stream)
{
    size_t read_size = 0;
    size_t bytes_per_frame = 0;

    if ((stream == NULL) || (stream->file == NULL) || (stream->pcm_buffer == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    stream->pcm_decoded_size = 0;
    stream->pcm_sample_offset = 0;

    if (stream->wav_bytes_remaining == 0U) {
        stream->exhausted = true;
        return ESP_OK;
    }

    bytes_per_frame = (size_t)stream->channels * sizeof(int16_t);
    read_size = stream->wav_bytes_remaining > stream->pcm_buffer_size ?
                stream->pcm_buffer_size :
                stream->wav_bytes_remaining;
    if ((bytes_per_frame > 0U) && ((read_size % bytes_per_frame) != 0U)) {
        read_size -= read_size % bytes_per_frame;
    }
    if (read_size == 0U) {
        stream->exhausted = true;
        return ESP_OK;
    }

    read_size = fread(stream->pcm_buffer, 1, read_size, stream->file);
    if (read_size == 0U) {
        stream->exhausted = true;
        return ferror(stream->file) ? ESP_FAIL : ESP_OK;
    }

    stream->wav_bytes_remaining -= (uint32_t)read_size;
    stream->pcm_decoded_size = (uint32_t)read_size;
    return ESP_OK;
}

static esp_err_t audio_service_mix_stream_refill_mp3(audio_service_mix_stream_t *stream)
{
    if ((stream == NULL) || (stream->file == NULL) ||
        (stream->decoder == NULL) || (stream->input_buffer == NULL) ||
        (stream->pcm_buffer == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    stream->pcm_decoded_size = 0;
    stream->pcm_sample_offset = 0;

    while (!stream->exhausted) {
        size_t read_size = 0;
        esp_audio_simple_dec_raw_t raw = {0};

        if ((stream->mp3_pending_len > 0U) || stream->mp3_eos_pending) {
            raw.buffer = stream->input_buffer;
            raw.len = stream->mp3_pending_len;
            raw.eos = stream->mp3_eos_pending;
            stream->mp3_pending_len = 0;
            stream->mp3_eos_pending = false;
        } else {
            read_size = fread(stream->input_buffer, 1, AUDIO_SERVICE_DECODER_INPUT_BYTES, stream->file);
            raw.buffer = stream->input_buffer;
            raw.len = (uint32_t)read_size;
            raw.eos = (read_size < AUDIO_SERVICE_DECODER_INPUT_BYTES);

            if ((read_size == 0U) && ferror(stream->file)) {
                return ESP_FAIL;
            }
            if ((read_size == 0U) && feof(stream->file)) {
                stream->exhausted = true;
                return ESP_OK;
            }
        }

        while ((raw.len > 0U) || raw.eos) {
            esp_audio_simple_dec_out_t frame = {
                .buffer = stream->pcm_buffer,
                .len = stream->pcm_buffer_size,
            };
            esp_audio_err_t audio_ret = esp_audio_simple_dec_process(stream->decoder, &raw, &frame);

            if (audio_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint8_t *new_buffer = realloc(stream->pcm_buffer, frame.needed_size);

                if (new_buffer == NULL) {
                    return ESP_ERR_NO_MEM;
                }
                stream->pcm_buffer = new_buffer;
                stream->pcm_buffer_size = frame.needed_size;
                continue;
            }
            if ((audio_ret != ESP_AUDIO_ERR_OK) && (audio_ret != ESP_AUDIO_ERR_CONTINUE) &&
                !(raw.eos && (audio_ret == ESP_AUDIO_ERR_DATA_LACK))) {
                return ESP_FAIL;
            }

            if (frame.decoded_size > 0U) {
                esp_audio_simple_dec_info_t decoder_info = {0};
                uint32_t remaining = raw.consumed > raw.len ? 0U : (raw.len - raw.consumed);

                audio_ret = esp_audio_simple_dec_get_info(stream->decoder, &decoder_info);
                if (audio_ret != ESP_AUDIO_ERR_OK) {
                    return ESP_FAIL;
                }

                stream->sample_rate = decoder_info.sample_rate;
                stream->channels = decoder_info.channel;
                stream->bits_per_sample = decoder_info.bits_per_sample;
                stream->info_ready = true;
                stream->pcm_decoded_size = frame.decoded_size;
                if (remaining > 0U) {
                    memmove(stream->input_buffer, raw.buffer + raw.consumed, remaining);
                }
                stream->mp3_pending_len = remaining;
                stream->mp3_eos_pending = raw.eos;
                return ESP_OK;
            }

            if ((raw.consumed == 0U) && (raw.len > 0U)) {
                if (raw.eos && (audio_ret == ESP_AUDIO_ERR_DATA_LACK)) {
                    stream->exhausted = true;
                    return ESP_OK;
                }
                return ESP_FAIL;
            }

            if (raw.consumed > raw.len) {
                raw.len = 0;
            } else {
                raw.len -= raw.consumed;
                raw.buffer += raw.consumed;
            }

            if ((raw.len == 0U) && raw.eos) {
                stream->exhausted = true;
                return ESP_OK;
            }
            if ((frame.decoded_size == 0U) && (raw.len == 0U)) {
                break;
            }
        }
    }

    return ESP_OK;
}

static esp_err_t audio_service_mix_stream_refill(audio_service_mix_stream_t *stream)
{
    if (stream == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stream->format == AUDIO_SERVICE_FORMAT_WAV) {
        return audio_service_mix_stream_refill_wav(stream);
    }
    if (stream->format == AUDIO_SERVICE_FORMAT_MP3) {
        return audio_service_mix_stream_refill_mp3(stream);
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t audio_service_mix_stream_ensure_info(audio_service_mix_stream_t *stream)
{
    esp_err_t ret = ESP_OK;

    if (stream == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    while (!stream->info_ready && !stream->exhausted) {
        ret = audio_service_mix_stream_refill(stream);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (!stream->info_ready) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((stream->bits_per_sample != 16U) || ((stream->channels != 1U) && (stream->channels != 2U))) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

static esp_err_t audio_service_mix_stream_read_frame(audio_service_mix_stream_t *stream,
                                                    int16_t *left,
                                                    int16_t *right,
                                                    bool *has_frame)
{
    esp_err_t ret = ESP_OK;
    int16_t *samples = NULL;
    size_t sample_count = 0;

    if ((stream == NULL) || (left == NULL) || (right == NULL) || (has_frame == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *left = 0;
    *right = 0;
    *has_frame = false;

    while (stream->pcm_sample_offset >= (stream->pcm_decoded_size / sizeof(int16_t))) {
        if (stream->exhausted) {
            return ESP_OK;
        }
        ret = audio_service_mix_stream_refill(stream);
        if (ret != ESP_OK) {
            return ret;
        }
        if ((stream->pcm_decoded_size == 0U) && stream->exhausted) {
            return ESP_OK;
        }
    }

    samples = (int16_t *)stream->pcm_buffer;
    sample_count = stream->pcm_decoded_size / sizeof(int16_t);
    if (stream->channels == 1U) {
        *left = samples[stream->pcm_sample_offset];
        *right = *left;
        stream->pcm_sample_offset++;
    } else if ((stream->channels == 2U) && ((stream->pcm_sample_offset + 1U) < sample_count)) {
        *left = samples[stream->pcm_sample_offset];
        *right = samples[stream->pcm_sample_offset + 1U];
        stream->pcm_sample_offset += 2U;
    } else {
        stream->pcm_sample_offset = sample_count;
        return audio_service_mix_stream_read_frame(stream, left, right, has_frame);
    }

    *has_frame = true;
    return ESP_OK;
}

static int16_t audio_service_mix_lerp_sample(int16_t from, int16_t to, uint32_t frac_q32)
{
    int64_t delta = (int64_t)to - (int64_t)from;
    int64_t scaled_delta = (delta * (int64_t)frac_q32) / (int64_t)(1ULL << 32);
    int64_t interpolated = (int64_t)from + scaled_delta;

    if (interpolated > INT16_MAX) {
        return INT16_MAX;
    }
    if (interpolated < INT16_MIN) {
        return INT16_MIN;
    }

    return (int16_t)interpolated;
}

static esp_err_t audio_service_mix_stream_init_resampler(audio_service_mix_stream_t *stream)
{
    esp_err_t ret = ESP_OK;
    bool has_frame = false;

    if ((stream == NULL) || (stream->sample_rate == 0U) || (stream->output_sample_rate == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    stream->resample_initialized = true;
    stream->resample_phase_q32 = 0;
    stream->resample_step_q32 = (((uint64_t)stream->sample_rate) << 32) /
                                (uint64_t)stream->output_sample_rate;
    if (stream->resample_step_q32 == 0U) {
        stream->resample_step_q32 = 1U;
    }

    ret = audio_service_mix_stream_read_frame(stream,
                                              &stream->resample_prev_left,
                                              &stream->resample_prev_right,
                                              &has_frame);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!has_frame) {
        return ESP_OK;
    }
    stream->resample_has_prev = true;

    ret = audio_service_mix_stream_read_frame(stream,
                                              &stream->resample_next_left,
                                              &stream->resample_next_right,
                                              &has_frame);
    if (ret != ESP_OK) {
        return ret;
    }
    stream->resample_has_next = has_frame;

    return ESP_OK;
}

static esp_err_t audio_service_mix_stream_read_output_frame(audio_service_mix_stream_t *stream,
                                                           int16_t *left,
                                                           int16_t *right,
                                                           bool *has_frame)
{
    esp_err_t ret = ESP_OK;

    if ((stream == NULL) || (left == NULL) || (right == NULL) || (has_frame == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((stream->output_sample_rate == 0U) || (stream->sample_rate == stream->output_sample_rate)) {
        return audio_service_mix_stream_read_frame(stream, left, right, has_frame);
    }

    *left = 0;
    *right = 0;
    *has_frame = false;

    if (!stream->resample_initialized) {
        ret = audio_service_mix_stream_init_resampler(stream);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (!stream->resample_has_prev) {
        return ESP_OK;
    }

    if (stream->resample_has_next) {
        uint32_t frac_q32 = (uint32_t)(stream->resample_phase_q32 & 0xFFFFFFFFULL);

        *left = audio_service_mix_lerp_sample(stream->resample_prev_left,
                                              stream->resample_next_left,
                                              frac_q32);
        *right = audio_service_mix_lerp_sample(stream->resample_prev_right,
                                               stream->resample_next_right,
                                               frac_q32);
    } else {
        *left = stream->resample_prev_left;
        *right = stream->resample_prev_right;
    }
    *has_frame = true;

    stream->resample_phase_q32 += stream->resample_step_q32;
    while (stream->resample_phase_q32 >= (1ULL << 32)) {
        bool has_next = false;

        stream->resample_phase_q32 -= (1ULL << 32);
        if (!stream->resample_has_next) {
            stream->resample_has_prev = false;
            break;
        }

        stream->resample_prev_left = stream->resample_next_left;
        stream->resample_prev_right = stream->resample_next_right;
        ret = audio_service_mix_stream_read_frame(stream,
                                                  &stream->resample_next_left,
                                                  &stream->resample_next_right,
                                                  &has_next);
        if (ret != ESP_OK) {
            return ret;
        }
        stream->resample_has_next = has_next;
    }

    return ESP_OK;
}

static int16_t audio_service_mix_sample(int16_t sample_a, bool has_a, int16_t sample_b, bool has_b)
{
    int32_t mixed = 0;

    if (has_a) {
        mixed += (int32_t)sample_a / 2;
    }
    if (has_b) {
        mixed += (int32_t)sample_b / 2;
    }
    if (mixed > INT16_MAX) {
        mixed = INT16_MAX;
    } else if (mixed < INT16_MIN) {
        mixed = INT16_MIN;
    }

    return (int16_t)mixed;
}

esp_err_t audio_service_play_mix(const char *path_a, const char *path_b, uint8_t volume_percent)
{
    audio_service_mix_stream_t stream_a = {0};
    audio_service_mix_stream_t stream_b = {0};
    int16_t *mix_buffer = NULL;
    esp_err_t ret = ESP_OK;

    if ((path_a == NULL) || (path_a[0] == '\0') || (path_b == NULL) || (path_b[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (volume_percent > 100U) {
        volume_percent = 100U;
    }

    audio_service_begin_playback();
    ESP_LOGI(TAG, "Mix playback request: path_a=%s path_b=%s volume=%u",
             path_a,
             path_b,
             (unsigned int)volume_percent);
    audio_service_log_heap("mix_before_open");

    ret = audio_service_mix_stream_open(&stream_a, path_a);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mix open failed for path_a=%s: %s", path_a, esp_err_to_name(ret));
        goto cleanup;
    }
    ret = audio_service_mix_stream_open(&stream_b, path_b);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mix open failed for path_b=%s: %s", path_b, esp_err_to_name(ret));
        goto cleanup;
    }

    ret = audio_service_mix_stream_ensure_info(&stream_a);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mix info failed for path_a=%s: %s", path_a, esp_err_to_name(ret));
        goto cleanup;
    }
    ret = audio_service_mix_stream_ensure_info(&stream_b);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mix info failed for path_b=%s: %s", path_b, esp_err_to_name(ret));
        goto cleanup;
    }
    stream_a.output_sample_rate = stream_a.sample_rate;
    stream_b.output_sample_rate = stream_a.sample_rate;

    ESP_LOGI(TAG,
             "Mix playback start: a=%s format=%s rate=%" PRIu32 " channels=%u b=%s format=%s rate=%" PRIu32 " channels=%u output_rate=%" PRIu32 " volume=%u",
             path_a,
             audio_service_format_name(stream_a.format),
             stream_a.sample_rate,
             stream_a.channels,
             path_b,
             audio_service_format_name(stream_b.format),
             stream_b.sample_rate,
             stream_b.channels,
             stream_a.output_sample_rate,
             (unsigned int)volume_percent);

    ret = audio_service_ensure_i2s(stream_a.sample_rate);
    if (ret != ESP_OK) {
        goto cleanup;
    }

    mix_buffer = calloc(AUDIO_SERVICE_STEREO_CHUNK_SAMPLES, sizeof(*mix_buffer));
    if (mix_buffer == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    while (!audio_service_should_stop()) {
        size_t frame_index = 0;

        for (frame_index = 0; frame_index < AUDIO_SERVICE_STEREO_CHUNK_FRAMES; ++frame_index) {
            int16_t left_a = 0;
            int16_t right_a = 0;
            int16_t left_b = 0;
            int16_t right_b = 0;
            bool has_a = false;
            bool has_b = false;

            if (audio_service_should_stop()) {
                frame_index = 0;
                break;
            }
            ret = audio_service_mix_stream_read_output_frame(&stream_a, &left_a, &right_a, &has_a);
            if (ret != ESP_OK) {
                goto cleanup;
            }
            ret = audio_service_mix_stream_read_output_frame(&stream_b, &left_b, &right_b, &has_b);
            if (ret != ESP_OK) {
                goto cleanup;
            }
            if (!has_a && !has_b) {
                break;
            }

            mix_buffer[frame_index * 2U] = audio_service_mix_sample(left_a, has_a, left_b, has_b);
            mix_buffer[(frame_index * 2U) + 1U] = audio_service_mix_sample(right_a, has_a, right_b, has_b);
        }

        if (frame_index == 0U) {
            break;
        }

        audio_service_scale_buffer(mix_buffer, frame_index * 2U, volume_percent);
        ret = audio_service_write_i2s(mix_buffer, frame_index * 2U * sizeof(*mix_buffer));
        if (ret != ESP_OK) {
            goto cleanup;
        }
    }

cleanup:
    free(mix_buffer);
    audio_service_mix_stream_close(&stream_b);
    audio_service_mix_stream_close(&stream_a);
    audio_service_finish_output();
    audio_service_log_heap("mix_after_cleanup");
    audio_service_end_playback();

    ESP_LOGI(TAG, "Mix playback end: path_a=%s path_b=%s result=%s", path_a, path_b, esp_err_to_name(ret));
    return ret;
}

esp_err_t audio_service_init(void)
{
    esp_err_t ret = ESP_OK;

    if (!s_decoder_registered) {
        esp_mp3_dec_register();
        s_decoder_registered = true;
    }
    ret = audio_service_ensure_i2s(AUDIO_SERVICE_DEFAULT_SAMPLE_RATE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S preinit failed: %s", esp_err_to_name(ret));
        return ret;
    }
    audio_service_finish_output();
    s_initialized = true;
    return ESP_OK;
}

esp_err_t audio_service_play(const char *path,
                             audio_service_format_t format,
                             uint8_t volume_percent,
                             uint32_t max_duration_ms)
{
    esp_err_t ret = ESP_OK;

    if ((path == NULL) || (path[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (volume_percent > 100) {
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

    audio_service_begin_playback();
    switch (format) {
        case AUDIO_SERVICE_FORMAT_WAV:
            ret = audio_service_play_wav(path, volume_percent, max_duration_ms);
            break;
        case AUDIO_SERVICE_FORMAT_MP3:
            ret = audio_service_play_mp3(path, volume_percent, max_duration_ms);
            break;
        case AUDIO_SERVICE_FORMAT_AUTO:
        default:
            ret = ESP_ERR_NOT_SUPPORTED;
            break;
    }
    audio_service_end_playback();
    return ret;
}

esp_err_t audio_service_stop(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_playback_active) {
        ESP_LOGI(TAG, "Stop requested with no active playback");
        return ESP_OK;
    }

    s_stop_requested = true;
    ESP_LOGI(TAG, "Stop requested for active playback");
    return ESP_OK;
}

bool audio_service_is_ready(void)
{
    return s_initialized;
}
