idf_component_register(
    SRCS "main.c"
         "app_config.c"
         "local_ap.c"
         "remote_sta.c"
         "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp_netif esp_event nvs_flash esp_wifi esp_http_server esp_timer cjson esp_driver_gpio
    PRIV_REQUIRES esp_wifi_remote esp_hosted wpa_supplicant
)

# 前端文件直接嵌进固件（符号名形如 _binary_index_html_start）
target_add_binary_data(${COMPONENT_LIB} "www/index.html" TEXT)
target_add_binary_data(${COMPONENT_LIB} "www/style.css"  TEXT)
target_add_binary_data(${COMPONENT_LIB} "www/app.js"     TEXT)
