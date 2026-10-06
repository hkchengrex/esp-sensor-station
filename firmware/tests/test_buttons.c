#include "button_filter.h"
#include <assert.h>
#include <stdio.h>

static void sample(button_filter_t *f, bool pressed, uint64_t now,
                   button_edge_t expected, bool activate) {
    button_change_t c=button_filter_update(f,pressed,now);
    assert(c.edge==expected && c.activate==activate);
}
static void none(button_filter_t *f, bool pressed, uint64_t now) {
    sample(f,pressed,now,BUTTON_NO_EDGE,false);
}

int main(void) {
    button_filter_t f;
    // Sample a full cycle at the production 10 ms scan interval. Bounce on
    // either side restarts the 30 ms qualification; holding never repeats.
    button_filter_init(&f,false,0);
    none(&f,true,10000); none(&f,false,20000); none(&f,true,30000);
    none(&f,true,40000); none(&f,true,50000);
    sample(&f,true,60000,BUTTON_PRESSED,false);
    for(uint64_t t=70000;t<=100000;t+=BUTTON_POLL_MS*1000ULL) none(&f,true,t);
    none(&f,false,110000); none(&f,true,120000); none(&f,false,130000);
    none(&f,false,140000); none(&f,false,150000);
    sample(&f,false,160000,BUTTON_RELEASED,true);
    for(uint64_t t=170000;t<=300000;t+=BUTTON_POLL_MS*1000ULL) none(&f,false,t);

    // Sub-threshold taps do not arm an action.
    button_filter_init(&f,false,0);
    none(&f,true,10000); none(&f,false,30000);
    for(uint64_t t=40000;t<=80000;t+=10000) none(&f,false,t);
    assert(!f.armed);

    // No action when releasing a key held at startup. A subsequent cycle works.
    button_filter_init(&f,true,0);
    none(&f,true,10000); none(&f,false,20000); none(&f,false,40000);
    sample(&f,false,50000,BUTTON_RELEASED,false);
    none(&f,true,60000); sample(&f,true,90000,BUTTON_PRESSED,false);
    none(&f,false,100000); sample(&f,false,130000,BUTTON_RELEASED,true);

    // Overlapping buttons and a bouncing neighbour stay independent.
    button_filter_t a,b;
    button_filter_init(&a,false,0); button_filter_init(&b,false,0);
    none(&a,true,10000); none(&b,true,10000);
    none(&a,true,20000); none(&b,false,20000);
    none(&a,true,30000); none(&b,true,30000);
    sample(&a,true,40000,BUTTON_PRESSED,false); none(&b,true,40000);
    none(&a,false,50000); none(&b,true,50000);
    none(&a,false,60000); sample(&b,true,60000,BUTTON_PRESSED,false);
    none(&a,false,70000); none(&b,false,70000);
    sample(&a,false,80000,BUTTON_RELEASED,true); none(&b,false,80000);
    none(&b,false,90000); sample(&b,false,100000,BUTTON_RELEASED,true);

    // Qualification uses elapsed microseconds, including scheduler jitter,
    // exact boundaries and long uptimes; stale timestamps cannot fire early.
    uint64_t base=1ULL<<40;
    button_filter_init(&f,false,base);
    none(&f,true,base+1000);
    none(&f,true,base); none(&f,true,base+10900);
    none(&f,true,base+21100); none(&f,true,base+30999);
    sample(&f,true,base+31000,BUTTON_PRESSED,false);
    none(&f,false,base+40000); none(&f,false,base+69999);
    sample(&f,false,base+70000,BUTTON_RELEASED,true);
    puts("Polling/debounce/rising-edge regression: PASS");
    return 0;
}
