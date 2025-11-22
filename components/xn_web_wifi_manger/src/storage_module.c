/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 18:20:00
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 18:30:00
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\src\storage_module.c
 * @Description: WiFi 存储模块实现
 * 
 * Copyright (c) 2025 by ${git_name_email}, All Rights Reserved. 
 */

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "storage_module.h"

/* 日志 TAG */
static const char *TAG = "wifi_storage";

/* 保存存储模块配置及初始化状态 */
static wifi_storage_config_t s_storage_cfg;
static bool                  s_storage_inited = false;

/* 存储 WiFi 列表所用的 NVS key 名称 */
static const char *WIFI_LIST_KEY = "wifi_list";

/**
 * @brief 初始化 NVS，用于 WiFi 存储模块
 */
static esp_err_t wifi_storage_init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    return ret;
}

/**
 * @brief 比较两个 wifi_config_t 是否属于同一 SSID
 *
 * 这里只比较 STA 模式下的 ssid 字段，假定其以 '\0' 结尾或零填充。
 */
static bool wifi_storage_is_same_ssid(const wifi_config_t *a, const wifi_config_t *b)
{
    return (memcmp(a->sta.ssid, b->sta.ssid, sizeof(a->sta.ssid)) == 0);
}

esp_err_t xn_wifi_storage_init(const wifi_storage_config_t *config)
{
    if (s_storage_inited) {
        return ESP_OK;
    }

    /* 若用户未提供配置，则使用默认配置 */
    if (config == NULL) {
        s_storage_cfg = WIFI_STORAGE_DEFAULT_CONFIG();
    } else {
        s_storage_cfg = *config;
    }

    /* max_wifi_num 不允许为 0，避免后续除零/数组长度为 0 的情况 */
    if (s_storage_cfg.max_wifi_num == 0) {
        s_storage_cfg.max_wifi_num = 1;
    }

    /* 初始化 NVS */
    esp_err_t ret = wifi_storage_init_nvs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_storage_inited = true;

    return ESP_OK;
}

esp_err_t xn_wifi_storage_load_all(wifi_config_t *configs, uint8_t *count_out)
{
    if (!s_storage_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (configs == NULL || count_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *count_out = 0;

    nvs_handle_t handle;
    esp_err_t    ret = nvs_open(s_storage_cfg.nvs_namespace, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        /* 命名空间不存在，视为当前没有保存任何 WiFi 配置 */
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t blob_size = 0;
    ret              = nvs_get_blob(handle, WIFI_LIST_KEY, NULL, &blob_size);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        /* 尚未保存过任何列表 */
        nvs_close(handle);
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(size) failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    if (blob_size == 0 || (blob_size % sizeof(wifi_config_t)) != 0) {
        /* 存储格式异常，直接返回错误 */
        ESP_LOGE(TAG, "invalid blob size: %u", (unsigned int)blob_size);
        nvs_close(handle);
        return ESP_FAIL;
    }

    uint8_t max_num     = s_storage_cfg.max_wifi_num;
    uint8_t stored_num  = blob_size / sizeof(wifi_config_t);
    uint8_t read_num    = (stored_num > max_num) ? max_num : stored_num;
    size_t  read_size   = read_num * sizeof(wifi_config_t);

    ret = nvs_get_blob(handle, WIFI_LIST_KEY, configs, &read_size);
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(data) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    *count_out = read_num;

    return ESP_OK;
}

esp_err_t xn_wifi_storage_on_connected(const wifi_config_t *config)
{
    if (!s_storage_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t max_num = s_storage_cfg.max_wifi_num;
    if (max_num == 0) {
        max_num = 1;
    }

    /* 1. 读取当前所有已保存的 WiFi 配置到内存列表中 */
    wifi_config_t *list = (wifi_config_t *)calloc(max_num, sizeof(wifi_config_t));
    if (list == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t  count = 0;
    esp_err_t ret   = xn_wifi_storage_load_all(list, &count);
    if (ret != ESP_OK) {
        free(list);
        return ret;
    }

    /* 2. 检查该 WiFi 是否已经存在列表中（按 SSID 匹配） */
    int existing_index = -1;
    for (uint8_t i = 0; i < count; ++i) {
        if (wifi_storage_is_same_ssid(&list[i], config)) {
            existing_index = (int)i;
            break;
        }
    }

    if (existing_index >= 0) {
        /* 已存在于列表中：将其移动到首位，保持相对顺序 */
        if (existing_index > 0) {
            wifi_config_t tmp = list[existing_index];
            memmove(&list[1], &list[0], existing_index * sizeof(wifi_config_t));
            list[0] = tmp;
        }
    } else {
        /* 不在列表中：插入到首位，必要时丢弃最后一个 */
        if (count < max_num) {
            if (count > 0) {
                memmove(&list[1], &list[0], count * sizeof(wifi_config_t));
            }
            list[0] = *config;
            count++;
        } else {
            /* 列表已满，将 [0..max_num-2] 后移一位，丢弃最后一个 */
            if (max_num > 1) {
                memmove(&list[1], &list[0], (max_num - 1) * sizeof(wifi_config_t));
            }
            list[0] = *config;
            count   = max_num;
        }
    }

    /* 3. 将更新后的列表写回 NVS */
    nvs_handle_t handle;
    ret = nvs_open(s_storage_cfg.nvs_namespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(write) failed: %s", esp_err_to_name(ret));
        free(list);
        return ret;
    }

    size_t blob_size = count * sizeof(wifi_config_t);
    ret              = nvs_set_blob(handle, WIFI_LIST_KEY, list, blob_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        free(list);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);
    free(list);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

