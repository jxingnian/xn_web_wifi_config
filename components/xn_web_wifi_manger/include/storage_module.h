/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 16:38:49
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 18:27:25
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\include\storage_module.h
 * @Description: WiFi 存储模块
 * 
 * Copyright (c) 2025 by ${git_name_email}, All Rights Reserved. 
 */

#ifndef STORAGE_MODULE_H
#define STORAGE_MODULE_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_wifi.h"  /* 提供 wifi_config_t 类型 */

/**
 * @brief WiFi 存储模块配置
 *
 * - nvs_namespace : 使用的 NVS 命名空间；
 * - max_wifi_num  : 最多保存的 WiFi 配置数量（用于限制内部列表长度）。
 */
typedef struct {
    const char *nvs_namespace;  ///< 存储使用的 NVS 命名空间
    uint8_t     max_wifi_num;   ///< 最多保存的 WiFi 配置数量
} wifi_storage_config_t;

/**
 * @brief WiFi 存储模块默认配置
 */
#define WIFI_STORAGE_DEFAULT_CONFIG()                 \
    (wifi_storage_config_t){                          \
        .nvs_namespace = "wifi_store",              \
        .max_wifi_num  = 5,                           \
    }

/**
 * @brief 初始化 WiFi 存储模块
 *
 * 负责准备 NVS 等底层环境，并加载必要的内部状态。
 *
 * @param config 存储模块配置；可为 NULL，此时使用 WIFI_STORAGE_DEFAULT_CONFIG
 * @return esp_err_t
 *         - ESP_OK           : 初始化成功
 *         - 其它错误码       : 初始化失败
 */
esp_err_t xn_wifi_storage_init(const wifi_storage_config_t *config);

/**
 * @brief 读取所有已保存的 WiFi 配置
 *
 * 顺序具有意义：
 * - 当 *count_out > 0 时，数组下标 0 对应的 WiFi 视为“优先尝试的 WiFi”
 *   （例如最近一次成功连接的 AP）。
 * - 返回的数量不会超过初始化时配置的 max_wifi_num。
 *
 * @param configs    调用方提供的数组，长度至少为初始化时配置的 max_wifi_num
 * @param count_out  输出参数：实际读取到的 WiFi 数量
 * @return esp_err_t
 *         - ESP_OK           : 读取成功
 *         - 其它错误码       : 读取失败
 */
esp_err_t xn_wifi_storage_load_all(wifi_config_t *configs, uint8_t *count_out);

/**
 * @brief 在 WiFi 成功连接后更新存储列表
 *
 * 管理模块在收到“连接成功并获取到 IP”的事件后调用本接口：
 * - 若存储中已存在相同 SSID 的配置，则将其移动到列表首位；
 * - 若不存在，则在列表中插入该配置作为首位（必要时丢弃最后一个）。
 *
 * 这样可以保证下次读取列表时，configs[0] 始终是“优先尝试”的 WiFi。
 *
 * @param config  本次成功连接使用的 WiFi 配置
 * @return esp_err_t
 *         - ESP_OK           : 更新成功
 *         - 其它错误码       : 更新失败
 */
esp_err_t xn_wifi_storage_on_connected(const wifi_config_t *config);

#endif
