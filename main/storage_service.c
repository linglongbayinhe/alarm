#include "storage_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_flash.h"
#include "esp_flash_spi_init.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

static const char *TAG = "STORAGE_SERVICE";
static const char *SPIFFS_PARTITION_LABEL = "storage";
#if CONFIG_APP_EXTERNAL_AUDIO_CACHE_ENABLE
static const char *EXT_FLASH_PARTITION_LABEL = "extcache";
#endif

#define STORAGE_SERVICE_SPIFFS_MAX_FILES 8
#if CONFIG_APP_EXTERNAL_AUDIO_CACHE_ENABLE
#define STORAGE_SERVICE_EXT_FLASH_HOST SPI3_HOST
#define STORAGE_SERVICE_EXT_FLASH_MOSI GPIO_NUM_32
#define STORAGE_SERVICE_EXT_FLASH_MISO GPIO_NUM_25
#define STORAGE_SERVICE_EXT_FLASH_SCLK GPIO_NUM_33
#define STORAGE_SERVICE_EXT_FLASH_CS   GPIO_NUM_27
#define STORAGE_SERVICE_EXT_FLASH_FREQ_MHZ 10
#endif

static bool s_internal_ready;
static bool s_external_ready;
#if CONFIG_APP_EXTERNAL_AUDIO_CACHE_ENABLE
static wl_handle_t s_external_wl_handle = WL_INVALID_HANDLE;
static esp_flash_t *s_external_flash;
static const esp_partition_t *s_external_partition;
#endif

#if CONFIG_APP_EXTERNAL_AUDIO_CACHE_ENABLE
static esp_err_t storage_service_ensure_directory(const char *path)
{
    struct stat info = {0};

    if (stat(path, &info) == 0) {
        if ((info.st_mode & S_IFDIR) != 0) {
            return ESP_OK;
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (mkdir(path, 0775) != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}
#endif

static esp_err_t storage_service_init_internal_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = STORAGE_SERVICE_INTERNAL_BASE_PATH,
        .partition_label = SPIFFS_PARTITION_LABEL,
        .max_files = STORAGE_SERVICE_SPIFFS_MAX_FILES,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = ESP_OK;
    size_t total = 0;
    size_t used = 0;

    if (s_internal_ready) {
        return ESP_OK;
    }

    ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount internal SPIFFS: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_spiffs_info(SPIFFS_PARTITION_LABEL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Internal SPIFFS mounted: total=%u used=%u",
                 (unsigned int)total,
                 (unsigned int)used);
    }

    s_internal_ready = true;
    return ESP_OK;
}

#if CONFIG_APP_EXTERNAL_AUDIO_CACHE_ENABLE
static esp_err_t storage_service_init_external_flash(void)
{
    spi_bus_config_t bus_config = {
        .mosi_io_num = STORAGE_SERVICE_EXT_FLASH_MOSI,
        .miso_io_num = STORAGE_SERVICE_EXT_FLASH_MISO,
        .sclk_io_num = STORAGE_SERVICE_EXT_FLASH_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
    };
    esp_flash_spi_device_config_t device_config = {
        .host_id = STORAGE_SERVICE_EXT_FLASH_HOST,
        .cs_id = 0,
        .cs_io_num = STORAGE_SERVICE_EXT_FLASH_CS,
        .io_mode = SPI_FLASH_DIO,
        .freq_mhz = STORAGE_SERVICE_EXT_FLASH_FREQ_MHZ,
    };
    esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 8,
        .format_if_mount_failed = true,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .use_one_fat = false,
    };
    esp_err_t ret = ESP_OK;
    uint32_t flash_id = 0;
    uint32_t size_bytes = 0;

    if (s_external_ready) {
        return ESP_OK;
    }

    ret = spi_bus_initialize(STORAGE_SERVICE_EXT_FLASH_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        ESP_LOGE(TAG, "External flash SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = spi_bus_add_flash_device(&s_external_flash, &device_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add external flash device: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_flash_init(s_external_flash);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init external flash: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_flash_read_id(s_external_flash, &flash_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read external flash ID: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_flash_get_size(s_external_flash, &size_bytes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read external flash size: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG,
             "External flash detected: id=0x%08" PRIx32 " size=%" PRIu32 " KB freq=%d MHz",
             flash_id,
             size_bytes / 1024,
             STORAGE_SERVICE_EXT_FLASH_FREQ_MHZ);

    ret = esp_partition_register_external(s_external_flash,
                                          0,
                                          size_bytes,
                                          EXT_FLASH_PARTITION_LABEL,
                                          ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_DATA_FAT,
                                          &s_external_partition);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        ESP_LOGE(TAG, "Failed to register external FAT partition: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_external_partition == NULL) {
        s_external_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                        ESP_PARTITION_SUBTYPE_DATA_FAT,
                                                        EXT_FLASH_PARTITION_LABEL);
    }
    if (s_external_partition == NULL) {
        ESP_LOGE(TAG, "Failed to resolve external FAT partition handle");
        return ESP_ERR_NOT_FOUND;
    }

    ret = esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_SERVICE_EXTERNAL_BASE_PATH,
                                           EXT_FLASH_PARTITION_LABEL,
                                           &mount_config,
                                           &s_external_wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Initial external FATFS mount failed: %s; erasing partition and retrying",
                 esp_err_to_name(ret));
        ret = esp_partition_erase_range(s_external_partition, 0, s_external_partition->size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase external FAT partition: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_SERVICE_EXTERNAL_BASE_PATH,
                                               EXT_FLASH_PARTITION_LABEL,
                                               &mount_config,
                                               &s_external_wl_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mount external FATFS after erase retry: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    ret = storage_service_ensure_directory(STORAGE_SERVICE_EXTERNAL_AUDIO_DIR);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create audio cache dir: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "External flash mounted at %s size=%" PRIu32 " KB",
             STORAGE_SERVICE_EXTERNAL_BASE_PATH,
             size_bytes / 1024);
    s_external_ready = true;
    return ESP_OK;
}
#endif

esp_err_t storage_service_init(void)
{
    esp_err_t ret = ESP_OK;

    ret = storage_service_init_internal_spiffs();
    if (ret != ESP_OK) {
        return ret;
    }

#if CONFIG_APP_EXTERNAL_AUDIO_CACHE_ENABLE
    ret = storage_service_init_external_flash();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "External cache unavailable, playback will use internal fallback only");
    }
#else
    ESP_LOGI(TAG, "External audio cache disabled by config");
#endif

    return ESP_OK;
}

bool storage_service_default_audio_exists(void)
{
    struct stat info = {0};

    if (!s_internal_ready) {
        return false;
    }

    return stat(STORAGE_SERVICE_DEFAULT_AUDIO_PATH, &info) == 0;
}

const char *storage_service_get_default_audio_path(void)
{
    return STORAGE_SERVICE_DEFAULT_AUDIO_PATH;
}

bool storage_service_external_cache_available(void)
{
    return s_external_ready;
}
