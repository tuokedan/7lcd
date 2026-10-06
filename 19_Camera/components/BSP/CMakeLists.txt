set(src_dirs
            CAMERA
            LCD
            SPI
            YOLO)

set(include_dirs
            CAMERA
            LCD
            SPI
            YOLO)

set(requires
            driver
            esp32-camera
            log)

idf_component_register(
    SRC_DIRS ${src_dirs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${requires}
)

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)
