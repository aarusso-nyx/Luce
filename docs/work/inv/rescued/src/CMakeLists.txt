# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources
    ${CMAKE_SOURCE_DIR}/src/*.c
    ${CMAKE_SOURCE_DIR}/src/*.cpp
)

idf_component_register(
    SRCS           ${app_sources}
    INCLUDE_DIRS   "${CMAKE_SOURCE_DIR}/src"
    REQUIRES
      esp_http_server    # HTTP server (esp_http_server.h)
      vfs                # VFS core + LittleFS (esp_vfs_littlefs.h)
      mqtt               # MQTT client (esp_mqtt_client.h)
      # mdns               # mDNS support (mdns.h)
      # i2cdev             # I2C helper (i2cdev.h)
    #   websocket_server   # WebSocket server (websocket_server.h)
)
