#include "sensirion_i2c_hal.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t sensor;
int16_t sensirion_i2c_hal_select_bus(uint8_t index) { return index ? -1 : 0; }
void sensirion_i2c_hal_init(void)
{
    if (sensor) return;
    i2c_master_bus_config_t cfg = {.i2c_port=0, .sda_io_num=CONFIG_STATION_SDA,
        .scl_io_num=CONFIG_STATION_SCL, .clk_source=I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt=7, .flags.enable_internal_pullup=true};
    if (i2c_new_master_bus(&cfg,&bus)!=ESP_OK) return;
    i2c_device_config_t dev = {.dev_addr_length=I2C_ADDR_BIT_LEN_7,
        .device_address=0x62, .scl_speed_hz=100000};
    if (i2c_master_bus_add_device(bus,&dev,&sensor)!=ESP_OK) {
        i2c_del_master_bus(bus); bus=NULL;
    }
}
void sensirion_i2c_hal_free(void)
{
    if (sensor) i2c_master_bus_rm_device(sensor);
    if (bus) i2c_del_master_bus(bus);
    sensor=NULL; bus=NULL;
}
int8_t sensirion_i2c_hal_read(uint8_t address,uint8_t *data,uint8_t count)
{
    return sensor && address==0x62 && i2c_master_receive(sensor,data,count,500)==ESP_OK ? 0 : -1;
}
int8_t sensirion_i2c_hal_write(uint8_t address,const uint8_t *data,uint8_t count)
{
    return sensor && address==0x62 && i2c_master_transmit(sensor,data,count,500)==ESP_OK ? 0 : -1;
}
void sensirion_i2c_hal_sleep_usec(uint32_t usec)
{
    // Round up, then add a tick to account for scheduling near a tick boundary.
    vTaskDelay(pdMS_TO_TICKS((usec+999)/1000)+1);
}
