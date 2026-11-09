# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

idf_component_register(
    SRCS main.c                 # list the source files of this component
    INCLUDE_DIRS                # optional, add here public include directories
    PRIV_INCLUDE_DIRS           # optional, add here private include directories
    REQUIRES                    # optional, list the public requirements (component names)
    PRIV_REQUIRES esp_driver_gpio esp_timer esp_wifi esp_event esp_netif esp_http_server nvs_flash
    EMBED_TXTFILES index.html   # web app servida por el ESP32 (sin internet)
)
