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

foreach(mdns_src IN LISTS IDF_LWIP_MDNS_SRCS)
  if(EXISTS "${mdns_src}")
    target_sources(${COMPONENT_LIB} PRIVATE "${mdns_src}")
  endif()
endforeach()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
  LWIP_MDNS_RESPONDER=1
)
