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

set(LUCE_WEBAPP_DIR "${CMAKE_SOURCE_DIR}/data/webapp")
set(LUCE_WEBAPP_EMBEDDING_INPUTS
  index.html webapp_index_html
  app.css webapp_app_css
  script.js webapp_script_js
)

set(LUCE_WEBAPP_GENERATED_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated_webapp_assets.cpp")
set(LUCE_WEBAPP_HAS_ASSETS OFF)
set(LUCE_WEBAPP_ASSET_BLOBS "extern \"C\" {\n")

list(LENGTH LUCE_WEBAPP_EMBEDDING_INPUTS _webapp_pair_count)
math(EXPR _webapp_pair_last "${_webapp_pair_count} - 1")
foreach(_webapp_idx RANGE 0 ${_webapp_pair_last} 2)
  list(GET LUCE_WEBAPP_EMBEDDING_INPUTS ${_webapp_idx} _asset_name)
  math(EXPR _webapp_stage_idx "${_webapp_idx} + 1")
  list(GET LUCE_WEBAPP_EMBEDDING_INPUTS ${_webapp_stage_idx} _asset_stage_name)
  set(_webapp_src "${LUCE_WEBAPP_DIR}/${_asset_name}")
  if(EXISTS "${_webapp_src}")
    execute_process(
      COMMAND
        xxd -i -n ${_asset_stage_name} ${_webapp_src}
      OUTPUT_VARIABLE _webapp_blob
      OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND_ERROR_IS_FATAL ANY
    )
    if(_webapp_blob)
      string(APPEND LUCE_WEBAPP_ASSET_BLOBS "${_webapp_blob}\n\n")
      set(LUCE_WEBAPP_HAS_ASSETS ON)
    endif()
  else()
    message(STATUS "[webapp] skipping missing captive portal asset: ${_webapp_src}")
  endif()
endforeach()

if(LUCE_WEBAPP_HAS_ASSETS)
  string(APPEND LUCE_WEBAPP_ASSET_BLOBS "}\n")
  file(WRITE "${LUCE_WEBAPP_GENERATED_CPP}" "${LUCE_WEBAPP_ASSET_BLOBS}")
  target_sources(${COMPONENT_LIB} PRIVATE "${LUCE_WEBAPP_GENERATED_CPP}")
endif()
