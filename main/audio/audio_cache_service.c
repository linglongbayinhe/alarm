#include "audio_cache_service.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
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
#define AUDIO_CACHE_DOWNLOAD_HEAP_FREE_MIN 50000
#define AUDIO_CACHE_DOWNLOAD_HEAP_LARGEST_MIN 24000
#define AUDIO_CACHE_DOWNLOAD_MAX_ATTEMPTS 3
#define AUDIO_CACHE_DOWNLOAD_RETRY_DELAY_MS 500
#define AUDIO_CACHE_HTTP_RX_BUFFER_SIZE 2048
#define AUDIO_CACHE_HTTP_TX_BUFFER_SIZE 1024
#define AUDIO_CACHE_REDIRECT_MAX_COUNT 3
#define AUDIO_CACHE_REDIRECT_URL_SIZE 1536

static bool s_ready;

static bool audio_cache_download_heap_ready(const char *audio_url);
static bool audio_cache_disable_wifi_power_save(wifi_ps_type_t *previous_ps);
static void audio_cache_restore_wifi_power_save(wifi_ps_type_t previous_ps, bool should_restore);
static bool audio_cache_is_redirect_status(int status_code);
static esp_err_t audio_cache_http_event_handler(esp_http_client_event_t *event);
static esp_err_t audio_cache_download_once(const char *audio_url,
                                           const char *final_path,
                                           const char *temp_path,
                                           int attempt);

typedef struct {
    char *location;
    size_t location_size;
} audio_cache_http_context_t;

typedef struct {
    char audio_url[AUDIO_CACHE_URL_SIZE];
    char path[AUDIO_CACHE_PATH_MAX];
    int64_t ring_at_epoch;
    bool cached;
} audio_cache_plan_item_t;

uint32_t audio_cache_service_hash_url(const char *audio_url)
{
    uint32_t hash = 2166136261U;

    if (audio_url == NULL) {
        return hash;
    }

    while (*audio_url != '\0') {
        hash ^= (uint8_t)*audio_url;
        hash *= 16777619U;
        ++audio_url;
    }

    return hash;
}

static const char *audio_cache_detect_extension(const char *audio_url)
{
    const char *query = NULL;
    const char *last_dot = NULL;
    const char *last_slash = NULL;

    if (audio_url == NULL) {
        return ".mp3";
    }

    query = strchr(audio_url, '?');
    last_dot = strrchr(audio_url, '.');
    last_slash = strrchr(audio_url, '/');
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

esp_err_t audio_cache_service_resolve_path(const char *instance_id,
                                           const char *audio_url,
                                           char *path_buffer,
                                           size_t path_buffer_size)
{
    const char *extension = NULL;
    uint32_t audio_hash = 0;

    (void)instance_id;

    if ((audio_url == NULL) || (audio_url[0] == '\0') ||
        (path_buffer == NULL) || (path_buffer_size == 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    extension = audio_cache_detect_extension(audio_url);
    audio_hash = audio_cache_service_hash_url(audio_url);
    if (snprintf(path_buffer,
                 path_buffer_size,
                 "%s/%08" PRIx32 "%s",
                 STORAGE_SERVICE_EXTERNAL_AUDIO_DIR,
                 audio_hash,
                 extension) >= (int)path_buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

bool audio_cache_service_find_existing(const char *instance_id,
                                       const char *audio_url,
                                       char *path_buffer,
                                       size_t path_buffer_size)
{
    char resolved_path[AUDIO_CACHE_PATH_MAX] = {0};
    int written = 0;

    if (audio_cache_service_resolve_path(instance_id,
                                         audio_url,
                                         resolved_path,
                                         sizeof(resolved_path)) != ESP_OK) {
        return false;
    }
    if (!audio_cache_service_file_exists(resolved_path)) {
        return false;
    }

    if ((path_buffer != NULL) && (path_buffer_size > 0)) {
        written = snprintf(path_buffer, path_buffer_size, "%s", resolved_path);
        if ((written < 0) || ((size_t)written >= path_buffer_size)) {
            return false;
        }
    }

    return true;
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

static esp_err_t audio_cache_download_once(const char *audio_url,
                                           const char *final_path,
                                           const char *temp_path,
                                           int attempt)
{
    esp_http_client_config_t client_config = {0};
    esp_http_client_handle_t client = NULL;
    FILE *file = NULL;
    char *redirect_url = NULL;
    char *current_url = NULL;
    audio_cache_http_context_t http_context = {0};
    const char *request_url = audio_url;
    uint8_t *download_buffer = NULL;
    esp_err_t ret = ESP_OK;
    size_t bytes_written_total = 0;
    int redirect_count = 0;

    redirect_url = calloc(1, AUDIO_CACHE_REDIRECT_URL_SIZE);
    if (redirect_url == NULL) {
        return ESP_ERR_NO_MEM;
    }
    current_url = calloc(1, AUDIO_CACHE_REDIRECT_URL_SIZE);
    if (current_url == NULL) {
        free(redirect_url);
        return ESP_ERR_NO_MEM;
    }

redirect:
    memset(&client_config, 0, sizeof(client_config));
    redirect_url[0] = '\0';
    http_context.location = redirect_url;
    http_context.location_size = AUDIO_CACHE_REDIRECT_URL_SIZE;
    client_config.url = request_url;
    client_config.method = HTTP_METHOD_GET;
    client_config.timeout_ms = AUDIO_CACHE_HTTP_TIMEOUT_MS;
    client_config.buffer_size = AUDIO_CACHE_HTTP_RX_BUFFER_SIZE;
    client_config.buffer_size_tx = AUDIO_CACHE_HTTP_TX_BUFFER_SIZE;
    client_config.disable_auto_redirect = false;
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

    ESP_LOGI(TAG,
             "Download attempt %d start: url=%s path=%s",
             attempt,
             request_url,
             final_path);
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
    ESP_LOGI(TAG, "Download connection opened: attempt=%d path=%s", attempt, final_path);

    {
        errno = 0;
        int64_t header_len = esp_http_client_fetch_headers(client);
        if (header_len < 0) {
            ESP_LOGE(TAG,
                     "Fetch download headers failed: attempt=%d header_len=%" PRId64
                     " status=%d errno=%d(%s) path=%s",
                     attempt,
                     header_len,
                     esp_http_client_get_status_code(client),
                     errno,
                     strerror(errno),
                     final_path);
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
                         request_url);
                ret = ESP_ERR_HTTP_MAX_REDIRECT;
                goto cleanup;
            }
            if (redirect_url[0] == '\0') {
                ESP_LOGE(TAG,
                         "Download redirect missing Location: attempt=%d status=%d url=%s",
                         attempt,
                         status_code,
                         request_url);
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
            if (snprintf(current_url, AUDIO_CACHE_REDIRECT_URL_SIZE, "%s", redirect_url) >=
                AUDIO_CACHE_REDIRECT_URL_SIZE) {
                ESP_LOGE(TAG,
                         "Download redirect Location too long: attempt=%d status=%d max=%u",
                         attempt,
                         status_code,
                         (unsigned int)(AUDIO_CACHE_REDIRECT_URL_SIZE - 1U));
                ret = ESP_ERR_INVALID_SIZE;
                goto cleanup;
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = NULL;
            request_url = current_url;
            ++redirect_count;
            goto redirect;
        }
    }

    if (esp_http_client_get_status_code(client) / 100 != 2) {
        ESP_LOGE(TAG,
                 "Download HTTP status rejected: attempt=%d status=%d path=%s",
                 attempt,
                 esp_http_client_get_status_code(client),
                 final_path);
        ret = ESP_FAIL;
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
                     final_path);
            if ((content_type != NULL) && (strstr(content_type, "audio/mpeg") == NULL)) {
                ESP_LOGW(TAG,
                         "Unexpected audio content type: %s path=%s",
                         content_type,
                         final_path);
            }
        }

        ESP_LOGI(TAG,
                 "Download headers: attempt=%d status=%d content_length=%" PRId64 " path=%s",
                 attempt,
                 esp_http_client_get_status_code(client),
                 content_length,
                 final_path);
        if (content_length > 0) {
            uint64_t free_bytes = audio_cache_get_free_bytes();
            if ((free_bytes > 0) &&
                (((uint64_t)content_length + AUDIO_CACHE_DOWNLOAD_MARGIN_BYTES) > free_bytes)) {
                ESP_LOGW(TAG,
                         "Not enough cache space for %s: need=%" PRIu64 " free=%" PRIu64,
                         final_path,
                         (uint64_t)content_length,
                         free_bytes);
                ret = ESP_ERR_NO_MEM;
                goto cleanup;
            }
        }
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
                     final_path);
            ret = ESP_FAIL;
            goto cleanup;
        }
        if (bytes_read == 0) {
            break;
        }
        errno = 0;
        if (fwrite(download_buffer, 1, (size_t)bytes_read, file) != (size_t)bytes_read) {
            ESP_LOGE(TAG,
                     "Write download file failed: attempt=%d bytes=%d written=%u errno=%d(%s) path=%s",
                     attempt,
                     bytes_read,
                     (unsigned int)bytes_written_total,
                     errno,
                     strerror(errno),
                     temp_path);
            ret = ESP_FAIL;
            goto cleanup;
        }
        bytes_written_total += (size_t)bytes_read;
    }

    fclose(file);
    file = NULL;

    remove(final_path);
    if (rename(temp_path, final_path) != 0) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    ESP_LOGI(TAG,
             "Download finished: attempt=%d path=%s bytes=%u url=%s",
             attempt,
             final_path,
             (unsigned int)bytes_written_total,
             audio_url);

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    if (ret != ESP_OK) {
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

esp_err_t audio_cache_service_download(const char *instance_id,
                                       const char *audio_url,
                                       char *path_buffer,
                                       size_t path_buffer_size)
{
    char final_path[AUDIO_CACHE_PATH_MAX] = {0};
    char temp_path[AUDIO_CACHE_PATH_MAX] = {0};
    esp_err_t ret = ESP_OK;
    wifi_ps_type_t previous_wifi_ps = WIFI_PS_NONE;
    bool restore_wifi_ps = false;
    int attempt = 0;

    if ((audio_url == NULL) || (audio_url[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = audio_cache_service_resolve_path(instance_id, audio_url, final_path, sizeof(final_path));
    if (ret != ESP_OK) {
        return ret;
    }
    if ((path_buffer != NULL) && (path_buffer_size > 0)) {
        snprintf(path_buffer, path_buffer_size, "%s", final_path);
    }

    if (audio_cache_service_file_exists(final_path)) {
        ESP_LOGI(TAG, "Cache hit: path=%s url=%s", final_path, audio_url);
        return ESP_OK;
    }

    if (snprintf(temp_path, sizeof(temp_path), "%s.part", final_path) >= (int)sizeof(temp_path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!audio_cache_download_heap_ready(audio_url)) {
        return ESP_ERR_NO_MEM;
    }
    restore_wifi_ps = audio_cache_disable_wifi_power_save(&previous_wifi_ps);

    for (attempt = 1; attempt <= AUDIO_CACHE_DOWNLOAD_MAX_ATTEMPTS; ++attempt) {
        ret = audio_cache_download_once(audio_url, final_path, temp_path, attempt);
        if (ret == ESP_OK) {
            break;
        }
        if ((ret == ESP_ERR_NO_MEM) || (attempt >= AUDIO_CACHE_DOWNLOAD_MAX_ATTEMPTS)) {
            break;
        }
        ESP_LOGW(TAG,
                 "Download attempt %d failed: ret=%s; retrying in %u ms path=%s",
                 attempt,
                 esp_err_to_name(ret),
                 (unsigned int)AUDIO_CACHE_DOWNLOAD_RETRY_DELAY_MS,
                 final_path);
        vTaskDelay(pdMS_TO_TICKS(AUDIO_CACHE_DOWNLOAD_RETRY_DELAY_MS));
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "Download failed: attempts=%d path=%s ret=%s url=%s",
                 attempt > AUDIO_CACHE_DOWNLOAD_MAX_ATTEMPTS ? AUDIO_CACHE_DOWNLOAD_MAX_ATTEMPTS : attempt,
                 final_path,
                 esp_err_to_name(ret),
                 audio_url);
    } else if ((path_buffer != NULL) && (path_buffer_size > 0)) {
        snprintf(path_buffer, path_buffer_size, "%s", final_path);
    }

    audio_cache_restore_wifi_power_save(previous_wifi_ps, restore_wifi_ps);

    return ret;
}

static esp_err_t audio_cache_service_cleanup_unused_with_protected(const char *const *keep_paths,
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

esp_err_t audio_cache_service_cleanup_unused(const char *const *keep_paths, size_t keep_path_count)
{
    return audio_cache_service_cleanup_unused_with_protected(keep_paths, keep_path_count, NULL);
}

esp_err_t audio_cache_service_cleanup_unused_protected(const char *const *keep_paths,
                                                       size_t keep_path_count,
                                                       const char *protected_path)
{
    return audio_cache_service_cleanup_unused_with_protected(keep_paths, keep_path_count, protected_path);
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
    return strcmp(left_item->path, right_item->path);
}

static bool audio_cache_plan_contains_path(const audio_cache_plan_item_t *items,
                                           size_t item_count,
                                           const char *path,
                                           size_t *index_out)
{
    size_t index = 0;

    if (path == NULL) {
        return false;
    }

    for (index = 0; index < item_count; ++index) {
        if (strcmp(items[index].path, path) == 0) {
            if (index_out != NULL) {
                *index_out = index;
            }
            return true;
        }
    }

    return false;
}

static void audio_cache_notify_result(audio_cache_maintenance_result_cb_t result_cb,
                                      void *result_ctx,
                                      const char *audio_url,
                                      esp_err_t ret,
                                      const char *local_path)
{
    if (result_cb != NULL) {
        result_cb(audio_url, ret, local_path == NULL ? "" : local_path, result_ctx);
    }
}

static bool audio_cache_download_heap_ready(const char *audio_url)
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
             audio_url == NULL ? "" : audio_url);
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

static void audio_cache_delete_later_cached_items(audio_cache_plan_item_t *items,
                                                  size_t item_count,
                                                  size_t current_index,
                                                  const char *protected_path,
                                                  audio_cache_maintenance_result_cb_t result_cb,
                                                  void *result_ctx)
{
    size_t index = item_count;

    while (index > (current_index + 1U)) {
        --index;
        if (!items[index].cached) {
            continue;
        }
        if ((protected_path != NULL) && (protected_path[0] != '\0') &&
            (strcmp(items[index].path, protected_path) == 0)) {
            continue;
        }

        ESP_LOGI(TAG, "Removing lower priority cached audio %s", items[index].path);
        if (remove(items[index].path) == 0) {
            items[index].cached = false;
            audio_cache_notify_result(result_cb,
                                      result_ctx,
                                      items[index].audio_url,
                                      ESP_ERR_NOT_FOUND,
                                      "");
        }
    }
}

esp_err_t audio_cache_service_maintain(const audio_cache_maintenance_item_t *items,
                                       size_t item_count,
                                       const char *protected_path,
                                       audio_cache_maintenance_result_cb_t result_cb,
                                       void *result_ctx)
{
    audio_cache_plan_item_t *plan = NULL;
    const char **keep_paths = NULL;
    size_t plan_count = 0;
    size_t index = 0;
    size_t plan_capacity = 0;
    esp_err_t final_ret = ESP_OK;
    bool download_heap_blocked = false;

    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((items == NULL) || (item_count == 0)) {
        return ESP_OK;
    }
    plan_capacity = item_count;
    plan = calloc(plan_capacity, sizeof(*plan));
    keep_paths = calloc(plan_capacity, sizeof(*keep_paths));
    if ((plan == NULL) || (keep_paths == NULL)) {
        final_ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    for (index = 0; index < item_count; ++index) {
        char path[AUDIO_CACHE_PATH_MAX] = {0};
        size_t existing_index = 0;

        if (items[index].audio_url[0] == '\0') {
            continue;
        }
        if (audio_cache_service_resolve_path(NULL,
                                             items[index].audio_url,
                                             path,
                                             sizeof(path)) != ESP_OK) {
            continue;
        }
        if (audio_cache_plan_contains_path(plan, plan_count, path, &existing_index)) {
            if (items[index].ring_at_epoch < plan[existing_index].ring_at_epoch) {
                plan[existing_index].ring_at_epoch = items[index].ring_at_epoch;
            }
            continue;
        }
        if (plan_count >= plan_capacity) {
            break;
        }

        snprintf(plan[plan_count].audio_url,
                 sizeof(plan[plan_count].audio_url),
                 "%s",
                 items[index].audio_url);
        snprintf(plan[plan_count].path, sizeof(plan[plan_count].path), "%s", path);
        plan[plan_count].ring_at_epoch = items[index].ring_at_epoch;
        plan[plan_count].cached = audio_cache_service_file_exists(path);
        ++plan_count;
    }

    if (plan_count > 1U) {
        qsort(plan, plan_count, sizeof(plan[0]), audio_cache_plan_compare_ring_time);
    }
    for (index = 0; index < plan_count; ++index) {
        keep_paths[index] = plan[index].path;
    }

    audio_cache_service_cleanup_unused_with_protected(keep_paths, plan_count, protected_path);

    for (index = 0; index < plan_count; ++index) {
        char downloaded_path[AUDIO_CACHE_PATH_MAX] = {0};
        esp_err_t ret = ESP_OK;

        if (audio_cache_service_file_exists(plan[index].path)) {
            plan[index].cached = true;
            audio_cache_notify_result(result_cb,
                                      result_ctx,
                                      plan[index].audio_url,
                                      ESP_OK,
                                      plan[index].path);
            continue;
        }

        if (download_heap_blocked || !audio_cache_download_heap_ready(plan[index].audio_url)) {
            download_heap_blocked = true;
            audio_cache_notify_result(result_cb,
                                      result_ctx,
                                      plan[index].audio_url,
                                      ESP_ERR_NO_MEM,
                                      "");
            continue;
        }

        ret = audio_cache_service_download(NULL,
                                           plan[index].audio_url,
                                           downloaded_path,
                                           sizeof(downloaded_path));
        if (ret == ESP_ERR_NO_MEM) {
            audio_cache_delete_later_cached_items(plan,
                                                  plan_count,
                                                  index,
                                                  protected_path,
                                                  result_cb,
                                                  result_ctx);
            ret = audio_cache_service_download(NULL,
                                               plan[index].audio_url,
                                               downloaded_path,
                                               sizeof(downloaded_path));
        }

        if ((ret == ESP_OK) && audio_cache_service_file_exists(downloaded_path)) {
            plan[index].cached = true;
            audio_cache_notify_result(result_cb,
                                      result_ctx,
                                      plan[index].audio_url,
                                      ESP_OK,
                                      downloaded_path);
        } else {
            audio_cache_notify_result(result_cb,
                                      result_ctx,
                                      plan[index].audio_url,
                                      ret,
                                      "");
        }
    }

cleanup:
    free(keep_paths);
    free(plan);
    return final_ret;
}
