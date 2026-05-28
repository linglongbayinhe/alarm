/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <inttypes.h>

#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"

#include "blufi_port.h"

#if !CONFIG_BT_BLUEDROID_ENABLED
#error "This project supports BLUFI with the Bluedroid host stack only."
#endif

esp_err_t esp_blufi_host_init(void)
{
    int ret;
    esp_bluedroid_config_t cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();

    ret = esp_bluedroid_init_with_cfg(&cfg);
    if (ret) {
        BLUFI_ERROR("%s init bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        BLUFI_ERROR("%s enable bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }

    BLUFI_INFO("BD ADDR: " ESP_BD_ADDR_STR "\n", ESP_BD_ADDR_HEX(esp_bt_dev_get_address()));

    ret = esp_ble_gap_set_device_name(CUSTOM_BLUFI_DEVICE_NAME);
    if (ret) {
        BLUFI_ERROR("%s set device name failed: %s\n", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }

    return ESP_OK;
}

#ifdef CONFIG_EXAMPLE_BLUFI_BLE_SMP_ENABLE
static void esp_blufi_set_ble_security_params(void)
{
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_OUT;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint32_t passkey = 123456;
    uint8_t auth_option = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE;
    uint8_t oob_support = ESP_BLE_OOB_DISABLE;

    BLUFI_INFO("BLE SMP passkey: %06" PRIu32 " (WARNING: Change this default value for production or don't use static passkey!)\n",
               passkey);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey, sizeof(uint32_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH, &auth_option, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_OOB_SUPPORT, &oob_support, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));
}
#endif

esp_err_t esp_blufi_host_deinit(void)
{
    int ret;

    ret = esp_blufi_profile_deinit();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_bluedroid_disable();
    if (ret) {
        BLUFI_ERROR("%s disable bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }

    ret = esp_bluedroid_deinit();
    if (ret) {
        BLUFI_ERROR("%s deinit bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t esp_blufi_gap_register_callback(void)
{
    int ret = esp_ble_gap_register_callback(esp_blufi_gap_event_handler);

    if (ret) {
        return ret;
    }

    return esp_blufi_profile_init();
}

esp_err_t esp_blufi_host_and_cb_init(esp_blufi_callbacks_t *callbacks)
{
    esp_err_t ret = ESP_OK;

    ret = esp_blufi_host_init();
    if (ret != ESP_OK) {
        BLUFI_ERROR("%s initialise host failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }

    ret = esp_blufi_register_callbacks(callbacks);
    if (ret != ESP_OK) {
        BLUFI_ERROR("%s blufi register failed, error code = %x\n", __func__, ret);
        return ret;
    }

    ret = esp_blufi_gap_register_callback();
    if (ret != ESP_OK) {
        BLUFI_ERROR("%s gap register failed, error code = %x\n", __func__, ret);
        return ret;
    }

#ifdef CONFIG_EXAMPLE_BLUFI_BLE_SMP_ENABLE
    esp_blufi_set_ble_security_params();
#endif

    return ESP_OK;
}

esp_err_t esp_blufi_controller_init(void)
{
    esp_err_t ret = ESP_OK;

#if CONFIG_IDF_TARGET_ESP32
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
#endif

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        BLUFI_ERROR("%s initialize bt controller failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        BLUFI_ERROR("%s enable bt controller failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }
    BLUFI_INFO("BLE controller enabled\n");

    return ESP_OK;
}

esp_err_t esp_blufi_controller_deinit(void)
{
    esp_err_t ret = ESP_OK;

    ret = esp_bt_controller_disable();
    if (ret != ESP_OK) {
        BLUFI_ERROR("%s disable bt controller failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_deinit();
    if (ret != ESP_OK) {
        BLUFI_ERROR("%s deinit bt controller failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}
