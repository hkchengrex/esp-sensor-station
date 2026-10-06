#pragma once
#include "mqtt_connection.h"
#include "esp_err.h"
esp_err_t station_initialize(const char *device_id);
void station_receive(const char *data, size_t length, bool retained, void *context);
esp_err_t station_publish(bool discovery);
