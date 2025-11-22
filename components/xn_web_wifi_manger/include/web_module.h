/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 19:10:00
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 19:24:08
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\include\web_module.h
 * @Description: Web 配网模块
 * 
 * 该模块负责：
 *  - 启动一个 HTTP 服务器；
 *  - 从 SPIFFS 中挂载并返回 index.html；
 *  - 提供 index.html 中使用的接口：
 *      /scan           : 扫描周围 WiFi，返回 JSON 列表；
 *      /configure      : 接收前端提交的 SSID/密码，发起配置；
 *      /api/status     : 返回当前连接状态（connected/未连接 等）；
 *      /api/saved      : 返回已保存的 WiFi 列表；
 *      /api/connect    : 根据 SSID 连接已保存 WiFi；
 *      /api/delete     : 删除已保存 WiFi；
 *      /api/reset_retry: 重置 WiFi 管理重试计数（由管理模块实现）。
 */

#ifndef WEB_MODULE_H
#define WEB_MODULE_H

#include <stdint.h>

#include "esp_err.h"

/**
 * @brief Web 配网模块配置
 */
typedef struct {
    uint16_t http_port;   ///< HTTP 服务器监听端口，一般为 80
} web_module_config_t;

/**
 * @brief Web 模块默认配置
 */
#define WEB_MODULE_DEFAULT_CONFIG()    \
    (web_module_config_t){             \
        .http_port = 80,               \
    }

/**
 * @brief 启动 Web 配网模块
 *
 * 内部会：
 *  - 初始化/启动 HTTP 服务器；
 *  - 注册与 index.html 对应的 URI 处理函数；
 *  - 挂载 SPIFFS 中的网页资源。
 *
 * @param config Web 模块配置指针；可为 NULL，为 NULL 时使用 WEB_MODULE_DEFAULT_CONFIG。
 * @return esp_err_t
 *         - ESP_OK           : 启动成功
 *         - 其它错误码       : 启动失败
 */
esp_err_t xn_web_module_start(const web_module_config_t *config);

/**
 * @brief 停止 Web 配网模块
 *
 * 关闭 HTTP 服务器并释放相关资源。
 *
 * @return esp_err_t
 */
esp_err_t xn_web_module_stop(void);

#endif /* WEB_MODULE_H */

