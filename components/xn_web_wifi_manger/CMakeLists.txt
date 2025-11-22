idf_component_register(
    SRCS 
        "src/xn_wifi_manage.c" 
        "src/wifi_module.c" 
        "src/web_module.c" 
        "src/storage_module.c"
    INCLUDE_DIRS 
        "include"
    REQUIRES 
        esp_http_server  
        spiffs         
        esp_wifi
        nvs_flash
)

# 创建SPIFFS分区镜像
spiffs_create_partition_image(wifi_spiffs wifi_spiffs FLASH_IN_PROJECT)