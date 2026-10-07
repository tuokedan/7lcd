set(src_dirs
            CAMERA
            LCD
            SPI
            YOLO
            VIDEO)

set(include_dirs
            CAMERA
            LCD
            SPI
            YOLO
            VIDEO)

set(requires
            driver
            esp32-camera
            log
            pedestrian_detect
            esp_http_server
            esp_wifi
            esp_event
            esp_netif
            nvs_flash)

idf_component_register(
    SRC_DIRS ${src_dirs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${requires}
)

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)
