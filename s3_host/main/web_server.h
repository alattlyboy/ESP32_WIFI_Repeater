#pragma once
#include "esp_err.h"
#include "app_config.h"

/* 固件版本：前端 /status 和启动日志都引用这里，改版本只需改一处 */
#define WEB_FW_VER "2.2.0"

esp_err_t web_server_start(const app_cfg_t *cfg);
