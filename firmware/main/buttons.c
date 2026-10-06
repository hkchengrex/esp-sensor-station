#include "buttons.h"
#include "button_filter.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "usb_provisioning.h"
#include "esp_timer.h"
#include "mqtt_connection.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const int pins[]={1,0,25,7,11,8,9,10};
static const char *pads[]={"D0","D1","D2","D3","D6","D8","D9","D10"};
static char event_topic[128], action_topic[128], status_topic[128], boot_id[33];
typedef struct { uint32_t sequence; int64_t at_us; uint8_t mask,index; bool pressed, activate; } event_t;
typedef struct { uint32_t sequence,dropped; uint8_t mask; } status_t;
static QueueHandle_t events, usb_events;
// An active USB monitor renews this lease; closing it cannot leave an endless
// diagnostic stream or block normal sensor/MQTT operation.
static int64_t usb_watch_until_us;
static uint32_t usb_dropped;
static portMUX_TYPE state_lock=portMUX_INITIALIZER_UNLOCKED;
static status_t status;
static button_filter_t levels[8];
static uint32_t sampled_changes[8], poll_count;

_Static_assert(pdMS_TO_TICKS(BUTTON_POLL_MS)>0, "Button polling requires a tick of 10 ms or faster");
_Static_assert((BUTTON_POLL_MS*configTICK_RATE_HZ)%1000==0, "Button polling period must be a whole number of ticks");

static void poll_buttons(void *unused) {
    (void)unused;
    TickType_t wake=xTaskGetTickCount();
    for(;;) {
        // One scan per 10 ms in a dedicated task. No GPIO interrupts, network
        // calls, USB writes or sensor telemetry work run in this task.
        portENTER_CRITICAL(&state_lock);
        uint64_t now=(uint64_t)esp_timer_get_time();
        poll_count++;
        for(unsigned i=0;i<8;i++) {
            bool raw_pressed=gpio_get_level(pins[i])==0;
            if(raw_pressed!=levels[i].candidate) sampled_changes[i]++;
            button_change_t change=button_filter_update(&levels[i],raw_pressed,now);
            if(change.edge!=BUTTON_NO_EDGE) {
                bool pressed=change.edge==BUTTON_PRESSED;
                if(pressed) status.mask|=1u<<i; else status.mask&=~(1u<<i);
                event_t event={.sequence=++status.sequence,.at_us=(int64_t)now,
                    .mask=status.mask,.index=i,.pressed=pressed,.activate=change.activate};
                if(xQueueSend(events,&event,0)!=pdTRUE) status.dropped++;
                if(change.activate && (int64_t)now<usb_watch_until_us) {
                    if(xQueueSend(usb_events,&event,0)!=pdTRUE) usb_dropped++;
                }
            }
        }
        portEXIT_CRITICAL(&state_lock);
        // Fixed-rate scheduling avoids accumulating scan-time drift. After a
        // missed deadline, resume from now instead of replaying old scans.
        if(xTaskDelayUntil(&wake,pdMS_TO_TICKS(BUTTON_POLL_MS))==pdFALSE)
            wake=xTaskGetTickCount();
    }
}

bool buttons_usb_command(const char *command) {
    if(strcmp(command,"BUTTONS_STATE")==0) {
        // Read-only snapshots distinguish unstable wiring from transport or
        // debounce faults. They do not alter the filter, GPIO or action state.
        for(unsigned i=0;i<8;i++) {
            button_filter_t filter;
            uint32_t count, scans;
            int raw;
            int64_t now;
            portENTER_CRITICAL(&state_lock);
            filter=levels[i]; count=sampled_changes[i]; scans=poll_count;
            raw=gpio_get_level(pins[i]); now=esp_timer_get_time();
            portEXIT_CRITICAL(&state_lock);
            gpio_io_config_t cfg={0};
            esp_err_t err=gpio_get_io_config(pins[i],&cfg);
            char line[384];
            int length=snprintf(line,sizeof(line),
                "BUTTON_STATE {\"boot_id\":\"%s\",\"gpio\":%d,\"pad\":\"%s\",\"uptime_ms\":%"PRId64",\"raw_level\":%d,\"pressed\":%d,\"candidate_pressed\":%d,\"armed\":%d,\"sampled_changes\":%"PRIu32",\"poll_count\":%"PRIu32",\"change_age_us\":%"PRId64",\"pullup\":%d,\"pulldown\":%d,\"input\":%d,\"output\":%d,\"function\":%"PRIu32",\"config_error\":%d}\n",
                boot_id,pins[i],pads[i],now/1000,raw,filter.stable,filter.candidate,filter.armed,count,scans,
                now-(int64_t)filter.changed_us,cfg.pu,cfg.pd,cfg.ie,cfg.oe,cfg.fun_sel,(int)err);
            if(length>0 && length<(int)sizeof(line))
                (void)usb_serial_jtag_write_bytes(line,length,pdMS_TO_TICKS(20));
        }
        return true;
    }
    bool watch=strcmp(command,"BUTTONS_WATCH")==0;
    bool stop=strcmp(command,"BUTTONS_STOP")==0;
    if(!watch && !stop) return false;
    portENTER_CRITICAL(&state_lock);
    usb_watch_until_us=watch ? esp_timer_get_time()+5000000 : 0;
    portEXIT_CRITICAL(&state_lock);
    usb_provisioning_send(watch ? "BUTTONS_WATCHING debounce_ms=30 trigger=rising poll_ms=10 reporting=polling_debounced\n" : "BUTTONS_STOPPED\n");
    return true;
}

static void publish_usb_edges(void *unused) {
    (void)unused;
    event_t event;
    for(;;) {
        if(xQueueReceive(usb_events,&event,portMAX_DELAY)!=pdTRUE) continue;
        int64_t now=esp_timer_get_time();
        portENTER_CRITICAL(&state_lock);
        bool active=now<usb_watch_until_us;
        portEXIT_CRITICAL(&state_lock);
        if(!active) continue;
        char line[128];
        int length=snprintf(line,sizeof(line),
            "BUTTON gpio=%d pad=%s sequence=%"PRIu32" event_us=%"PRId64" age_us=%"PRId64"\n",
            pins[event.index],pads[event.index],event.sequence,event.at_us,now-event.at_us);
        // Independent of MQTT and its outbox: an offline/slow network cannot
        // delay the local pin test. A full USB buffer drops instead of waits.
        if(usb_serial_jtag_write_bytes(line,length,0)!=length) {
            portENTER_CRITICAL(&state_lock); usb_dropped++; portEXIT_CRITICAL(&state_lock);
        }
    }
}

static void publish_edges(void *unused) {
    (void)unused;
    event_t event;
    for(;;) {
        // Only settled transitions arrive here; USB actions have their own queue.
        if(xQueueReceive(events,&event,portMAX_DELAY)!=pdTRUE) continue;
        char json[384];
        snprintf(json,sizeof(json),"{\"boot_id\":\"%s\",\"sequence\":%"PRIu32",\"uptime_ms\":%"PRId64",\"gpio\":%d,\"pad\":\"%s\",\"edge\":\"%s\",\"pressed_mask\":%u}",
            boot_id,event.sequence,event.at_us/1000,pins[event.index],pads[event.index],event.pressed?"press":"release",event.mask);
        // Both settled edges are diagnostics; only an armed rising/release edge
        // is published on the dedicated action topic for HA button bindings.
        bool delivered=mqtt_connection_is_connected();
        if(delivered) {
            delivered=mqtt_connection_publish(event_topic,json,false)==ESP_OK;
            if(event.activate && mqtt_connection_publish(action_topic,json,false)!=ESP_OK) delivered=false;
        }
        if(!delivered) {
            portENTER_CRITICAL(&state_lock); status.dropped++; portEXIT_CRITICAL(&state_lock);
        }
    }
}
esp_err_t buttons_initialize(const char *device,const char *boot) {
    snprintf(event_topic,sizeof(event_topic),"esp-monitor/%s/buttons/event",device);
    snprintf(action_topic,sizeof(action_topic),"esp-monitor/%s/buttons/action",device);
    snprintf(status_topic,sizeof(status_topic),"esp-monitor/%s/buttons/status",device);
    snprintf(boot_id,sizeof(boot_id),"%s",boot);
    events=xQueueCreate(64,sizeof(event_t));
    usb_events=xQueueCreate(128,sizeof(event_t));
    if(!events || !usb_events) return ESP_ERR_NO_MEM;
    uint64_t mask=0;
    for(int i=0;i<8;i++) {
        esp_err_t err=gpio_reset_pin(pins[i]); if(err!=ESP_OK) return err;
        mask|=1ULL<<pins[i];
    }
    gpio_config_t cfg={.pin_bit_mask=mask,.mode=GPIO_MODE_INPUT,
        .pull_up_en=GPIO_PULLUP_ENABLE,.pull_down_en=GPIO_PULLDOWN_DISABLE,.intr_type=GPIO_INTR_DISABLE};
    esp_err_t err=gpio_config(&cfg); if(err!=ESP_OK) return err;
    for(int i=0;i<8;i++) {
        button_filter_init(&levels[i],gpio_get_level(pins[i])==0,esp_timer_get_time());
        if(levels[i].stable) status.mask|=1u<<i;
    }
    if(xTaskCreate(publish_edges,"button_publish",4096,NULL,6,NULL)!=pdPASS) return ESP_ERR_NO_MEM;
    if(xTaskCreate(publish_usb_edges,"button_usb",3072,NULL,7,NULL)!=pdPASS) return ESP_ERR_NO_MEM;
    if(xTaskCreate(poll_buttons,"button_poll",3072,NULL,8,NULL)!=pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
esp_err_t buttons_publish(bool connected_now) {
    char json[384];
    static int64_t next_status; int64_t now=esp_timer_get_time();
    if(connected_now || now>=next_status) {
        status_t copy;
        portENTER_CRITICAL(&state_lock); copy=status; uint32_t usb_lost=usb_dropped; portEXIT_CRITICAL(&state_lock);
        snprintf(json,sizeof(json),"{\"mode\":\"button_mapping\",\"debounce_ms\":30,\"reporting\":\"polling_debounced\",\"poll_ms\":10,\"trigger\":\"rising\",\"boot_id\":\"%s\",\"sequence\":%"PRIu32",\"uptime_ms\":%"PRId64",\"pressed_mask\":%u,\"dropped\":%"PRIu32",\"usb_dropped\":%"PRIu32",\"gpios\":[1,0,25,7,11,8,9,10]}",boot_id,copy.sequence,now/1000,copy.mask,copy.dropped,usb_lost);
        esp_err_t err=mqtt_connection_publish(status_topic,json,false); if(err!=ESP_OK) return err;
        next_status=now+2000000;
    }
    return ESP_OK;
}
