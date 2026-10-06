if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/certs/mqtt_ca.crt")
    message(FATAL_ERROR "Add your MQTT broker CA certificate at main/certs/mqtt_ca.crt before building.")
endif()
set(SCD "${CMAKE_CURRENT_LIST_DIR}/../vendor/scd4x")
idf_component_register(SRCS "app_main.c" "station.c" "buttons.c" "radar.c" "scd_hal.c"
    "${SCD}/scd4x_i2c.c" "${SCD}/sensirion_common.c" "${SCD}/sensirion_i2c.c"
    INCLUDE_DIRS "." "${SCD}"
    PRIV_REQUIRES credential_store wifi_manager mqtt_connection ha_discovery
        usb_provisioning esp_event esp_netif esp_timer esp_wifi esp_driver_i2c
        esp_driver_uart esp_hal_uart esp_driver_usb_serial_jtag esp_driver_gpio nvs_flash lwip esp_hw_support
    EMBED_TXTFILES "certs/mqtt_ca.crt")
