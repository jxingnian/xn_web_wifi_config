/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 16:38:01
 * @LastEditors: xingnian jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 18:30:06
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\src\wifi_module.c
 * @Description: WiFi 模块实现
 * 
 * Copyright (c) 2025 by ${git_name_email}, All Rights Reserved. 
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "wifi_module.h"

/* 日志 TAG，用于 ESP_LOGx 系列接口 */
static const char *TAG = "wifi_module";

/* 保存 WiFi 模块配置（是否启用 STA/AP、AP SSID/密码、事件回调等） */
static wifi_module_config_t s_wifi_cfg;

/* 标记 WiFi 模块是否已经完成初始化（调用过 xn_wifi_module_init 且成功） */
static bool s_wifi_inited = false;

/* 标记当前是否处于“正在尝试连接 STA”的过程，用于区分连接失败还是已连接后断开 */
static bool s_connecting = false;

/* 保存创建出来的 STA / AP 网络接口句柄（esp_netif） */
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

/**
 * @brief 统一封装事件回调调用
 *
 * @param event 当前发生的 WiFi 模块事件
 */
static void wifi_module_handle_event(wifi_module_event_t event)
{
    if (s_wifi_cfg.event_cb) {
        s_wifi_cfg.event_cb(event);
    }
}

/**
 * @brief ESP-IDF WiFi 事件处理函数
 *
 * @param arg        用户自定义参数（当前未使用）
 * @param event_base 事件基类型（这里只关注 WIFI_EVENT）
 * @param event_id   具体事件 ID（如 WIFI_EVENT_STA_CONNECTED 等）
 * @param event_data 事件数据指针（当前未使用）
 */
static void wifi_module_event_handler(void *arg,
                                      esp_event_base_t event_base,
                                      int32_t event_id,
                                      void *event_data)
{
    (void)arg;
    (void)event_data;

    /* 只处理 WiFi 事件，其它事件直接忽略 */
    if (event_base != WIFI_EVENT) {
        return;
    }

    switch (event_id) {
    case WIFI_EVENT_WIFI_READY:
        /* WiFi 驱动已就绪 */
        break;

    case WIFI_EVENT_SCAN_DONE:
        /* 扫描完成 */
        break;

    case WIFI_EVENT_STA_START:
        /* STA 接口启动 */
        break;

    case WIFI_EVENT_STA_STOP:
        /* STA 接口停止 */
        break;

    case WIFI_EVENT_STA_CONNECTED:
        /* STA 已成功连接到 AP，表示连接过程结束 */
        s_connecting = false;
        wifi_module_handle_event(WIFI_MODULE_EVENT_STA_CONNECTED);
        break;

    case WIFI_EVENT_STA_DISCONNECTED:
        /* STA 断开连接：
         * - 如果当前标记为“正在连接”，则说明是本次连接尝试失败；
         * - 否则说明是已连接状态下发生断开。
         */
        if (s_connecting) {
            s_connecting = false;
            wifi_module_handle_event(WIFI_MODULE_EVENT_STA_CONNECT_FAILED);
        } else {
            wifi_module_handle_event(WIFI_MODULE_EVENT_STA_DISCONNECTED);
        }
        break;

    case WIFI_EVENT_STA_AUTHMODE_CHANGE:
        /* 连接 AP 的加密方式发生变化 */
        break;

    case WIFI_EVENT_STA_WPS_ER_SUCCESS:
        /* WPS 注册成功 */
        break;

    case WIFI_EVENT_STA_WPS_ER_FAILED:
        /* WPS 注册失败 */
        break;

    case WIFI_EVENT_STA_WPS_ER_TIMEOUT:
        /* WPS 超时 */
        break;

    case WIFI_EVENT_STA_WPS_ER_PIN:
        /* WPS PIN 事件 */
        break;

    case WIFI_EVENT_STA_WPS_ER_PBC_OVERLAP:
        /* WPS PBC（按钮）冲突 */
        break;

    case WIFI_EVENT_AP_START:
        /* AP 接口启动（热点已开启） */
        break;

    case WIFI_EVENT_AP_STOP:
        /* AP 接口停止（热点已关闭） */
        break;

    case WIFI_EVENT_AP_STACONNECTED:
        /* 有 STA（终端设备）连接到本 AP */
        break;

    case WIFI_EVENT_AP_STADISCONNECTED:
        /* 有 STA（终端设备）从本 AP 断开 */
        break;

    case WIFI_EVENT_AP_PROBEREQRECVED:
        /* 收到 STA 的探测请求（用于扫描附近 AP） */
        break;

    default:
        /* 未来 IDF 版本可能新增 WIFI_EVENT，此处作为兜底占位 */
        break;
    }
}

/**
 * @brief IP 事件处理函数
 *
 * 罗列常见 IP_EVENT 事件，当前仅做占位和注释，
 * 方便后续根据需要补充具体逻辑。
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
        /* STA 获取到 IPv4 地址，认为 WiFi 已完全连接成功 */
        s_connecting = false;
        wifi_module_handle_event(WIFI_MODULE_EVENT_STA_GOT_IP);
        break;

    case IP_EVENT_STA_LOST_IP:
        /* STA 丢失 IPv4 地址 */
        break;

    case IP_EVENT_AP_STAIPASSIGNED:
        /* 给连接到 AP 的 STA 分配了 IPv4 地址 */
        break;

    case IP_EVENT_GOT_IP6:
        /* 获取到 IPv6 地址 */
        break;

    case IP_EVENT_ETH_GOT_IP:
        /* 以太网接口获取到 IPv4 地址 */
        break;

    case IP_EVENT_ETH_LOST_IP:
        /* 以太网接口丢失 IPv4 地址 */
        break;

    case IP_EVENT_PPP_GOT_IP:
        /* PPP 接口获取到 IPv4 地址 */
        break;

    case IP_EVENT_PPP_LOST_IP:
        /* PPP 接口丢失 IPv4 地址 */
        break;

    default:
        /* 未来 IDF 版本可能新增 IP_EVENT，此处作为兜底占位 */
        break;
    }
}

/**
 * @brief 初始化 NVS（用于 WiFi 组件存储 RF 校准等数据）
 *
 * 若检测到 NVS 分区空间不足或版本不兼容，则自动擦除并重新初始化。
 *
 * @return esp_err_t
 *         - ESP_OK                : 初始化成功
 *         - 其它错误码            : 初始化失败
 */
static esp_err_t wifi_module_init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();

    /* 如果是空间不足或版本变更，则先擦除再重新初始化 */
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    return ret;
}

/**
 * @brief WiFi 模块初始化
 *
 * @note
 *  - 该接口可多次调用，多次调用时只会在第一次有效执行。
 *  - config 允许为 NULL，NULL 时使用 WIFI_MODULE_DEFAULT_CONFIG 默认配置。
 *
 * @param config WiFi 模块配置指针
 * @return esp_err_t
 *         - ESP_OK                : 初始化成功（或已初始化）
 *         - 其它错误码            : 任何一步初始化失败时返回相应错误
 */
esp_err_t xn_wifi_module_init(const wifi_module_config_t *config)
{
    /* 若用户未传入配置，则使用默认配置宏 */
    if (config == NULL) {
        s_wifi_cfg = WIFI_MODULE_DEFAULT_CONFIG();
    } else {
        s_wifi_cfg = *config;
    }

    /* 若已初始化过，则直接返回成功，避免重复初始化底层组件 */
    if (s_wifi_inited) {
        return ESP_OK;
    }

    /* 1. 初始化 NVS（WiFi 组件依赖） */
    esp_err_t ret = wifi_module_init_nvs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2. 初始化 TCP/IP 网络栈（esp_netif），允许多次调用 */
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. 创建系统默认事件循环（若已存在则返回 ESP_ERR_INVALID_STATE，可忽略） */
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 4. 根据配置创建默认 STA / AP netif
     *
     *    - esp_netif_create_default_wifi_sta / ap 内部会创建 netif，并绑定到默认事件循环。
     *    - 只在指针为 NULL 且对应模式启用时创建一次。
     */
    if (s_wifi_cfg.enable_sta && s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    if (s_wifi_cfg.enable_ap && s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    /* 5. 初始化 WiFi 驱动（若已初始化，则可能返回 ESP_ERR_WIFI_INIT_STATE，可忽略） */
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_init_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 6. 根据配置设置 WiFi 工作模式
     *
     *    - enable_sta && enable_ap   -> WIFI_MODE_APSTA
     *    - enable_sta                -> WIFI_MODE_STA
     *    - enable_ap                 -> WIFI_MODE_AP
     *    - 都不启用                  -> WIFI_MODE_NULL（不设置）
     */
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

    /* 7. 如启用 AP 模式，则配置 AP 参数 */
    if (s_wifi_cfg.enable_ap) {
        /* 将配置结构清零，避免残留未初始化字段导致意外行为 */
        wifi_config_t ap_cfg = {0};

        /* 复制 SSID，并确保以 '\0' 结尾，防止越界 */
        strncpy((char *)ap_cfg.ap.ssid, s_wifi_cfg.ap_ssid, sizeof(ap_cfg.ap.ssid));
        ap_cfg.ap.ssid[sizeof(ap_cfg.ap.ssid) - 1] = '\0';
        ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);

        /* 复制密码，同样确保结尾 '\0' */
        strncpy((char *)ap_cfg.ap.password, s_wifi_cfg.ap_password, sizeof(ap_cfg.ap.password));
        ap_cfg.ap.password[sizeof(ap_cfg.ap.password) - 1] = '\0';

        /* 设置 AP 工作信道与最大连接数 */
        ap_cfg.ap.channel = s_wifi_cfg.ap_channel;
        ap_cfg.ap.max_connection = s_wifi_cfg.max_sta_conn;

        /* 根据密码是否为空选择加密方式：
         * - 空密码 -> 开放网络（WIFI_AUTH_OPEN）
         * - 非空   -> WPA/WPA2-PSK
         */
        ap_cfg.ap.authmode = (strlen(s_wifi_cfg.ap_password) == 0)
                                 ? WIFI_AUTH_OPEN
                                 : WIFI_AUTH_WPA_WPA2_PSK;

        /* 将 AP 配置写入 WiFi 驱动 */
        ret = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    /* 8. 注册 WiFi / IP 事件处理函数
     *
     *    - event_base = WIFI_EVENT
     *    - event_id   = ESP_EVENT_ANY_ID（监听所有 WiFi 事件）
     *    - handler    = wifi_module_event_handler
     */
    ret = esp_event_handler_register(WIFI_EVENT,
                                     ESP_EVENT_ANY_ID,
                                     &wifi_module_event_handler,
                                     NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_handler_register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 注册 IP_EVENT 处理函数，用于处理 STA/AP/ETH/PPP 相关 IP 事件 */
    ret = esp_event_handler_register(IP_EVENT,
                                     ESP_EVENT_ANY_ID,
                                     &wifi_module_ip_event_handler,
                                     NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_handler_register(IP_EVENT) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 9. 启动 WiFi 驱动
     *
     *    - esp_wifi_start 如果已经启动，某些版本可能返回 ESP_ERR_WIFI_CONN，可忽略。
     */
    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 标记状态：已初始化且 WiFi 已启动 */
    s_wifi_inited = true;

    return ESP_OK;
}

/**
 * @brief 以 STA 模式连接到指定 WiFi AP
 *
 *
 * @param ssid     目标 AP 的 SSID（不可为 NULL，且非空字符串）
 * @param password 目标 AP 的密码，可为 NULL 或空字符串表示开放网络
 *
 * @return esp_err_t
 *         - ESP_OK                : 已成功发起连接流程
 *         - ESP_ERR_INVALID_STATE : WiFi 模块尚未初始化或未启用 STA 模式
 *         - ESP_ERR_INVALID_ARG   : SSID 参数不合法
 *         - 其它错误码            : 设置配置/启动 WiFi/连接失败等
 */
esp_err_t xn_wifi_module_connect(const char *ssid, const char *password)
{
    /* 1. 检查 WiFi 模块是否初始化 */
    if (!s_wifi_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 2. 检查是否启用了 STA 模式 */
    if (!s_wifi_cfg.enable_sta) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 3. 检查 SSID 合法性（必须非 NULL 且非空） */
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* 将 STA 配置结构清零，避免未初始化字段影响行为 */
    wifi_config_t sta_cfg = {0};

    /* 拷贝 SSID，确保结尾 '\0' */
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    sta_cfg.sta.ssid[sizeof(sta_cfg.sta.ssid) - 1] = '\0';

    /* 拷贝密码（可选），为空则保持默认即可 */
    if (password != NULL) {
        strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));
        sta_cfg.sta.password[sizeof(sta_cfg.sta.password) - 1] = '\0';
    }

    esp_err_t ret;

    /* 4. 获取当前 WiFi 工作模式 */
    wifi_mode_t mode = WIFI_MODE_NULL;
    ret = esp_wifi_get_mode(&mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 若当前模式不支持 STA，则根据配置切换到 STA 或 APSTA 模式 */
    if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
        mode = s_wifi_cfg.enable_ap ? WIFI_MODE_APSTA : WIFI_MODE_STA;
        ret = esp_wifi_set_mode(mode);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    /* 5. 设置 STA 配置 */
    ret = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(STA) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 6. 发起连接请求，并设置“正在连接”标志
     *
     * 连接结果将在 wifi_module_event_handler 中处理，
     * 并通过 wifi_module_handle_event 上报给上层。
     */
    s_connecting = true;
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        s_connecting = false;
        return ret;
    }

    return ESP_OK;
}
