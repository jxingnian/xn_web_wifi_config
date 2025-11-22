/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 21:45:00
 * @LastEditors: xingnian && jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 21:45:00
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\src\web_module.c
 * @Description: Web 配网模块实现（HTTP 服务器 + SPIFFS 静态资源）
 *
 * 仅负责：
 *  - 挂载 SPIFFS 分区并暴露静态网页资源；
 *  - 根据回调提供简单的状态查询接口；
 *
 * 不直接依赖 WiFi / 存储模块，由上层通过回调注入所需能力。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"

#include "web_module.h"

/* 日志 TAG */
static const char *TAG = "web_module";

/* Web 模块配置与状态 */
static bool               s_web_inited = false;
static web_module_config_t s_web_cfg;        /* 保存一份配置副本 */
static httpd_handle_t      s_http_server = NULL;

/* -------------------- SPIFFS 挂载辅助 -------------------- */

/**
 * @brief 挂载存放网页资源的 SPIFFS 分区
 *
 * 分区在 partitions.csv 中命名为 "wifi_spiffs"：
 * - base_path:  "/spiffs"（HTTP 处理函数按此路径访问文件）
 * - max_files:  4（当前仅三个静态文件，预留 1 个）
 */
static esp_err_t web_module_mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = "wifi_spiffs",
        .max_files              = 4,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret == ESP_ERR_INVALID_STATE) {
        /* 已经挂载，直接视为成功 */
        return ESP_OK;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spiffs register failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

/* -------------------- 静态文件响应辅助 -------------------- */

/**
 * @brief 以分块响应的方式发送一个静态文件
 *
 * @param req          HTTP 请求对象
 * @param file_path    文件在 SPIFFS 上的完整路径（如 "/spiffs/index.html"）
 * @param content_type Content-Type 头部值
 */
static esp_err_t web_module_serve_file(httpd_req_t *req,
                                       const char  *file_path,
                                       const char  *content_type)
{
    FILE *f = fopen(file_path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "open file failed: %s", file_path);
        httpd_resp_send_err(req,
                            HTTPD_500_INTERNAL_SERVER_ERROR,
                            "open file failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    char  buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            httpd_resp_sendstr_chunk(req, NULL); /* 结束分块 */
            return ESP_FAIL;
        }
    }

    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); /* 告知响应结束 */
    return ESP_OK;
}

/* -------------------- 具体 URI 处理函数 -------------------- */

/**
 * @brief 根路径与 /index.html：返回主页面
 */
static esp_err_t web_module_root_get_handler(httpd_req_t *req)
{
    return web_module_serve_file(req, "/spiffs/index.html", "text/html");
}

/**
 * @brief app.css：页面样式
 */
static esp_err_t web_module_css_get_handler(httpd_req_t *req)
{
    return web_module_serve_file(req, "/spiffs/app.css", "text/css");
}

/**
 * @brief app.js：前端脚本
 */
static esp_err_t web_module_js_get_handler(httpd_req_t *req)
{
    return web_module_serve_file(req, "/spiffs/app.js", "application/javascript");
}

/**
 * @brief /api/wifi/status：查询当前 WiFi 状态（可选）
 *
 * 若未配置回调，返回简单的占位结果，方便前端调试。
 */
static esp_err_t web_module_status_get_handler(httpd_req_t *req)
{
    web_wifi_status_t status = {0};
    char              json[192];

    if (s_web_cfg.get_status_cb) {
        if (s_web_cfg.get_status_cb(&status) != ESP_OK) {
            httpd_resp_send_err(req,
                                HTTPD_500_INTERNAL_SERVER_ERROR,
                                "status query failed");
            return ESP_OK;
        }
    } else {
        /* 未提供回调时给出一个简单占位值 */
        status.connected = false;
        status.state     = WEB_WIFI_STATUS_STATE_IDLE;
        strncpy(status.ssid, "-", sizeof(status.ssid));
        status.ssid[sizeof(status.ssid) - 1] = '\0';
        strncpy(status.ip, "-", sizeof(status.ip));
        status.ip[sizeof(status.ip) - 1] = '\0';
        status.rssi = 0;
        strncpy(status.mode, "-", sizeof(status.mode));
        status.mode[sizeof(status.mode) - 1] = '\0';
    }

    /* 简单 JSON 序列化：假定 SSID/IP/mode 不包含引号等特殊字符 */
    int len = snprintf(json,
                       sizeof(json),
                       "{\"connected\":%s,\"state\":%d,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"mode\":\"%s\"}",
                       status.connected ? "true" : "false",
                       (int)status.state,
                       status.ssid,
                       status.ip,
                       (int)status.rssi,
                       status.mode);

    if (len < 0) {
        httpd_resp_send_err(req,
                            HTTPD_500_INTERNAL_SERVER_ERROR,
                            "format json failed");
        return ESP_OK;
    }
    if (len >= (int)sizeof(json)) {
        len = (int)sizeof(json) - 1;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, json, len);
    return ESP_OK;
}

/**
 * @brief /api/wifi/saved：获取已保存 WiFi 列表
 */
static esp_err_t web_module_saved_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    /* 未提供回调时返回空列表，方便前端统一处理 */
    if (s_web_cfg.get_saved_list_cb == NULL) {
        static const char *EMPTY_JSON = "{\"items\":[]}";
        httpd_resp_send(req, EMPTY_JSON, strlen(EMPTY_JSON));
        return ESP_OK;
    }

    /* 第一次调用仅查询需要的条目数量，避免多余的堆分配 */
    size_t    cnt = 0;
    esp_err_t ret = s_web_cfg.get_saved_list_cb(NULL, &cnt);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req,
                            HTTPD_500_INTERNAL_SERVER_ERROR,
                            "load saved wifi failed");
        return ESP_OK;
    }

    if (cnt == 0) {
        static const char *EMPTY_JSON = "{\"items\":[]}";
        httpd_resp_send(req, EMPTY_JSON, strlen(EMPTY_JSON));
        return ESP_OK;
    }

    web_saved_wifi_info_t *list = (web_saved_wifi_info_t *)malloc(cnt * sizeof(web_saved_wifi_info_t));
    if (list == NULL) {
        httpd_resp_send_err(req,
                            HTTPD_500_INTERNAL_SERVER_ERROR,
                            "no memory");
        return ESP_OK;
    }

    size_t cap = cnt;
    ret        = s_web_cfg.get_saved_list_cb(list, &cap);
    if (ret != ESP_OK) {
        free(list);
        httpd_resp_send_err(req,
                            HTTPD_500_INTERNAL_SERVER_ERROR,
                            "load saved wifi failed");
        return ESP_OK;
    }

    if (cap > cnt) {
        cap = cnt;
    }

    /* 序列化为形如 {"items":[{"index":0,"ssid":"xxx"}, ...]} 的 JSON */
    char  json[512];
    size_t offset = 0;

    offset += (size_t)snprintf(json + offset, sizeof(json) - offset, "{\"items\":[");

    for (size_t i = 0; i < cap && offset < sizeof(json); i++) {
        const char *comma = (i == 0) ? "" : ",";
        offset += (size_t)snprintf(json + offset,
                                   sizeof(json) - offset,
                                   "%s{\"index\":%u,\"ssid\":\"%s\"}",
                                   comma,
                                   (unsigned)i,
                                   list[i].ssid);
    }

    free(list);

    if (offset >= sizeof(json)) {
        /* 理论上不会超出，若超出则截断为一个空列表作为兜底 */
        const char *FALLBACK = "{\"items\":[]}";
        httpd_resp_send(req, FALLBACK, strlen(FALLBACK));
        return ESP_OK;
    }

    offset += (size_t)snprintf(json + offset, sizeof(json) - offset, "]}");

    httpd_resp_send(req, json, (int)offset);
    return ESP_OK;
}

/**
 * @brief /api/wifi/saved/delete：按 SSID 删除一条已保存 WiFi
 */
static esp_err_t web_module_saved_delete_handler(httpd_req_t *req)
{
    if (s_web_cfg.delete_saved_cb == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "delete not supported");
        return ESP_OK;
    }

    /* 解析 URL 查询字符串中的 ssid 参数 */
    char  query[64] = {0};
    char  ssid[32]  = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
        return ESP_OK;
    }

    if (httpd_query_key_value(query, "ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_OK;
    }

    esp_err_t ret = s_web_cfg.delete_saved_cb(ssid);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req,
                            HTTPD_500_INTERNAL_SERVER_ERROR,
                            "delete failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", strlen("{\"ok\":true}"));
    return ESP_OK;
}

/* -------------------- HTTP 服务器启动 -------------------- */

/**
 * @brief 启动 HTTP 服务器并注册基础 URI
 */
static esp_err_t web_module_start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    if (s_web_cfg.http_port > 0) {
        config.server_port = (uint16_t)s_web_cfg.http_port;
    }

    /*
     * 若服务器已启动则直接返回成功，避免重复 start。
     */
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    esp_err_t ret = httpd_start(&s_http_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        s_http_server = NULL;
        return ret;
    }

    /* 静态文件路由：根路径与 /index.html 指向同一处理函数 */
    static const httpd_uri_t uri_root = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = web_module_root_get_handler,
        .user_ctx = NULL,
    };

    static const httpd_uri_t uri_index = {
        .uri      = "/index.html",
        .method   = HTTP_GET,
        .handler  = web_module_root_get_handler,
        .user_ctx = NULL,
    };

    static const httpd_uri_t uri_css = {
        .uri      = "/app.css",
        .method   = HTTP_GET,
        .handler  = web_module_css_get_handler,
        .user_ctx = NULL,
    };

    static const httpd_uri_t uri_js = {
        .uri      = "/app.js",
        .method   = HTTP_GET,
        .handler  = web_module_js_get_handler,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(s_http_server, &uri_root);
    httpd_register_uri_handler(s_http_server, &uri_index);
    httpd_register_uri_handler(s_http_server, &uri_css);
    httpd_register_uri_handler(s_http_server, &uri_js);

    /* 仅在配置了回调的前提下注册状态接口，保持职责清晰 */
    if (s_web_cfg.get_status_cb != NULL) {
        static const httpd_uri_t uri_status = {
            .uri      = "/api/wifi/status",
            .method   = HTTP_GET,
            .handler  = web_module_status_get_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(s_http_server, &uri_status);
    }

    /* 已保存 WiFi 列表接口（可选） */
    if (s_web_cfg.get_saved_list_cb != NULL) {
        static const httpd_uri_t uri_saved = {
            .uri      = "/api/wifi/saved",
            .method   = HTTP_GET,
            .handler  = web_module_saved_get_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(s_http_server, &uri_saved);
    }

    /* 删除已保存 WiFi 接口（可选） */
    if (s_web_cfg.delete_saved_cb != NULL) {
        static const httpd_uri_t uri_saved_del = {
            .uri      = "/api/wifi/saved/delete",
            .method   = HTTP_POST,
            .handler  = web_module_saved_delete_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(s_http_server, &uri_saved_del);
    }

    return ESP_OK;
}

/* -------------------- 对外初始化接口 -------------------- */

esp_err_t web_module_init(const web_module_config_t *config)
{
    /* 使用默认配置或外部配置 */
    if (config == NULL) {
        s_web_cfg = WEB_MODULE_DEFAULT_CONFIG();
    } else {
        s_web_cfg = *config;
    }

    /* 已初始化则直接返回 */
    if (s_web_inited) {
        return ESP_OK;
    }

    esp_err_t ret = web_module_mount_spiffs();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = web_module_start_server();
    if (ret != ESP_OK) {
        return ret;
    }

    s_web_inited = true;
    return ESP_OK;
}

