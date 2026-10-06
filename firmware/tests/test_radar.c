#include "radar.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    uint8_t frame[]={0xf4,0xf3,0xf2,0xf1,13,0,2,0xaa,1,30,0,50,0,0,0,30,0,0x55,0,0xf8,0xf7,0xf6,0xf5};
    for (size_t split=0;split<=sizeof(frame);split++) {
        radar_t r={0};
        radar_feed(&r,frame,split,100);
        radar_feed(&r,frame+split,sizeof(frame)-split,100);
        assert(radar_fresh(&r,100) && r.moving && r.occupied && r.distance_cm==30);
        assert(!radar_fresh(&r,2000100));
    }
    radar_t r={0};
    uint8_t garbage[]={0xf4,0xf3,0xf2,0xf1,255,255,1,2,3};
    radar_feed(&r,garbage,sizeof(garbage),100);
    frame[8]=2;
    radar_feed(&r,frame,sizeof(frame),100);
    assert(radar_fresh(&r,100) && !r.moving && r.occupied);
    frame[8]=3; frame[18]=1;
    radar_feed(&r,frame,sizeof(frame),200);
    assert(r.last_report_us==100);
    frame[18]=0; frame[6]=1;
    radar_feed(&r,frame,sizeof(frame),300);
    assert(r.moving && r.occupied && r.last_report_us==300);
    frame[8]=0;
    radar_feed(&r,frame,sizeof(frame),400);
    assert(!r.moving && !r.occupied);
    puts("Radar fragmentation, malformed length/footer, status and freshness passed");
}
