/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 19:20:00
 * @LastEditors: xingnian && jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 19:29:24
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\src\web_module.c
 * @Description: Web 配网模块实现（基于 ESP-IDF HTTP Server + SPIFFS）
 */

#include "web_module.h"

#include <string.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"

/* 本模块日志 TAG */
static const char *TAG = "web_module";

/* HTTP Server 句柄（非 NULL 表示已启动） */
static httpd_handle_t      s_httpd = NULL;
/* 由上层提供的回调与配置 */
static web_module_config_t s_cfg;

/* 简单的 JSON 响应工具：设置 Content-Type 为 application/json 并发送整段字符串 */
static esp_err_t web_send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* 根路径处理：读取 SPIFFS 中的 /spiffs/index.html 并以分块方式发送给浏览器 */
static esp_err_t web_handle_root(httpd_req_t *req)
{
    FILE *f = fopen("/spiffs/index.html", "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "index.html not found");
        return ESP_FAIL;
    }

    /* 指定 HTML 编码为 UTF-8，避免中文乱码 */
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    char   buf[512];
    size_t n;
    /* 使用 HTTP chunk 发送文件内容，可避免申请过大缓冲区 */
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            /* 发送 NULL 结束 chunked 传输 */
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }

    fclose(f);
    /* 最后一块 NULL，表示完成传输 */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* /scan: 调用上层提供的扫描回调，返回附近 WiFi 的 JSON 列表 */
static esp_err_t web_handle_scan(httpd_req_t *req)
{
    /* 回调未配置直接返回错误 */
    if (s_cfg.scan_cb == NULL) {
        return web_send_json(req, "{\"status\":\"error\",\"message\":\"scan cb null\"}");
    }

    /* 固定数组保存扫描结果，按需调整容量 */
    web_scan_result_t list[16];
    uint16_t          count = sizeof(list) / sizeof(list[0]);

    esp_err_t ret = s_cfg.scan_cb(list, &count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "scan cb failed: %s", esp_err_to_name(ret));
        return web_send_json(req, "{\"status\":\"error\",\"message\":\"scan failed\"}");
    }

    httpd_resp_set_type(req, "application/json");
    /* 先发 JSON 头部与数组开始 */
    httpd_resp_sendstr_chunk(req, "{\"status\":\"ok\",\"networks\":[");

    for (uint16_t i = 0; i < count; ++i) {
        /* 跳过空 SSID 条目 */
        if (list[i].ssid[0] == '\0') {
            continue;
        }
        char item[256];
        /* 第一项前不加逗号，其余项前加逗号 */
        snprintf(item,
                 sizeof(item),
                 "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                 (i == 0) ? "" : ",",
                 list[i].ssid,
                 list[i].rssi);
        httpd_resp_sendstr_chunk(req, item);
    }

    /* 结束数组与对象 */
    httpd_resp_sendstr_chunk(req, "]}");
    /* 发送 NULL 结束 chunk */
    httpd_resp_sendstr_chunk(req, NULL);

    return ESP_OK;
}

/**
 * @brief 读取 HTTP 请求体到缓冲区（最多 buf_size-1 字节，自动补 '\0'）
 *
 * @param req      HTTP 请求指针
 * @param buf      目标缓冲区
 * @param buf_size 缓冲区大小
 *
 * @return 实际读取字节数（不含结尾 '\0'）
 */
static int web_read_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    int total = 0;
    int ret;
    while (total < (int)buf_size - 1) {
        /* httpd_req_recv 可能被多次调用才能收完一个请求体 */
        ret = httpd_req_recv(req, buf + total, buf_size - 1 - total);
        if (ret <= 0) {
            /* 出错或对端关闭，直接退出 */
            break;
        }
        total += ret;
        /* 本次读取未填满剩余空间，认为对端已发完 */
        if (ret < (int)(buf_size - 1 - total)) {
            break;
        }
    }
    buf[total] = '\0';
    return total;
}

/**
 * @brief 从简单 JSON 中提取字符串字段值
 *
 * 仅支持非常简单格式：{"key":"value"} 或包含该片段的字符串，
 * 不处理转义字符、空格等复杂情况，适合本模块简单交互使用。
 *
 * @param json     JSON 文本
 * @param key      需要提取的字段名
 * @param out      输出缓冲区
 * @param out_size 输出缓冲区大小
 *
 * @return true  提取成功
 * @return false 未找到 key 或解析失败
 */
static bool web_extract_json_string(const char *json, const char *key, char *out, size_t out_size)
{
    char  pattern[64];
    char *p;

    /* 构造查找模式："key":" */
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    p = strstr(json, pattern);
    if (!p) {
        return false;
    }
    /* 指针移动到 value 起始处 */
    p += strlen(pattern);

    size_t i = 0;
    /* 读取到下一个双引号或缓冲区满为止 */
    while (*p && *p != '"' && i + 1 < out_size) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

/* /configure: 提交新的 WiFi 配置（SSID 必填，密码可选） */
static esp_err_t web_handle_configure(httpd_req_t *req)
{
    char body[256];
    web_read_body(req, body, sizeof(body));

    char ssid[33]     = {0};  /* 32 字节 SSID + '\0' */
    char password[65] = {0};  /* 64 字节密码 + '\0' */

    /* 必须提供 SSID */
    if (!web_extract_json_string(body, "ssid", ssid, sizeof(ssid))) {
        return web_send_json(req, "{\"status\":\"error\",\"message\":\"ssid missing\"}");
    }
    /* 密码非必需，提取失败不视为错误 */
    (void)web_extract_json_string(body, "password", password, sizeof(password));

    if (s_cfg.configure_cb == NULL) {
        return web_send_json(req, "{\"status\":\"error\",\"message\":\"cfg cb null\"}");
    }

    /* 若密码为空字符串，则传 NULL 交由上层决定开放网络处理方式 */
    esp_err_t ret = s_cfg.configure_cb(ssid, (password[0] == '\0') ? NULL : password);
    if (ret != ESP_OK) {
        return web_send_json(req, "{\"status\":\"error\",\"message\":\"connect failed\"}");
    }

    return web_send_json(req, "{\"status\":\"ok\"}");
}

/* /api/status: 返回当前 WiFi 连接状态（已连接 / 未连接 / 出错） */
static esp_err_t web_handle_status(httpd_req_t *req)
{
    /* 未配置状态回调时，统一视为未连接 */
    if (s_cfg.get_status_cb == NULL) {
        return web_send_json(req, "{\"status\":\"disconnected\"}");
    }

    web_wifi_status_t status;
    memset(&status, 0, sizeof(status));

    esp_err_t ret = s_cfg.get_status_cb(&status);
    if (ret != ESP_OK) {
        return web_send_json(req, "{\"status\":\"error\"}");
    }

    if (!status.connected) {
        return web_send_json(req, "{\"status\":\"disconnected\"}");
    }

    /* 已连接时返回详细信息 */
    char json[256];
    snprintf(json,
             sizeof(json),
             "{\"status\":\"connected\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"bssid\":\"%s\"}",
             status.ssid,
             status.ip,
             status.rssi,
             status.bssid);

    return web_send_json(req, json);
}

/* /api/saved: 返回已保存 WiFi 列表（仅包含 SSID 数组） */
static esp_err_t web_handle_saved(httpd_req_t *req)
{
    if (s_cfg.get_saved_cb == NULL) {
        return web_send_json(req, "[]");
    }

    web_saved_wifi_t list[16];
    uint8_t          count = sizeof(list) / sizeof(list[0]);

    esp_err_t ret = s_cfg.get_saved_cb(list, &count);
    if (ret != ESP_OK || count == 0) {
        return web_send_json(req, "[]");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");

    for (uint8_t i = 0; i < count; ++i) {
        if (list[i].ssid[0] == '\0') {
            continue;
        }
        char item[64];
        snprintf(item,
                 sizeof(item),
                 "%s{\"ssid\":\"%s\"}",
                 (i == 0) ? "" : ",",
                 list[i].ssid);
        httpd_resp_sendstr_chunk(req, item);
    }

    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);

    return ESP_OK;
}

/* /api/connect: 根据给定 SSID 连接到已保存的网络（不修改存储，仅触发重连） */
static esp_err_t web_handle_connect(httpd_req_t *req)
{
    char body[128];
    web_read_body(req, body, sizeof(body));

    char ssid[33] = {0};
    if (!web_extract_json_string(body, "ssid", ssid, sizeof(ssid))) {
        return web_send_json(req, "{\"status\":\"error\",\"message\":\"ssid missing\"}");
    }

    if (s_cfg.connect_saved_cb == NULL) {
        return web_send_json(req, "{\"status\":\"error\"}");
    }

    esp_err_t ret = s_cfg.connect_saved_cb(ssid);
    if (ret != ESP_OK) {
        return web_send_json(req, "{\"status\":\"error\"}");
    }
    return web_send_json(req, "{\"status\":\"ok\"}");
}

/* /api/delete: 删除指定 SSID 的已保存 WiFi 记录 */
static esp_err_t web_handle_delete(httpd_req_t *req)
{
    char body[128];
    web_read_body(req, body, sizeof(body));

    char target_ssid[33] = {0};
    if (!web_extract_json_string(body, "ssid", target_ssid, sizeof(target_ssid))) {
        return web_send_json(req, "{\"status\":\"error\",\"message\":\"ssid missing\"}");
    }

    if (s_cfg.delete_saved_cb == NULL) {
        return web_send_json(req, "{\"status\":\"error\"}");
    }

    esp_err_t ret = s_cfg.delete_saved_cb(target_ssid);
    if (ret != ESP_OK) {
        return web_send_json(req, "{\"status\":\"error\"}");
    }

    return web_send_json(req, "{\"status\":\"ok\"}");
}

/* /api/reset_retry: 复位“重试计数/状态”之类的逻辑，由上层管理模块实现 */
static esp_err_t web_handle_reset_retry(httpd_req_t *req)
{
    if (s_cfg.reset_retry_cb != NULL) {
        (void)s_cfg.reset_retry_cb();
    }
    return web_send_json(req, "{\"status\":\"ok\"}");
}

/**
 * @brief 启动 HTTP Server 并注册所有 URI 处理函数
 *
 * @param port HTTP 端口号
 *
 * @return ESP_OK 启动成功
 * @return 其他    启动失败
 */
static esp_err_t web_start_httpd(uint16_t port)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port    = port;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_httpd = NULL;
        return ESP_FAIL;
    }

    /* 根页面：配网页面静态资源 */
    httpd_uri_t root_uri = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = web_handle_root,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &root_uri);

    /* WiFi 扫描接口 */
    httpd_uri_t scan_uri = {
        .uri      = "/scan",
        .method   = HTTP_GET,
        .handler  = web_handle_scan,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &scan_uri);

    /* 提交新的 WiFi 配置 */
    httpd_uri_t cfg_uri = {
        .uri      = "/configure",
        .method   = HTTP_POST,
        .handler  = web_handle_configure,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &cfg_uri);

    /* 查询当前连接状态 */
    httpd_uri_t status_uri = {
        .uri      = "/api/status",
        .method   = HTTP_GET,
        .handler  = web_handle_status,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &status_uri);

    /* 获取已保存 WiFi 列表 */
    httpd_uri_t saved_uri = {
        .uri      = "/api/saved",
        .method   = HTTP_GET,
        .handler  = web_handle_saved,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &saved_uri);

    /* 连接到指定已保存 WiFi */
    httpd_uri_t connect_uri = {
        .uri      = "/api/connect",
        .method   = HTTP_POST,
        .handler  = web_handle_connect,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &connect_uri);

    /* 删除已保存 WiFi */
    httpd_uri_t delete_uri = {
        .uri      = "/api/delete",
        .method   = HTTP_POST,
        .handler  = web_handle_delete,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &delete_uri);

    /* 重置重试状态接口 */
    httpd_uri_t reset_uri = {
        .uri      = "/api/reset_retry",
        .method   = HTTP_POST,
        .handler  = web_handle_reset_retry,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &reset_uri);

    return ESP_OK;
}

/**
 * @brief 挂载用于存放静态页面的 SPIFFS 分区
 *
 * base_path       固定为 /spiffs
 * partition_label 在 partition.csv 中需与此处保持一致（wifi_spiffs）
 * max_files       同时打开的最大文件数
 */
static esp_err_t web_mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = "wifi_spiffs",
        .max_files              = 4,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief 启动 Web 配网模块
 *
 * - 挂载 SPIFFS（存放 index.html）
 * - 启动 HTTP Server 并注册 URI
 * - 记录上层回调配置
 */
esp_err_t web_module_start(const web_module_config_t *config)
{
    /* 已经启动则直接返回 */
    if (s_httpd != NULL) {
        return ESP_OK;
    }

    /* 基于默认配置，再覆盖用户传入字段 */
    web_module_config_t cfg = WEB_MODULE_DEFAULT_CONFIG();
    if (config != NULL) {
        cfg = *config;
    }

    s_cfg = cfg;

    ESP_LOGI(TAG, "web module start, http_port=%u", (unsigned)cfg.http_port);

    esp_err_t ret = web_mount_spiffs();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = web_start_httpd(cfg.http_port);
    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief 停止 Web 配网模块
 *
 * 仅停止 HTTP Server。SPIFFS 可在整个应用生命周期中保持挂载，
 * 如需卸载可在上层调用 esp_vfs_spiffs_unregister("wifi_spiffs")。
 */
esp_err_t web_module_stop(void)
{
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }

    return ESP_OK;
}
