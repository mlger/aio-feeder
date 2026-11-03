# AIO Feeder hub main component. The portable core (hub_core.h & co.) stays in
# ../src so the host test suites keep compiling it unchanged; lib/proto and
# link_core are shared with the nodes.
idf_component_register(
    SRCS "main.cpp" "ui/ui.cpp" "display_hw.cpp" "i2c_bus.cpp" "remote_net.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        lwip
        esp_timer
        driver
        console
        esp_netif
        esp_event
        esp_app_format
        fatfs
        sdmmc
        esp_lcd
        esp_http_server
        mqtt
)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../src"
    "${CMAKE_CURRENT_LIST_DIR}/../../lib/proto"
    "${CMAKE_CURRENT_LIST_DIR}/../../link_core"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    AIO_HUB=1
    AIO_FIRMWARE_NAME="aio-feeder-hub"
)
