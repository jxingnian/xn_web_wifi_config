/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 21:45:00
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 22:51:55
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\include\web_module.h
 * @Description: Web 配网模块对外接口（HTTP 服务器 + 回调驱动的业务能力）
 *
 * 设计要点：
 * - 只关心 HTTP 与静态资源本身，不直接依赖 WiFi / 存储实现；
 * - 通过回调从上层获取 WiFi 状态等信息，实现低耦合；
 * - 由 wifi_manage 在初始化时提供回调实现与端口配置。
 */

#ifndef WEB_MODULE_H
#define WEB_MODULE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

/**
 * @brief Web 配网模块关注的 WiFi 状态视图
 *
 * 仅保留网页展示需要的少量字段，由上层通过回调填充。
 */
typedef enum {
    WEB_WIFI_STATUS_STATE_IDLE = 0,       ///< 空闲/未连接
    WEB_WIFI_STATUS_STATE_CONNECTING,     ///< 正在尝试连接
    WEB_WIFI_STATUS_STATE_CONNECTED,      ///< 已连接并获取到 IP
    WEB_WIFI_STATUS_STATE_FAILED,         ///< 一轮尝试后认为连接失败
} web_wifi_status_state_t;

typedef struct {
    web_wifi_status_state_t state; ///< 抽象连接状态，便于前端区分展示
    bool                    connected;        ///< 是否已成功连接路由器
    char                    ssid[32];         ///< 当前连接的 SSID（无连接时可为 "-")
    char                    ip[16];           ///< 当前 STA 的 IPv4 地址字符串，如 "192.168.4.2"
    int8_t                  rssi;             ///< 当前连接的信号强度（dBm），无连接时可为 0
    char                    mode[8];          ///< 当前工作模式字符串，如 "AP" / "STA" / "AP+STA"
} web_wifi_status_t;

/**
 * @brief Web 端展示用的“已保存 WiFi”精简信息
 */
typedef struct {
    char ssid[32]; ///< WiFi 名称（仅保留 SSID，忽略密码等敏感信息）
} web_saved_wifi_info_t;

/**
 * @brief Web 模块查询 WiFi 状态的回调
 *
 * 由上层（通常是 wifi_manage）实现，用于向 Web 模块提供当前 WiFi 状态。
 *
 * @param[out] out_status  输出的状态结构体指针，不可为 NULL
 *
 * @return
 *  - ESP_OK              : 查询成功
 *  - 其它 esp_err_t      : 查询失败（Web 模块将返回简单错误响应）
 */
typedef esp_err_t (*web_get_status_cb_t)(web_wifi_status_t *out_status);

/**
 * @brief 获取已保存 WiFi 列表的回调
 *
 * @param[in,out] list      Web 模块提供的缓存数组
 * @param[in,out] inout_cnt 入口为缓存容量，出口为实际填充数量
 */
typedef esp_err_t (*web_get_saved_list_cb_t)(web_saved_wifi_info_t *list, size_t *inout_cnt);

/**
 * @brief 删除已保存 WiFi 的回调（按 SSID 匹配）
 */
typedef esp_err_t (*web_delete_saved_cb_t)(const char *ssid);

/**
 * @brief 连接已保存 WiFi 的回调（按 SSID 匹配）
 */
typedef esp_err_t (*web_connect_saved_cb_t)(const char *ssid);

/**
 * @brief Web 模块配置
 *
 * 该配置在初始化时传入一次，模块内部会保存副本。
 */
typedef struct {
    int                   http_port;        ///< HTTP 监听端口（典型为 80/8080，<=0 时使用默认 80）
    web_get_status_cb_t   get_status_cb;    ///< 查询当前 WiFi 状态回调
    web_get_saved_list_cb_t get_saved_list_cb; ///< 获取已保存 WiFi 列表回调
    web_delete_saved_cb_t delete_saved_cb;  ///< 删除已保存 WiFi 的回调
    web_connect_saved_cb_t connect_saved_cb; ///< 连接已保存 WiFi 的回调
} web_module_config_t;

/**
 * @brief Web 模块默认配置
 */
#define WEB_MODULE_DEFAULT_CONFIG()            \
    (web_module_config_t){                     \
        .http_port        = 80,                \
        .get_status_cb    = NULL,              \
        .get_saved_list_cb = NULL,             \
        .delete_saved_cb  = NULL,              \
        .connect_saved_cb = NULL,              \
    }

/**
 * @brief 初始化 Web 配网模块
 *
 * 负责：
 * - 挂载 SPIFFS 分区（label: "wifi_spiffs"，base_path: "/spiffs"）；
 * - 启动 HTTP 服务器并注册静态文件路由；
 * - 如配置了 get_status_cb，则注册 /api/wifi/status 接口。
 *
 * @param config 配置指针，可为 NULL，NULL 时使用 WEB_MODULE_DEFAULT_CONFIG。
 *
 * @return
 *  - ESP_OK                 : 初始化成功（可重复调用，后续调用直接返回 ESP_OK）
 *  - ESP_ERR_NO_MEM 等      : 内部资源不足
 *  - 其它 esp_err_t         : SPIFFS / HTTP 服务器相关错误
 */
esp_err_t web_module_init(const web_module_config_t *config);

#endif /* WEB_MODULE_H */

