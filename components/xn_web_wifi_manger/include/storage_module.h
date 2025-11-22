/*
 * @Author: 星年 && jixingnian@gmail.com
 * @Date: 2025-11-22 16:38:49
 * @LastEditors: xingnian && jixingnian@gmail.com
 * @LastEditTime: 2025-11-22 21:00:00
 * @FilePath: \xn_web_wifi_config\components\xn_web_wifi_manger\include\storage_module.h
 * @Description: WiFi 存储模块（基于 NVS 的 WiFi 列表管理接口）
 *
 * 仅负责“存 / 取 / 删”WiFi 配置，不直接操作 WiFi 连接。
 */

#ifndef STORAGE_MODULE_H
#define STORAGE_MODULE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"  /* 提供 wifi_config_t 类型 */

/**
 * @brief WiFi 存储模块配置
 *
 * - nvs_namespace : 使用的 NVS 命名空间（建议单独使用一个命名空间）；
 * - max_wifi_num  : 最多保存的 WiFi 条目数量（>0，按“最近成功连接优先”排序）。
 */
typedef struct {
    const char *nvs_namespace;  ///< NVS 命名空间名（只保存字符串指针，不拷贝）
    uint8_t     max_wifi_num;   ///< WiFi 最大保存数量（0 时内部会强制设为 1）
} wifi_storage_config_t;

/**
 * @brief WiFi 存储模块默认配置
 *
 * - 命名空间： "wifi_store"
 * - 最多保存： 5 条 WiFi 配置
 */
#define WIFI_STORAGE_DEFAULT_CONFIG()        \
    (wifi_storage_config_t){                 \
        .nvs_namespace = "wifi_store",       \
        .max_wifi_num  = 5,                  \
    }

/**
 * @brief 初始化 WiFi 存储模块
 *
 * 负责：
 *  - 初始化 NVS（若空间不足或版本不兼容会自动擦除重建）；
 *  - 保存配置参数，用于后续读写 WiFi 列表。
 *
 * @param config 外部配置；可为 NULL，NULL 时使用 WIFI_STORAGE_DEFAULT_CONFIG。
 *
 * @return
 *  - ESP_OK                 : 成功（可重复调用，后续调用直接返回 ESP_OK）
 *  - ESP_ERR_INVALID_ARG    : 配置非法（理论上不会出现，内部已做兜底）
 *  - 其它 esp_err_t         : NVS 初始化相关错误
 */
esp_err_t wifi_storage_init(const wifi_storage_config_t *config);

/**
 * @brief 读取所有已保存的 WiFi 配置
 *
 * 列表按“最近成功连接优先”排序：
 *  - 下标 0 为当前推荐优先尝试连接的 WiFi；
 *  - 返回数量不超过初始化时设置的 max_wifi_num。
 *
 * @param[out] configs    调用方提供的数组，长度需 >= max_wifi_num
 * @param[out] count_out  实际读取到的条目数量（无数据时为 0）
 *
 * @return
 *  - ESP_OK              : 读取成功（包括无任何配置的情况）
 *  - ESP_ERR_INVALID_ARG : 参数为空
 *  - ESP_ERR_INVALID_STATE : 模块未初始化
 *  - 其它 esp_err_t      : NVS 读失败 / 数据格式异常等
 */
esp_err_t wifi_storage_load_all(wifi_config_t *configs, uint8_t *count_out);

/**
 * @brief 在 WiFi 成功连接后更新存储列表
 *
 * 一般在“STA 成功获取 IP”事件中调用，用于维护“最近成功连接”的有序列表。
 *
 * 策略：
 *  - 已存在同名 SSID：对应条目移动到首位，保持其余顺序不变；
 *  - 不存在该 SSID：
 *      - 若列表未满：将该配置插入首位；
 *      - 若列表已满：将该配置插入首位并丢弃最后一条。
 *
 * @param[in] config 本次成功连接使用的 wifi_config_t（完整结构体）
 *
 * @return
 *  - ESP_OK               : 更新成功
 *  - ESP_ERR_INVALID_ARG  : config 为空
 *  - ESP_ERR_INVALID_STATE: 模块未初始化
 *  - 其它 esp_err_t       : NVS 写失败等
 */
esp_err_t wifi_storage_on_connected(const wifi_config_t *config);

/**
 * @brief 按 SSID 删除已保存的 WiFi 配置
 *
 * 精确匹配 SSID（区分大小写），忽略密码等其它字段。
 * 若删除后列表为空，将擦除对应 NVS key。
 *
 * @param[in] ssid 要删除的 WiFi SSID（以 '\0' 结尾的字符串）
 *
 * @return
 *  - ESP_OK               : 删除成功（包括未找到目标时）
 *  - ESP_ERR_INVALID_ARG  : ssid 为空或空字符串
 *  - ESP_ERR_INVALID_STATE: 模块未初始化
 *  - 其它 esp_err_t       : NVS 读/写失败等
 */
esp_err_t wifi_storage_delete_by_ssid(const char *ssid);

#endif /* STORAGE_MODULE_H */
