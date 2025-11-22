/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 16:37:33
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 17:48:19
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\include\wifi_module.h
 * @Description: WiFi 模块
 * 
 * Copyright (c) 2025 by ${git_name_email}, All Rights Reserved. 
 */

#ifndef WIFI_MODULE_H
#define WIFI_MODULE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief WiFi 模块事件
 *
 * 该事件枚举专门为 WiFi 管理组件设计，用于描述底层 WiFi 状态变化，
 * 由 WiFi 模块通过回调上报给上层的 WiFi 管理组件。
 */
typedef enum {
    WIFI_MODULE_EVENT_STA_CONNECTED = 0,   ///< STA 已与 AP 建立链接（不保证已获取到 IP）
    WIFI_MODULE_EVENT_STA_DISCONNECTED,    ///< STA 已从 AP 断开
    WIFI_MODULE_EVENT_STA_CONNECT_FAILED,  ///< STA 连接指定 AP 失败
    WIFI_MODULE_EVENT_STA_GOT_IP,          ///< STA 获取到 IP，认为 WiFi 完全连接成功
} wifi_module_event_t;

/**
 * @brief WiFi 模块事件回调类型
 *
 * @param event  当前发生的 WiFi 模块事件
 */
typedef void (*wifi_module_event_cb_t)(wifi_module_event_t event);

/**
 * @brief WiFi 模块配置
 *
 * 该结构体仅包含 WiFi 模块自身需要关心的参数，
 * 上层 WiFi 管理组件可在初始化时根据自身配置填充此结构体。
 */
typedef struct {
    bool enable_sta;                        ///< 是否启用 STA 模式
    bool enable_ap;                         ///< 是否启用 AP 模式（用于配网等场景）
    char ap_ssid[32];                       ///< AP 模式下的热点名称
    char ap_password[64];                   ///< AP 模式下的热点密码
    char ap_ip[16];                         ///< AP 模式下的 IP 地址字符串，例如 "192.168.4.1"
    uint8_t ap_channel;                     ///< AP 信道
    uint8_t max_sta_conn;                   ///< AP 模式下的最大连接数
    wifi_module_event_cb_t event_cb;        ///< WiFi 模块事件回调
} wifi_module_config_t;

/**
 * @brief WiFi 模块默认配置
 *
 * 该宏提供一份合理的默认配置，便于上层在不关心细节时快速初始化：
 * - 同时启用 STA + AP；
 * - AP SSID/密码为示例值；
 * - AP 信道为 1；
 * - 最大连接数为 4；
 * - 无默认事件回调（event_cb = NULL）。
 */
#define WIFI_MODULE_DEFAULT_CONFIG()                            \
    (wifi_module_config_t){                                     \
        .enable_sta   = true,                                   \
        .enable_ap    = true,                                   \
        .ap_ssid      = "XingNian",                           \
        .ap_password  = "12345678",                           \
        .ap_ip        = "192.168.4.1",                        \
        .ap_channel   = 1,                                      \
        .max_sta_conn = 4,                                      \
        .event_cb     = NULL,                                   \
    }

/**
 * @brief 初始化 WiFi 模块
 *
 * @param config WiFi 模块配置；可为 NULL，此时使用 @ref WIFI_MODULE_DEFAULT_CONFIG
 * @return esp_err_t
 *         - ESP_OK           : 初始化成功
 *         - 其它错误码       : 初始化失败
 */
esp_err_t xn_wifi_module_init(const wifi_module_config_t *config);

/**
 * @brief 连接指定 WiFi（STA 模式）
 *
 * @param ssid      目标 WiFi 的 SSID 字符串
 * @param password  目标 WiFi 的密码字符串
 * @return esp_err_t
 *         - ESP_OK           : 已成功发起连接流程
 *         - 其它错误码       : 参数错误或硬件/驱动未就绪
 */
esp_err_t xn_wifi_module_connect(const char *ssid, const char *password);

#endif
