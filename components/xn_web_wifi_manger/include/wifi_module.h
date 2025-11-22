/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 16:37:33
 * @LastEditors: xingnian && jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 22:00:00
 * @FilePath: \web_wifi_config\components\web_wifi_manger\include\wifi_module.h
 * @Description: WiFi 底层封装，仅负责 STA/AP 初始化、连接与事件上报
 *
 * 上层（如 wifi_manage）通过：
 *  - wifi_module_init()  配置并初始化 WiFi 驱动、STA/AP 接口；
 *  - wifi_module_connect()  发起一次 STA 连接流程；
 *  - wifi_module_scan()     执行同步扫描，获取附近 AP 列表；
 * 以及注册的 event_cb 获取 WiFi 状态变化。
 */

#ifndef WIFI_MODULE_H
#define WIFI_MODULE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* -------------------------------------------------------------------------- */
/*                               事件与回调类型                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief WiFi 模块向上层上报的事件
 *
 * 所有事件都发生在系统事件回调上下文中（由 IDF 事件循环驱动），
 * 上层应尽量在回调中做“标记 / 通知”，避免耗时阻塞操作。
 */
typedef enum {
    WIFI_MODULE_EVENT_STA_CONNECTED = 0,   ///< STA 已与 AP 建立链路（尚不保证拿到 IP）
    WIFI_MODULE_EVENT_STA_DISCONNECTED,    ///< STA 与 AP 断开（包括主动断开和异常掉线）
    WIFI_MODULE_EVENT_STA_CONNECT_FAILED,  ///< 本次 STA 连接尝试失败（认证错误、超时等）
    WIFI_MODULE_EVENT_STA_GOT_IP,          ///< STA 成功获取 IPv4 地址，认为连接完成
} wifi_module_event_t;

/**
 * @brief WiFi 模块事件回调
 *
 * @param event 当前发生的 WiFi 事件
 */
typedef void (*wifi_module_event_cb_t)(wifi_module_event_t event);

/* -------------------------------------------------------------------------- */
/*                                   配置体                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief WiFi 模块初始化配置
 *
 * 仅包含与 WiFi 驱动本身相关的参数，不涉及存储或业务逻辑。
 * 一般由上层管理模块在初始化时根据自身需求填充。
 */
typedef struct {
    bool  enable_sta;                       ///< 是否启用 STA（连接路由器）
    bool  enable_ap;                        ///< 是否启用 AP（本地配网 / 调试热点）
    char  ap_ssid[32];                      ///< AP SSID（UTF-8，<=31 字符，结尾自动补 '\0'）
    char  ap_password[64];                  ///< AP 密码（>=8 字符，空串表示开放网络）
    char  ap_ip[16];                        ///< AP 网关 IP（如 "192.168.4.1"）
    uint8_t ap_channel;                     ///< AP 信道（1~13，非法值由实现做修正）
    uint8_t max_sta_conn;                   ///< AP 可同时接入的 STA 数量
    wifi_module_event_cb_t event_cb;        ///< 事件回调，可为 NULL（不回调）
} wifi_module_config_t;

/* -------------------------------------------------------------------------- */
/*                                扫描结果结构体                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief WiFi 扫描结果中单个 AP 信息（精简版）
 */
typedef struct {
    char   ssid[32];   ///< SSID（UTF-8，<=31 字符，结尾自动补 '\0'）
    int8_t rssi;       ///< RSSI（dBm）
} wifi_module_scan_result_t;

/* -------------------------------------------------------------------------- */
/*                                  默认配置                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief WiFi 模块默认配置
 *
 * 同时开启 STA + AP，AP 使用简单示例参数，方便开箱即用。
 * 上层可在此基础上按需覆盖部分字段。
 */
#define WIFI_MODULE_DEFAULT_CONFIG()                            \
    (wifi_module_config_t){                                     \
        .enable_sta   = true,                                   \
        .enable_ap    = true,                                   \
        .ap_ssid      = "XingNian",                             \
        .ap_password  = "12345678",                             \
        .ap_ip        = "192.168.4.1",                          \
        .ap_channel   = 1,                                      \
        .max_sta_conn = 4,                                      \
        .event_cb     = NULL,                                   \
    }

/* -------------------------------------------------------------------------- */
/*                                  对外接口                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化 WiFi 模块（可多次调用，仅首次生效）
 *
 * 内部负责：
 *  - 初始化 NVS（若尚未初始化）；
 *  - 初始化网络栈与事件循环（esp_netif / esp_event）；
 *  - 创建默认 STA/AP netif（按 enable_sta/enable_ap 决定）；
 *  - 配置 AP（IP、SSID、密码、信道、连接数）并启动 WiFi 驱动。
 *
 * @param config 配置指针；为 NULL 时等同于使用 WIFI_MODULE_DEFAULT_CONFIG()
 * @return
 *      - ESP_OK                 初始化成功（或已初始化）
 *      - ESP_ERR_NO_MEM 等      底层资源不足
 *      - 其它 esp_err_t         具体错误见日志
 */
esp_err_t wifi_module_init(const wifi_module_config_t *config);

/**
 * @brief 以 STA 模式连接指定 AP
 *
 * 若当前 WiFi 未处于 STA / APSTA 模式，接口会根据配置自动切换模式后再连接。
 * 调用成功仅表示“已发起连接流程”，连接结果通过 event_cb 上报。
 *
 * @param ssid     目标 AP SSID，必须非 NULL 且非空
 * @param password 目标 AP 密码，可为 NULL/空串 表示开放网络
 * @return
 *      - ESP_OK                 已成功提交连接请求
 *      - ESP_ERR_INVALID_ARG    ssid 非法
 *      - ESP_ERR_INVALID_STATE  WiFi 模块未初始化或未启用 STA
 *      - 其它 esp_err_t         具体错误见日志
 */
esp_err_t wifi_module_connect(const char *ssid, const char *password);

/**
 * @brief 同步扫描附近可见的 WiFi 列表
 *
 * 内部会发起一次阻塞式扫描，完成后将结果拷贝到调用方提供的数组中。
 *
 * @param results     结果数组指针，不可为 NULL
 * @param count_inout 输入：数组容量；输出：实际写入条目数
 * @return
 *      - ESP_OK                 扫描成功
 *      - ESP_ERR_INVALID_ARG    参数为 NULL 或容量为 0
 *      - ESP_ERR_INVALID_STATE  WiFi 模块未初始化
 *      - 其它 esp_err_t         具体错误见日志
 */
esp_err_t wifi_module_scan(wifi_module_scan_result_t *results, uint16_t *count_inout);

#endif /* WIFI_MODULE_H */
