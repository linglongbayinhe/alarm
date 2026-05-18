#include "audio_cache_service.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage_service.h"

#if !defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY) || !CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
#include "esp_crt_bundle.h"
#endif

static const char *TAG = "AUDIO_CACHE";

#define AUDIO_CACHE_HTTP_TIMEOUT_MS 30000
#define AUDIO_CACHE_DOWNLOAD_BUFFER_SIZE 1024
#define AUDIO_CACHE_DOWNLOAD_MARGIN_BYTES 4096
#define AUDIO_CACHE_DOWNLOAD_HEAP_FREE_MIN 42000
#define AUDIO_CACHE_DOWNLOAD_HEAP_LARGEST_MIN 24000
#define AUDIO_CACHE_DOWNLOAD_MAX_ATTEMPTS 3
#define AUDIO_CACHE_DOWNLOAD_RETRY_DELAY_MS 500
#define AUDIO_CACHE_HTTP_RX_BUFFER_SIZE 4096
#define AUDIO_CACHE_HTTP_TX_BUFFER_SIZE 1024
#define AUDIO_CACHE_REDIRECT_MAX_COUNT 3

typedef struct {
    char *location;
    size_t location_size;
} audio_cache_http_context_t;

typedef struct {
    char request_url[AUDIO_CACHE_URL_SIZE];
    char canonical_url[AUDIO_CACHE_CANONICAL_URL_SIZE];
    char audio_id[AUDIO_CACHE_ID_SIZE];
    char local_path[AUDIO_CACHE_PATH_MAX];
    int64_t ring_at_epoch;
    bool cached;
} audio_cache_plan_item_t;

typedef struct {
    char request_url[AUDIO_CACHE_URL_SIZE];
    char canonical_url[AUDIO_CACHE_CANONICAL_URL_SIZE];
    char audio_id[AUDIO_CACHE_ID_SIZE];
    char local_path[AUDIO_CACHE_PATH_MAX];
    bool cached;
} audio_cache_download_result_t;

static bool s_ready;

uint32_t audio_cache_service_hash_url(const char *url)
{
    uint32_t hash = 2166136261U;

    if (url == NULL) {
        return hash;
    }

    while (*url != '\0') {
        hash ^= (uint8_t)*url;
        hash *= 16777619U;
        ++url;
    }

    return hash;
}

static void audio_cache_copy_string(char *destination, size_t destination_size, const char *source)
{
    if ((destination == NULL) || (destination_size == 0)) {
        return;
    }

    snprintf(destination, destination_size, "%s", source == NULL ? "" : source);
}

static const char *audio_cache_detect_extension(const char *url)
{
    const char *query = NULL;
    const char *last_dot = NULL;
    const char *last_slash = NULL;

    if (url == NULL) {
        return ".mp3";
    }

    query = strchr(url, '?');
    last_dot = strrchr(url, '.');
    last_slash = strrchr(url, '/');
    if ((last_dot == NULL) || ((last_slash != NULL) && (last_dot < last_slash))) {
        return ".mp3";
    }
    if ((query != NULL) && (last_dot > query)) {
        return ".mp3";
    }

    if (strncasecmp(last_dot, ".wav", 4) == 0) {
        return ".wav";
    }
    if (strncasecmp(last_dot, ".mp3", 4) == 0) {
        return ".mp3";
    }

    return ".mp3";
}

static bool audio_cache_is_id_char(char ch)
{
    return isalnum((unsigned char)ch) || (ch == '_') || (ch == '-');
}

static bool audio_cache_extract_audio_id(const char *canonical_url, char *audio_id, size_t audio_id_size)
{
    const char *filename = NULL;
    const char *end = NULL;
    size_t length = 0;
    size_t index = 0;

    if ((canonical_url == NULL) || (audio_id == NULL) || (audio_id_size == 0)) {
        return false;
    }

    filename = strrchr(canonical_url, '/');
    filename = filename == NULL ? canonical_url : filename + 1;
    end = strchr(filename, '?');
    if (end == NULL) {
        end = filename + strlen(filename);
    }
    while ((end > filename) && (*(end - 1) != '.')) {
        --end;
    }
    if ((end > filename) && (*(end - 1) == '.')) {
        --end;
    } else {
        end = strchr(filename, '?');
        if (end == NULL) {
            end = filename + strlen(filename);
        }
    }

    length = (size_t)(end - filename);
    if ((length == 0) || (length >= audio_id_size)) {
        return false;
    }

    for (index = 0; index < length; ++index) {
        if (!audio_cache_is_id_char(filename[index])) {
            return false;
        }
    }

    memcpy(audio_id, filename, length);
    audio_id[length] = '\0';
    return true;
}

static void audio_cache_fallback_audio_id(const char *canonical_url, char *audio_id, size_t audio_id_size)
{
    snprintf(audio_id,
             audio_id_size,
             "url_%08" PRIx32,
             audio_cache_service_hash_url(canonical_url));
}

static esp_err_t audio_cache_build_path(const char *audio_id,
                                        const char *extension,
                                        char *path_buffer,
                                        size_t path_buffer_size)
{
    if ((audio_id == NULL) || (audio_id[0] == '\0') ||
        (extension == NULL) || (path_buffer == NULL) || (path_buffer_size == 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (snprintf(path_buffer,
                 path_buffer_size,
                 "%s/%s%s",
                 STORAGE_SERVICE_EXTERNAL_AUDIO_DIR,
                 audio_id,
                 extension) >= (int)path_buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static uint64_t audio_cache_get_free_bytes(void)
{
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;

    if (!s_ready ||
        (esp_vfs_fat_info(STORAGE_SERVICE_EXTERNAL_BASE_PATH, &total_bytes, &free_bytes) != ESP_OK)) {
        return 0;
    }

    return free_bytes;
}

static bool audio_cache_path_is_kept(const char *path,
                                     const char *const *keep_paths,
                                     size_t keep_path_count,
                                     const char *protected_path)
{
    size_t index = 0;

    if ((protected_path != NULL) && (protected_path[0] != '\0') &&
        (path != NULL) && (strcmp(path, protected_path) == 0)) {
        return true;
    }

    for (index = 0; index < keep_path_count; ++index) {
        if ((keep_paths[index] != NULL) && (strcmp(path, keep_paths[index]) == 0)) {
            return true;
        }
    }

    return false;
}

static esp_err_t audio_cache_cleanup_unused_protected(const char *const *keep_paths,
                                                      size_t keep_path_count,
                                                      const char *protected_path)
{
    DIR *directory = NULL;
    struct dirent *entry = NULL;
    char full_path[AUDIO_CACHE_PATH_MAX] = {0};

    if (!s_ready) {
        return ESP_OK;
    }

    directory = opendir(STORAGE_SERVICE_EXTERNAL_AUDIO_DIR);
    if (directory == NULL) {
        return ESP_OK;
    }

    while ((entry = readdir(directory)) != NULL) {
        if ((strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0)) {
            continue;
        }
        if (snprintf(full_path,
                     sizeof(full_path),
                     "%s/%s",
                     STORAGE_SERVICE_EXTERNAL_AUDIO_DIR,
                     entry->d_name) >= (int)sizeof(full_path)) {
            continue;
        }
        if (!audio_cache_path_is_kept(full_path, keep_paths, keep_path_count, protected_path)) {
            ESP_LOGI(TAG, "Removing unused cached audio %s", full_path);
            remove(full_path);
        }
    }

    closedir(directory);
    return ESP_OK;
}

esp_err_t audio_cache_service_init(void)
{
    s_ready = storage_service_external_cache_available();
    return ESP_OK;
}

bool audio_cache_service_is_ready(void)
{
    return s_ready;
}

bool audio_cache_service_file_exists(const char *path)
{
    struct stat info = {0};

    if (path == NULL) {
        return false;
    }

    return stat(path, &info) == 0;
}

static bool audio_cache_is_redirect_status(int status_code)
{
    return (status_code == 301) ||
           (status_code == 302) ||
           (status_code == 303) ||
           (status_code == 307) ||
           (status_code == 308);
}

static esp_err_t audio_cache_http_event_handler(esp_http_client_event_t *event)
{
    audio_cache_http_context_t *context = NULL;

    if ((event == NULL) || (event->event_id != HTTP_EVENT_ON_HEADER)) {
        return ESP_OK;
    }

    context = (audio_cache_http_context_t *)event->user_data;
    if ((context == NULL) ||
        (context->location == NULL) ||
        (context->location_size == 0) ||
        (event->header_key == NULL) ||
        (event->header_value == NULL)) {
        return ESP_OK;
    }

    if (strcasecmp(event->header_key, "Location") == 0) {
        snprintf(context->location, context->location_size, "%s", event->header_value);
    }

    return ESP_OK;
}

static bool audio_cache_download_heap_ready(const char *download_url)
{
    uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    if ((free_heap >= AUDIO_CACHE_DOWNLOAD_HEAP_FREE_MIN) &&
        (largest >= AUDIO_CACHE_DOWNLOAD_HEAP_LARGEST_MIN)) {
        return true;
    }

    ESP_LOGW(TAG,
             "Audio download heap insufficient: free=%u/%u largest=%u/%u url=%s",
             (unsigned int)free_heap,
             (unsigned int)AUDIO_CACHE_DOWNLOAD_HEAP_FREE_MIN,
             (unsigned int)largest,
             (unsigned int)AUDIO_CACHE_DOWNLOAD_HEAP_LARGEST_MIN,
             download_url == NULL ? "" : download_url);
    return false;
}

static bool audio_cache_disable_wifi_power_save(wifi_ps_type_t *previous_ps)
{
    esp_err_t ret = ESP_OK;

    if (previous_ps == NULL) {
        return false;
    }

    ret = esp_wifi_get_ps(previous_ps);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not read WiFi power save before audio download: %s", esp_err_to_name(ret));
        return false;
    }
    if (*previous_ps == WIFI_PS_NONE) {
        return false;
    }

    ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not disable WiFi power save for audio download: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "WiFi power save disabled for audio download: previous=%d", (int)*previous_ps);
    return true;
}

static void audio_cache_restore_wifi_power_save(wifi_ps_type_t previous_ps, bool should_restore)
{
    esp_err_t ret = ESP_OK;

    if (!should_restore) {
        return;
    }

    ret = esp_wifi_set_ps(previous_ps);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not restore WiFi power save after audio download: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "WiFi power save restored after audio download: mode=%d", (int)previous_ps);
}

static esp_err_t audio_cache_prepare_download_result(const char *request_url,
                                                     const char *canonical_url,
                                                     audio_cache_download_result_t *result)
{
    const char *extension = NULL;

    if ((request_url == NULL) || (canonical_url == NULL) || (result == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_cache_copy_string(result->request_url, sizeof(result->request_url), request_url);
    audio_cache_copy_string(result->canonical_url, sizeof(result->canonical_url), canonical_url);
    if (!audio_cache_extract_audio_id(canonical_url, result->audio_id, sizeof(result->audio_id))) {
        audio_cache_fallback_audio_id(canonical_url, result->audio_id, sizeof(result->audio_id));
    }

    extension = audio_cache_detect_extension(canonical_url);
    return audio_cache_build_path(result->audio_id, extension, result->local_path, sizeof(result->local_path));
}

static esp_err_t audio_cache_download_once(const char *request_url,
                                           audio_cache_download_result_t *result,
                                           int attempt)
{
    esp_http_client_config_t client_config = {0};
    esp_http_client_handle_t client = NULL;
    FILE *file = NULL;
    char *redirect_url = NULL;
    char *current_url = NULL;
    char temp_path[AUDIO_CACHE_PATH_MAX] = {0};
    audio_cache_http_context_t http_context = {0};
    const char *active_url = request_url;
    uint8_t *download_buffer = NULL;
    esp_err_t ret = ESP_OK;
    size_t bytes_written_total = 0;
    int redirect_count = 0;

    if ((request_url == NULL) || (request_url[0] == '\0') || (result == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    audio_cache_copy_string(result->request_url, sizeof(result->request_url), request_url);

    redirect_url = calloc(1, AUDIO_CACHE_CANONICAL_URL_SIZE);
    current_url = calloc(1, AUDIO_CACHE_CANONICAL_URL_SIZE);
    if ((redirect_url == NULL) || (current_url == NULL)) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

redirect:
    memset(&client_config, 0, sizeof(client_config));
    redirect_url[0] = '\0';
    http_context.location = redirect_url;
    http_context.location_size = AUDIO_CACHE_CANONICAL_URL_SIZE;
    client_config.url = active_url;
    client_config.method = HTTP_METHOD_GET;
    client_config.timeout_ms = AUDIO_CACHE_HTTP_TIMEOUT_MS;
    client_config.buffer_size = AUDIO_CACHE_HTTP_RX_BUFFER_SIZE;
    client_config.buffer_size_tx = AUDIO_CACHE_HTTP_TX_BUFFER_SIZE;
    client_config.disable_auto_redirect = true;
    client_config.max_redirection_count = AUDIO_CACHE_REDIRECT_MAX_COUNT;
    client_config.event_handler = audio_cache_http_event_handler;
    client_config.user_data = &http_context;
#if defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY) && CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    client_config.skip_cert_common_name_check = true;
#else
    client_config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    client = esp_http_client_init(&client_config);
    if (client == NULL) {
        ret = ESP_FAIL;
        goto cleanup;
    }
    (void)esp_http_client_set_header(client, "User-Agent", "ESP32-Alarm/1.0");
    (void)esp_http_client_set_header(client, "Accept", "audio/mpeg,*/*");
    (void)esp_http_client_set_header(client, "Accept-Encoding", "identity");
    (void)esp_http_client_set_header(client, "Connection", "close");

    ESP_LOGI(TAG, "Download attempt %d start: url=%s", attempt, active_url);
    errno = 0;
    ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Open download failed: attempt=%d ret=%s errno=%d(%s)",
                 attempt,
                 esp_err_to_name(ret),
                 errno,
                 strerror(errno));
        goto cleanup;
    }
    ESP_LOGI(TAG, "Download connection opened: attempt=%d", attempt);

    {
        errno = 0;
        int64_t header_len = esp_http_client_fetch_headers(client);
        if (header_len < 0) {
            ESP_LOGE(TAG,
                     "Fetch download headers failed: attempt=%d header_len=%" PRId64
                     " status=%d errno=%d(%s)",
                     attempt,
                     header_len,
                     esp_http_client_get_status_code(client),
                     errno,
                     strerror(errno));
            ret = ESP_FAIL;
            goto cleanup;
        }
    }

    {
        int status_code = esp_http_client_get_status_code(client);
        if (audio_cache_is_redirect_status(status_code)) {
            if (redirect_count >= AUDIO_CACHE_REDIRECT_MAX_COUNT) {
                ESP_LOGE(TAG,
                         "Download redirect limit reached: attempt=%d status=%d url=%s",
                         attempt,
                         status_code,
                         active_url);
                ret = ESP_ERR_HTTP_MAX_REDIRECT;
                goto cleanup;
            }
            if (redirect_url[0] == '\0') {
                ESP_LOGE(TAG,
                         "Download redirect missing Location: attempt=%d status=%d url=%s",
                         attempt,
                         status_code,
                         active_url);
                ret = ESP_FAIL;
                goto cleanup;
            }
            if ((strncmp(redirect_url, "https://", 8) != 0) &&
                (strncmp(redirect_url, "http://", 7) != 0)) {
                ESP_LOGE(TAG,
                         "Download redirect Location is not absolute: attempt=%d status=%d location=%s",
                         attempt,
                         status_code,
                         redirect_url);
                ret = ESP_ERR_INVALID_RESPONSE;
                goto cleanup;
            }

            ESP_LOGI(TAG,
                     "Download redirect: attempt=%d status=%d location=%s",
                     attempt,
                     status_code,
                     redirect_url);
            if (snprintf(current_url, AUDIO_CACHE_CANONICAL_URL_SIZE, "%s", redirect_url) >=
                AUDIO_CACHE_CANONICAL_URL_SIZE) {
                ESP_LOGE(TAG,
                         "Download redirect Location too long: attempt=%d status=%d max=%u",
                         attempt,
                         status_code,
                         (unsigned int)(AUDIO_CACHE_CANONICAL_URL_SIZE - 1U));
                ret = ESP_ERR_INVALID_SIZE;
                goto cleanup;
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = NULL;
            active_url = current_url;
            ++redirect_count;
            goto redirect;
        }
    }

    if (esp_http_client_get_status_code(client) / 100 != 2) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 409) {
            ESP_LOGW(TAG,
                     "Download audio not ready: attempt=%d status=%d requestUrl=%s",
                     attempt,
                     status_code,
                     active_url);
            ret = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
        ESP_LOGE(TAG,
                 "Download HTTP status rejected: attempt=%d status=%d url=%s",
                 attempt,
                 status_code,
                 active_url);
        ret = ESP_FAIL;
        goto cleanup;
    }

    ret = audio_cache_prepare_download_result(request_url, active_url, result);
    if (ret != ESP_OK) {
        goto cleanup;
    }

    if (audio_cache_service_file_exists(result->local_path)) {
        result->cached = true;
        ESP_LOGI(TAG,
                 "Cache hit: audioId=%s path=%s requestUrl=%s canonicalUrl=%s",
                 result->audio_id,
                 result->local_path,
                 request_url,
                 result->canonical_url);
        ret = ESP_OK;
        goto cleanup;
    }

    {
        int64_t content_length = esp_http_client_get_content_length(client);
        char *content_type = NULL;

        if (esp_http_client_get_header(client, "Content-Type", &content_type) == ESP_OK) {
            ESP_LOGI(TAG,
                     "Download content type: attempt=%d type=%s path=%s",
                     attempt,
                     content_type,
                     result->local_path);
            if ((content_type != NULL) && (strstr(content_type, "audio/mpeg") == NULL)) {
                ESP_LOGW(TAG,
                         "Unexpected audio content type: %s path=%s",
                         content_type,
                         result->local_path);
            }
        }

        ESP_LOGI(TAG,
                 "Download headers: attempt=%d status=%d content_length=%" PRId64
                 " free=%" PRIu64 " audioId=%s path=%s",
                 attempt,
                 esp_http_client_get_status_code(client),
                 content_length,
                 audio_cache_get_free_bytes(),
                 result->audio_id,
                 result->local_path);
        if (content_length > 0) {
            uint64_t free_bytes = audio_cache_get_free_bytes();
            if ((free_bytes > 0) &&
                (((uint64_t)content_length + AUDIO_CACHE_DOWNLOAD_MARGIN_BYTES) > free_bytes)) {
                ESP_LOGW(TAG,
                         "Not enough cache space for %s: need=%" PRIu64 " free=%" PRIu64,
                         result->local_path,
                         (uint64_t)content_length,
                         free_bytes);
                ret = ESP_ERR_NO_MEM;
                goto cleanup;
            }
        }
    }

    if (snprintf(temp_path, sizeof(temp_path), "%s.part", result->local_path) >= (int)sizeof(temp_path)) {
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    file = fopen(temp_path, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Open temp audio file failed: errno=%d(%s) path=%s", errno, strerror(errno), temp_path);
        ret = ESP_FAIL;
        goto cleanup;
    }

    download_buffer = calloc(1, AUDIO_CACHE_DOWNLOAD_BUFFER_SIZE);
    if (download_buffer == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    while (true) {
        errno = 0;
        int bytes_read = esp_http_client_read(client,
                                              (char *)download_buffer,
                                              AUDIO_CACHE_DOWNLOAD_BUFFER_SIZE);
        if (bytes_read < 0) {
            ESP_LOGE(TAG,
                     "Read download body failed: attempt=%d bytes=%d written=%u errno=%d(%s) path=%s",
                     attempt,
                     bytes_read,
                     (unsigned int)bytes_written_total,
                     errno,
                     strerror(errno),
                     result->local_path);
            ret = ESP_FAIL;
            goto cleanup;
        }
        if (bytes_read == 0) {
            break;
        }
        errno = 0;
        if (fwrite(download_buffer, 1, (size_t)bytes_read, file) != (size_t)bytes_read) {
            ESP_LOGE(TAG,
                     "Write download file failed: attempt=%d bytes=%d written=%u free=%" PRIu64
                     " errno=%d(%s) requestUrl=%s canonicalUrl=%s path=%s",
                     attempt,
                     bytes_read,
                     (unsigned int)bytes_written_total,
                     audio_cache_get_free_bytes(),
                     errno,
                     strerror(errno),
                     request_url,
                     result->canonical_url,
                     temp_path);
            ret = ESP_FAIL;
            goto cleanup;
        }
        bytes_written_total += (size_t)bytes_read;
    }

    fclose(file);
    file = NULL;

    remove(result->local_path);
    if (rename(temp_path, result->local_path) != 0) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    ESP_LOGI(TAG,
             "Download finished: attempt=%d audioId=%s path=%s bytes=%u requestUrl=%s canonicalUrl=%s",
             attempt,
             result->audio_id,
             result->local_path,
             (unsigned int)bytes_written_total,
             request_url,
             result->canonical_url);

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    if ((ret != ESP_OK) && (temp_path[0] != '\0')) {
        remove(temp_path);
    }
    free(download_buffer);
    free(redirect_url);
    free(current_url);
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    return ret;
}

static void audio_cache_notify_result(audio_cache_maintenance_result_cb_t result_cb,
                                      void *result_ctx,
                                      const audio_cache_download_result_t *result,
                                      esp_err_t ret)
{
    if (result_cb != NULL) {
        result_cb(result == NULL ? "" : result->request_url,
                  result == NULL ? "" : result->canonical_url,
                  result == NULL ? "" : result->audio_id,
                  ret,
                  result == NULL ? "" : result->local_path,
                  result_ctx);
    }
}

static int audio_cache_plan_compare_ring_time(const void *left, const void *right)
{
    const audio_cache_plan_item_t *left_item = (const audio_cache_plan_item_t *)left;
    const audio_cache_plan_item_t *right_item = (const audio_cache_plan_item_t *)right;

    if (left_item->ring_at_epoch < right_item->ring_at_epoch) {
        return -1;
    }
    if (left_item->ring_at_epoch > right_item->ring_at_epoch) {
        return 1;
    }
    return strcmp(left_item->request_url, right_item->request_url);
}

static bool audio_cache_plan_contains_request(const audio_cache_plan_item_t *items,
                                              size_t item_count,
                                              const char *request_url)
{
    size_t index = 0;

    if (request_url == NULL) {
        return false;
    }

    for (index = 0; index < item_count; ++index) {
        if (strcmp(items[index].request_url, request_url) == 0) {
            return true;
        }
    }

    return false;
}

static size_t audio_cache_collect_known_keep_paths(const audio_cache_plan_item_t *plan,
                                                   size_t plan_count,
                                                   const char **keep_paths,
                                                   size_t keep_path_capacity)
{
    size_t index = 0;
    size_t keep_count = 0;

    for (index = 0; (index < plan_count) && (keep_count < keep_path_capacity); ++index) {
        if (plan[index].local_path[0] == '\0') {
            continue;
        }
        keep_paths[keep_count++] = plan[index].local_path;
    }

    return keep_count;
}

static esp_err_t audio_cache_download_with_reclaim(audio_cache_plan_item_t *item,
                                                   const audio_cache_plan_item_t *plan,
                                                   size_t plan_count,
                                                   const char *protected_path)
{
    audio_cache_download_result_t result = {0};
    esp_err_t ret = ESP_OK;
    int attempt = 0;
    bool cleanup_retried = false;
    const char **keep_paths = NULL;

    if (item == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!audio_cache_download_heap_ready(item->request_url)) {
        return ESP_ERR_NO_MEM;
    }

    for (attempt = 1; attempt <= AUDIO_CACHE_DOWNLOAD_MAX_ATTEMPTS; ++attempt) {
        ret = audio_cache_download_once(item->request_url, &result, attempt);
        if (result.canonical_url[0] != '\0') {
            audio_cache_copy_string(item->canonical_url, sizeof(item->canonical_url), result.canonical_url);
            audio_cache_copy_string(item->audio_id, sizeof(item->audio_id), result.audio_id);
            audio_cache_copy_string(item->local_path, sizeof(item->local_path), result.local_path);
            item->cached = result.cached || audio_cache_service_file_exists(result.local_path);
        }

        if (ret == ESP_ERR_NO_MEM && !cleanup_retried) {
            size_t keep_count = 0;
            keep_paths = calloc(plan_count + 1U, sizeof(*keep_paths));
            if (keep_paths != NULL) {
                keep_count = audio_cache_collect_known_keep_paths(plan, plan_count, keep_paths, plan_count + 1U);
                if ((item->local_path[0] != '\0') && (keep_count < (plan_count + 1U))) {
                    keep_paths[keep_count++] = item->local_path;
                }
                ESP_LOGI(TAG, "Reclaiming audio cache after no-space result keep=%u", (unsigned int)keep_count);
                (void)audio_cache_cleanup_unused_protected(keep_paths, keep_count, protected_path);
                free(keep_paths);
                keep_paths = NULL;
                cleanup_retried = true;
                vTaskDelay(pdMS_TO_TICKS(AUDIO_CACHE_DOWNLOAD_RETRY_DELAY_MS));
                continue;
            }
        }

        if (ret == ESP_OK) {
            break;
        }
        if ((ret == ESP_ERR_NO_MEM) || (ret == ESP_ERR_INVALID_STATE) ||
            (attempt >= AUDIO_CACHE_DOWNLOAD_MAX_ATTEMPTS)) {
            break;
        }
        ESP_LOGW(TAG,
                 "Download attempt %d failed: ret=%s; retrying in %u ms requestUrl=%s",
                 attempt,
                 esp_err_to_name(ret),
                 (unsigned int)AUDIO_CACHE_DOWNLOAD_RETRY_DELAY_MS,
                 item->request_url);
        vTaskDelay(pdMS_TO_TICKS(AUDIO_CACHE_DOWNLOAD_RETRY_DELAY_MS));
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "Download failed: attempts=%d ret=%s requestUrl=%s canonicalUrl=%s audioId=%s path=%s",
                 attempt > AUDIO_CACHE_DOWNLOAD_MAX_ATTEMPTS ? AUDIO_CACHE_DOWNLOAD_MAX_ATTEMPTS : attempt,
                 esp_err_to_name(ret),
                 item->request_url,
                 item->canonical_url,
                 item->audio_id,
                 item->local_path);
    }

    return ret;
}

esp_err_t audio_cache_service_maintain(const audio_cache_maintenance_item_t *items,
                                       size_t item_count,
                                       const char *protected_path,
                                       audio_cache_maintenance_result_cb_t result_cb,
                                       void *result_ctx)
{
    audio_cache_plan_item_t *plan = NULL;
    size_t plan_count = 0;
    size_t index = 0;
    esp_err_t final_ret = ESP_OK;
    bool download_heap_blocked = false;
    wifi_ps_type_t previous_wifi_ps = WIFI_PS_NONE;
    bool restore_wifi_ps = false;

    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((items == NULL) || (item_count == 0)) {
        return ESP_OK;
    }

    plan = calloc(item_count, sizeof(*plan));
    if (plan == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (index = 0; index < item_count; ++index) {
        if (items[index].download_url[0] == '\0') {
            continue;
        }
        if (audio_cache_plan_contains_request(plan, plan_count, items[index].download_url)) {
            continue;
        }
        audio_cache_copy_string(plan[plan_count].request_url,
                                sizeof(plan[plan_count].request_url),
                                items[index].download_url);
        plan[plan_count].ring_at_epoch = items[index].ring_at_epoch;
        ++plan_count;
    }

    if (plan_count > 1U) {
        qsort(plan, plan_count, sizeof(plan[0]), audio_cache_plan_compare_ring_time);
    }

    restore_wifi_ps = audio_cache_disable_wifi_power_save(&previous_wifi_ps);

    for (index = 0; index < plan_count; ++index) {
        esp_err_t ret = ESP_OK;
        audio_cache_download_result_t callback_result = {0};

        if (download_heap_blocked || !audio_cache_download_heap_ready(plan[index].request_url)) {
            download_heap_blocked = true;
            audio_cache_copy_string(callback_result.request_url,
                                    sizeof(callback_result.request_url),
                                    plan[index].request_url);
            audio_cache_notify_result(result_cb, result_ctx, &callback_result, ESP_ERR_NO_MEM);
            if (final_ret == ESP_OK) {
                final_ret = ESP_ERR_NO_MEM;
            }
            continue;
        }

        ret = audio_cache_download_with_reclaim(&plan[index], plan, plan_count, protected_path);
        audio_cache_copy_string(callback_result.request_url,
                                sizeof(callback_result.request_url),
                                plan[index].request_url);
        audio_cache_copy_string(callback_result.canonical_url,
                                sizeof(callback_result.canonical_url),
                                plan[index].canonical_url);
        audio_cache_copy_string(callback_result.audio_id,
                                sizeof(callback_result.audio_id),
                                plan[index].audio_id);
        audio_cache_copy_string(callback_result.local_path,
                                sizeof(callback_result.local_path),
                                plan[index].local_path);

        audio_cache_notify_result(result_cb, result_ctx, &callback_result, ret);
        if ((ret != ESP_OK) && (final_ret == ESP_OK)) {
            final_ret = ret;
        }
    }

    audio_cache_restore_wifi_power_save(previous_wifi_ps, restore_wifi_ps);
    free(plan);
    return final_ret;
}
