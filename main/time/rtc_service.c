#include "rtc_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "RTC_SERVICE";

#define RTC_I2C_PORT I2C_NUM_0
#define RTC_I2C_SDA GPIO_NUM_21
#define RTC_I2C_SCL GPIO_NUM_22
#define RTC_I2C_SPEED_HZ 100000
#define RTC_TIMEOUT_MS 1000
#define DS3231_ADDRESS 0x68

static i2c_master_bus_handle_t s_rtc_bus;
static i2c_master_dev_handle_t s_rtc_device;
static bool s_rtc_ready;

/* Converts DS3231 register data from BCD into decimal. */
static uint8_t rtc_bcd_to_dec(uint8_t value)
{
    return ((value >> 4) * 10) + (value & 0x0F);
}

/* Converts decimal data into the DS3231 BCD register format. */
static uint8_t rtc_dec_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

/* Rejects obviously invalid RTC values before restoring system time. */
static bool rtc_is_reasonable_time(const struct tm *timeinfo)
{
    int full_year = 0;

    if (timeinfo == NULL) {
        return false;
    }

    full_year = timeinfo->tm_year + 1900;
    if (full_year < 2024) {
        return false;
    }
    if (timeinfo->tm_mon < 0 || timeinfo->tm_mon > 11) {
        return false;
    }
    if (timeinfo->tm_mday < 1 || timeinfo->tm_mday > 31) {
        return false;
    }
    if (timeinfo->tm_hour < 0 || timeinfo->tm_hour > 23) {
        return false;
    }
    if (timeinfo->tm_min < 0 || timeinfo->tm_min > 59) {
        return false;
    }
    if (timeinfo->tm_sec < 0 || timeinfo->tm_sec > 59) {
        return false;
    }

    return true;
}

/* Reads a contiguous DS3231 register block over I2C. */
static esp_err_t rtc_read_registers(uint8_t start_register, uint8_t *data, size_t data_length)
{
    if (!s_rtc_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_transmit_receive(s_rtc_device,
                                       &start_register,
                                       sizeof(start_register),
                                       data,
                                       data_length,
                                       RTC_TIMEOUT_MS);
}

/* Writes a contiguous DS3231 register block over I2C. */
static esp_err_t rtc_write_registers(uint8_t start_register, const uint8_t *data, size_t data_length)
{
    uint8_t buffer[8];

    if (!s_rtc_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data_length + 1 > sizeof(buffer)) {
        return ESP_ERR_INVALID_SIZE;
    }

    buffer[0] = start_register;
    memcpy(&buffer[1], data, data_length);

    return i2c_master_transmit(s_rtc_device, buffer, data_length + 1, RTC_TIMEOUT_MS);
}

/* Creates the I2C bus and registers the DS3231 device once at boot. */
esp_err_t rtc_service_init(void)
{
    esp_err_t result = ESP_OK;
    i2c_master_bus_config_t bus_config = {
        .i2c_port = RTC_I2C_PORT,
        .sda_io_num = RTC_I2C_SDA,
        .scl_io_num = RTC_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_ADDRESS,
        .scl_speed_hz = RTC_I2C_SPEED_HZ,
    };

    if (s_rtc_ready) {
        return ESP_OK;
    }

    result = i2c_new_master_bus(&bus_config, &s_rtc_bus);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(result));
        return result;
    }

    result = i2c_master_bus_add_device(s_rtc_bus, &device_config, &s_rtc_device);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add DS3231 device: %s", esp_err_to_name(result));
        i2c_del_master_bus(s_rtc_bus);
        s_rtc_bus = NULL;
        return result;
    }

    s_rtc_ready = true;
    ESP_LOGI(TAG, "RTC service initialized on SDA=%d SCL=%d", RTC_I2C_SDA, RTC_I2C_SCL);

    return ESP_OK;
}

/* Reads DS3231 time registers and converts them into struct tm. */
esp_err_t rtc_service_read(struct tm *timeinfo)
{
    esp_err_t result = ESP_OK;
    uint8_t raw_data[7] = {0};

    if (timeinfo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    result = rtc_read_registers(0x00, raw_data, sizeof(raw_data));
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read RTC: %s", esp_err_to_name(result));
        return result;
    }

    memset(timeinfo, 0, sizeof(*timeinfo));
    timeinfo->tm_sec = rtc_bcd_to_dec(raw_data[0] & 0x7F);
    timeinfo->tm_min = rtc_bcd_to_dec(raw_data[1] & 0x7F);
    if ((raw_data[2] & 0x40) != 0) {
        bool pm_mode = (raw_data[2] & 0x20) != 0;
        int hour = rtc_bcd_to_dec(raw_data[2] & 0x1F);
        if (hour == 12) {
            hour = 0;
        }
        timeinfo->tm_hour = pm_mode ? hour + 12 : hour;
    } else {
        timeinfo->tm_hour = rtc_bcd_to_dec(raw_data[2] & 0x3F);
    }
    timeinfo->tm_mday = rtc_bcd_to_dec(raw_data[4] & 0x3F);
    timeinfo->tm_mon = rtc_bcd_to_dec(raw_data[5] & 0x1F) - 1;
    timeinfo->tm_year = 100 + rtc_bcd_to_dec(raw_data[6]);
    timeinfo->tm_isdst = -1;

    return ESP_OK;
}

/* Normalizes local time and writes it back into DS3231 registers. */
esp_err_t rtc_service_write(const struct tm *timeinfo)
{
    struct tm normalized_time;
    time_t epoch_seconds = 0;
    uint8_t raw_data[7] = {0};

    if (timeinfo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    normalized_time = *timeinfo;
    normalized_time.tm_isdst = -1;
    epoch_seconds = mktime(&normalized_time);
    if (epoch_seconds == (time_t)-1) {
        return ESP_ERR_INVALID_ARG;
    }
    if (localtime_r(&epoch_seconds, &normalized_time) == NULL) {
        return ESP_FAIL;
    }

    raw_data[0] = rtc_dec_to_bcd((uint8_t)normalized_time.tm_sec);
    raw_data[1] = rtc_dec_to_bcd((uint8_t)normalized_time.tm_min);
    raw_data[2] = rtc_dec_to_bcd((uint8_t)normalized_time.tm_hour);
    raw_data[3] = rtc_dec_to_bcd((uint8_t)(normalized_time.tm_wday == 0 ? 7 : normalized_time.tm_wday));
    raw_data[4] = rtc_dec_to_bcd((uint8_t)normalized_time.tm_mday);
    raw_data[5] = rtc_dec_to_bcd((uint8_t)(normalized_time.tm_mon + 1));
    raw_data[6] = rtc_dec_to_bcd((uint8_t)((normalized_time.tm_year + 1900) - 2000));

    return rtc_write_registers(0x00, raw_data, sizeof(raw_data));
}

/* Checks whether the RTC is ready and currently holds a plausible timestamp. */
bool rtc_service_has_valid_time(void)
{
    struct tm current_time = {0};

    if (!s_rtc_ready) {
        return false;
    }
    if (rtc_service_read(&current_time) != ESP_OK) {
        return false;
    }

    return rtc_is_reasonable_time(&current_time);
}

/* Reports whether RTC initialization has completed successfully. */
bool rtc_service_is_ready(void)
{
    return s_rtc_ready;
}
