idf_component_register(SRCS "main.c" "camera_driver.c" "status_led.c" "flash_led.c" "config_manager.c" "frame_broadcaster.c" "wifi_manager.c" "web_server.c" "mjpeg_streamer.c" "rtsp_server.cpp" "ai_pipeline.cpp" "onvif_discovery.c" "onvif_service.c" "at_command.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp-dl human_face_detect quirc nvs_flash spiffs
                    esp_driver_gpio esp_driver_uart esp_wifi esp_netif esp_event esp_http_server espressif__cjson mdns)
target_compile_definitions(${COMPONENT_LIB} PRIVATE AI_FACE_DETECT_ENABLED=1)
