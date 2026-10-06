#include <inttypes.h>
#include <stdio.h>
#include <time.h>
#include "credential_store.h"
#include "station.h"
#include "buttons.h"
#include "wifi_manager.h"
#include "mqtt_connection.h"
#include "ha_discovery.h"
#include "usb_provisioning.h"
#include "esp_event.h"
#include "esp_chip_info.h"
#include "esp_private/startup_internal.h"
#include "hal/uart_ll.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ESP32-C5 rev1.0 ROM waits for UART0_READY even when flashing over USB.
// IDF 6.0.2 gates this clock when the UART console is disabled; a USB core
// reset preserves that state and then hangs in ROM. Restore only the clocks
// early in startup. Do not install UART0 or route its TX onto button GPIO11.
// https://github.com/espressif/esp-idf/issues/18089
ESP_SYSTEM_INIT_FN(keep_c5_usb_reset_working, CORE, BIT(0), 100)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    if (chip.model == CHIP_ESP32C5 && chip.revision == 100) {
        uart_ll_enable_bus_clock(UART_NUM_0, true);
        uart_ll_sclk_enable(UART_LL_GET_HW(UART_NUM_0));
    }
    return ESP_OK;
}

extern const uint8_t mqtt_ca_crt_start[] asm("_binary_mqtt_ca_crt_start");
static char s_device_id[32];
static char s_state_topic[128];
static char s_availability_topic[128];
static const char *TAG = "esp_monitor";

static bool time_ready(void)
{
    return time(NULL) > 1735689600;
}

static bool wait_for_time(void)
{
    return time_ready() || esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK;
}

static void application_task(void *arg)
{
    (void)arg;
    uint32_t published_generation = 0;

    int64_t next_start = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (wifi_manager_is_connected() && time_ready() && now >= next_start) {
            // Handles delayed Wi-Fi or clock recovery even when boot was offline.
            (void)mqtt_connection_start_saved();
            next_start = now + 5000000;
        }
        if (mqtt_connection_is_connected()) {
            uint32_t generation = mqtt_connection_generation();
            if (station_publish(generation != published_generation) == ESP_OK)
                published_generation = generation;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(usb_provisioning_initialize(wait_for_time));
    usb_provisioning_send("\nESP32-C5 indoor sensor station\n");
    // Provision HMAC KEY0 for encrypted NVS on first boot; never erase NVS on failure.
    ESP_ERROR_CHECK(credential_store_init(true));
    ESP_LOGI(TAG, "Encrypted NVS ready");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(wifi_manager_initialize("esp32-station"));
    // Keep modem power saving; the fast MQTT outbox poll removes sender delays.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_device_id, sizeof(s_device_id), "esp-c5-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_state_topic, sizeof(s_state_topic), "esp-monitor/%s/state", s_device_id);
    snprintf(s_availability_topic, sizeof(s_availability_topic), "esp-monitor/%s/availability", s_device_id);
    ESP_ERROR_CHECK(station_initialize(s_device_id));
    static char command_topic[128];
    snprintf(command_topic,sizeof(command_topic),"esp-monitor/%s/calibration/command",s_device_id);
    const mqtt_connection_options_t options = {
        .device_id = s_device_id, .ca_certificate = (const char *)mqtt_ca_crt_start,
        .availability_topic = s_availability_topic,
        .command_topic = command_topic, .on_message = station_receive,
        .outbox_limit_bytes = 32768,
    };
    ESP_ERROR_CHECK(mqtt_connection_initialize(&options));
    // Keep SNTP alive so an initial network outage does not prevent TLS forever.
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.start = false;
    sntp.server_from_dhcp = false;
    sntp.renew_servers_after_new_IP = true;
    sntp.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));
    ESP_ERROR_CHECK(esp_netif_sntp_start());
    esp_err_t error = wifi_manager_start_saved();
    usb_provisioning_send(error == ESP_OK ? "WIFI_CONNECTING_SAVED\n" : "WIFI_SETUP_REQUIRED\n");
    if (xTaskCreate(application_task, "app_telemetry", 10240, NULL, 5, NULL) != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    usb_provisioning_set_command_handler(buttons_usb_command);
    usb_provisioning_run();
}
