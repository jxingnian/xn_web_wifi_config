/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 18:20:00
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 20:05:14
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\src\storage_module.c
 * @Description: WiFi 存储模块实现（基于 NVS，保存常用 WiFi 列表）
 * 
 * Copyright (c) 2025 by ${git_name_email}, All Rights Reserved.
 */

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "storage_module.h"

/* 本模块日志 TAG */
static const char *TAG = "wifi_storage";

/* 存储模块配置与初始化标志 */
static wifi_storage_config_t s_storage_cfg;
static bool                  s_storage_inited = false;

/* NVS 中保存 WiFi 列表使用的 key 名称 */
static const char *WIFI_LIST_KEY = "wifi_list";

/**
 * @brief 初始化 NVS（供存储模块使用）
 *
 * - 正常初始化或已初始化：返回 ESP_OK；
 * - 如遇页面不足或版本升级：擦除整个 NVS 分区后重新初始化。
 */
static esp_err_t wifi_storage_init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* 全分区擦除后再初始化，防止旧格式残留 */
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    return ret;
}

/**
 * @brief 判断两个 wifi_config_t 是否为同一 SSID（仅比较 STA 模式）
 *
 * 使用 memcmp 比较完整 ssid 缓冲区，避免依赖 '\0' 结尾。
 */
static bool wifi_storage_is_same_ssid(const wifi_config_t *a, const wifi_config_t *b)
{
    return memcmp(a->sta.ssid, b->sta.ssid, sizeof(a->sta.ssid)) == 0;
}

/**
 * @brief 初始化 WiFi 存储模块
 *
 * - 可重复调用，多次调用仅第一次生效；
 * - 若 config 为 NULL，使用 WIFI_STORAGE_DEFAULT_CONFIG；
 * - 强制保证 max_wifi_num >= 1。
 */
esp_err_t wifi_storage_init(const wifi_storage_config_t *config)
{
    if (s_storage_inited) {
        return ESP_OK;
    }

    /* 加载配置：优先使用用户配置，否则使用默认 */
    s_storage_cfg = (config == NULL) ? WIFI_STORAGE_DEFAULT_CONFIG() : *config;

    /* 防止后续申请 0 长度数组等问题 */
    if (s_storage_cfg.max_wifi_num == 0) {
        s_storage_cfg.max_wifi_num = 1;
    }

    /* NVS 初始化 */
    esp_err_t ret = wifi_storage_init_nvs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_storage_inited = true;
    return ESP_OK;
}

/**
 * @brief 读取所有已保存 WiFi 配置
 *
 * @param configs    外部提供的数组缓冲，长度需 >= max_wifi_num
 * @param count_out  实际读取到的数量（可能小于 max_wifi_num）
 *
 * @note 若当前没有任何配置，返回 ESP_OK 且 *count_out = 0。
 */
esp_err_t wifi_storage_load_all(wifi_config_t *configs, uint8_t *count_out)
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
        /* 命名空间不存在，理解为尚未保存过任何 WiFi */
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(read) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 首先获取 blob 大小，用于计算配置数量 */
    size_t blob_size = 0;
    ret              = nvs_get_blob(handle, WIFI_LIST_KEY, NULL, &blob_size);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        /* 未保存过列表 */
        nvs_close(handle);
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(size) failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    /* blob_size 必须是 wifi_config_t 的整数倍，且非 0 */
    if (blob_size == 0 || (blob_size % sizeof(wifi_config_t)) != 0) {
        ESP_LOGE(TAG, "invalid blob size: %u", (unsigned int)blob_size);
        nvs_close(handle);
        return ESP_FAIL;
    }

    uint8_t max_num    = s_storage_cfg.max_wifi_num;
    uint8_t stored_num = blob_size / sizeof(wifi_config_t);
    uint8_t read_num   = (stored_num > max_num) ? max_num : stored_num;
    size_t  read_size  = read_num * sizeof(wifi_config_t);

    ret = nvs_get_blob(handle, WIFI_LIST_KEY, configs, &read_size);
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(data) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    *count_out = read_num;
    return ESP_OK;
}

/**
 * @brief 在 STA 成功连接后更新 WiFi 列表
 *
 * 策略：
 * - 若该 SSID 已存在：移动到列表首位（保持其他顺序）；
 * - 若不存在且列表未满：插入到首位；
 * - 若不存在且列表已满：插入到首位并丢弃最后一个。
 */
esp_err_t wifi_storage_on_connected(const wifi_config_t *config)
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

    /* 分配工作缓冲，用于读写列表 */
    wifi_config_t *list = (wifi_config_t *)calloc(max_num, sizeof(wifi_config_t));
    if (list == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t   count = 0;
    esp_err_t ret   = wifi_storage_load_all(list, &count);
    if (ret != ESP_OK) {
        free(list);
        return ret;
    }

    /* 查找是否已存在相同 SSID */
    int existing_index = -1;
    for (uint8_t i = 0; i < count; ++i) {
        if (wifi_storage_is_same_ssid(&list[i], config)) {
            existing_index = (int)i;
            break;
        }
    }

    if (existing_index >= 0) {
        /* 已存在：移动到首位 */
        if (existing_index > 0) {
            wifi_config_t tmp = list[existing_index];
            memmove(&list[1], &list[0], existing_index * sizeof(wifi_config_t));
            list[0] = tmp;
        }
    } else {
        /* 不存在：插入到首位（可能挤掉最后一个） */
        if (count < max_num) {
            if (count > 0) {
                memmove(&list[1], &list[0], count * sizeof(wifi_config_t));
            }
            list[0] = *config;
            count++;
        } else {
            if (max_num > 1) {
                memmove(&list[1], &list[0], (max_num - 1) * sizeof(wifi_config_t));
            }
            list[0] = *config;
            count   = max_num;
        }
    }

    /* 写回 NVS */
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
        ESP_LOGE(TAG, "nvs_set_blob(write) failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        free(list);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);
    free(list);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(write) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief 按 SSID 删除已保存的 WiFi 配置
 *
 * @param ssid  需要删除的 SSID 字符串（以 '\0' 结尾）
 *
 * 若删除后列表为空，则直接擦除 WIFI_LIST_KEY。
 */
esp_err_t wifi_storage_delete_by_ssid(const char *ssid)
{
    if (!s_storage_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t max_num = s_storage_cfg.max_wifi_num;
    if (max_num == 0) {
        max_num = 1;
    }

    wifi_config_t *list = (wifi_config_t *)calloc(max_num, sizeof(wifi_config_t));
    if (list == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t   count = 0;
    esp_err_t ret   = wifi_storage_load_all(list, &count);
    if (ret != ESP_OK) {
        free(list);
        return ret;
    }

    if (count == 0) {
        free(list);
        return ESP_OK;
    }

    /* 构造一个只设置 SSID 的临时配置，复用比较函数 */
    wifi_config_t target;
    memset(&target, 0, sizeof(target));
    strncpy((char *)target.sta.ssid, ssid, sizeof(target.sta.ssid) - 1);

    /* 过滤出保留的条目 */
    uint8_t write_idx = 0;
    for (uint8_t i = 0; i < count; ++i) {
        if (wifi_storage_is_same_ssid(&list[i], &target)) {
            /* 跳过待删除条目 */
            continue;
        }
        if (write_idx != i) {
            list[write_idx] = list[i];
        }
        write_idx++;
    }

    nvs_handle_t handle;
    ret = nvs_open(s_storage_cfg.nvs_namespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(delete) failed: %s", esp_err_to_name(ret));
        free(list);
        return ret;
    }

    if (write_idx == 0) {
        /* 已无任何配置：擦除 key */
        ret = nvs_erase_key(handle, WIFI_LIST_KEY);
        if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "nvs_erase_key failed: %s", esp_err_to_name(ret));
            nvs_close(handle);
            free(list);
            return ret;
        }
    } else {
        /* 仍有剩余配置：回写精简后的列表 */
        size_t blob_size = write_idx * sizeof(wifi_config_t);
        ret              = nvs_set_blob(handle, WIFI_LIST_KEY, list, blob_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nvs_set_blob(delete) failed: %s", esp_err_to_name(ret));
            nvs_close(handle);
            free(list);
            return ret;
        }
    }

    ret = nvs_commit(handle);
    nvs_close(handle);
    free(list);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(delete) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}
