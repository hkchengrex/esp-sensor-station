#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint8_t bytes[256];
    size_t used;
    int64_t last_report_us;
    bool seen, moving, occupied;
    uint16_t distance_cm;
} radar_t;
void radar_feed(radar_t *radar, const uint8_t *data, size_t size, int64_t now);
bool radar_fresh(const radar_t *radar, int64_t now);
