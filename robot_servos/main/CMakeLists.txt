idf_component_register(
    SRCS main.c
    PRIV_REQUIRES esp_driver_ledc esp_driver_gpio esp_wifi esp_event esp_netif esp_http_server nvs_flash
    EMBED_TXTFILES index.html   # web app servida por el ESP32 (sin internet)
)
