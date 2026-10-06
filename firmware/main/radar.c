#include "radar.h"
#include <string.h>
static const uint8_t header[] = {0xf4,0xf3,0xf2,0xf1};
static const uint8_t footer[] = {0xf8,0xf7,0xf6,0xf5};
static void discard(radar_t *r, size_t n)
{
    memmove(r->bytes, r->bytes+n, r->used-n);
    r->used -= n;
}
void radar_feed(radar_t *r, const uint8_t *data, size_t size, int64_t now)
{
    for (size_t i=0; i<size; ++i) {
        if (r->used == sizeof(r->bytes)) discard(r, 1);
        r->bytes[r->used++] = data[i];
        while (r->used >= 4) {
            if (memcmp(r->bytes, header, 4)) { discard(r,1); continue; }
            if (r->used < 6) break;
            size_t length = r->bytes[4] | ((size_t)r->bytes[5]<<8);
            if (length < 13 || length > sizeof(r->bytes)-10) { discard(r,1); continue; }
            if (r->used < length+10) break;
            uint8_t *p = r->bytes+6;
            if (memcmp(p+length, footer, 4) || p[length-2]!=0x55 || p[length-1]!=0 ||
                (p[0]!=1 && p[0]!=2) || p[1]!=0xaa) { discard(r,1); continue; }
            // Noise calibration statuses are not presence measurements.
            if (p[2]<=3) {
                r->seen=true; r->last_report_us=now;
                r->moving=p[2]==1 || p[2]==3;
                r->occupied=p[2]!=0;
                r->distance_cm=p[9] | ((uint16_t)p[10]<<8);
            }
            discard(r,length+10);
        }
    }
}
bool radar_fresh(const radar_t *r, int64_t now)
{
    return r->seen && now >= r->last_report_us && now-r->last_report_us < 2000000;
}
