/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 16:24:42
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 19:05:46
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\include\xn_wifi_manage.h
 * @Description: 提供管理wifi的接口
 * 
 * Copyright (c) 2025 by ${git_name_email}, All Rights Reserved. 
 */

#ifndef XN_WIFI_MANAGE_H
#define XN_WIFI_MANAGE_H

#include "esp_err.h"

#define WIFI_MANAGE_STEP_INTERVAL_MS 1000   // WiFi管理状态机运行间隔时间(ms)

/**
 * @brief wifi管理的状态
 */
typedef enum {
    WIFI_MANAGE_STATE_DISCONNECTED = 0, ///< wifi已断开
    WIFI_MANAGE_STATE_CONNECTED,        ///< wifi已连接
    WIFI_MANAGE_STATE_CONNECT_FAILED,   ///< wifi连接失败
} wifi_manage_state_t;

typedef void (*wifi_event_cb_t)(wifi_manage_state_t state);

/**
 * @brief wifi管理的配置
 */
typedef struct {
    int max_retry_count;                    ///< wifi连接最大重试次数
    int reconnect_interval_ms;              ///< wifi连接重试间隔时间(ms),-1表示不重连
    char ap_ssid[32];                       ///< 配网AP热点名称
    char ap_password[64];                   ///< 配网AP热点密码
    char ap_ip[16];                         ///< 配网IP地址
    wifi_event_cb_t wifi_event_cb;          ///< wifi管理事件回调
    int save_wifi_count;                    ///< wifi保存数量（超过20时，建议增加状态机栈空间）
    int web_port;                           ///< Web 配网端口，默认80
} wifi_manage_config_t;
 
#define WIFI_MANAGE_DEFAULT_CONFIG()                         \
    (wifi_manage_config_t){                                  \
        .max_retry_count       = 5,                          \
        .reconnect_interval_ms = 10000,                      \
        .ap_ssid               = "XingNian",           \
        .ap_password           = "12345678",               \
        .ap_ip                 = "192.168.4.1",            \
        .wifi_event_cb         = NULL,                       \
        .save_wifi_count       = 5,                          \
        .web_port              = 80,                         \
    }

/**
 * @brief wifi管理模块初始化
 * @param config wifi管理的配置
 * @return esp_err_t 错误码
 */
esp_err_t xn_wifi_manage_init(const wifi_manage_config_t *config);

#endif
