/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 16:38:01
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 20:47:00
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\src\wifi_module.c
 * @Description: WiFi 模块实现
 * 
 * Copyright (c) 2025 by ${git_name_email}, All Rights Reserved. 
 */

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "wifi_module.h"

/* 日志 TAG */
static const char *TAG = "wifi_module";

/* WiFi 模块配置（是否启用 STA/AP、AP SSID/密码、事件回调等） */
static wifi_module_config_t s_wifi_cfg;

/* 是否已完成 WiFi 模块初始化 */
static bool s_wifi_inited = false;

/* 当前是否处于“STA 正在尝试连接”的过程 */
static bool s_connecting = false;

/* STA / AP 网络接口句柄 */
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;

/**
 * @brief 统一转发 WiFi 模块事件到上层回调
 */
static void wifi_module_handle_event(wifi_module_event_t event)
{
    if (s_wifi_cfg.event_cb) {
        s_wifi_cfg.event_cb(event);
    }
}

/**
 * @brief WiFi 事件回调
 *
 * 只关心 WIFI_EVENT，统一在此转换为 wifi_module_event_t 上报。
 */
static void wifi_module_event_handler(void *arg,
                                      esp_event_base_t event_base,
                                      int32_t event_id,
                                      void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base != WIFI_EVENT) {
        return;
    }

    switch (event_id) {
    case WIFI_EVENT_WIFI_READY:
        /* WiFi 驱动就绪（一般无需处理） */
        break;

    case WIFI_EVENT_SCAN_DONE:
        /* 扫描完成（wifi_module_scan 为同步扫描，此处通常也不处理） */
        break;

    case WIFI_EVENT_STA_START:
        /* STA 接口已启动 */
        break;

    case WIFI_EVENT_STA_STOP:
        /* STA 接口已停止 */
        break;

    case WIFI_EVENT_STA_CONNECTED:
        /* STA 已与 AP 建立连接（不一定拿到 IP） */
        s_connecting = false;
        wifi_module_handle_event(WIFI_MODULE_EVENT_STA_CONNECTED);
        break;

    case WIFI_EVENT_STA_DISCONNECTED:
        /* STA 断开：
         * - 若当前标记为“正在连接”，视为本次连接尝试失败；
         * - 否则视为已连接后意外断开。
         */
        if (s_connecting) {
            s_connecting = false;
            wifi_module_handle_event(WIFI_MODULE_EVENT_STA_CONNECT_FAILED);
        } else {
            wifi_module_handle_event(WIFI_MODULE_EVENT_STA_DISCONNECTED);
        }
        break;

    case WIFI_EVENT_STA_AUTHMODE_CHANGE:
        /* AP 加密方式变化（一般无需处理） */
        break;

    case WIFI_EVENT_STA_WPS_ER_SUCCESS:
    case WIFI_EVENT_STA_WPS_ER_FAILED:
    case WIFI_EVENT_STA_WPS_ER_TIMEOUT:
    case WIFI_EVENT_STA_WPS_ER_PIN:
    case WIFI_EVENT_STA_WPS_ER_PBC_OVERLAP:
        /* WPS 相关事件，此项目未使用，保留占位 */
        break;

    case WIFI_EVENT_AP_START:
        /* AP 已启动 */
        break;

    case WIFI_EVENT_AP_STOP:
        /* AP 已停止 */
        break;

    case WIFI_EVENT_AP_STACONNECTED:
        /* 有终端连上 AP */
        break;

    case WIFI_EVENT_AP_STADISCONNECTED:
        /* 有终端从 AP 断开 */
        break;

    case WIFI_EVENT_AP_PROBEREQRECVED:
        /* 收到探测请求（扫描用），一般无需处理 */
        break;

    default:
        /* 预留给未来新增的 WIFI_EVENT */
        break;
    }
}

/**
 * @brief IP 事件回调
 *
 * 仅对需要的事件做处理，其余保持占位，方便后续扩展。
 */
static void wifi_module_ip_event_handler(void *arg,
                                         esp_event_base_t event_base,
                                         int32_t event_id,
                                         void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base != IP_EVENT) {
        return;
    }

    switch (event_id) {
    case IP_EVENT_STA_GOT_IP:
        /* STA 获取到 IPv4 地址，视为 WiFi 完全连接成功 */
        s_connecting = false;
        wifi_module_handle_event(WIFI_MODULE_EVENT_STA_GOT_IP);
        break;

    case IP_EVENT_STA_LOST_IP:
        /* STA 丢失 IPv4 地址，可视场景决定是否上报给上层 */
        break;

    case IP_EVENT_AP_STAIPASSIGNED:
        /* 给连上 AP 的 STA 分配了 IPv4 地址 */
        break;

    case IP_EVENT_GOT_IP6:
        /* 获取到 IPv6 地址 */
        break;

    case IP_EVENT_ETH_GOT_IP:
    case IP_EVENT_ETH_LOST_IP:
    case IP_EVENT_PPP_GOT_IP:
    case IP_EVENT_PPP_LOST_IP:
        /* 以太网/PPP 相关事件，此处不使用 */
        break;

    default:
        /* 预留给未来新增的 IP_EVENT */
        break;
    }
}

/**
 * @brief 初始化 NVS（WiFi 组件依赖）
 *
 * 若遇到“空间不足 / 版本不兼容”，自动擦除并重建。
 */
static esp_err_t wifi_module_init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    return ret;
}

/**
 * @brief WiFi 模块初始化
 *
 * - 可多次调用，仅第一次有效。
 * - config 为 NULL 时使用 WIFI_MODULE_DEFAULT_CONFIG。
 */
esp_err_t wifi_module_init(const wifi_module_config_t *config)
{
    /* 配置加载：优先使用用户配置，否则使用默认配置 */
    if (config == NULL) {
        s_wifi_cfg = WIFI_MODULE_DEFAULT_CONFIG();
    } else {
        s_wifi_cfg = *config;
    }

    /* 已初始化则直接返回 */
    if (s_wifi_inited) {
        return ESP_OK;
    }

    /* 1. 初始化 NVS */
    esp_err_t ret = wifi_module_init_nvs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2. 初始化网络栈（可重入） */
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. 创建默认事件循环（已存在时返回 ESP_ERR_INVALID_STATE，可忽略） */
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 4. 创建默认 STA / AP netif（各创建一次） */
    if (s_wifi_cfg.enable_sta && s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (s_wifi_cfg.enable_ap && s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    /* 如启用 AP 且配置了 ap_ip，则设置 AP IP（网关跟随 IP） */
    if (s_wifi_cfg.enable_ap && s_ap_netif != NULL && s_wifi_cfg.ap_ip[0] != '\0') {
        esp_netif_ip_info_t ip_info = {0};
        if (esp_netif_str_to_ip4(s_wifi_cfg.ap_ip, &ip_info.ip) == ESP_OK) {
            ip_info.gw = ip_info.ip;
            (void)esp_netif_set_ip_info(s_ap_netif, &ip_info);
        }
    }

    /* 5. 初始化 WiFi 驱动 */
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_init_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 6. 设置 WiFi 模式 */
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (s_wifi_cfg.enable_sta && s_wifi_cfg.enable_ap) {
        mode = WIFI_MODE_APSTA;
    } else if (s_wifi_cfg.enable_sta) {
        mode = WIFI_MODE_STA;
    } else if (s_wifi_cfg.enable_ap) {
        mode = WIFI_MODE_AP;
    }

    if (mode != WIFI_MODE_NULL) {
        ret = esp_wifi_set_mode(mode);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    /* 7. 如启用 AP，配置 AP 参数 */
    if (s_wifi_cfg.enable_ap) {
        wifi_config_t ap_cfg = {0};

        /* SSID */
        strncpy((char *)ap_cfg.ap.ssid, s_wifi_cfg.ap_ssid, sizeof(ap_cfg.ap.ssid));
        ap_cfg.ap.ssid[sizeof(ap_cfg.ap.ssid) - 1] = '\0';
        ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);

        /* 密码 */
        strncpy((char *)ap_cfg.ap.password, s_wifi_cfg.ap_password, sizeof(ap_cfg.ap.password));
        ap_cfg.ap.password[sizeof(ap_cfg.ap.password) - 1] = '\0';

        /* 信道、最大连接数 */
        ap_cfg.ap.channel       = s_wifi_cfg.ap_channel;
        ap_cfg.ap.max_connection = s_wifi_cfg.max_sta_conn;

        /* 加密方式：空密码为开放网络 */
        ap_cfg.ap.authmode = (strlen(s_wifi_cfg.ap_password) == 0)
                                 ? WIFI_AUTH_OPEN
                                 : WIFI_AUTH_WPA_WPA2_PSK;

        ret = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    /* 8. 注册 WiFi / IP 事件处理函数 */
    ret = esp_event_handler_register(WIFI_EVENT,
                                     ESP_EVENT_ANY_ID,
                                     &wifi_module_event_handler,
                                     NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_handler_register(WIFI_EVENT) failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_register(IP_EVENT,
                                     ESP_EVENT_ANY_ID,
                                     &wifi_module_ip_event_handler,
                                     NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_handler_register(IP_EVENT) failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    /* 9. 启动 WiFi 驱动 */
    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_wifi_inited = true;
    return ESP_OK;
}

/**
 * @brief 以 STA 模式连接指定 AP
 *
 * @param ssid     目标 AP SSID，必须非 NULL 且非空
 * @param password AP 密码，可为 NULL/空串 表示开放网络
 */
esp_err_t wifi_module_connect(const char *ssid, const char *password)
{
    if (!s_wifi_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_wifi_cfg.enable_sta) {
        return ESP_ERR_INVALID_STATE;
    }

    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t sta_cfg = {0};

    /* SSID */
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    sta_cfg.sta.ssid[sizeof(sta_cfg.sta.ssid) - 1] = '\0';

    /* 密码（可选） */
    if (password != NULL) {
        strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));
        sta_cfg.sta.password[sizeof(sta_cfg.sta.password) - 1] = '\0';
    }

    esp_err_t   ret;
    wifi_mode_t mode = WIFI_MODE_NULL;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        (void)esp_wifi_disconnect();
    }

    /* 获取当前模式 */
    ret = esp_wifi_get_mode(&mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 不支持 STA 时根据配置切换为 STA 或 APSTA */
    if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
        mode = s_wifi_cfg.enable_ap ? WIFI_MODE_APSTA : WIFI_MODE_STA;
        ret  = esp_wifi_set_mode(mode);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    /* 设置 STA 配置 */
    ret = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(STA) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 发起连接 */
    s_connecting = true;
    ret          = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        s_connecting = false;
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief 同步扫描附近 AP
 *
 * @param results     输出数组，长度由 *count_inout 指定
 * @param count_inout 入参：results 最大容量；出参：实际返回数量
 */
esp_err_t wifi_module_scan(wifi_module_scan_result_t *results, uint16_t *count_inout)
{
    if (!s_wifi_inited || !s_wifi_cfg.enable_sta ||
        results == NULL || count_inout == NULL || *count_inout == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_scan_config_t scan_cfg = {0};

    ESP_LOGI(TAG, "start wifi scan, max_out=%u", (unsigned)(*count_inout));

    /* 阻塞式扫描，直到完成 */
    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi scan start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint16_t ap_num = 0;
    ret             = esp_wifi_scan_get_ap_num(&ap_num);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi scan get num failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ap_num == 0) {
        ESP_LOGI(TAG, "wifi scan done: found 0 AP");
        *count_inout = 0;
        return ESP_OK;
    }

    uint16_t max_out = *count_inout;
    if (max_out == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ap_num > max_out) {
        ap_num = max_out;
    }

    wifi_ap_record_t *ap_list = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (ap_list == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = esp_wifi_scan_get_ap_records(&ap_num, ap_list);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi scan get records failed: %s", esp_err_to_name(ret));
        free(ap_list);
        return ret;
    }

    for (uint16_t i = 0; i < ap_num; ++i) {
        memset(results[i].ssid, 0, sizeof(results[i].ssid));
        strncpy(results[i].ssid,
                (const char *)ap_list[i].ssid,
                sizeof(results[i].ssid) - 1);
        results[i].rssi = ap_list[i].rssi;
    }

    *count_inout = ap_num;
    ESP_LOGI(TAG, "wifi scan done: found %u AP(s), out=%u", (unsigned)ap_num, (unsigned)(*count_inout));
    free(ap_list);
    return ESP_OK;
}
