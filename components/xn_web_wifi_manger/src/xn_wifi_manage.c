/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 16:24:42
 * @LastEditors: xingnian && jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 20:30:00
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\src\xn_wifi_manage.c
 * @Description: WiFi 管理模块实现（封装 WiFi / 存储 / Web 配网，提供自动重连与状态管理）
 */

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_netif.h"

#include "wifi_module.h"
#include "storage_module.h"
#include "web_module.h"
#include "xn_wifi_manage.h"

/* 日志 TAG（如需日志输出，使用 ESP_LOGx(TAG, ...)） */
static const char *TAG = "wifi_manage";

/* 当前 WiFi 管理状态 */
static wifi_manage_state_t  s_wifi_manage_state = WIFI_MANAGE_STATE_DISCONNECTED;
/* 上层传入的管理配置（保存 WiFi 数量、重连间隔、AP 信息等） */
static wifi_manage_config_t s_wifi_cfg;
/* WiFi 管理任务句柄 */
static TaskHandle_t         s_wifi_manage_task  = NULL;

/* 遍历已保存 WiFi 时的状态 */
static bool       s_wifi_connecting   = false;  /* 当前是否有一次 STA 连接正在进行 */
static uint8_t    s_wifi_try_index    = 0;      /* 本轮遍历中，正在尝试的 WiFi 下标 */
static TickType_t s_connect_failed_ts = 0;      /* 最近一次全轮尝试失败的时间戳 */

/* -------------------- Web 回调：扫描附近 WiFi -------------------- */
/**
 * @brief Web 模块回调：同步扫描附近 AP
 *
 * @param list         输出结果数组（由 Web 模块分配）
 * @param count_inout  入参为数组最大容量，出参为实际填充数量
 */
static esp_err_t web_cb_scan(web_scan_result_t *list, uint16_t *count_inout)
{
    if (list == NULL || count_inout == NULL || *count_inout == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t                   count = *count_inout;
    /* 临时缓冲区，用于承接底层扫描结果 */
    wifi_module_scan_result_t *tmp   = calloc(count, sizeof(wifi_module_scan_result_t));
    if (tmp == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 调用底层 WiFi 模块执行扫描 */
    esp_err_t ret = wifi_module_scan(tmp, &count);
    if (ret != ESP_OK) {
        free(tmp);
        return ret;
    }

    /* 拷贝 SSID / RSSI 到 Web 层定义的结构体 */
    for (uint16_t i = 0; i < count; ++i) {
        memset(list[i].ssid, 0, sizeof(list[i].ssid));
        strncpy(list[i].ssid, tmp[i].ssid, sizeof(list[i].ssid) - 1);
        list[i].rssi = tmp[i].rssi;
    }

    *count_inout = count;
    free(tmp);
    return ESP_OK;
}

/* -------------------- Web 回调：提交新 WiFi 配置 -------------------- */
/**
 * @brief Web 模块回调：根据输入的 SSID / 密码发起一次连接
 */
static esp_err_t web_cb_configure(const char *ssid, const char *password)
{
    return wifi_module_connect(ssid, password);
}

/* -------------------- Web 回调：获取当前 WiFi 状态 -------------------- */
/**
 * @brief Web 模块回调：查询 STA 当前连接状态、IP、BSSID 等
 */
static esp_err_t web_cb_get_status(web_wifi_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));

    /* 仅在 STA 或 APSTA 模式下才认为可能已连接路由器 */
    wifi_mode_t mode;
    esp_err_t   ret = esp_wifi_get_mode(&mode);
    if (ret != ESP_OK) {
        return ret;
    }

    if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
        status->connected = false;
        return ESP_OK;
    }

    /* 获取当前连接 AP 的信息（若未连接会返回错误） */
    wifi_ap_record_t ap_info;
    ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ret != ESP_OK) {
        status->connected = false;
        return ESP_OK;
    }

    status->connected = true;
    strncpy(status->ssid, (const char *)ap_info.ssid, sizeof(status->ssid) - 1);
    status->rssi = ap_info.rssi;

    /* 查询 STA 接口 IP 地址 */
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t        *netif   = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        (void)esp_netif_get_ip_info(netif, &ip_info);
    }

    snprintf(status->ip,
             sizeof(status->ip),
             "%d.%d.%d.%d",
             IP2STR(&ip_info.ip));

    snprintf(status->bssid,
             sizeof(status->bssid),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             ap_info.bssid[0],
             ap_info.bssid[1],
             ap_info.bssid[2],
             ap_info.bssid[3],
             ap_info.bssid[4],
             ap_info.bssid[5]);

    return ESP_OK;
}

/* -------------------- Web 回调：查询已保存 WiFi 列表 -------------------- */
/**
 * @brief Web 模块回调：获取 NVS 中保存的 WiFi SSID 列表
 *
 * 仅返回 SSID，不含密码。
 */
static esp_err_t web_cb_get_saved(web_saved_wifi_t *list, uint8_t *count_inout)
{
    if (list == NULL || count_inout == NULL || *count_inout == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t max_out = *count_inout;

    /* 管理配置中指定的最大保存数量，最少为 1 */
    uint8_t max_internal = (s_wifi_cfg.save_wifi_count <= 0)
                               ? 1
                               : (uint8_t)s_wifi_cfg.save_wifi_count;

    wifi_config_t *cfg_list = calloc(max_internal, sizeof(wifi_config_t));
    if (cfg_list == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t   count = 0;
    esp_err_t ret   = wifi_storage_load_all(cfg_list, &count);
    if (ret != ESP_OK) {
        free(cfg_list);
        return ret;
    }

    if (count == 0) {
        *count_inout = 0;
        free(cfg_list);
        return ESP_OK;
    }

    if (count > max_out) {
        count = max_out;
    }

    /* 仅拷贝 SSID 给 Web 层，避免泄露密码 */
    for (uint8_t i = 0; i < count; ++i) {
        memset(list[i].ssid, 0, sizeof(list[i].ssid));
        strncpy(list[i].ssid,
                (const char *)cfg_list[i].sta.ssid,
                sizeof(list[i].ssid) - 1);
    }

    *count_inout = count;
    free(cfg_list);
    return ESP_OK;
}

/* -------------------- Web 回调：按已保存配置连接 -------------------- */
/**
 * @brief Web 模块回调：根据传入 SSID 查找已保存配置，并发起连接
 */
static esp_err_t web_cb_connect_saved(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t max_internal = (s_wifi_cfg.save_wifi_count <= 0)
                               ? 1
                               : (uint8_t)s_wifi_cfg.save_wifi_count;

    wifi_config_t *cfg_list = calloc(max_internal, sizeof(wifi_config_t));
    if (cfg_list == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t   count = 0;
    esp_err_t ret   = wifi_storage_load_all(cfg_list, &count);
    if (ret != ESP_OK) {
        free(cfg_list);
        return ret;
    }

    /* 按 SSID 精确匹配，然后用保存的密码发起连接 */
    for (uint8_t i = 0; i < count; ++i) {
        if (strncmp((const char *)cfg_list[i].sta.ssid,
                    ssid,
                    sizeof(cfg_list[i].sta.ssid)) == 0) {
            const char *pwd = (cfg_list[i].sta.password[0] == '\0')
                                  ? NULL
                                  : (const char *)cfg_list[i].sta.password;
            ret = wifi_module_connect(ssid, pwd);
            free(cfg_list);
            return ret;
        }
    }

    free(cfg_list);
    return ESP_ERR_NOT_FOUND;
}

/* -------------------- Web 回调：删除已保存 WiFi -------------------- */
/**
 * @brief Web 模块回调：按 SSID 删除保存的条目
 */
static esp_err_t web_cb_delete_saved(const char *ssid)
{
    return wifi_storage_delete_by_ssid(ssid);
}

/* -------------------- Web 回调：重置状态机重试状态 -------------------- */
/**
 * @brief Web 模块回调：重置自动重连相关变量
 */
static esp_err_t web_cb_reset_retry(void)
{
    s_wifi_try_index    = 0;
    s_wifi_connecting   = false;
    s_connect_failed_ts = 0;
    s_wifi_manage_state = WIFI_MANAGE_STATE_DISCONNECTED;
    return ESP_OK;
}

/* -------------------- WiFi 模块事件回调 -------------------- */
/**
 * @brief 供 WiFi 模块调用的事件回调，用于驱动管理状态机
 */
static void wifi_manage_on_wifi_event(wifi_module_event_t event)
{
    switch (event) {
    case WIFI_MODULE_EVENT_STA_CONNECTED:
        /* 已与 AP 建立物理连接，但可能尚未获取 IP，此处仅作占位 */
        break;

    case WIFI_MODULE_EVENT_STA_GOT_IP: {
        /* 获取到 IP，认为一次连接流程成功结束 */
        s_wifi_manage_state = WIFI_MANAGE_STATE_CONNECTED;
        s_wifi_connecting   = false;
        s_wifi_try_index    = 0;      /* 下次自动重连从首选 WiFi 开始 */
        s_connect_failed_ts = 0;

        /* 将当前配置上报给存储模块，用于调整优先级等策略 */
        wifi_config_t current_cfg = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &current_cfg) == ESP_OK) {
            (void)wifi_storage_on_connected(&current_cfg);
        }
        break;
    }

    case WIFI_MODULE_EVENT_STA_DISCONNECTED:
        /* 连接断开，等待管理任务按策略进行重连 */
        s_wifi_manage_state = WIFI_MANAGE_STATE_DISCONNECTED;
        s_wifi_connecting   = false;
        s_wifi_try_index    = 0;
        break;

    case WIFI_MODULE_EVENT_STA_CONNECT_FAILED:
        /* 本次尝试失败，简单移动到下一条配置 */
        s_wifi_connecting = false;
        s_wifi_try_index++;
        break;

    default:
        /* 其他事件暂不关心 */
        break;
    }
}

/* -------------------- 状态机核心逻辑 -------------------- */
/**
 * @brief 单步执行 WiFi 管理状态机
 *
 * 按当前状态决定是否发起连接、切换状态或等待重试。
 */
static void wifi_manage_step(void)
{
    switch (s_wifi_manage_state) {
    case WIFI_MANAGE_STATE_DISCONNECTED: {
        /* 断开状态：按顺序遍历已保存 WiFi，逐个尝试连接 */

        if (s_wifi_connecting) {
            /* 已经有一个连接操作在进行，等待事件回调给结果 */
            break;
        }

        /* 从存储模块加载全部配置，数量受 save_wifi_count 限制 */
        uint8_t max_num = (s_wifi_cfg.save_wifi_count <= 0)
                              ? 1
                              : (uint8_t)s_wifi_cfg.save_wifi_count;

        wifi_config_t list[max_num];
        uint8_t       count = 0;

        if (wifi_storage_load_all(list, &count) != ESP_OK || count == 0) {
            /* 没有可用配置，交由上层决定是否启用纯 AP 配网等逻辑 */
            break;
        }

        if (s_wifi_try_index >= count) {
            /* 本轮所有配置均尝试过，仍未连接成功，进入“整轮失败”状态 */
            s_wifi_manage_state = WIFI_MANAGE_STATE_CONNECT_FAILED;
            s_connect_failed_ts = xTaskGetTickCount();
            s_wifi_try_index    = 0;
            s_wifi_connecting   = false;
            break;
        }

        wifi_config_t *cfg = &list[s_wifi_try_index];
        if (cfg->sta.ssid[0] == '\0') {
            /* 跳过无效 SSID */
            s_wifi_try_index++;
            break;
        }

        const char *ssid     = (const char *)cfg->sta.ssid;
        const char *password = (cfg->sta.password[0] == '\0')
                                   ? NULL
                                   : (const char *)cfg->sta.password;

        /* 尝试发起连接，成功则等待事件回调，失败则立即切换到下一条 */
        if (wifi_module_connect(ssid, password) == ESP_OK) {
            s_wifi_connecting = true;
        } else {
            s_wifi_try_index++;
        }
        break;
    }

    case WIFI_MANAGE_STATE_CONNECTED:
        /* 已连接状态下，当前不做周期性操作，保持静默 */
        break;

    case WIFI_MANAGE_STATE_CONNECT_FAILED: {
        /* 一轮全部失败，根据配置的重连间隔决定何时重新遍历 */

        if (s_wifi_cfg.reconnect_interval_ms < 0) {
            /* 小于 0 表示关闭自动重连，保持在失败状态 */
            break;
        }

        TickType_t now   = xTaskGetTickCount();
        TickType_t delta = now - s_connect_failed_ts;
        TickType_t need  = pdMS_TO_TICKS((s_wifi_cfg.reconnect_interval_ms <= 0)
                                             ? 0
                                             : s_wifi_cfg.reconnect_interval_ms);

        if (delta >= need) {
            /* 到达重试时间，从头开始新一轮遍历 */
            s_wifi_try_index    = 0;
            s_wifi_connecting   = false;
            s_wifi_manage_state = WIFI_MANAGE_STATE_DISCONNECTED;
        }
        break;
    }

    default:
        /* 理论上不应到达，保留作防护 */
        break;
    }
}

/* -------------------- WiFi 管理任务 -------------------- */
/**
 * @brief 管理任务：周期性驱动状态机运行
 */
static void wifi_manage_task(void *arg)
{
    (void)arg;

    for (;;) {
        wifi_manage_step();
        vTaskDelay(pdMS_TO_TICKS(WIFI_MANAGE_STEP_INTERVAL_MS));
    }
}

/* -------------------- 管理模块初始化 -------------------- */
/**
 * @brief  WiFi 管理模块初始化入口
 *
 * 负责：
 * 1. 保存并标准化上层配置
 * 2. 初始化 WiFi 模块（STA+AP）
 * 3. 初始化存储模块（保存常用 WiFi）
 * 4. 初始化 Web 配网模块（HTTP 服务与回调）
 * 5. 创建管理任务，启动状态机
 */
esp_err_t wifi_manage_init(const wifi_manage_config_t *config)
{
    /* 使用默认配置或上层传入配置 */
    if (config == NULL) {
        s_wifi_cfg = WIFI_MANAGE_DEFAULT_CONFIG();
    } else {
        s_wifi_cfg = *config;
    }

    /* ---- 初始化 WiFi 模块 ---- */
    wifi_module_config_t wifi_cfg = WIFI_MODULE_DEFAULT_CONFIG();

    /* 管理模块要求同时启用 STA + AP：
     * - STA 负责连接路由器上网
     * - AP 负责本地配网访问
     */
    wifi_cfg.enable_sta = true;
    wifi_cfg.enable_ap  = true;

    /* 配网 AP SSID */
    strncpy(wifi_cfg.ap_ssid, s_wifi_cfg.ap_ssid, sizeof(wifi_cfg.ap_ssid));
    wifi_cfg.ap_ssid[sizeof(wifi_cfg.ap_ssid) - 1] = '\0';

    /* 配网 AP 密码 */
    strncpy(wifi_cfg.ap_password, s_wifi_cfg.ap_password, sizeof(wifi_cfg.ap_password));
    wifi_cfg.ap_password[sizeof(wifi_cfg.ap_password) - 1] = '\0';

    /* 配网 AP IP 地址 */
    strncpy(wifi_cfg.ap_ip, s_wifi_cfg.ap_ip, sizeof(wifi_cfg.ap_ip));
    wifi_cfg.ap_ip[sizeof(wifi_cfg.ap_ip) - 1] = '\0';

    /* 绑定 WiFi 事件回调 */
    wifi_cfg.event_cb = wifi_manage_on_wifi_event;

    /* 初始化底层 WiFi 模块 */
    esp_err_t ret = wifi_module_init(&wifi_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    /* ---- 初始化存储模块 ---- */
    wifi_storage_config_t storage_cfg = WIFI_STORAGE_DEFAULT_CONFIG();

    /* 保存 WiFi 数量下限为 1，避免 0 导致逻辑异常 */
    if (s_wifi_cfg.save_wifi_count <= 0) {
        storage_cfg.max_wifi_num = 1;
    } else {
        storage_cfg.max_wifi_num = (uint8_t)s_wifi_cfg.save_wifi_count;
    }

    ret = wifi_storage_init(&storage_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    /* ---- 初始化 Web 配网模块 ---- */
    web_module_config_t web_cfg = WEB_MODULE_DEFAULT_CONFIG();

    /* HTTP 端口：合法范围 (0, 65535]，否则使用默认值 */
    if (s_wifi_cfg.web_port > 0 && s_wifi_cfg.web_port <= 65535) {
        web_cfg.http_port = (uint16_t)s_wifi_cfg.web_port;
    }

    /* 绑定所有 Web 回调接口 */
    web_cfg.scan_cb          = web_cb_scan;
    web_cfg.configure_cb     = web_cb_configure;
    web_cfg.get_status_cb    = web_cb_get_status;
    web_cfg.get_saved_cb     = web_cb_get_saved;
    web_cfg.connect_saved_cb = web_cb_connect_saved;
    web_cfg.delete_saved_cb  = web_cb_delete_saved;
    web_cfg.reset_retry_cb   = web_cb_reset_retry;

    ret = web_module_start(&web_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    // 创建WiFi管理任务
    if (s_wifi_manage_task == NULL) {
        BaseType_t ret_task = xTaskCreate(
            wifi_manage_task,
            "wifi_manage",
            4096,               // 任务栈大小，可根据实际需要调整
            NULL,
            tskIDLE_PRIORITY + 1,   // 任务优先级，可根据实际需要调整
            &s_wifi_manage_task);

        if (ret_task != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}
