#include "audio_cache_service.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "storage_service.h"

static const char *TAG = "AUDIO_CACHE";

#define AUDIO_CACHE_PATH_MAX 160
#define AUDIO_CACHE_HTTP_TIMEOUT_MS 15000
#define AUDIO_CACHE_DOWNLOAD_BUFFER_SIZE 2048

static bool s_ready;

static bool audio_cache_is_safe_char(char value)
{
    return ((value >= 'a') && (value <= 'z')) ||
           ((value >= 'A') && (value <= 'Z')) ||
           ((value >= '0') && (value <= '9')) ||
           (value == '_') ||
           (value == '-');
}

static void audio_cache_sanitize_name(const char *source, char *destination, size_t destination_size)
{
    size_t index = 0;

    if ((destination == NULL) || (destination_size == 0)) {
        return;
    }

    if (source == NULL) {
        destination[0] = '\0';
        return;
    }

    for (index = 0; (index + 1) < destination_size && source[index] != '\0'; ++index) {
        destination[index] = audio_cache_is_safe_char(source[index]) ? source[index] : '_';
    }
    destination[index] = '\0';
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
                                     size_t keep_path_count)
{
    size_t index = 0;

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

esp_err_t audio_cache_service_resolve_path(const char *instance_id,
                                           const char *audio_url,
                                           char *path_buffer,
                                           size_t path_buffer_size)
{
    char safe_name[80] = {0};
    const char *extension = NULL;

    if ((instance_id == NULL) || (path_buffer == NULL) || (path_buffer_size == 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_cache_sanitize_name(instance_id, safe_name, sizeof(safe_name));
    extension = audio_cache_detect_extension(audio_url);
    if (snprintf(path_buffer,
                 path_buffer_size,
                 "%s/%s%s",
                 STORAGE_SERVICE_EXTERNAL_AUDIO_DIR,
                 safe_name,
                 extension) >= (int)path_buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t audio_cache_service_download(const char *instance_id,
                                       const char *audio_url,
                                       char *path_buffer,
                                       size_t path_buffer_size)
{
    char final_path[AUDIO_CACHE_PATH_MAX] = {0};
    char temp_path[AUDIO_CACHE_PATH_MAX] = {0};
    esp_http_client_config_t client_config = {0};
    esp_http_client_handle_t client = NULL;
    FILE *file = NULL;
    uint8_t *download_buffer = NULL;
    esp_err_t ret = ESP_OK;

    if ((instance_id == NULL) || (audio_url == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = audio_cache_service_resolve_path(instance_id, audio_url, final_path, sizeof(final_path));
    if (ret != ESP_OK) {
        return ret;
    }

    if (audio_cache_service_file_exists(final_path)) {
        if ((path_buffer != NULL) && (path_buffer_size > 0)) {
            snprintf(path_buffer, path_buffer_size, "%s", final_path);
        }
        return ESP_OK;
    }

    if (snprintf(temp_path, sizeof(temp_path), "%s.part", final_path) >= (int)sizeof(temp_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    client_config.url = audio_url;
    client_config.method = HTTP_METHOD_GET;
    client_config.timeout_ms = AUDIO_CACHE_HTTP_TIMEOUT_MS;
#if defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY) && CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    client_config.skip_cert_common_name_check = true;
#else
    client_config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    client = esp_http_client_init(&client_config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Open download failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        ret = ESP_FAIL;
        goto cleanup;
    }
    if (esp_http_client_get_status_code(client) / 100 != 2) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    file = fopen(temp_path, "wb");
    if (file == NULL) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    download_buffer = calloc(1, AUDIO_CACHE_DOWNLOAD_BUFFER_SIZE);
    if (download_buffer == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    while (true) {
        int bytes_read = esp_http_client_read(client,
                                              (char *)download_buffer,
                                              AUDIO_CACHE_DOWNLOAD_BUFFER_SIZE);
        if (bytes_read < 0) {
            ret = ESP_FAIL;
            goto cleanup;
        }
        if (bytes_read == 0) {
            break;
        }
        if (fwrite(download_buffer, 1, (size_t)bytes_read, file) != (size_t)bytes_read) {
            ret = ESP_FAIL;
            goto cleanup;
        }
    }

    fclose(file);
    file = NULL;

    remove(final_path);
    if (rename(temp_path, final_path) != 0) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    if ((path_buffer != NULL) && (path_buffer_size > 0)) {
        snprintf(path_buffer, path_buffer_size, "%s", final_path);
    }

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    if (ret != ESP_OK) {
        remove(temp_path);
    }
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(download_buffer);

    return ret;
}

esp_err_t audio_cache_service_cleanup_unused(const char *const *keep_paths, size_t keep_path_count)
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
        if (!audio_cache_path_is_kept(full_path, keep_paths, keep_path_count)) {
            remove(full_path);
        }
    }

    closedir(directory);
    return ESP_OK;
}
