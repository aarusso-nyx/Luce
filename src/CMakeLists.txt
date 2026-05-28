FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

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
