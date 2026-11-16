idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_event nvs_flash
    PRIV_REQUIRES esp_hosted
)
