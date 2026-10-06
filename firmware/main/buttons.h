#pragma once
#include "esp_err.h"
#include <stdbool.h>
esp_err_t buttons_initialize(const char *device, const char *boot);
esp_err_t buttons_publish(bool connected_now);
bool buttons_usb_command(const char *command);
