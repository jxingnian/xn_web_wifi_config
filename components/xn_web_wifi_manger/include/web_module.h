/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 19:10:00
 * @LastEditors: xingnian && jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 21:30:00
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\include\web_module.h
 * @Description: Web 配网模块对外接口。
 *
 * 提供一个基于 HTTP Server + SPIFFS 的简易配网页面及 REST 接口：
 *  - /               : 返回 index.html（存放于 SPIFFS）；
 *  - /scan           : 扫描附近 WiFi，返回 JSON 列表；
 *  - /configure      : 提交 SSID/密码，触发一次连接（并由上层决定是否保存）；
 *  - /api/status     : 查询当前连接状态与简单信息；
 *  - /api/saved      : 查询已保存 WiFi 列表；
 *  - /api/connect    : 按 SSID 连接某个已保存 WiFi；
 *  - /api/delete     : 删除某个已保存 WiFi；
 *  - /api/reset_retry: 通知上层重置“自动重连/重试”状态。
 *
 * 本头文件仅定义数据结构与回调类型，不涉及具体实现细节。
 */

#ifndef WEB_MODULE_H
#define WEB_MODULE_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

/* ====================== Web 层数据结构 ====================== */

/**
 * @brief Web 层展示用：一次扫描结果中的单个 AP 信息
 */
typedef struct {
    char   ssid[32];   ///< AP 名称（UTF-8，最长 31 字符，末尾保留 '\0'）
    int8_t rssi;       ///< 信号强度（dBm）
} web_scan_result_t;

/**
 * @brief Web 层展示用：已保存的 WiFi 条目（只关心 SSID）
 *
 * 由上层存储模块返回，仅包含 SSID，是否保存密码由上层自行维护。
 */
typedef struct {
    char ssid[32];     ///< 已保存的 WiFi 名称
} web_saved_wifi_t;

/**
 * @brief Web 层展示用：当前 WiFi 状态
 *
 * 前端通常只需要“是否已连接 + SSID + IP + 信号强度 + BSSID”这几个字段。
 */
typedef struct {
    bool  connected;   ///< 是否已连接到路由器（true 表示已建立链接并认为可用）
    char  ssid[32];    ///< 当前连接的 SSID（未连接时置空）
    char  ip[16];      ///< 当前 IPv4 地址字符串（如 "192.168.1.100"，未连接时可为空）
    int8_t rssi;       ///< 当前信号强度（未连接时可设为 0 或保留上次值）
    char  bssid[18];   ///< BSSID 文本形式 "xx:xx:xx:xx:xx:xx"
} web_wifi_status_t;

/* ====================== 回调函数类型 ====================== */

/**
 * @brief 扫描附近 AP 的回调
 *
 * 由上层实现，Web 模块在 /scan 被访问时调用。
 *
 * @param list         输出数组，由调用方（Web 模块）提前分配好。
 * @param count_inout  入参为数组容量，回调需填入实际数量并更新该值。
 *
 * @return
 *      - ESP_OK         : 扫描成功并写入结果
 *      - 其他           : 扫描失败，Web 模块会按失败处理
 */
typedef esp_err_t (*web_scan_cb_t)(web_scan_result_t *list, uint16_t *count_inout);

/**
 * @brief 提交新 WiFi 配置的回调
 *
 * 用于 /configure 接口：前端提交 SSID/密码后，Web 模块调用此回调。
 * 该回调只负责“尝试连接”，至于是否保存配置由上层决定。
 *
 * @param ssid      目标 AP 的 SSID
 * @param password  对应密码，可为空字符串表示无密码
 */
typedef esp_err_t (*web_configure_cb_t)(const char *ssid, const char *password);

/**
 * @brief 获取当前 WiFi 状态的回调
 *
 * 用于 /api/status 接口：上层填充 web_wifi_status_t，供前端展示。
 *
 * @param status  输出结构体指针，回调需完整填充
 */
typedef esp_err_t (*web_get_status_cb_t)(web_wifi_status_t *status);

/**
 * @brief 获取已保存 WiFi 列表的回调
 *
 * 用于 /api/saved 接口：通常由存储模块实现，从 NVS 或其他介质读出。
 *
 * @param list         输出数组，由 Web 模块分配
 * @param count_inout  入参为数组容量，出参为实际条目数
 */
typedef esp_err_t (*web_get_saved_cb_t)(web_saved_wifi_t *list, uint8_t *count_inout);

/**
 * @brief 按 SSID 连接已保存 WiFi 的回调
 *
 * 用于 /api/connect 接口：根据 SSID 在已保存列表中查找并触发一次连接。
 *
 * @param ssid  目标已保存 WiFi 的 SSID
 */
typedef esp_err_t (*web_connect_saved_cb_t)(const char *ssid);

/**
 * @brief 删除指定已保存 WiFi 的回调
 *
 * 用于 /api/delete 接口：根据 SSID 删除对应存储条目。
 *
 * @param ssid  需要删除的 SSID
 */
typedef esp_err_t (*web_delete_saved_cb_t)(const char *ssid);

/**
 * @brief 重置 WiFi 管理重试计数的回调
 *
 * 用于 /api/reset_retry 接口：一般由 WiFi 管理模块实现，
 * 在用户手动切换网络或重新发起连接时清空内部重试计数和状态。
 */
typedef esp_err_t (*web_reset_retry_cb_t)(void);

/* ====================== Web 模块配置 ====================== */

/**
 * @brief Web 配网模块初始化配置
 *
 * 仅包含与 Web 服务直接相关的参数，逻辑行为通过各类回调由上层控制。
 */
typedef struct {
    uint16_t              http_port;         ///< HTTP 服务器监听端口（常用 80）
    web_scan_cb_t         scan_cb;           ///< /scan           扫描附近 WiFi
    web_configure_cb_t    configure_cb;      ///< /configure      提交新配置
    web_get_status_cb_t   get_status_cb;     ///< /api/status     查询当前状态
    web_get_saved_cb_t    get_saved_cb;      ///< /api/saved      查询已保存列表
    web_connect_saved_cb_t connect_saved_cb; ///< /api/connect    连接已保存 WiFi
    web_delete_saved_cb_t  delete_saved_cb;  ///< /api/delete     删除已保存 WiFi
    web_reset_retry_cb_t   reset_retry_cb;   ///< /api/reset_retry 重置重试计数
} web_module_config_t;

/**
 * @brief Web 模块默认配置宏
 *
 * 未显式设置的回调默认为 NULL，即对应接口将不可用或返回简单错误。
 * 典型用法：
 *
 *     web_module_config_t cfg = WEB_MODULE_DEFAULT_CONFIG();
 *     cfg.scan_cb       = ...;
 *     cfg.configure_cb  = ...;
 *     web_module_start(&cfg);
 */
#define WEB_MODULE_DEFAULT_CONFIG()            \
    (web_module_config_t){                     \
        .http_port        = 80,                \
        .scan_cb          = NULL,              \
        .configure_cb     = NULL,              \
        .get_status_cb    = NULL,              \
        .get_saved_cb     = NULL,              \
        .connect_saved_cb = NULL,              \
        .delete_saved_cb  = NULL,              \
        .reset_retry_cb   = NULL,              \
    }

/* ====================== 对外接口函数 ====================== */

/**
 * @brief 启动 Web 配网模块
 *
 * 内部流程示意：
 *  1. 若尚未挂载 SPIFFS，则先尝试挂载；
 *  2. 创建并启动 HTTP Server（使用 http_port）；
 *  3. 注册静态页面（index.html 等）和各 REST 接口；
 *  4. 保存回调配置指针，后续请求直接调用回调。
 *
 * @param config  Web 模块配置指针；若为 NULL，则使用 WEB_MODULE_DEFAULT_CONFIG。
 *
 * @return
 *      - ESP_OK   : 启动成功（HTTP Server 已运行）
 *      - 其他     : 启动失败，具体错误由底层返回
 */
esp_err_t web_module_start(const web_module_config_t *config);

/**
 * @brief 停止 Web 配网模块
 *
 * 关闭 HTTP Server，并在必要时卸载 SPIFFS / 释放资源。
 * 若模块尚未启动，调用应安全返回（建议返回 ESP_OK）。
 *
 * @return
 *      - ESP_OK   : 停止成功或模块本就未启动
 *      - 其他     : 关闭 HTTP Server 过程中的错误
 */
esp_err_t web_module_stop(void);

#endif /* WEB_MODULE_H */
