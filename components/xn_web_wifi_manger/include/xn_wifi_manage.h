/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 16:24:42
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 22:36:38
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\include\xn_wifi_manage.h
 * @Description: WiFi 管理模块对外接口（封装 WiFi / 存储 / Web 配网）
 *
 * - 负责自动重连、连接结果上报；
 * - 可选保存多组 WiFi 配置并轮询尝试；
 * - 内置 AP + Web 配网能力。
 *
 * Copyright (c) 2025 by ${git_name_email}, All Rights Reserved.
 */

#ifndef XN_WIFI_MANAGE_H
#define XN_WIFI_MANAGE_H

#include "esp_err.h"

/**
 * @brief WiFi 管理状态机单步运行周期（单位：ms）
 *
 * 管理任务会按该间隔周期性调用内部状态机：
 * - 间隔过小：状态更新更及时，但占用更多 CPU；
 * - 间隔过大：重连/轮询响应变慢。
 */
#define WIFI_MANAGE_STEP_INTERVAL_MS 1000

/**
 * @brief WiFi 管理层抽象的连接状态
 *
 * 仅关注“是否连上路由器”这一结果，不暴露底层驱动细节。
 */
typedef enum {
    WIFI_MANAGE_STATE_DISCONNECTED = 0, ///< 已断开，无可用连接
    WIFI_MANAGE_STATE_CONNECTED,        ///< 已成功连接路由器并拿到 IP
    WIFI_MANAGE_STATE_CONNECT_FAILED,   ///< 本轮全部候选 WiFi 连接失败
} wifi_manage_state_t;

/**
 * @brief 上层应用关注的 WiFi 管理事件回调
 *
 * 由管理模块在状态变化时调用，用于通知应用层做相应处理：
 * - 可用于更新 UI / 打日志 / 上报云端等。
 *
 * @param state 当前 WiFi 管理状态（见 @ref wifi_manage_state_t）
 */
typedef void (*wifi_event_cb_t)(wifi_manage_state_t state);

/**
 * @brief WiFi 管理模块配置
 *
 * 该结构体仅在初始化时读取一次，之后由管理模块内部持有副本。
 */
typedef struct {
    int  max_retry_count;          ///< 单个 AP 最多连续重试次数（<=0 表示只尝试一次）
    int  reconnect_interval_ms;    ///< 整轮失败后等待多久再自动重试；<0 表示关闭自动重试
    char ap_ssid[32];              ///< 配网 AP SSID（最长 31 字符，需手动保证 '\0' 结尾）
    char ap_password[64];          ///< 配网 AP 密码（8~63 字符，留 1 字节给 '\0'）
    char ap_ip[16];                ///< 配网 AP 网口 IP 地址，如 "192.168.4.1"
    wifi_event_cb_t wifi_event_cb; ///< 状态变化回调，可为 NULL 表示不关心
    int  save_wifi_count;          ///< 最多保存的 WiFi 条数（<=0 使用 1；值越大占用更多 NVS/堆内存）
    int  web_port;                 ///< Web 配网页面 HTTP 监听端口（典型为 80/8080）
} wifi_manage_config_t;

/**
 * @brief WiFi 管理模块默认配置宏
 *
 * 可在原样基础上仅修改关注字段，例如：
 * @code
 * wifi_manage_config_t cfg = WIFI_MANAGE_DEFAULT_CONFIG();
 * strcpy(cfg.ap_ssid, "MyAP");
 * wifi_manage_init(&cfg);
 * @endcode
 */
#define WIFI_MANAGE_DEFAULT_CONFIG()                       \
    (wifi_manage_config_t){                                \
        .max_retry_count       = 5,                        \
        .reconnect_interval_ms = 10000,                    \
        .ap_ssid               = "XN-ESP32-AP",            \
        .ap_password           = "12345678",               \
        .ap_ip                 = "192.168.4.1",            \
        .wifi_event_cb         = NULL,                     \
        .save_wifi_count       = 5,                        \
        .web_port              = 80,                       \
    }

/**
 * @brief 初始化 WiFi 管理模块
 *
 * 功能概览：
 * - 初始化内部 WiFi / 存储 / Web 配网子模块；
 * - 创建管理任务并启动状态机；
 * - 根据配置启动 STA + AP 模式。
 *
 * @param config 若为 NULL，则使用 @ref WIFI_MANAGE_DEFAULT_CONFIG
 *
 * @return
 *      - ESP_OK      : 初始化成功
 *      - 其它 esp_err_t : 由底层子模块返回的错误码
 */
esp_err_t wifi_manage_init(const wifi_manage_config_t *config);

#endif /* XN_WIFI_MANAGE_H */
