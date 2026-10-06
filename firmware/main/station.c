#include "station.h"
#include "buttons.h"
#include "radar.h"
#include "scd4x_i2c.h"
#include "sensirion_i2c_hal.h"
#include "ha_discovery.h"
#include "wifi_manager.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG="station";
static char device[32], boot[33], state_topic[128], radar_topic[128], result_topic[128];
static SemaphoreHandle_t lock;
static QueueHandle_t commands;
static bool pins_ready;
static void indicator_set(bool on)
{
    if (!pins_ready || CONFIG_STATION_LED<0) return;
#ifdef CONFIG_STATION_LED_ACTIVE_LOW
    gpio_set_level(CONFIG_STATION_LED,!on);
#else
    gpio_set_level(CONFIG_STATION_LED,on);
#endif
}
typedef struct { char json[512]; } command_t;
typedef struct {
    uint16_t co2;
    int32_t temperature, humidity;
    uint32_t sequence;
    int64_t measured_us;
    time_t measured_at;
    bool valid, asc, calibrating;
    radar_t radar;
    char result[768];
} snapshot_t;
static snapshot_t state;
static bool calibration_storage_ok;
static int64_t periodic_since;
static int64_t now_us(void) { return esp_timer_get_time(); }
static void iso_time(time_t t,char *out,size_t n)
{
    struct tm utc; gmtime_r(&t,&utc);
    strftime(out,n,"%Y-%m-%dT%H:%M:%S+00:00",&utc);
}
static esp_err_t publish_json(const char *topic,cJSON *json,bool retain)
{
    if (!json) return ESP_ERR_NO_MEM;
    char *text=cJSON_PrintUnformatted(json);
    esp_err_t err=text ? mqtt_connection_publish(topic,text,retain) : ESP_ERR_NO_MEM;
    cJSON_free(text); cJSON_Delete(json); return err;
}
static bool persist_result(const char *result)
{
    nvs_handle_t h;
    if (nvs_open("station_cal",NVS_READWRITE,&h)!=ESP_OK) return false;
    esp_err_t e=nvs_set_str(h,"result",result);
    if (e==ESP_OK) e=nvs_commit(h);
    nvs_close(h);
    return e==ESP_OK;
}
static bool set_result(cJSON *result)
{
    char buf[sizeof(state.result)];
    if (!cJSON_PrintPreallocated(result,buf,sizeof(buf),false)) return false;
    bool ok=persist_result(buf);
    xSemaphoreTake(lock,portMAX_DELAY);
    snprintf(state.result,sizeof(state.result),"%s",buf);
    xSemaphoreGive(lock);
    return ok;
}
void station_receive(const char *data,size_t length,bool retained,void *context)
{
    (void)context;
    if (retained || length>=sizeof(((command_t *)0)->json)) return;
    command_t command={0}; memcpy(command.json,data,length);
    // Queue full deliberately drops the command; caller times out without retry.
    xQueueSend(commands,&command,0);
}
static bool valid_id(const char *id)
{
    if (!id || strlen(id)!=32) return false;
    for (const char *p=id;*p;p++) if (!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f'))) return false;
    return true;
}
static void handle_command(const command_t *command)
{
    // Commands contain a single flat object. Reject nesting before invoking the
    // recursive JSON parser on an embedded task's bounded stack.
    const char *first=strchr(command->json,'{');
    if (!first || strchr(first+1,'{') || strchr(command->json,'[')) return;
    cJSON *root=cJSON_ParseWithOpts(command->json,NULL,true);
    if (!root) return;
    const char *id=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root,"request_id"));
    const char *kind=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root,"command"));
    cJSON *reference=cJSON_GetObjectItemCaseSensitive(root,"reference_ppm");
    cJSON *expiry=cJSON_GetObjectItemCaseSensitive(root,"expires_at");
    if (!valid_id(id) || !kind || strcmp(kind,"calibrate") || !cJSON_IsNumber(reference) ||
        reference->valuedouble!=reference->valueint || reference->valueint<400 || reference->valueint>2000 ||
        !cJSON_IsNumber(expiry) || !isfinite(expiry->valuedouble) ||
        floor(expiry->valuedouble)!=expiry->valuedouble ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"confirmed"))) { cJSON_Delete(root); return; }
    char previous[sizeof(state.result)];
    xSemaphoreTake(lock,portMAX_DELAY);
    memcpy(previous,state.result,sizeof(previous));
    xSemaphoreGive(lock);
    cJSON *old=cJSON_Parse(previous);
    const char *old_id=old ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(old,"request_id")) : NULL;
    cJSON *old_expiry=old ? cJSON_GetObjectItemCaseSensitive(old,"expires_at") : NULL;
    // Same ID is reconciled by periodic result publication. Older commands can
    // never execute after a newer command, even when their MQTT delivery is late.
    bool duplicate=old_id && !strcmp(old_id,id);
    bool older=cJSON_IsNumber(old_expiry) && expiry->valuedouble<=old_expiry->valuedouble;
    cJSON_Delete(old);
    time_t now=time(NULL);
    if (duplicate || older || now<1735689600 || expiry->valuedouble<=now ||
        expiry->valuedouble>now+120 || !calibration_storage_ok) { cJSON_Delete(root); return; }
    cJSON *result=cJSON_CreateObject();
    cJSON_AddStringToObject(result,"device_id",device);
    cJSON_AddStringToObject(result,"request_id",id);
    cJSON_AddNumberToObject(result,"expires_at",expiry->valuedouble);
    cJSON_AddNumberToObject(result,"reference_ppm",reference->valueint);
    bool ready=pins_ready && periodic_since && now_us()-periodic_since>=180000000;
    cJSON_AddStringToObject(result,"status",ready ? "pending" : "failed");
    if (!ready) cJSON_AddStringToObject(result,"error","Sensor must measure continuously for at least three minutes");
    calibration_storage_ok=set_result(result);
    if (!ready || !calibration_storage_ok) { cJSON_Delete(result); cJSON_Delete(root); return; }
    xSemaphoreTake(lock,portMAX_DELAY); state.calibrating=true; state.valid=false; xSemaphoreGive(lock);
#ifndef CONFIG_STATION_LED_MOTION
    indicator_set(true);
#endif
    uint16_t correction=0xffff;
    int16_t error=scd4x_stop_periodic_measurement(); // official driver waits 500 ms
    if (!error) error=scd4x_perform_forced_recalibration(reference->valueint,&correction);
    int16_t restart=scd4x_start_periodic_measurement();
    periodic_since=restart ? 0 : now_us();
#ifndef CONFIG_STATION_LED_MOTION
    indicator_set(false);
#endif
    cJSON_ReplaceItemInObjectCaseSensitive(result,"status",cJSON_CreateString(!error && correction!=0xffff ? "succeeded" : "failed"));
    char timestamp[40]; iso_time(time(NULL),timestamp,sizeof(timestamp));
    cJSON_AddStringToObject(result,"calibrated_at",timestamp);
    xSemaphoreTake(lock,portMAX_DELAY);
    state.calibrating=false;
    cJSON_AddBoolToObject(result,"auto_calibration_enabled",state.asc);
    xSemaphoreGive(lock);
    if (!error && correction!=0xffff) cJSON_AddNumberToObject(result,"correction_value",(int)correction-0x8000);
    else cJSON_AddStringToObject(result,"error","SCD4x recalibration failed");
    cJSON_AddBoolToObject(result,"measurement_resumed",restart==0);
    calibration_storage_ok=set_result(result);
    cJSON_Delete(result); cJSON_Delete(root);
}
static void sensor_task(void *arg)
{
    (void)arg;
    int64_t next_read=0,next_init=0;
    for (;;) {
        int64_t now=now_us();
        if (pins_ready && !periodic_since && now>=next_init) {
            sensirion_i2c_hal_init(); scd4x_init(0x62);
            uint16_t asc=0;
            int16_t error=scd4x_stop_periodic_measurement();
            if (!error) error=scd4x_get_automatic_self_calibration_enabled(&asc);
            if (!error) error=scd4x_start_periodic_measurement();
            if (!error) {
                periodic_since=now_us(); next_read=periodic_since+5000000;
                xSemaphoreTake(lock,portMAX_DELAY); state.asc=asc!=0; xSemaphoreGive(lock);
            }
            next_init=now_us()+10000000;
        }
        command_t command;
        if (xQueueReceive(commands,&command,0)==pdTRUE) handle_command(&command);
        now=now_us();
        if (periodic_since && now>=next_read) {
            bool ready=false;
            uint16_t co2=0; int32_t temperature=0,humidity=0;
            int16_t error=scd4x_get_data_ready_status(&ready);
            if (!error && ready) error=scd4x_read_measurement(&co2,&temperature,&humidity);
            xSemaphoreTake(lock,portMAX_DELAY);
            if (!error && ready && co2) {
                state.co2=co2; state.temperature=temperature; state.humidity=humidity;
                state.measured_us=now_us(); state.measured_at=time(NULL);
                state.sequence++; state.valid=true;
                next_read=now_us()+30000000;
            } else {
                if (error) state.valid=false;
                next_read=now_us()+1000000;
            }
            bool stale=now_us()-(state.measured_us ? state.measured_us : periodic_since)>90000000;
            xSemaphoreGive(lock);
            if (error || stale) { periodic_since=0; next_init=now_us()+10000000; }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
static void radar_task(void *arg)
{
    (void)arg;
    radar_t radar={0};
    bool uart_ready=false;
    if (pins_ready) {
        uart_config_t cfg={.baud_rate=256000,.data_bits=UART_DATA_8_BITS,
            .parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,
            .flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
        uart_ready=uart_param_config(UART_NUM_1,&cfg)==ESP_OK &&
            uart_set_pin(UART_NUM_1,UART_PIN_NO_CHANGE,CONFIG_STATION_RADAR_RX,
                         UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE)==ESP_OK &&
            uart_driver_install(UART_NUM_1,2048,0,0,NULL,0)==ESP_OK;
    }
    int sock=-1;
    struct sockaddr_in peer={.sin_family=AF_INET,.sin_port=htons(8765)};
    bool wake_enabled=inet_pton(AF_INET,CONFIG_STATION_WAKE_HOST,&peer.sin_addr)==1;
    bool was_moving=false;
    int64_t next_wake=0;
    for (;;) {
        uint8_t data[256];
        int count=uart_ready ? uart_read_bytes(UART_NUM_1,data,sizeof(data),pdMS_TO_TICKS(50)) : 0;
        int64_t now=now_us();
        if (count>0) radar_feed(&radar,data,count,now);
        bool moving=radar_fresh(&radar,now) && radar.moving;
#ifdef CONFIG_STATION_LED_MOTION
        indicator_set(moving);
#endif
        bool connected=wifi_manager_is_connected();
        if (wake_enabled && moving && connected && (!was_moving || now>=next_wake)) {
            if (sock<0) {
                sock=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
                if (sock>=0) { struct timeval timeout={.tv_usec=100000}; setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)); }
            }
            const char wake[]="SCEPTER_MOVEMENT_WAKE_V1";
            if (sock>=0 && sendto(sock,wake,sizeof(wake)-1,0,(struct sockaddr *)&peer,sizeof(peer))==(int)sizeof(wake)-1)
                next_wake=now+5000000;
            else { if (sock>=0) close(sock); sock=-1; next_wake=now+1000000; }
        }
        was_moving=moving && connected;
        xSemaphoreTake(lock,portMAX_DELAY); state.radar=radar; xSemaphoreGive(lock);
        if (!uart_ready) vTaskDelay(pdMS_TO_TICKS(50));
    }
}
esp_err_t station_initialize(const char *id)
{
    snprintf(device,sizeof(device),"%s",id);
    snprintf(state_topic,sizeof(state_topic),"esp-monitor/%s/state",id);
    snprintf(radar_topic,sizeof(radar_topic),"esp-monitor/%s/presence",id);
    snprintf(result_topic,sizeof(result_topic),"esp-monitor/%s/calibration/result",id);
    uint32_t random[4]; esp_fill_random(random,sizeof(random));
    snprintf(boot,sizeof(boot),"%08"PRIx32"%08"PRIx32"%08"PRIx32"%08"PRIx32,random[0],random[1],random[2],random[3]);
    lock=xSemaphoreCreateMutex(); commands=xQueueCreate(4,sizeof(command_t));
    if (!lock || !commands) return ESP_ERR_NO_MEM;
#ifdef CONFIG_STATION_PINS_VERIFIED
    int pins[]={CONFIG_STATION_SDA,CONFIG_STATION_SCL,CONFIG_STATION_RADAR_RX};
    pins_ready=GPIO_IS_VALID_OUTPUT_GPIO(pins[0]) && GPIO_IS_VALID_OUTPUT_GPIO(pins[1]) && GPIO_IS_VALID_GPIO(pins[2]);
    for (int i=0;i<3;i++) for (int j=i+1;j<3;j++) if (pins[i]==pins[j]) pins_ready=false;
    if (CONFIG_STATION_LED>=0) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_STATION_LED)) pins_ready=false;
        for (int i=0;i<3;i++) if (pins[i]==CONFIG_STATION_LED) pins_ready=false;
    }
    if (!pins_ready) return ESP_ERR_INVALID_ARG;
#endif
    if (pins_ready && CONFIG_STATION_LED>=0) {
        gpio_reset_pin(CONFIG_STATION_LED);
        indicator_set(false);
        gpio_set_direction(CONFIG_STATION_LED,GPIO_MODE_OUTPUT);
    }
    nvs_handle_t h;
    esp_err_t e=nvs_open("station_cal",NVS_READWRITE,&h);
    calibration_storage_ok=e==ESP_OK;
    if (e==ESP_OK) {
        size_t n=sizeof(state.result); e=nvs_get_str(h,"result",state.result,&n); nvs_close(h);
        calibration_storage_ok=e==ESP_OK || e==ESP_ERR_NVS_NOT_FOUND;
        if (e==ESP_OK) {
            cJSON *saved=cJSON_Parse(state.result);
            const char *status=saved ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(saved,"status")) : NULL;
            if (!status) calibration_storage_ok=false;
            else if (!strcmp(status,"pending")) {
                cJSON_ReplaceItemInObjectCaseSensitive(saved,"status",cJSON_CreateString("interrupted"));
                cJSON_AddStringToObject(saved,"error","Restart during calibration; outcome unknown; not repeated");
                calibration_storage_ok=set_result(saved);
            }
            cJSON_Delete(saved);
        }
    }
    ESP_LOGI(TAG,"Sensor wiring %s; calibration journal %s",pins_ready ? "enabled" : "unverified/disabled",calibration_storage_ok ? "ready" : "unavailable");
    esp_err_t button_error=buttons_initialize(device,boot);
    if (button_error!=ESP_OK) return button_error;
    if (xTaskCreate(sensor_task,"scd4x",6144,NULL,5,NULL)!=pdPASS ||
        xTaskCreate(radar_task,"ld2410",4096,NULL,6,NULL)!=pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
static esp_err_t publish_discovery(void)
{
    ha_device_t dev={.id=device,.name="Indoor sensor station",.manufacturer="Espressif / Sensirion / Hi-Link",
        .model="ESP32-C5 SCD4x LD2410C",.state_topic=state_topic,.availability_topic=state_topic};
    const ha_entity_t entities[]={
        {.sensor={.id="co2",.name="Indoor CO2",.value_template="{{ value_json.co2 }}",.unit="ppm",.device_class="carbon_dioxide",.state_class="measurement"},.expire_after_s=90},
        {.sensor={.id="temperature",.name="Indoor Temperature",.value_template="{{ value_json.temperature }}",.unit="°C",.device_class="temperature",.state_class="measurement"},.expire_after_s=90},
        {.sensor={.id="humidity",.name="Indoor Humidity",.value_template="{{ value_json.humidity }}",.unit="%",.device_class="humidity",.state_class="measurement"},.expire_after_s=90},
        {.sensor={.id="movement",.name="Movement",.value_template="{{ 'ON' if value_json.moving else 'OFF' }}",.device_class="motion"},.expire_after_s=3,.binary=true},
        {.sensor={.id="occupancy",.name="Occupancy",.value_template="{{ 'ON' if value_json.occupied else 'OFF' }}",.device_class="occupancy"},.expire_after_s=3,.binary=true}
    };
    for (size_t i=0;i<sizeof(entities)/sizeof(entities[0]);i++) {
        ha_entity_t entity=entities[i];
        dev.state_topic=dev.availability_topic=entity.binary ? radar_topic : state_topic;
        entity.availability_template="{{ 'online' if value_json.valid else 'offline' }}";
        if (!entity.binary) entity.json_attributes_template="{{ {'last_measured_time': value_json.last_measured_time} | tojson }}";
        char topic[160],payload[1536];
        esp_err_t e=ha_discovery_entity(&dev,&entity,topic,sizeof(topic),payload,sizeof(payload));
        if (e==ESP_OK) e=mqtt_connection_publish(topic,payload,true);
        if (e!=ESP_OK) return e;
    }
    return ESP_OK;
}
esp_err_t station_publish(bool discovery)
{
    esp_err_t button_error=buttons_publish(discovery);
    if (button_error!=ESP_OK) return button_error;
    static uint32_t last_sequence;
    static int64_t next_state;
    snapshot_t copy;
    xSemaphoreTake(lock,portMAX_DELAY); copy=state; xSemaphoreGive(lock);
    if (discovery) { esp_err_t e=publish_discovery(); if (e!=ESP_OK) return e; }
    int64_t now=now_us();
    bool valid=copy.valid && !copy.calibrating && now-copy.measured_us<90000000 && copy.measured_at>1735689600;
    static bool last_valid;
    if (discovery || now>=next_state || last_sequence!=copy.sequence || last_valid!=valid) {
        cJSON *json=cJSON_CreateObject();
        cJSON_AddStringToObject(json,"device_id",device); cJSON_AddStringToObject(json,"boot_id",boot);
        cJSON_AddNumberToObject(json,"sample_sequence",copy.sequence);
        cJSON_AddBoolToObject(json,"valid",valid);
        cJSON_AddBoolToObject(json,"auto_calibration_enabled",copy.asc);
        cJSON_AddBoolToObject(json,"calibration_in_progress",copy.calibrating);
        cJSON_AddNumberToObject(json,"uptime_s",now/1000000);
        cJSON_AddNumberToObject(json,"free_heap_bytes",esp_get_free_heap_size());
        cJSON_AddNumberToObject(json,"minimum_free_heap_bytes",esp_get_minimum_free_heap_size());
        cJSON_AddNumberToObject(json,"telemetry_stack_free_bytes",uxTaskGetStackHighWaterMark(NULL));
        cJSON_AddNumberToObject(json,"reset_reason",esp_reset_reason());
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap)==ESP_OK) cJSON_AddNumberToObject(json,"wifi_rssi_dbm",ap.rssi);
        if (valid) {
            char ts[40]; iso_time(copy.measured_at,ts,sizeof(ts));
            cJSON_AddStringToObject(json,"last_measured_time",ts);
            cJSON_AddNumberToObject(json,"co2",copy.co2);
            cJSON_AddNumberToObject(json,"temperature",copy.temperature/1000.0);
            cJSON_AddNumberToObject(json,"humidity",copy.humidity/1000.0);
        } else {
            cJSON_AddNullToObject(json,"last_measured_time"); cJSON_AddNullToObject(json,"co2");
            cJSON_AddNullToObject(json,"temperature"); cJSON_AddNullToObject(json,"humidity");
        }
        esp_err_t e=publish_json(state_topic,json,false);
        if (e!=ESP_OK) return e;
        last_sequence=copy.sequence; next_state=now+30000000; last_valid=valid;
    }
    cJSON *radar=cJSON_CreateObject();
    cJSON_AddBoolToObject(radar,"valid",radar_fresh(&copy.radar,now));
    cJSON_AddBoolToObject(radar,"moving",radar_fresh(&copy.radar,now) && copy.radar.moving);
    cJSON_AddBoolToObject(radar,"occupied",radar_fresh(&copy.radar,now) && copy.radar.occupied);
    cJSON_AddNumberToObject(radar,"distance_cm",copy.radar.distance_cm);
    esp_err_t e=publish_json(radar_topic,radar,false);
    static char last_result[sizeof(state.result)];
    static int64_t next_result;
    if (copy.result[0] && (discovery || strcmp(last_result,copy.result) || now>=next_result)) {
        esp_err_t r=mqtt_connection_publish(result_topic,copy.result,true);
        if (r!=ESP_OK) return r;
        memcpy(last_result,copy.result,sizeof(last_result)); next_result=now+30000000;
    }
    return e;
}
