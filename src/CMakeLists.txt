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

# Developer-only TLS material. Generated locally by tools/gen_dev_cert.sh
# and never committed. When both files are present, embed them into the
# firmware as `extern "C"` arrays so dev-mode firmware can fall back to
# them when NVS has no http/cert_pem and http/key_pem. Production builds
# should never have these files in the tree — set LUCE_FORBID_DEV_CERT=1
# in the environment to fail the build if they slip in.
set(LUCE_DEV_CERT_PEM "${CMAKE_SOURCE_DIR}/tools/dev_cert.pem")
set(LUCE_DEV_KEY_PEM  "${CMAKE_SOURCE_DIR}/tools/dev_key.pem")
set(LUCE_DEV_CERT_GENERATED_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated_dev_cert.cpp")
set(LUCE_DEV_CERT_PRESENT OFF)

if(EXISTS "${LUCE_DEV_CERT_PEM}" AND EXISTS "${LUCE_DEV_KEY_PEM}")
  if(DEFINED ENV{LUCE_FORBID_DEV_CERT})
    message(FATAL_ERROR
      "tools/dev_cert.pem / dev_key.pem found but LUCE_FORBID_DEV_CERT is set. "
      "Delete the local dev cert before producing a production firmware.")
  endif()
  execute_process(
    COMMAND xxd -i -n luce_dev_cert_pem ${LUCE_DEV_CERT_PEM}
    OUTPUT_VARIABLE _dev_cert_blob
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
  )
  execute_process(
    COMMAND xxd -i -n luce_dev_key_pem ${LUCE_DEV_KEY_PEM}
    OUTPUT_VARIABLE _dev_key_blob
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
  )
  file(WRITE "${LUCE_DEV_CERT_GENERATED_CPP}"
       "extern \"C\" {\n${_dev_cert_blob}\n\n${_dev_key_blob}\n}\n")
  target_sources(${COMPONENT_LIB} PRIVATE "${LUCE_DEV_CERT_GENERATED_CPP}")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE LUCE_HAS_DEV_CERT=1)
  set(LUCE_DEV_CERT_PRESENT ON)
  message(STATUS "[tls] embedded dev cert from tools/dev_cert.pem")
else()
  message(STATUS "[tls] no tools/dev_cert.pem — dev-mode HTTPS will need NVS provisioning")
endif()
