# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
if(NOT DEFINED IDF_LWIP_ROOT)
  set(IDF_LWIP_ROOT "$ENV{IDF_PATH}")
  if(IDF_LWIP_ROOT STREQUAL "")
    set(IDF_LWIP_ROOT "${IDF_PATH}")
  endif()
endif()

set(IDF_LWIP_MDNS_SRCS
  ${IDF_LWIP_ROOT}/components/lwip/lwip/src/apps/mdns/mdns.c
  ${IDF_LWIP_ROOT}/components/lwip/lwip/src/apps/mdns/mdns_domain.c
  ${IDF_LWIP_ROOT}/components/lwip/lwip/src/apps/mdns/mdns_out.c
)

idf_component_register(
  SRCS ${app_sources}
  INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/include
  PRIV_INCLUDE_DIRS
    ${IDF_LWIP_ROOT}/components/lwip/include
    ${IDF_LWIP_ROOT}/components/lwip/lwip/src/include
    ${IDF_LWIP_ROOT}/components/lwip/lwip/src/apps/mdns
  REQUIRES
    app_update
    esp_http_server
    esp_https_server
    esp_netif
    esp_event
    esp_wifi
    lwip
    mqtt
    nvs_flash
)

if(NOT "${IDF_LWIP_ROOT}" STREQUAL "")
  target_sources(${COMPONENT_LIB} PRIVATE ${IDF_LWIP_MDNS_SRCS})
  target_compile_definitions(${COMPONENT_LIB} PRIVATE LWIP_MDNS_RESPONDER=1)
endif()
